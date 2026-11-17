#include "game/spell_description_format.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <vector>

namespace wowee {
namespace game {
namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isAlpha(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; }

/// Minimum and maximum of one effect, signed, as WorldObject::
/// CalculateSpellEffectValue computes them before spell modifiers: base
/// points, level scaling, then the dice.
struct EffectRange { double min = 0.0; double max = 0.0; };

EffectRange effectRange(const SpellDescriptionRecord& r, int index, uint32_t casterLevel) {
    double base = r.basePoints[index];
    int32_t random = r.dieSides[index];
    if (r.realPointsPerLevel[index] != 0.0f || r.dicePerLevel[index] != 0.0f) {
        int32_t level = static_cast<int32_t>(casterLevel);
        if (r.maxLevel > 0 && level > static_cast<int32_t>(r.maxLevel)) level = static_cast<int32_t>(r.maxLevel);
        else if (level < static_cast<int32_t>(r.baseLevel)) level = static_cast<int32_t>(r.baseLevel);
        level -= static_cast<int32_t>(r.spellLevel);
        base += level * r.realPointsPerLevel[index];
        random += static_cast<int32_t>(level * r.dicePerLevel[index]);
    }
    const int32_t dice = r.baseDice[index];
    EffectRange out;
    if (random == 0 || random == 1) {
        out.min = out.max = base + dice;
    } else {
        out.min = base + std::min(dice, random);
        out.max = base + std::max(dice, random);
    }
    // The server keeps an int32; level scaling truncates toward zero.
    out.min = std::trunc(out.min);
    out.max = std::trunc(out.max);
    return out;
}

std::string integerText(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(std::llround(v)));
    return buf;
}

class Formatter {
public:
    Formatter(uint32_t selfId, const SpellDescriptionEnvironment& env) : self_(selfId), env_(env) {}

    std::string run(const std::string& text);

private:
    uint32_t self_;
    const SpellDescriptionEnvironment& env_;
    double lastNumber_ = 0.0;
    bool haveNumber_ = false;
    int depth_ = 0;

    const SpellDescriptionRecord* record(uint32_t id) const {
        return env_.spell ? env_.spell(id) : nullptr;
    }
    void report(const std::string& token) const {
        if (env_.unresolved) env_.unresolved(token);
    }

    /// A single-letter token of spell `id`; signed for expressions.
    bool letterValue(uint32_t id, char code, int index, bool signedValue, bool maximum,
                     double& out, std::string* textOut);

