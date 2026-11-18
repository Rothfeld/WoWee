#include "game/text_tokens.hpp"

#include "core/logger.hpp"

#include <algorithm>
#include <string>

namespace wowee {
namespace game {

namespace {

std::string lowered(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::string trimmedSpaces(const std::string& s, size_t begin, size_t end) {
    while (begin < end && s[begin] == ' ') ++begin;
    while (end > begin && s[end - 1] == ' ') --end;
    return s.substr(begin, end - begin);
}

/// A "male:female;" switch starting at `pos`. Native reads the male half up to
/// the first ':' and the switch up to the first ';' after that, wherever those
/// are - a $g with no ';' of its own takes the next one in the text. The
/// female half stops at that ';' or at a second ':'. Measured:
///   "$gonly;]6[$gm:f]7[x;" reads male "only;]6[$gm", female "f]7[x"
///   "$gA;B:C;"             reads male "A;B", female "C"
///   "$gma:fe:nb;"          reads male "ma", female "fe"
///   "$g ma : fe ;"         reads "ma" and "fe" (spaces trimmed)
/// With no ':' after it nothing is read; with a ':' and no ';' after that,
/// only the male half is consumed. Either way nothing is written.
struct Switch {
    bool complete = false;
    std::string male, female;
    size_t next = 0;  // where reading continues
};

Switch readSwitch(const std::string& text, size_t pos) {
    Switch s;
    const size_t colon = text.find(':', pos);
    if (colon == std::string::npos) { s.next = pos; return s; }
    const size_t semi = text.find(';', colon + 1);
    if (semi == std::string::npos) { s.next = colon; return s; }
    s.complete = true;
    s.male = trimmedSpaces(text, pos, colon);
    s.female = trimmedSpaces(text, colon + 1, std::min(semi, text.find(':', colon + 1)));
    s.next = semi + 1;
    return s;
}

}  // namespace

std::string resolveTextTokens(const std::string& text, const TextSubject& subject) {
    if (text.find('$') == std::string::npos) return text;

    std::string out;
    out.reserve(text.size() + 32);
    size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c != '$') { out += c; ++i; continue; }

        // "$", then an optional number (a world state id), then the letter.
        size_t p = i + 1;
        uint32_t number = 0;
        while (p < text.size() && text[p] >= '0' && text[p] <= '9') {
            number = number * 10u + static_cast<uint32_t>(text[p] - '0');
            ++p;
        }
        const char letter = p < text.size() ? text[p] : '\0';
        const size_t after = p + 1;
        switch (letter) {
            case 'n': case 'N': out += subject.name; i = after; continue;
            case 'c': out += lowered(subject.className); i = after; continue;
            case 'C': out += subject.className; i = after; continue;
            case 'r': out += lowered(subject.raceName); i = after; continue;
            case 'R': out += subject.raceName; i = after; continue;
            case 'b': case 'B': out += '\n'; i = after; continue;
            case 'w': case 'W':
                out += std::to_string(subject.worldState ? subject.worldState(number) : 0u);
                i = after;
                continue;
            case 'd': case 'D': case 'e': case 'E': {
                // Every case measured on 8606 wrote 0 here - "$4262d" reads
                // "0 |4minute:minutes;", "$4262e" reads "0" - whatever the
                // number and whatever world state of that id the server had
                // sent, small counts, unix times and packed dates alike. What
                // the value would be read from is not known, so it is written
                // as the zero native shows, and said once.
                static bool told = false;
                if (!told) {
                    told = true;
                    LOG_WARNING("Text token $", number, letter,
                                " written as 0: its source is not known (native showed 0 in every case measured)");
                }
                out += (letter == 'd' || letter == 'D') ? "0 |4minute:minutes;" : "0";
                i = after;
                continue;
            }
            case 'g': case 'G': {
                const Switch s = readSwitch(text, after);
                if (s.complete) out += subject.female ? s.female : s.male;
                i = s.next;
                continue;
            }
            case 't': case 'T': {
                // Reads a male:female; switch the way $g does and then writes
                // neither half: the honour rank's name. "$tma:fe;" read
                // "private" for an Alliance character of rank 0.
                const Switch s = readSwitch(text, after);
                if (s.complete) out += letter == 't' ? lowered(subject.pvpRankName) : subject.pvpRankName;
                i = s.next;
                continue;
            }
            default:
                // Not a token. The "$" stays and reading resumes at the
                // letter - any digits read are dropped.
                out += '$';
                i = p;
                continue;
        }
    }
    return out;
}

}  // namespace game
}  // namespace wowee
