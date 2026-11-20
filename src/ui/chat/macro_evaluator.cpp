// MacroEvaluator - WoW macro conditional parser and evaluator.
// Moved from evaluateMacroConditionals() in chat_panel_commands.cpp (Phase 4.4).
#include "ui/chat/macro_evaluator.hpp"
#include "ui/chat/i_game_state.hpp"
#include "ui/chat/i_modifier_state.hpp"
#include <algorithm>
#include <cctype>
#include <vector>

namespace wowee { namespace ui {

MacroEvaluator::MacroEvaluator(IGameState& gameState, IModifierState& modState)
    : gameState_(gameState), modState_(modState) {}

uint64_t MacroEvaluator::resolveEffectiveTarget(uint64_t tgt) const {
    if (tgt != static_cast<uint64_t>(-1) && tgt != 0)
        return tgt;
    return gameState_.getTargetGuid();
}

bool MacroEvaluator::evalCondition(const std::string& raw, uint64_t& tgt, std::string& tgtToken) const {
    // Trim
    std::string c = raw;
    size_t s = c.find_first_not_of(" \t");
    c = (s != std::string::npos) ? c.substr(s) : "";
    size_t e = c.find_last_not_of(" \t");
    if (e != std::string::npos) c.resize(e + 1);
    if (c.empty()) return true;

    auto resolveSpec = [&](const std::string& rawSpec) -> bool {
        std::string spec = rawSpec;
        for (char& ch : spec) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        tgtToken = rawSpec;
        if (spec == "player")          tgt = gameState_.getPlayerGuid();
        else if (spec == "focus")      tgt = gameState_.getFocusGuid();
        else if (spec == "target")     tgt = gameState_.getTargetGuid();
        else if (spec == "pet") {
            uint64_t pg = gameState_.getPetGuid();
            if (pg != 0) tgt = pg; else return false;
        }
        else if (spec == "mouseover") {
            uint64_t mo = gameState_.getMouseoverGuid();
            if (mo != 0) tgt = mo; else return false;
        }
        return true;
    };

    // --- @target specifiers ---
    if (c[0] == '@') {
        return resolveSpec(c.substr(1));
    }

    std::string cl = c;
    for (char& ch : cl) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

    // --- target=X specifiers ---
    if (cl.rfind("target=", 0) == 0) {
        return resolveSpec(c.substr(7));
    }

    // --- Modifier keys (both 2.0 modifier: and alias mod:) ---
    const bool shiftHeld = modState_.isShiftHeld();
    const bool ctrlHeld  = modState_.isCtrlHeld();
    const bool altHeld   = modState_.isAltHeld();
    const bool anyMod    = shiftHeld || ctrlHeld || altHeld;

    if (cl == "nomod" || cl == "nomodifier" || cl == "mod:none" || cl == "modifier:none")
        return !anyMod;
    if (cl == "mod" || cl == "modifier")
        return anyMod;
    if (cl.rfind("mod:", 0) == 0 || cl.rfind("modifier:", 0) == 0) {
        std::string mods = cl.substr(cl.find(':') + 1);
        bool ok = true;
        if (mods.find("shift") != std::string::npos && !shiftHeld) ok = false;
        if (mods.find("ctrl")  != std::string::npos && !ctrlHeld)  ok = false;
        if (mods.find("alt")   != std::string::npos && !altHeld)   ok = false;
        return ok;
    }

    // --- Combat ---
    if (c == "combat")   return gameState_.isInCombat();
    if (c == "nocombat") return !gameState_.isInCombat();

    // --- Effective target for exists/dead/help/harm ---
    uint64_t eff = resolveEffectiveTarget(tgt);

    if (c == "exists")   return gameState_.entityExists(eff);
    if (c == "noexists") return !gameState_.entityExists(eff);

    if (c == "dead")     return gameState_.entityIsDead(eff);
    if (c == "nodead")   return !gameState_.entityIsDead(eff);

    if (c == "harm" || c == "nohelp") return gameState_.entityIsHostile(eff);
    if (c == "help" || c == "noharm") return !gameState_.entityIsHostile(eff);

    // --- Mounted / swimming / flying ---
    if (c == "mounted")   return gameState_.isMounted();
    if (c == "nomounted") return !gameState_.isMounted();
    if (c == "swimming")   return gameState_.isSwimming();
    if (c == "noswimming") return !gameState_.isSwimming();
    if (c == "flying")   return gameState_.isFlying();
    if (c == "noflying") return !gameState_.isFlying();

    // --- Channeling / casting ---
    if (c == "channeling")   return gameState_.isCasting() && gameState_.isChanneling();
    if (c == "nochanneling") return !(gameState_.isCasting() && gameState_.isChanneling());
    if (c.rfind("channeling:", 0) == 0 && c.size() > 11) {
        if (!gameState_.isChanneling()) return false;
        std::string want = c.substr(11);
        for (char& ch : want) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        uint32_t castSpellId = gameState_.getCurrentCastSpellId();
        std::string sn = gameState_.getSpellName(castSpellId);
        for (char& ch : sn) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return sn == want;
    }
    if (c == "casting")   return gameState_.isCasting();
    if (c == "nocasting") return !gameState_.isCasting();

    // --- Stealthed ---
    // 2.4.3 spells it [stealth]/[nostealth]; [stealthed] is later.
    if (c == "stealthed" || c == "stealth")     return gameState_.isStealthed();
    if (c == "nostealthed" || c == "nostealth") return !gameState_.isStealthed();

    // --- Pet ---
    if (c == "pet")   return gameState_.hasPet();
    if (c == "nopet") return !gameState_.hasPet();

    // --- Indoors / outdoors ---
    if (c == "indoors" || c == "nooutdoors")  return gameState_.isIndoors();
    if (c == "outdoors" || c == "noindoors")  return !gameState_.isIndoors();

    // --- Group / raid ---
    if (c == "group" || c == "party") return gameState_.isInGroup();
    if (c == "nogroup")               return !gameState_.isInGroup();
    if (c == "raid")   return gameState_.isInRaid();
    if (c == "noraid") return !gameState_.isInRaid();

    // --- Talent spec ---
    if (c.rfind("spec:", 0) == 0) {
        uint8_t wantSpec = 0;
        try { wantSpec = static_cast<uint8_t>(std::stoul(c.substr(5))); } catch (...) {}
        return wantSpec > 0 && gameState_.getActiveTalentSpec() == (wantSpec - 1);
    }

    // --- Form / stance ---
    if (c == "noform" || c == "nostance" || c == "form:0" || c == "stance:0")
        return !gameState_.hasFormAura();

    // --- Buff / debuff ---
    if (c.rfind("buff:", 0) == 0 && c.size() > 5)
        return gameState_.hasAuraByName(tgt, c.substr(5), false);
    if (c.rfind("nobuff:", 0) == 0 && c.size() > 7)
        return !gameState_.hasAuraByName(tgt, c.substr(7), false);
    if (c.rfind("debuff:", 0) == 0 && c.size() > 7)
        return gameState_.hasAuraByName(tgt, c.substr(7), true);
    if (c.rfind("nodebuff:", 0) == 0 && c.size() > 9)
        return !gameState_.hasAuraByName(tgt, c.substr(9), true);

    // --- Vehicle ---
    if (c == "vehicle")   return gameState_.getVehicleId() != 0;
    if (c == "novehicle") return gameState_.getVehicleId() == 0;

    // Unknown → permissive (don't block)
    return true;
}

std::string MacroEvaluator::evaluate(const std::string& rawArg,
                                     uint64_t& targetOverride) const {
    const MacroParseResult res = evaluateDetailed(rawArg);
    targetOverride = res.targetGuid;
    return res.matched ? res.action : std::string{};
}

MacroParseResult MacroEvaluator::evaluateDetailed(const std::string& rawArg) const {
    // Split rawArg on ';' → alternatives
    std::vector<std::string> alts;
    {
        std::string cur;
        for (char ch : rawArg) {
            if (ch == ';') { alts.push_back(cur); cur.clear(); }
            else            cur += ch;
        }
        alts.push_back(cur);
    }

    // A bare empty / whitespace input (e.g. `/cleartarget` -> SecureCmdOptionParse(""))
    // matches with an empty action.
    if (alts.size() == 1 && alts[0].find_first_not_of(" \t") == std::string::npos) {
        return {.matched = true, .action = {}, .targetToken = {}, .targetGuid = static_cast<uint64_t>(-1)};
    }

    for (auto& alt : alts) {
        size_t fs = alt.find_first_not_of(" \t");
        if (fs == std::string::npos) continue;
        alt = alt.substr(fs);
        size_t ls = alt.find_last_not_of(" \t");
        if (ls != std::string::npos) alt.resize(ls + 1);

        if (!alt.empty() && alt[0] == '[') {
            // One or more [cond1,cond2][cond3] groups before the action text:
            // the first group whose conditions all hold wins.
            size_t pos = 0;
            bool malformed = false;
            bool matchedGroup = false;
            uint64_t matchedGuid = static_cast<uint64_t>(-1);
            std::string matchedToken;

            while (pos < alt.size() && alt[pos] == '[') {
                const size_t close = alt.find(']', pos + 1);
                if (close == std::string::npos) {
                    malformed = true;
                    break;
                }
                const std::string condStr = alt.substr(pos + 1, close - pos - 1);
                if (!matchedGroup) {
                    uint64_t tgt = static_cast<uint64_t>(-1);
                    std::string tokName;
                    bool pass = true;
                    size_t cp = 0;
                    while (pass) {
                        const size_t comma = condStr.find(',', cp);
                        const std::string tok = condStr.substr(
                            cp, comma == std::string::npos ? std::string::npos : comma - cp);
                        if (!evalCondition(tok, tgt, tokName)) { pass = false; break; }
                        if (comma == std::string::npos) break;
                        cp = comma + 1;
                    }
                    if (pass) {
                        matchedGroup = true;
                        matchedGuid = tgt;
                        matchedToken = std::move(tokName);
                    }
                }
                pos = close + 1;
                while (pos < alt.size() && (alt[pos] == ' ' || alt[pos] == '\t')) ++pos;
            }
            if (malformed) continue;
            if (matchedGroup) {
                std::string argPart = pos < alt.size() ? alt.substr(pos) : std::string{};
                const size_t as = argPart.find_first_not_of(" \t");
                argPart = (as != std::string::npos) ? argPart.substr(as) : "";
                return {.matched = true,
                        .action = std::move(argPart),
                        .targetToken = std::move(matchedToken),
                        .targetGuid = matchedGuid};
            }
        } else {
            // No condition block - default fallback always matches
            return {.matched = true, .action = alt, .targetToken = {}, .targetGuid = static_cast<uint64_t>(-1)};
        }
    }
    return {};
}

} // namespace ui
} // namespace wowee