    // Expression parser.
    bool expression(const std::string& s, size_t& i, double& out);
    bool term(const std::string& s, size_t& i, double& out);
    bool factor(const std::string& s, size_t& i, double& out);
    bool reference(const std::string& s, size_t& i, double& out);
    bool arguments(const std::string& s, size_t& i, std::vector<double>& args);
    bool wholeExpression(const std::string& s, double& out);
};

bool Formatter::letterValue(uint32_t id, char code, int index, bool signedValue, bool maximum,
                            double& out, std::string* textOut) {
    const SpellDescriptionRecord* r = record(id);
    if (!r || index < 0 || index > 2) return false;
    const auto magnitude = [&](double v) { return signedValue ? v : std::abs(v); };
    switch (code) {
        case 's': case 'S': {
            const EffectRange e = effectRange(*r, index, env_.casterLevel);
            double lo = magnitude(e.min), hi = magnitude(e.max);
            if (lo > hi) std::swap(lo, hi);
            out = maximum ? hi : lo;
            if (textOut) {
                *textOut = lo == hi ? integerText(lo) : integerText(lo) + " to " + integerText(hi);
            }
            return true;
        }
        case 'm': case 'M': {
            const EffectRange e = effectRange(*r, index, env_.casterLevel);
            out = magnitude(code == 'M' ? e.max : e.min);
            if (textOut) *textOut = integerText(out);
            return true;
        }
        case 'o': case 'O': {
            uint32_t period = r->amplitudeMs[index];
            // Health and power regeneration auras (84, 85) state their amount
            // per five seconds and carry no interval.
            if (period == 0 && (r->auraType[index] == 84 || r->auraType[index] == 85)) period = 5000;
            if (period == 0 || r->durationMs <= 0) return false;
            const EffectRange e = effectRange(*r, index, env_.casterLevel);
            // Not whole ticks: native Darnassian Bleu (433, 17 per 5 sec over
            // 18 sec) reads "Restores 61 health" = 17 * 18 / 5.
            const double ticks = static_cast<double>(r->durationMs) / period;
            out = magnitude(e.min) * ticks;
            if (textOut) *textOut = integerText(out);
            return true;
        }
        case 't': case 'T':
            if (r->amplitudeMs[index] == 0) return false;
            out = r->amplitudeMs[index] / 1000.0;
            if (textOut) *textOut = formatSpellDescriptionNumber(out);
            return true;
        case 'd': case 'D':
            if (r->durationMs == 0) return false;
            out = r->durationMs / 1000.0;
            if (textOut) {
                if (r->durationMs < 0) return false;
                *textOut = formatSpellDescriptionDuration(r->durationMs);
            }
            return true;
        case 'a': case 'A':
            out = r->radius[index];
            if (textOut) *textOut = formatSpellDescriptionNumber(out);
            return true;
        case 'h': case 'H':
            out = r->procChance;
            break;
        case 'n': case 'N':
            out = r->procCharges;
            break;
        case 'u': case 'U':
            out = r->stackAmount;
            break;
        case 'x': case 'X':
            out = r->chainTargets[index];
            break;
        case 'i': case 'I':
            out = r->maxTargets;
            break;
        case 'v': case 'V':
            out = r->maxTargetLevel;
            break;
        case 'q': case 'Q':
            out = magnitude(r->miscValue[index]);
            break;
        case 'e': case 'E':
            out = r->multipleValue[index];
            if (textOut) *textOut = formatSpellDescriptionNumber(out);
            return true;
        case 'f': case 'F':
            out = r->damageMultiplier[index];
            if (textOut) *textOut = formatSpellDescriptionNumber(out);
            return true;
        case 'b': case 'B':
            out = r->pointsPerComboPoint[index];
            if (textOut) *textOut = formatSpellDescriptionNumber(out);
            return true;
        case 'r': case 'R':
            out = r->rangeMax;
            if (textOut) *textOut = formatSpellDescriptionNumber(out);
            return true;
        default:
            return false;
    }
    if (textOut) *textOut = integerText(out);
    return true;
}

// ---- expressions --------------------------------------------------------

void skipSpace(const std::string& s, size_t& i) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
}

bool Formatter::wholeExpression(const std::string& s, double& out) {
    size_t i = 0;
    if (!expression(s, i, out)) return false;
    skipSpace(s, i);
    return i == s.size();
}

bool Formatter::expression(const std::string& s, size_t& i, double& out) {
    if (!term(s, i, out)) return false;
    for (;;) {
        skipSpace(s, i);
        if (i >= s.size() || (s[i] != '+' && s[i] != '-')) return true;
        const char op = s[i++];
        double rhs = 0.0;
        if (!term(s, i, rhs)) return false;
        out = op == '+' ? out + rhs : out - rhs;
    }
}

bool Formatter::term(const std::string& s, size_t& i, double& out) {
    if (!factor(s, i, out)) return false;
    for (;;) {
        skipSpace(s, i);
        if (i >= s.size() || (s[i] != '*' && s[i] != '/')) return true;
        const char op = s[i++];
        double rhs = 0.0;
        if (!factor(s, i, rhs)) return false;
        if (op == '*') out *= rhs;
        else {
            if (rhs == 0.0) return false;
            out /= rhs;
        }
    }
}

