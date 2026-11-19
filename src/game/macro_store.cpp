#include "game/macro_store.hpp"

#include <algorithm>
#include <cctype>

namespace wowee::game {

namespace {

std::string lowerCopy(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string escapeText(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (char c : text) {
        if (c == '\n') escaped += "\\n";
        else if (c == '\r') continue;
        else if (c == '\\') escaped += "\\\\";
        else escaped += c;
    }
    return escaped;
}

std::string unescapeText(const std::string& val) {
    std::string unescaped;
    unescaped.reserve(val.size());
    for (size_t i = 0; i < val.size(); ++i) {
        if (val[i] == '\\' && i + 1 < val.size() && val[i + 1] == 'n') { unescaped += '\n'; ++i; }
        else if (val[i] == '\\' && i + 1 < val.size() && val[i + 1] == '\\') { unescaped += '\\'; ++i; }
        else unescaped += val[i];
    }
    return unescaped;
}

constexpr char kPrefix[] = "macro_";
constexpr size_t kPrefixLength = sizeof(kPrefix) - 1;

}  // namespace

bool sameMacro(const Macro& a, const Macro& b) {
    return a.text == b.text && a.name == b.name && lowerCopy(a.icon) == lowerCopy(b.icon);
}

size_t macroHalfSize(const MacroSet& macros, bool character) {
    return static_cast<size_t>(std::count_if(macros.begin(), macros.end(), [&](const auto& entry) {
        return isCharacterMacroId(entry.first) == character;
    }));
}

uint32_t nextMacroId(const MacroSet& macros, bool character) {
    uint32_t next = 1;
    for (const auto& [id, macro] : macros) {
        (void)macro;
        if (isCharacterMacroId(id) == character) next = std::max(next, (id & 0x00FFFFFFu) + 1);
    }
    return (character ? kCharacterMacroFlag : 0u) | next;
}

uint32_t macroKeyId(const std::string& key) {
    if (key.compare(0, kPrefixLength, kPrefix) != 0) return 0;
    const size_t under = key.find('_', kPrefixLength);
    if (under == std::string::npos || under == kPrefixLength) return 0;
    const std::string field = key.substr(under + 1);
    if (field != "text" && field != "name" && field != "icon") return 0;
    uint32_t id = 0;
    for (size_t i = kPrefixLength; i < under; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(key[i]))) return 0;
        id = id * 10 + static_cast<uint32_t>(key[i] - '0');
    }
    return id;
}

bool readMacroKey(const std::string& key, const std::string& value, MacroSet& into) {
    const uint32_t id = macroKeyId(key);
    if (id == 0) return false;
    const std::string field = key.substr(key.find('_', kPrefixLength) + 1);
    Macro& macro = into[id];
    if (field == "text") macro.text = unescapeText(value);
    else if (field == "name") macro.name = value;
    else macro.icon = value;
    return true;
}

void writeMacroKeys(std::ostream& out, const MacroSet& macros) {
    for (const auto& [id, macro] : macros) {
        out << kPrefix << id << "_text=" << escapeText(macro.text) << "\n";
        if (!macro.name.empty()) out << kPrefix << id << "_name=" << macro.name << "\n";
        if (!macro.icon.empty()) out << kPrefix << id << "_icon=" << macro.icon << "\n";
    }
}

std::vector<MacroMergeStep> mergeAccountMacros(MacroSet& account, const MacroSet& found,
                                               uint32_t accountSlots, uint32_t characterSlots,
                                               MacroSet* ownCharacter) {
    using Outcome = MacroMergeStep::Outcome;
    std::vector<MacroMergeStep> steps;
    for (const auto& [id, macro] : found) {
        if (isCharacterMacroId(id)) continue;
        MacroMergeStep step{id, 0, Outcome::Deferred};
        const auto held = account.find(id);
        const auto twin = std::find_if(account.begin(), account.end(),
                                       [&](const auto& entry) { return sameMacro(entry.second, macro); });
        if (held != account.end() && sameMacro(held->second, macro)) {
            step = {id, id, Outcome::Present};
        } else if (held == account.end() && twin == account.end() &&
                   macroHalfSize(account, false) < accountSlots) {
            account[id] = macro;
            step = {id, id, Outcome::Added};
        } else if (ownCharacter && twin != account.end()) {
            step = {id, twin->first, Outcome::Joined};
        } else if (ownCharacter && macroHalfSize(account, false) < accountSlots) {
            const uint32_t to = nextMacroId(account, false);
            account[to] = macro;
            step = {id, to, Outcome::Moved};
        } else if (ownCharacter && macroHalfSize(*ownCharacter, true) < characterSlots) {
            const uint32_t to = nextMacroId(*ownCharacter, true);
            (*ownCharacter)[to] = macro;
            step = {id, to, Outcome::Moved};
        }
        steps.push_back(step);
    }
    return steps;
}

const char* macroMergeOutcomeName(MacroMergeStep::Outcome outcome) {
    switch (outcome) {
        case MacroMergeStep::Outcome::Present:  return "already in the account";
        case MacroMergeStep::Outcome::Added:    return "added to the account";
        case MacroMergeStep::Outcome::Joined:   return "joined the identical account macro";
        case MacroMergeStep::Outcome::Moved:    return "moved to a new id";
        case MacroMergeStep::Outcome::Deferred: return "left in the character's config";
    }
    return "?";
}

}  // namespace wowee::game