bool Formatter::arguments(const std::string& s, size_t& i, std::vector<double>& args) {
    skipSpace(s, i);
    if (i >= s.size() || s[i] != '(') return false;
    ++i;
    for (;;) {
        double v = 0.0;
        if (!expression(s, i, v)) return false;
        args.push_back(v);
        skipSpace(s, i);
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == ')') { ++i; return true; }
        return false;
    }
}

bool Formatter::factor(const std::string& s, size_t& i, double& out) {
    skipSpace(s, i);
    if (i >= s.size()) return false;
    if (s[i] == '(') {
        ++i;
        if (!expression(s, i, out)) return false;
        skipSpace(s, i);
        if (i >= s.size() || s[i] != ')') return false;
        ++i;
        return true;
    }
    if (s[i] == '-') {
        ++i;
        if (!factor(s, i, out)) return false;
        out = -out;
        return true;
    }
    if (isDigit(s[i]) || s[i] == '.') {
        size_t j = i;
        while (j < s.size() && (isDigit(s[j]) || s[j] == '.')) ++j;
        out = std::strtod(s.substr(i, j - i).c_str(), nullptr);
        i = j;
        return true;
    }
    if (s[i] != '$') return false;
    ++i;
    return reference(s, i, out);
}

bool Formatter::reference(const std::string& s, size_t& i, double& out) {
    if (i >= s.size()) return false;
    if (s[i] == '{') {
        ++i;
        if (!expression(s, i, out)) return false;
        skipSpace(s, i);
        if (i >= s.size() || s[i] != '}') return false;
        ++i;
        return true;
    }
    if (s[i] == '<') {
        const size_t close = s.find('>', i + 1);
        if (close == std::string::npos || depth_ > 8) return false;
        const std::string name = s.substr(i + 1, close - i - 1);
        i = close + 1;
        const SpellDescriptionRecord* r = record(self_);
        if (!r || !r->declarations) return false;
        const std::string needle = "$" + name + "=";
        const size_t at = r->declarations->find(needle);
        if (at == std::string::npos) return false;
        const size_t from = at + needle.size();
        const size_t end = r->declarations->find_first_of("\r\n", from);
        const std::string decl = r->declarations->substr(from, end == std::string::npos ? std::string::npos : end - from);
        ++depth_;
        const bool ok = wholeExpression(decl, out);
        --depth_;
        return ok;
    }
    if (s[i] == '?') {
        ++i;
        if (i >= s.size() || (s[i] != 's' && s[i] != 'S')) return false;
        ++i;
        uint32_t id = 0;
        while (i < s.size() && isDigit(s[i])) id = id * 10 + static_cast<uint32_t>(s[i++] - '0');
        auto bracket = [&](std::string& body) {
            if (i >= s.size() || s[i] != '[') return false;
            int open = 0;
            const size_t start = ++i;
            for (; i < s.size(); ++i) {
                if (s[i] == '[') ++open;
                else if (s[i] == ']') {
                    if (open-- == 0) { body = s.substr(start, i - start); ++i; return true; }
                }
            }
            return false;
        };
        std::string yes, no;
        if (!bracket(yes) || !bracket(no) || depth_ > 8) return false;
        const bool known = env_.knowsSpell && env_.knowsSpell(id);
        ++depth_;
        const bool ok = wholeExpression(known ? yes : no, out);
        --depth_;
        return ok;
    }

    uint32_t refId = 0;
    bool hasRef = false;
    while (i < s.size() && isDigit(s[i])) {
        refId = refId * 10 + static_cast<uint32_t>(s[i++] - '0');
        hasRef = true;
    }
    size_t j = i;
    while (j < s.size() && isAlpha(s[j])) ++j;
    const std::string word = s.substr(i, j - i);
    if (word.empty()) return false;

    // Functions.
    if (!hasRef && (word == "cond" || word == "eq" || word == "gt" || word == "lt" ||
                    word == "gte" || word == "lte" || word == "max" || word == "min")) {
        i = j;
        std::vector<double> a;
        if (!arguments(s, i, a)) return false;
        if (word == "cond") { if (a.size() != 3) return false; out = a[0] != 0.0 ? a[1] : a[2]; return true; }
        if (a.size() != 2) return false;
        if (word == "eq") out = a[0] == a[1];
        else if (word == "gt") out = a[0] > a[1];
        else if (word == "lt") out = a[0] < a[1];
        else if (word == "gte") out = a[0] >= a[1];
        else if (word == "lte") out = a[0] <= a[1];
        else if (word == "max") out = std::max(a[0], a[1]);
        else out = std::min(a[0], a[1]);
        return true;
    }

    // Player quantities: $AP, $RAP, $PL, $MWS, $rwb and the rest.
    if (word.size() >= 2) {
        double v = 0.0;
        if (hasRef || !env_.player || !env_.player(word, v)) return false;
        i = j;
        out = v;
        return true;
    }

    // One letter and an optional effect index.
    const char code = s[i];
    i = j;
    int index = 0;
    if (i < s.size() && s[i] >= '1' && s[i] <= '3') index = s[i++] - '1';
    return letterValue(hasRef ? refId : self_, code, index, /*signedValue=*/true,
                       code == 'M', out, nullptr);
}

// ---- text ---------------------------------------------------------------

std::string Formatter::run(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    const size_t n = text.size();
    size_t i = 0;
    while (i < n) {
        const char c = text[i];
        if (c != '$' || i + 1 >= n) { out += c; ++i; continue; }
        const size_t start = i;
        const char next = text[i + 1];

        // $lsingular:plural;  $gmale:female;
        if (next == 'l' || next == 'L' || next == 'g' || next == 'G') {
            const size_t semi = text.find(';', i + 2);
            const size_t colon = text.find(':', i + 2);
            if (semi != std::string::npos && colon != std::string::npos && colon < semi) {
                const std::string a = text.substr(i + 2, colon - (i + 2));
                const std::string b = text.substr(colon + 1, semi - colon - 1);
                if (next == 'l' || next == 'L') out += (haveNumber_ && lastNumber_ == 1.0) ? a : b;
                else out += env_.female ? b : a;
                i = semi + 1;
                continue;
            }
        }

        // ${expr} and ${expr}.N
        if (next == '{') {
            int open = 0;
            size_t close = std::string::npos;
            for (size_t k = i + 2; k < n; ++k) {
                if (text[k] == '{') ++open;
                else if (text[k] == '}') { if (open-- == 0) { close = k; break; } }
            }
            if (close != std::string::npos) {
                double v = 0.0;
                size_t end = close + 1;
                int decimals = -1;
                if (end + 1 < n && text[end] == '.' && isDigit(text[end + 1])) {
                    decimals = text[end + 1] - '0';
                    end += 2;
                }
                if (wholeExpression(text.substr(i + 2, close - (i + 2)), v)) {
                    if (decimals < 0) {
                        out += integerText(v);
                        lastNumber_ = static_cast<double>(std::llround(v));
                    } else {
                        char buf[64];
                        std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
                        out += buf;
                        lastNumber_ = v;
                    }
                    haveNumber_ = true;
                    i = end;
                    continue;
                }
                report(text.substr(start, end - start));
                out += text.substr(start, end - start);
                i = end;
                continue;
            }
        }

        // $<name>
        if (next == '<') {
            const size_t close = text.find('>', i + 2);
            if (close != std::string::npos) {
                size_t k = i + 1;
                double v = 0.0;
                if (reference(text, k, v)) {
                    out += integerText(v);
                    lastNumber_ = static_cast<double>(std::llround(v));
                    haveNumber_ = true;
                } else {
                    report(text.substr(start, close + 1 - start));
                    out += text.substr(start, close + 1 - start);
                }
                i = close + 1;
                continue;
            }
        }

        // $/N;tok and $*N;tok
        if (next == '/' || next == '*') {
            size_t k = i + 2;
            double factor = 0.0;
            bool digits = false;
            while (k < n && isDigit(text[k])) { factor = factor * 10 + (text[k] - '0'); ++k; digits = true; }
            if (digits && k < n && text[k] == ';') {
                ++k;
                uint32_t refId = 0;
                bool hasRef = false;
                while (k < n && isDigit(text[k])) { refId = refId * 10 + static_cast<uint32_t>(text[k] - '0'); ++k; hasRef = true; }
                if (k < n && isAlpha(text[k])) {
                    const char code = text[k++];
                    int index = 0;
                    if (k < n && text[k] >= '1' && text[k] <= '3') index = text[k++] - '1';
                    double v = 0.0;
                    if (letterValue(hasRef ? refId : self_, code, index, false, code == 'M', v, nullptr) &&
                        (next == '*' || factor != 0.0)) {
                        v = next == '/' ? v / factor : v * factor;
                        out += formatSpellDescriptionNumber(v);
                        lastNumber_ = v;
                        haveNumber_ = true;
                        i = k;
                        continue;
                    }
                    report(text.substr(start, k - start));
                    out += text.substr(start, k - start);
                    i = k;
                    continue;
                }
            }
        }

        // $[id]letter[index]
        size_t k = i + 1;
        uint32_t refId = 0;
        bool hasRef = false;
        while (k < n && isDigit(text[k])) { refId = refId * 10 + static_cast<uint32_t>(text[k] - '0'); ++k; hasRef = true; }
        if (k < n && isAlpha(text[k])) {
            const char code = text[k++];
            if (code == 'z' && !hasRef) {
                if (!env_.homeName.empty()) { out += env_.homeName; i = k; continue; }
                report("$z");
                out += "$z";
                i = k;
                continue;
            }
            int index = 0;
            if (k < n && text[k] >= '1' && text[k] <= '3') index = text[k++] - '1';
            double v = 0.0;
            std::string shown;
            if (letterValue(hasRef ? refId : self_, code, index, false, code == 'M', v, &shown)) {
                out += shown;
                lastNumber_ = v;
                haveNumber_ = true;
                i = k;
                continue;
            }
            report(text.substr(start, k - start));
            out += text.substr(start, k - start);
            i = k;
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}

}  // namespace

std::string formatSpellDescriptionNumber(double value) {
    char buf[64];
    if (std::abs(value - std::round(value)) < 1e-9) {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(std::llround(value)));
        return buf;
    }
    std::snprintf(buf, sizeof(buf), "%.2f", value);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

std::string formatSpellDescriptionDuration(int32_t ms) {
    // GlobalStrings INT_SPELL_DURATION_DAYS/HOURS/MIN/SEC: "%d |4day:days;",
    // "%d |4hour:hrs;", "%d min", "%d sec". The largest unit that divides
    // the duration evenly.
    constexpr int32_t kDay = 86400000, kHour = 3600000, kMin = 60000;
    char buf[64];
    if (ms >= kDay && ms % kDay == 0) {
        const int v = ms / kDay;
        std::snprintf(buf, sizeof(buf), "%d %s", v, v == 1 ? "day" : "days");
    } else if (ms >= kHour && ms % kHour == 0) {
        const int v = ms / kHour;
        std::snprintf(buf, sizeof(buf), "%d %s", v, v == 1 ? "hour" : "hrs");
    } else if (ms >= kMin && ms % kMin == 0) {
        std::snprintf(buf, sizeof(buf), "%d min", ms / kMin);
    } else {
        return formatSpellDescriptionNumber(ms / 1000.0) + " sec";
    }
    return buf;
}

std::string formatSpellDescriptionText(uint32_t spellId, const std::string& text,
                                       const SpellDescriptionEnvironment& env) {
    Formatter f(spellId, env);
    return f.run(text);
}

}  // namespace game
}  // namespace wowee
