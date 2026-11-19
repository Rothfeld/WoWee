#include "game/spell_prerequisites.hpp"
// lua_spell_api.cpp - Spell info, casting, auras, and targeting Lua API bindings.
// Extracted from lua_engine.cpp as part of §5.1 (Tame LuaEngine).
#include "game/shapeshift_forms.hpp"
#include "addons/lua_api_helpers.hpp"
#include "game/item_text.hpp"
#include "game/game_utils.hpp"
#include "addons/lua_engine.hpp"
#include "core/logger.hpp"
#include <set>
#include <optional>
#include <limits>

namespace wowee::addons {

namespace {

/// The highest-rank known spell with this name, or 0 when the player knows
/// none by that name.
///
/// This is what `/cast Frostbolt` means: the name alone picks the best rank the
/// player has. Two bindings resolved it with their own copy of the walk, and
/// picking the wrong one is silent - the spell goes off, it is simply the
/// rank-one version.
///
/// Rank arrives from the DBC as text, "Rank 4", so the number is parsed out of
/// it. A spell with no rank string, which is most of them, sorts as rank zero:
/// right, because it is the only one of its name.
uint32_t highestKnownRankByName(game::GameHandler* gh, const std::string& name) {
    std::string wanted(name);
    toLowerInPlace(wanted);

    // The spellbook's spells, as native looks names up: Righteous Fury is
    // 25780 there; 25781, a hidden spell of the same name the server also
    // sends, is not a candidate.
    std::vector<uint32_t> book;
    for (const auto& tab : gh->getSpellBookTabs()) book.insert(book.end(), tab.spellIds.begin(), tab.spellIds.end());
    uint32_t bestId = 0;
    int bestRank = -1;
    for (uint32_t sid : book) {
        std::string known = gh->getSpellName(sid);
        toLowerInPlace(known);
        if (known != wanted) continue;

        int rank = 0;
        const std::string& rankText = gh->getSpellRank(sid);
        if (!rankText.empty()) {
            std::string lowered = rankText;
            toLowerInPlace(lowered);
            if (lowered.rfind("rank ", 0) == 0) {
                try { rank = std::stoi(lowered.substr(5)); } catch (...) {}
            }
        }
        if (rank > bestRank) { bestRank = rank; bestId = sid; }
    }
    return bestId;
}

}  // namespace


// ---- Finishing a spell that is waiting for a target ----
//
// A spell cast with no target leaves the cursor holding it until something is
// clicked; these are what the unit and party frames call when that click lands
// on them. This client resolves a target at cast time instead - there is no
// pending spell on the cursor to complete - so each answers false: nothing was
// waiting, so nothing was consumed, and the frame falls through to its ordinary
// click handling rather than swallowing it.
static int lua_SpellTargetUnit(lua_State* L) { lua_pushboolean(L, 0); return 1; }
static int lua_SpellTargetItem(lua_State* L) { lua_pushboolean(L, 0); return 1; }

// Dropping what the cursor holds onto a unit, which opens a trade. The cursor
// carries no item here, so there is nothing to drop.
static int lua_DropItemOnUnit(lua_State* L) { lua_pushboolean(L, 0); return 1; }

// The totem bar's per-slot spell choice, which needs the multi-cast action bar
// this client does not model.
static int lua_SetMultiCastSpell(lua_State* L) { (void)L; return 0; }


// --- Totems ---
//
// Four slots, each holding a spell and how long it has left. The interface
// wants the moment one was placed rather than its age, on the same clock
// GetTime() reports - a bar that is drawn from start and duration cannot use a
// figure measured from a different zero.

// GetTotemInfo(slot) → haveTotem, name, startTime, duration, icon
static int lua_GetTotemInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int slot = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!gh || slot < 1 || slot > game::GameHandler::NUM_TOTEM_SLOTS) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "");
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        lua_pushnil(L);
        return 5;
    }
    const auto& totem = gh->getTotemSlot(slot - 1);
    const bool active = totem.active();
    const double durationSec = totem.durationMs / 1000.0;
    const double remainingSec = totem.remainingMs() / 1000.0;

    lua_pushboolean(L, active ? 1 : 0);
    lua_pushstring(L, active ? gh->getSpellName(totem.spellId).c_str() : "");
    // Placed at however long ago it has already run for.
    lua_pushnumber(L, active ? luaGetTimeNow() - (durationSec - remainingSec) : 0.0);
    lua_pushnumber(L, active ? durationSec : 0.0);
    if (active) {
        const std::string icon = gh->getSpellIconPath(totem.spellId);
        if (!icon.empty()) lua_pushstring(L, icon.c_str()); else lua_pushnil(L);
    } else {
        lua_pushnil(L);
    }
    return 5;
}

// GetTotemTimeLeft(slot) → seconds
static int lua_GetTotemTimeLeft(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int slot = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!gh || slot < 1 || slot > game::GameHandler::NUM_TOTEM_SLOTS) {
        lua_pushnumber(L, 0);
        return 1;
    }
    lua_pushnumber(L, gh->getTotemSlot(slot - 1).remainingMs() / 1000.0);
    return 1;
}

static int lua_IsSpellInRange(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    const char* spellNameOrId = luaL_checkstring(L, 1);
    const char* uid = luaL_optstring(L, 2, "target");

    // Resolve spell ID
    uint32_t spellId = 0;
    if (spellNameOrId[0] >= '0' && spellNameOrId[0] <= '9') {
        spellId = static_cast<uint32_t>(strtoul(spellNameOrId, nullptr, 10));
    } else {
        // The rank that would actually be cast, not the first one found. A
        // range check exists to say whether pressing the button will work, so
        // it has to be asked of the same spell /cast would pick; where ranks
        // differ in range, answering about rank one greys out a button that
        // would have reached.
        spellId = highestKnownRankByName(gh, spellNameOrId);
    }
    if (spellId == 0) { return luaReturnNil(L); }

    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    return pushSpellRangeAnswer(L, gh, spellId, resolveUnitGuid(gh, uidStr));
}

// UnitIsVisible(unit) → boolean (entity exists in the client's entity manager)

/// Whether the player's class can remove a debuff of this dispel type.
///
/// The "RAID" filter FrameXML passes on party frames means "only what I can do
/// something about", and it is what showDispelDebuffs turns on. Without it the
/// filter string was accepted and ignored, so the frames showed every debuff
/// however little the healer could do - and answering the CVar without this
/// would have claimed a filter that does not filter.
///
/// Dispel types are Spell.dbc's: 1 Magic, 2 Curse, 3 Disease, 4 Poison. The
/// class table is 3.3.5's, and it is about what the class can remove from a
/// *friendly* target rather than what it can purge from an enemy.
static bool classCanDispel(uint8_t classId, uint8_t dispelType) {
    if (dispelType == 0) return false;
    constexpr uint8_t kMagic = 1, kCurse = 2, kDisease = 3, kPoison = 4;
    switch (classId) {
        case 2:  // Paladin - Cleanse
            return dispelType == kMagic || dispelType == kPoison || dispelType == kDisease;
        case 5:  // Priest - Dispel Magic, Abolish Disease
            return dispelType == kMagic || dispelType == kDisease;
        case 7:  // Shaman - Cure Toxins, Cleanse Spirit at 51
            return dispelType == kCurse || dispelType == kPoison || dispelType == kDisease;
        case 8:  // Mage - Remove Curse
            return dispelType == kCurse;
        case 11: // Druid - Remove Curse, Abolish Poison
            return dispelType == kCurse || dispelType == kPoison;
        case 6:  // Death Knight - the pet's Leech, and unholy's disease removal
            return dispelType == kDisease;
        case 9:  // Warlock - the Felhunter's Devour Magic
            return dispelType == kMagic;
        default: // Warrior, Hunter, Rogue: nothing
            return false;
    }
}

static bool auraVisible(game::GameHandler& gh, uint32_t spellId) {
    gh.getSpellName(spellId);
    const auto found = gh.spellNameCacheRef().find(spellId);
    // Server-only helper spells have no client DBC entry or display icon.
    if (found == gh.spellNameCacheRef().end()) return false;
    return !(found->second.attr & 0x80u) && !(found->second.attrEx & 0x10000000u);
}

/// Milliseconds on the same clock AuraSlot::receivedAtMs was stamped from.
static uint64_t auraNowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

const game::AuraSlot* findUnitAura(game::GameHandler& gh, std::string unit, int index,
                                   bool wantBuff, const char* filter) {
    if (index < 1) return nullptr;
    toLowerInPlace(unit);

    const std::vector<game::AuraSlot>* auras = nullptr;
    if (unit == "player")      auras = &gh.getPlayerAuras();
    else if (unit == "target") auras = &gh.getTargetAuras();
    else {
        // Try party/raid/focus via GUID lookup in unitAurasCache
        uint64_t guid = resolveUnitGuid(&gh, unit);
        if (guid != 0) auras = gh.getUnitAuras(guid);
    }
    if (!auras) return nullptr;

    // "RAID" on a debuff means only what this character can remove. The index
    // counts into the filtered list, so the test has to come before the count
    // - filtering after it would answer the wrong aura for every index past
    // the first one skipped.
    bool dispellableOnly = false;
    if (!wantBuff && filter) {
        std::string f(filter);
        for (char& c : f) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        dispellableOnly = f.find("RAID") != std::string::npos;
    }
    const uint8_t playerClass = gh.getPlayerClass();

    // Filter to buffs or debuffs and find the Nth one
    int found = 0;
    for (const auto& aura : *auras) {
        if (aura.isEmpty() || aura.spellId == 0 || !auraVisible(gh, aura.spellId)) continue;
        bool isDebuff = (aura.flags & 0x80) != 0;
        if (wantBuff ? isDebuff : !isDebuff) continue;
        if (dispellableOnly &&
            !classCanDispel(playerClass, gh.getSpellDispelType(aura.spellId))) {
            continue;
        }
        if (++found == index) return &aura;
    }
    return nullptr;
}

static int lua_UnitAura(lua_State* L, bool wantBuff) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    std::string uidStr(luaL_optstring(L, 1, "player"));
    toLowerInPlace(uidStr);
    const int index = static_cast<int>(luaL_optnumber(L, 2, 1));
    const game::AuraSlot* found = findUnitAura(*gh, uidStr, index, wantBuff, luaL_optstring(L, 3, ""));
    if (found) {
        const game::AuraSlot& aura = *found;
        // Return: name, rank, icon, count, debuffType, duration, expirationTime, ...spellId
        std::string name = gh->getSpellName(aura.spellId);
        if (name.empty()) {
            // An aura this client's Spell.dbc has no row for draws as
            // "Unknown" with no icon, and nothing said which one it was.
            // A realm's own spells are the usual cause - a server can put
            // any id on a unit, and only a patched client knows its name.
            static std::set<uint32_t> saidNoName;
            if (saidNoName.insert(aura.spellId).second) {
                LOG_WARNING("Aura spell ", aura.spellId, " on ", uidStr,
                            " has no Spell.dbc row, so it shows as 'Unknown' with no "
                            "icon (", (aura.flags & 0x80) ? "debuff" : "buff",
                            ", flags 0x", std::hex, static_cast<int>(aura.flags), std::dec,
                            ", caster 0x", std::hex, aura.casterGuid, std::dec, ")");
            }
        }
        lua_pushstring(L, name.empty() ? "Unknown" : name.c_str()); // name
        // Spell.dbc Rank; 2.4.3 answers "Rank 8" for Devotion Aura on the
        // player and on a party member, "" for Righteous Fury.
        lua_pushstring(L, gh->getSpellRank(aura.spellId).c_str());
        std::string iconPath = gh->getSpellIconPath(aura.spellId);
        if (!iconPath.empty()) lua_pushstring(L, iconPath.c_str());
        else lua_pushnil(L);             // icon texture path
        lua_pushnumber(L, aura.charges); // count
        // 2.4.3's shape, which stock targetframe.lua reads as
        //   UnitBuff   -> name, rank, icon, count, duration, timeLeft
        //   UnitDebuff -> name, rank, icon, count, debuffType, duration, timeLeft
        // with duration nil for an aura whose timer this client does not know.
        // Answering the 3.x shape put the dispel type where TargetFrame reads a
        // buff's duration: `duration > 0` raised on any Magic buff on the
        // target ("attempt to compare number with string") and the target
        // frame drew none of its buffs or debuffs.
        const int iface = interfaceVersion(L);
        if (iface >= 20000 && iface < 30000) {
            if (!wantBuff) {
                switch (gh->getSpellDispelType(aura.spellId)) {
                    case 1:  lua_pushstring(L, "Magic");   break;
                    case 2:  lua_pushstring(L, "Curse");   break;
                    case 3:  lua_pushstring(L, "Disease"); break;
                    case 4:  lua_pushstring(L, "Poison");  break;
                    default: lua_pushnil(L);               break;
                }
            }
            // The player's own auras all have a known timer: a permanent one
            // (Devotion Aura) answers duration 0 and timeLeft 0 on 2.4.3.
            // Another unit's untimed aura answers neither.
            const bool own = resolveUnitGuid(gh, uidStr) == gh->getPlayerGuid();
            if (aura.durationMs > 0 && aura.maxDurationMs > 0) {
                lua_pushnumber(L, aura.maxDurationMs / 1000.0);
                lua_pushnumber(L, aura.getRemainingMs(auraNowMs()) / 1000.0);
            } else if (own) {
                lua_pushnumber(L, 0);
                lua_pushnumber(L, 0);
            } else {
                lua_pushnil(L);
                lua_pushnil(L);
            }
            return wantBuff ? 6 : 7;
        }
        // debuffType: resolve from Spell.dbc dispel type
        {
            uint8_t dt = gh->getSpellDispelType(aura.spellId);
            switch (dt) {
                case 1:  lua_pushstring(L, "Magic");  break;
                case 2:  lua_pushstring(L, "Curse");  break;
                case 3:  lua_pushstring(L, "Disease"); break;
                case 4:  lua_pushstring(L, "Poison"); break;
                default: lua_pushnil(L);              break;
            }
        }
        lua_pushnumber(L, aura.maxDurationMs > 0 ? aura.maxDurationMs / 1000.0 : 0); // duration
        // expirationTime: GetTime() + remaining seconds (so addons can compute countdown)
        if (aura.durationMs > 0) {
            uint64_t auraNowMs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            int32_t remMs = aura.getRemainingMs(auraNowMs);
            lua_pushnumber(L, luaGetTimeNow() + remMs / 1000.0);
        } else {
            lua_pushnumber(L, 0);  // permanent aura
        }
        // caster: return unit ID string if caster is known
        if (aura.casterGuid != 0) {
            if (aura.casterGuid == gh->getPlayerGuid())
                lua_pushstring(L, "player");
            else if (aura.casterGuid == gh->getTargetGuid())
                lua_pushstring(L, "target");
            else if (aura.casterGuid == gh->getFocusGuid())
                lua_pushstring(L, "focus");
            else if (aura.casterGuid == gh->getPetGuid())
                lua_pushstring(L, "pet");
            else {
                char cBuf[32];
                snprintf(cBuf, sizeof(cBuf), "0x%016llX", (unsigned long long)aura.casterGuid);
                lua_pushstring(L, cBuf);
            }
        } else {
            lua_pushnil(L);
        }
        lua_pushboolean(L, 0);           // isStealable
        lua_pushboolean(L, 0);           // shouldConsolidate
        lua_pushnumber(L, aura.spellId); // spellId
        return 11;
    }
    lua_pushnil(L);
    return 1;
}

// ── The 1.12 buff API ───────────────────────────────────────────────────────
//
// A vanilla interface does not call UnitBuff. It asks for a *slot* and then
// asks that slot four more questions, which is a different shape: UnitBuff
// answers a name and counts into a filtered list, and these answer an opaque
// number the caller hands straight back to the rest of them.
//
// buffframe.lua walks slots upward until it is told there is nothing there,
// and the terminator is -1 rather than nil. Unbound, GetPlayerBuff answered
// nil through the missing-API fallback, so `if buffIndex < 0` compared nil
// with a number on the first line of BuffButton_Update. That raised while the
// file was being read, which loses the file whole: no buff has ever appeared
// on a 1.12 interface, and BuffFrame.xml never built.

/// The player's aura in a slot, or nullptr for an empty or out-of-range one.
///
/// The slot is the index into the player's own aura array rather than a
/// position in a filtered list, so it stays meaningful after the caller has
/// stopped filtering - which is the whole point of handing it back.
static const game::AuraSlot* playerAuraAt(lua_State* L, game::GameHandler* gh, int slot) {
    if (interfaceVersion(L) >= 20000) --slot;
    if (!gh || slot < 0) return nullptr;
    const auto& auras = gh->getPlayerAuras();
    if (static_cast<size_t>(slot) >= auras.size()) return nullptr;
    const game::AuraSlot& aura = auras[static_cast<size_t>(slot)];
    return aura.isEmpty() ? nullptr : &aura;
}

/// Whether a filter asks for debuffs. 1.12 spells that "HARMFUL"; the other
/// words it can carry - PASSIVE, CANCELABLE - do not change which half is
/// wanted, so they are not read.
static bool filterWantsHarmful(const char* filter) {
    if (!filter) return false;
    std::string upper(filter);
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return upper.find("HARMFUL") != std::string::npos;
}

// GetPlayerBuff(index, filter) -> slot, untilCancelled
//
// Vanilla uses zero-based indexes and -1 for no buff; TBC uses one-based
// indexes and 0. Returned slots use the same convention as input indexes.
static int lua_GetPlayerBuff(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int base = interfaceVersion(L) >= 20000 ? 1 : 0;
    const int wanted = static_cast<int>(luaL_optnumber(L, 1, base)) - base;
    const bool harmful = filterWantsHarmful(luaL_optstring(L, 2, "HELPFUL"));
    if (gh && wanted >= 0) {
        const auto& auras = gh->getPlayerAuras();
        int seen = 0;
        for (size_t i = 0; i < auras.size(); ++i) {
            const game::AuraSlot& aura = auras[i];
            if (aura.isEmpty() || !auraVisible(*gh, aura.spellId)) continue;
            const bool isDebuff = (aura.flags & 0x80) != 0;
            if (isDebuff != harmful) continue;
            if (seen++ != wanted) continue;
            lua_pushnumber(L, static_cast<double>(i + base));
            // A buff with no duration is one the player dismisses rather than
            // waits out, and the interface draws it without a timer.
            lua_pushnumber(L, aura.durationMs > 0 ? 0 : 1);
            return 2;
        }
    }
    // Match the loaded interface's empty-slot sentinel.
    lua_pushnumber(L, base - 1);
    lua_pushnil(L);
    return 2;
}

// GetPlayerBuffTexture(slot) -> texture
static int lua_GetPlayerBuffTexture(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto* aura = playerAuraAt(L, gh, static_cast<int>(luaL_optnumber(L, 1, -1)));
    if (!aura) return luaReturnNil(L);
    const std::string icon = gh->getSpellIconPath(aura->spellId);
    if (icon.empty()) return luaReturnNil(L);
    lua_pushstring(L, icon.c_str());
    return 1;
}

// GetPlayerBuffName(slot) -> name, rank. 2.4.3: "Devotion Aura", "Rank 8".
static int lua_GetPlayerBuffName(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto* aura = playerAuraAt(L, gh, static_cast<int>(luaL_optnumber(L, 1, -1)));
    if (!aura) return luaReturnNil(L);
    lua_pushstring(L, gh->getSpellName(aura->spellId).c_str());
    lua_pushstring(L, gh->getSpellRank(aura->spellId).c_str());
    return 2;
}

// GetPlayerBuffTimeLeft(slot) -> seconds
static int lua_GetPlayerBuffTimeLeft(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto* aura = playerAuraAt(L, gh, static_cast<int>(luaL_optnumber(L, 1, -1)));
    // Zero rather than nil: buffframe.lua divides and compares this, and a
    // permanent aura is honestly zero time left rather than an absent answer.
    if (!aura || aura->durationMs <= 0) { lua_pushnumber(L, 0); return 1; }
    lua_pushnumber(L, aura->getRemainingMs(auraNowMs()) / 1000.0);
    return 1;
}

// GetPlayerBuffApplications(slot) -> count
static int lua_GetPlayerBuffApplications(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto* aura = playerAuraAt(L, gh, static_cast<int>(luaL_optnumber(L, 1, -1)));
    lua_pushnumber(L, aura ? aura->charges : 0);
    return 1;
}

// GetPlayerBuffDispelType(slot) -> "Magic" | "Curse" | "Disease" | "Poison"
static int lua_GetPlayerBuffDispelType(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto* aura = playerAuraAt(L, gh, static_cast<int>(luaL_optnumber(L, 1, -1)));
    if (!aura) return luaReturnNil(L);
    switch (gh->getSpellDispelType(aura->spellId)) {
        case 1:  lua_pushstring(L, "Magic");   return 1;
        case 2:  lua_pushstring(L, "Curse");   return 1;
        case 3:  lua_pushstring(L, "Disease"); return 1;
        case 4:  lua_pushstring(L, "Poison");  return 1;
        default: break;
    }
    return luaReturnNil(L);
}

// CancelPlayerBuff(slot)
static int lua_CancelPlayerBuff(lua_State* L) {
    auto* gh = getGameHandler(L);
    // PossessBar_Clicked(2) passes the name GetPossessInfo(1) gave: the
    // possession's spell, which for Mind Control is the player's channel and
    // for Control Piece an aura on the player.
    if (gh && lua_type(L, 1) == LUA_TSTRING) {
        const std::string name = lua_tostring(L, 1);
        const uint32_t possess = gh->getPossessSpellId();
        if (possess && gh->getSpellName(possess) == name) {
            if (gh->isChanneling() && gh->getCurrentCastSpellId() == possess) gh->cancelCast();
            else gh->cancelAura(possess);
            return 0;
        }
        for (const auto& a : gh->getPlayerAuras())
            if (!a.isEmpty() && gh->getSpellName(a.spellId) == name) { gh->cancelAura(a.spellId); return 0; }
        return 0;
    }
    const auto* aura = playerAuraAt(L, gh, static_cast<int>(luaL_optnumber(L, 1, -1)));
    if (aura) gh->cancelAura(aura->spellId);
    return 0;
}

static int lua_UnitBuff(lua_State* L) { return lua_UnitAura(L, true); }
static int lua_UnitDebuff(lua_State* L) { return lua_UnitAura(L, false); }

// UnitAura(unit, index, filter) - generic aura query with filter string
// filter: "HELPFUL" = buffs, "HARMFUL" = debuffs, "PLAYER" = cast by player,
//         "HELPFUL|PLAYER" = buffs cast by player, etc.
static int lua_UnitAuraGeneric(lua_State* L) {
    const char* filter = luaL_optstring(L, 3, "HELPFUL");
    std::string f(filter ? filter : "HELPFUL");
    for (char& c : f) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    bool wantBuff = (f.find("HARMFUL") == std::string::npos);
    return lua_UnitAura(L, wantBuff);
}

// ---------- UnitCastingInfo / UnitChannelInfo ----------
// Internal helper: pushes cast/channel info for a unit.
// Returns number of Lua return values (0 if not casting/channeling the requested type).
static int lua_UnitCastInfo(lua_State* L, bool wantChannel) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }

    const char* uid = luaL_optstring(L, 1, "player");
    std::string uidStr(uid ? uid : "player");

    // Use shared GetTime() epoch for consistent timestamps
    double nowSec = luaGetTimeNow();

    // Resolve cast state for the unit
    bool isCasting = false;
    bool isChannel = false;
    uint32_t spellId = 0;
    float timeTotal = 0.0f;
    float timeRemaining = 0.0f;
    bool interruptible = true;

    if (uidStr == "player") {
        isCasting = gh->isCasting();
        isChannel = gh->isChanneling();
        spellId = gh->getCurrentCastSpellId();
        timeTotal = gh->getCastTimeTotal();
        timeRemaining = gh->getCastTimeRemaining();
        // Player interruptibility: always true for own casts (server controls actual interrupt)
        interruptible = true;
    } else {
        uint64_t guid = resolveUnitGuid(gh, uidStr);
        if (guid == 0) { return luaReturnNil(L); }
        const auto* state = gh->getUnitCastState(guid);
        if (!state) { return luaReturnNil(L); }
        isCasting = state->casting;
        isChannel = state->isChannel;
        spellId = state->spellId;
        timeTotal = state->timeTotal;
        timeRemaining = state->timeRemaining;
        interruptible = state->interruptible;
    }

    if (!isCasting) { return luaReturnNil(L); }

    // UnitCastingInfo: only returns for non-channel casts
    // UnitChannelInfo: only returns for channels
    if (wantChannel != isChannel) { return luaReturnNil(L); }

    // Spell name + icon
    const std::string& name = gh->getSpellName(spellId);
    std::string iconPath = gh->getSpellIconPath(spellId);

    // Time values in milliseconds (WoW API convention)
    double startTimeMs = (nowSec - (timeTotal - timeRemaining)) * 1000.0;
    double endTimeMs   = (nowSec + timeRemaining) * 1000.0;

    // The real signatures, which are one wider than what this used to send:
    //
    //   UnitCastingInfo → name, nameSubtext, text, texture, startTime,
    //                     endTime, isTradeSkill, castID, notInterruptible
    //   UnitChannelInfo → the same without castID
    //
    // nameSubtext is the rank, and leaving it out shifted every value after it
    // by one. castingbarframe.lua destructures all nine by name, so it read the
    // icon path as the bar's text, a timestamp as the texture, and a boolean as
    // endTime - then computed (endTime - startTime) and raised, which is why no
    // cast bar was ever drawn rather than a wrong one.
    lua_pushstring(L, name.empty() ? "Unknown" : name.c_str()); // name
    const std::string& rank = gh->getSpellRank(spellId);
    lua_pushstring(L, rank.c_str());                             // nameSubtext
    // What the bar is labelled with, which is the third value and not the
    // first: castingbarframe.lua reads `text` into barText and never touches
    // `name`. An empty string there is a bar that fills up saying nothing -
    // no way to tell a Fireball from a Frostbolt, or a cast from a channel.
    //
    // The spell's own name, except for an OPEN_LOCK cast at a game object,
    // where it is the object's opening or closing text - empty for most. The
    // original shows Opening at the Scarlet Monastery torch as name "Opening"
    // and text "", and at Rizzle's Guarded Plans as text "Examining".
    //
    // A channel is labelled with GlobalStrings CHANNELING ("Channeling"),
    // which no FrameXML file reads: the client hands it over as `text`.
    // Original 2.4.3 (pair 17, Strange Gong 187359 in Zul'Aman):
    // UnitChannelInfo("player") answered name "Zul'Aman Object Visual",
    // rank "", text "Channeling", and the bar read "Channeling".
    std::optional<std::string> caption;
    if (uidStr == "player" && !wantChannel) caption = gh->getCastBarCaption();
    if (wantChannel) {
        lua_getglobal(L, "CHANNELING");
        if (!lua_isstring(L, -1))
            return luaL_error(L, "UnitChannelInfo: GlobalStrings CHANNELING is not loaded");
    } else if (caption) {
        lua_pushstring(L, caption->c_str());                    // text
    } else {
        lua_pushstring(L, name.empty() ? "Unknown" : name.c_str());
    }
    if (!iconPath.empty()) lua_pushstring(L, iconPath.c_str());
    else lua_pushstring(L, "Interface\\Icons\\INV_Misc_QuestionMark");  // texture
    lua_pushnumber(L, startTimeMs);                              // startTime (ms)
    lua_pushnumber(L, endTimeMs);                                // endTime (ms)
    lua_pushboolean(L, gh->isProfessionSpell(spellId) ? 1 : 0); // isTradeSkill
    if (!wantChannel) {
        lua_pushnumber(L, spellId);                              // castID (UnitCastingInfo only)
    }
    lua_pushboolean(L, interruptible ? 0 : 1);                  // notInterruptible
    return wantChannel ? 8 : 9;
}

static int lua_UnitCastingInfo(lua_State* L) { return lua_UnitCastInfo(L, false); }
static int lua_UnitChannelInfo(lua_State* L) { return lua_UnitCastInfo(L, true); }

static int lua_CastSpellByName(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const char* name = luaL_checkstring(L, 1);
    if (!name || !*name) return 0;

    const uint32_t bestId = highestKnownRankByName(gh, name);
    if (bestId != 0) {
        // The same second argument, and the same branch of SECURE_ACTIONS.spell
        // reaches here: a spell named rather than numbered is cast by name, and
        // the unit clicked still says who it lands on.
        uint64_t target = 0;
        if (const char* unit = luaL_optstring(L, 2, nullptr)) {
            std::string u(unit);
            toLowerInPlace(u);
            target = resolveUnitGuid(gh, u);
        }
        if (target == 0 && gh->hasTarget()) target = gh->getTargetGuid();
        gh->castSpell(bestId, target);
    }
    return 0;
}

static int lua_IsSpellKnown(lua_State* L) {
    auto* gh = getGameHandler(L);
    uint32_t spellId = static_cast<uint32_t>(luaL_checknumber(L, 1));
    lua_pushboolean(L, gh && gh->getKnownSpells().count(spellId));
    return 1;
}

// --- Spell Book Tab API ---

// GetNumSpellTabs() → count
static int lua_GetNumSpellTabs(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnZero(L); }
    lua_pushnumber(L, gh->getSpellBookTabs().size());
    return 1;
}

// GetSpellTabInfo(tabIndex) → name, texture, offset, numSpells,
//                             highestRankOffset, highestRankNumSpells
// tabIndex is 1-based; offset is 1-based global spell book slot
//
// 2.4.3 answers the first four only (FrameXML there reads four). From WotLK:
// six values, and the last two are not optional. SpellBook_GetTabInfo unpacks
// all six and then, when ShowAllSpellRanks is off - which is the default -
// throws away offset and numSpells and uses the highest-rank pair instead. So
// a four-value answer leaves both nil on the ordinary path and the spellbook
// raises on the arithmetic, showing nothing at all.
//
// The comment here used to say four. Cutting the returns to match it would
// have emptied the spellbook, which is exactly what a stubbed four-value
// version did when it was tried.
static int lua_GetSpellTabInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    int tabIdx = static_cast<int>(luaL_checknumber(L, 1));
    // An empty tab rather than one nil, because the caller does arithmetic on
    // what comes back without checking it.
    //
    // SpellBookFrame's own OnLoad ends with SpellBookSkillLineTab_OnClick(nil,
    // 1), and three frames down that divides numSpells by the page size. In
    // WoW tab one is General and always exists, so FrameXML never guards it -
    // here it does not exist until the spell list has arrived, and a single
    // nil made that a raise inside OnLoad, which loses the whole file.
    // SpellBookFrame.xml was failing to load outright.
    //
    // Six zeroes give a book with no pages, which is what a player with no
    // spells yet has, and the frame loads and fills in when they arrive.
    auto emptyTab = [](lua_State* s) {
        lua_pushstring(s, "");   // name
        lua_pushstring(s, "");   // texture
        lua_pushnumber(s, 0);    // offset
        lua_pushnumber(s, 0);    // numSpells
        lua_pushnumber(s, 0);    // highestRankOffset
        lua_pushnumber(s, 0);    // highestRankNumSpells
        return 6;
    };
    // 2.4.3 answers four values, and a tab that does not exist is
    // nil, nil, 0, 0 (tab 0 and tab GetNumSpellTabs() + 1 alike).
    const bool tbc = game::isActiveExpansion("tbc");
    auto missingTbcTab = [](lua_State* s) {
        lua_pushnil(s);
        lua_pushnil(s);
        lua_pushnumber(s, 0);
        lua_pushnumber(s, 0);
        return 4;
    };
    if (!gh || tabIdx < 1) {
        return tbc ? missingTbcTab(L) : emptyTab(L);
    }
    const auto& tabs = gh->getSpellBookTabs();
    if (tabIdx > static_cast<int>(tabs.size())) {
        return tbc ? missingTbcTab(L) : emptyTab(L);
    }
    // Compute offset: sum of spells in all preceding tabs (1-based)
    int offset = 0;
    for (int i = 0; i < tabIdx - 1; ++i)
        offset += static_cast<int>(tabs[i].spellIds.size());
    const auto& tab = tabs[tabIdx - 1];
    lua_pushstring(L, tab.name.c_str());           // name
    lua_pushstring(L, tab.texture.c_str());        // texture
    lua_pushnumber(L, offset);                     // offset (0-based for WoW compat)
    lua_pushnumber(L, tab.spellIds.size());        // numSpells
    if (tbc) return 4;
    // The highest-rank pair, which is what FrameXML actually reads: with
    // ShowAllSpellRanks off - the default - SpellBook_GetTabInfo throws away
    // the first offset and count and keeps these. Returning four values left
    // numSpells nil and the page count divided by nothing. This client does
    // not track ranks separately, so the whole tab is the highest rank.
    lua_pushnumber(L, offset);                     // highestRankOffset
    lua_pushnumber(L, tab.spellIds.size());        // highestRankNumSpells
    return 6;
}

// GetSpellBookItemInfo(slot, bookType) → "SPELL", spellId
// slot is 1-based global spell book index
static int lua_GetSpellBookItemInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    int slot = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || slot < 1) {
        lua_pushstring(L, "SPELL");
        lua_pushnumber(L, 0);
        return 2;
    }
    const auto& tabs = gh->getSpellBookTabs();
    int idx = slot; // 1-based
    for (const auto& tab : tabs) {
        if (idx <= static_cast<int>(tab.spellIds.size())) {
            lua_pushstring(L, "SPELL");
            lua_pushnumber(L, tab.spellIds[idx - 1]);
            return 2;
        }
        idx -= static_cast<int>(tab.spellIds.size());
    }
    lua_pushstring(L, "SPELL");
    lua_pushnumber(L, 0);
    return 2;
}

// GetSpellBookItemName(slot, bookType) → name, subName
static int lua_GetSpellBookItemName(lua_State* L) {
    auto* gh = getGameHandler(L);
    int slot = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || slot < 1) { return luaReturnNil(L); }
    const auto& tabs = gh->getSpellBookTabs();
    int idx = slot;
    for (const auto& tab : tabs) {
        if (idx <= static_cast<int>(tab.spellIds.size())) {
            uint32_t spellId = tab.spellIds[idx - 1];
            const std::string& name = gh->getSpellName(spellId);
            lua_pushstring(L, name.empty() ? "Unknown" : name.c_str());
            lua_pushstring(L, gh->getSpellRank(spellId).c_str());
            return 2;
        }
        idx -= static_cast<int>(tab.spellIds.size());
    }
    lua_pushnil(L);
    return 1;
}


// ── The spellbook's older API ──────────────────────────────────────────────
//
// SpellBookFrame in WotLK uses both generations at once: the newer
// GetSpellBookItem* alongside GetSpellName and CastSpell, which take the same
// slot and book. The newer half was implemented and the older was not, so the
// panel could name a spell and not cast it.


/// Whether this is the (slot, bookType) form with a slot below 1, which 2.4.3
/// refuses with "Invalid spell slot" from GetSpellName and GetSpellTexture.
static bool bookSlotBelowOne(lua_State* L) {
    return !lua_isnoneornil(L, 2) && lua_isnumber(L, 1) && lua_tonumber(L, 1) < 1;
}

/// GetSpellName(slot, bookType) → name, rank.
static int lua_GetSpellName(lua_State* L) {
    auto* gh = getGameHandler(L);
    // Through spellIdForCall, which reads the book as well as the slot. The
    // spellbook passes SpellBookFrame.bookType to every one of these, and
    // resolving a pet slot through the player's tabs names one of the player's
    // own spells - the wrong answer wearing the shape of a right one.
    if (game::isActiveExpansion("tbc") && bookSlotBelowOne(L))
        return luaL_error(L, "GetSpellName(): Invalid spell slot");
    const uint32_t id = spellIdForCall(L, gh);
    if (id == 0) {
        // Past the end of the book 2.4.3 answers nil, nil.
        lua_pushnil(L);
        lua_pushnil(L);
        return 2;
    }
    const std::string& name = gh->getSpellName(id);
    lua_pushstring(L, name.empty() ? "Unknown" : name.c_str());
    lua_pushstring(L, gh->getSpellRank(id).c_str());
    return 2;
}

/// CastSpell(slot, bookType) - cast what is in that slot at the current
/// target, which is what clicking a spellbook entry does.
static int lua_CastSpell(lua_State* L) {
    auto* gh = getGameHandler(L);
    // Same resolver, and here it decides what is cast: clicking a pet spell
    // resolved through the player's tabs cast whichever of the player's spells
    // sat at that index.
    const uint32_t id = spellIdForCall(L, gh);
    if (id != 0) gh->castSpell(id, gh->getTargetGuid());
    return 0;
}

/// IsPassiveSpell(id or index, bookType) → whether the spell is never cast.
///
/// It answered no for everything, so the spell book drew a passive with the
/// same cast border as an ability and let it be dragged onto the action bar,
/// where it would sit doing nothing. Spell.dbc's base attribute word carries
/// it - bit 6 - beside the Ex word the client already read for
/// interruptibility.
/// The spell's disposition, or nullopt when its Spell.dbc row is unknown.
static std::optional<game::spellclass::SpellDisposition> spellDispositionOf(
        game::GameHandler& gh, uint32_t spellId) {
    gh.getSpellName(spellId);
    const auto found = gh.spellNameCacheRef().find(spellId);
    if (found == gh.spellNameCacheRef().end()) return std::nullopt;
    return game::spellclass::spellDisposition(found->second.implicitTargetsA,
                                              found->second.implicitTargetsB);
}

/// IsHarmfulSpell / IsHelpfulSpell: (slot, bookType) or a known spell's name.
///
/// 2.4.3 answers 1 or nil, and no value at all for a name the player does not
/// know. A bare spell id is not a form it takes: IsHarmfulSpell(133) answered
/// nil with Fireball known.
static int spellDispositionAnswer(lua_State* L, game::spellclass::SpellDisposition want) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    if (lua_isnumber(L, 1) && lua_isnoneornil(L, 2)) { lua_pushnil(L); return 1; }
    const uint32_t id = spellIdForCall(L, gh);
    if (id == 0) return 0;
    const auto disposition = spellDispositionOf(*gh, id);
    if (disposition == want) lua_pushnumber(L, 1);
    else lua_pushnil(L);
    return 1;
}

/// IsHarmfulItem / IsHelpfulItem: an item id, link or name. The item's
/// on-use spell decides, classified as above; equip and chance-on-hit spells
/// do not count (2.4.3: a Shortsword of Vengeance and Gnoll Casting Gloves are
/// neither, a Minor Healthstone, whose use spell is its second, is helpful).
static int itemDispositionAnswer(lua_State* L, game::spellclass::SpellDisposition want) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const uint32_t spell = itemUseSpell(gh, itemIdForCall(L, 1, gh));
    const auto disposition = spell ? spellDispositionOf(*gh, spell) : std::nullopt;
    if (disposition == want) lua_pushnumber(L, 1);
    else lua_pushnil(L);
    return 1;
}

static int lua_IsPassiveSpell(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnil(L); return 1; }
    // Both forms: IsPassiveSpell(spellId) and IsPassiveSpell(slot, bookType),
    // which is the one the spellbook uses - and reading a slot as an id
    // answered for whichever spell happens to carry the number 1, 2 or 3.
    // The result decides whether a spell is drawn as castable at all.
    const uint32_t id = spellIdForCall(L, gh);
    // 1 or nil, as 2.4.3 answers.
    if (gh->isSpellPassive(id)) lua_pushnumber(L, 1);
    else lua_pushnil(L);
    return 1;
}

/// IsSelectedSpell(slot, bookType) → whether the book is highlighting it.
/// Nothing selects a spell here; the highlight follows the cursor.
static int lua_IsSelectedSpell(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}

/// GetSpellAutocast(slot, bookType) → autocastable, currently autocasting.
/// Pet spells only, and pet action bars are not modelled: false says the
/// little rotating border is not drawn, which is right for every spell a
/// player has.
static int lua_GetSpellAutocast(lua_State* L) {
    lua_pushboolean(L, 0);
    lua_pushboolean(L, 0);
    return 2;
}
/// GetKnownSlotFromHighestRankSlot(slot) → the slot actually known. The book
/// shows the highest rank and this maps back; with one entry per spell here
/// the two are the same slot.
static int lua_GetKnownSlotFromHighestRankSlot(lua_State* L) {
    lua_pushnumber(L, luaL_optnumber(L, 1, 0));
    return 1;
}

/// UpdateSpells() - redraw the spell book page.
///
/// The list itself needs no rebuilding, which is why this was a no-op. But in
/// WoW the call is what makes the client announce SPELLS_CHANGED, and that
/// event is the only thing that redraws a spell button: SpellButton_OnEvent
/// answers it with SpellButton_UpdateButton, and nothing else calls that.
///
/// Six places in spellbookframe.lua call this, and every one of them is a
/// "now redraw the page" - after a skill-line tab is clicked, after a page
/// turn, after the book type changes. SpellBookFrame_ShowSpells only calls
/// Show() on buttons that are already shown, so no OnShow fires and without
/// the event the page kept whatever it was displaying before. That is the
/// whole of "the spell book does not update when the tab changes": the offset
/// moved and nothing ever re-read it.
static int lua_UpdateSpells(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "wowee_lua_engine");
    auto* engine = static_cast<LuaEngine*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    // Safe against the obvious loop: SpellBookFrame_OnEvent answers
    // SPELLS_CHANGED with SpellBookFrame_Update() and no argument, and the
    // branch that would call back here only runs when it is passed one.
    if (engine) engine->fireEvent("SPELLS_CHANGED", {});
    return 0;
}


static int lua_GetSpellDescription(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushstring(L, ""); return 1; }
    uint32_t spellId = static_cast<uint32_t>(luaL_checknumber(L, 1));
    // The shared formatter, which is what every other description path uses.
    // The local one this replaced knew $s and $o and wrote a literal "X" for
    // $d and the rest, so a duration read as "lasts X sec".
    const std::string desc =
        gh->formatSpellDescription(spellId, gh->getSpellDescription(spellId));
    lua_pushstring(L, desc.c_str());
    return 1;
}


static int lua_GetEnchantInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    uint32_t enchantId = static_cast<uint32_t>(luaL_checknumber(L, 1));
    std::string name = gh->getEnchantName(enchantId);
    if (name.empty()) { return luaReturnNil(L); }
    lua_pushstring(L, name.c_str());
    return 1;
}

static int lua_GetSpellCooldown(lua_State* L) {
    auto* gh = getGameHandler(L);
    // Three values on this path too, like every other. The cooldown frame does
    // `start > 0 and duration > 0 and enable > 0`, and `and` short-circuits, so
    // a missing third value is only ever reached when the first two say a
    // cooldown is running - which this path cannot say, because it answers
    // zero. Safe by accident rather than by design, and the next person to give
    // this branch a real start time would have found out the hard way.
    if (!gh) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 1); return 3; }
    // A name, an id, or a book slot with the book beside it - the spellbook
    // asks in the last of those three.
    const uint32_t spellId = spellIdForCall(L, gh);
    if (pushPendingCooldown(L, *gh, spellId)) return 3;
    float cd = gh->getSpellCooldown(spellId);
    // Also check GCD - if spell has no individual cooldown but GCD is active,
    // return the GCD timing (this is how WoW handles it)
    // Only spells the global cooldown covers (StartRecoveryCategory 133).
    float gcdRem = game::onGlobalCooldown(*gh, spellId) ? gh->getGCDRemaining() : 0.0f;
    float gcdTotal = gh->getGCDTotal();

    // WoW returns (start, duration, enabled) where remaining = start + duration - GetTime()
    double nowSec = luaGetTimeNow();

    if (cd > 0.01f) {
        // Spell-specific cooldown (longer than GCD). The pair asked for is
        // (start, duration), not (now, remaining) - the cooldown frame draws a
        // sweep of `duration` beginning at `start`, so saying it began now with
        // a length of whatever is left restarts the swirl at full every time
        // the interface asks. It asks on every ACTIONBAR_UPDATE_COOLDOWN, which
        // fires on every cast, so a five-minute cooldown looked like it kept
        // starting over. Wound back the same way the GCD branch below already
        // does it.
        double total = gh->getSpellCooldownTotal(spellId);
        if (total < cd) total = cd;   // a total never recorded falls back to what is left
        double start = nowSec - (total - cd);
        lua_pushnumber(L, start);
        lua_pushnumber(L, total);
    } else if (gcdRem > 0.01f) {
        // GCD is active - return GCD timing
        double elapsed = gcdTotal - gcdRem;
        double start = nowSec - elapsed;
        lua_pushnumber(L, start);
        lua_pushnumber(L, gcdTotal);
    } else {
        lua_pushnumber(L, 0);       // not on cooldown
        lua_pushnumber(L, 0);
    }
    lua_pushnumber(L, 1);           // enabled
    return 3;
}

static int lua_HasTarget(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->hasTarget());
    return 1;
}

// TargetUnit(unitIdOrName, exactMatch) is also the entry point for TBC's
// /target and /targetexact commands in ChatFrame.lua.
static int lua_TargetUnit(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const char* uid = luaL_checkstring(L, 1);
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0 && !uidStr.empty()) {
        const bool exactMatch = lua_toboolean(L, 2) != 0;
        const auto player = gh->getEntityManager().getEntity(gh->getPlayerGuid());
        if (!player) return 0;
        float nearestDistanceSq = std::numeric_limits<float>::infinity();
        for (const auto& [candidateGuid, entity] : gh->getEntityManager().getEntities()) {
            if (!entity || !entity->isUnit()) continue;
            std::string name = entity->getType() == game::ObjectType::PLAYER
                ? std::static_pointer_cast<game::Player>(entity)->getName()
                : std::static_pointer_cast<game::Unit>(entity)->getName();
            toLowerInPlace(name);
            if (exactMatch ? name != uidStr : !name.starts_with(uidStr)) continue;
            const float dx = entity->getX() - player->getX();
            const float dy = entity->getY() - player->getY();
            const float dz = entity->getZ() - player->getZ();
            const float distanceSq = dx * dx + dy * dy + dz * dz;
            if (distanceSq < nearestDistanceSq ||
                (distanceSq == nearestDistanceSq && candidateGuid < guid)) {
                guid = candidateGuid;
                nearestDistanceSq = distanceSq;
            }
        }
    }
    if (guid != 0) gh->setTarget(guid);
    return 0;
}

// ClearTarget() - clear current target. Answers whether there was one, which
// ToggleGameMenu reads to stop there instead of also opening the game menu.
static int lua_ClearTarget(lua_State* L) {
    auto* gh = getGameHandler(L);
    const bool hadTarget = gh && gh->hasTarget();
    if (gh) gh->clearTarget();
    lua_pushboolean(L, hadTarget);
    return 1;
}

// FocusUnit(unitId) - set focus target
static int lua_FocusUnit(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const char* uid = luaL_optstring(L, 1, nullptr);
    if (!uid || !*uid) return 0;
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid != 0) gh->setFocus(guid);
    return 0;
}

// ClearFocus() - clear focus target
static int lua_ClearFocus(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh) gh->clearFocus();
    return 0;
}

// AssistUnit(unitId) - target whatever the given unit is targeting
static int lua_AssistUnit(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const char* uid = luaL_optstring(L, 1, "target");
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    // "/assist" with nothing targeted: "You have no target." on the error
    // line (pair 6, the original).
    if (guid == 0) {
        gh->errorGlobal("ERR_GENERIC_NO_TARGET");
        return 0;
    }
    uint64_t theirTarget = getEntityTargetGuid(gh, guid);
    if (theirTarget != 0) gh->setTarget(theirTarget);
    return 0;
}

// TargetLastTarget() - re-target previous target
static int lua_TargetLastTarget(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh) gh->targetLastTarget();
    return 0;
}

// TargetNearestEnemy() - tab-target nearest enemy
// The argument is the direction, and it was being thrown away. Bindings.xml
// binds two keys to each of these - `TargetNearestEnemy()` forwards and
// `TargetNearestEnemy(1)` backwards, with a comment on that line saying so -
// so cycling backwards through targets went forwards instead, and the second
// key of every such pair did what the first did.
static bool wantsReverse(lua_State* L) {
    return lua_toboolean(L, 1) != 0;
}

static int lua_TargetNearestEnemy(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh) gh->targetEnemy(wantsReverse(L));
    return 0;
}

// TargetNearestFriend([reverse]) - target nearest friendly unit
static int lua_TargetNearestFriend(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh) gh->targetFriend(wantsReverse(L));
    return 0;
}

// GetRaidTargetIndex(unit) → icon index (1-8) or nil
static int lua_GetRaidTargetIndex(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    const char* uid = luaL_optstring(L, 1, "target");
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { return luaReturnNil(L); }
    uint8_t mark = gh->getEntityRaidMark(guid);
    if (mark == 0xFF) { return luaReturnNil(L); }
    lua_pushnumber(L, mark + 1); // WoW uses 1-indexed (1=Star, 2=Circle, ... 8=Skull)
    return 1;
}

// SetRaidTarget(unit, index) - set raid marker (1-8, or 0 to clear)
static int lua_SetRaidTarget(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const char* uid = luaL_optstring(L, 1, "target");
    int index = static_cast<int>(luaL_checknumber(L, 2));
    std::string uidStr(uid);
    toLowerInPlace(uidStr);
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) return 0;
    if (index >= 1 && index <= 8)
        gh->setRaidMark(guid, static_cast<uint8_t>(index - 1));
    else if (index == 0)
        gh->setRaidMark(guid, 0xFF); // clear
    return 0;
}

// GetSpellPowerCost(spellId) → {{ type=powerType, cost=manaCost, name=powerName }}

static int lua_GetSpellPowerCost(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_newtable(L); return 1; }
    uint32_t spellId = static_cast<uint32_t>(luaL_checknumber(L, 1));
    auto data = gh->getSpellData(spellId);
    lua_newtable(L); // outer table (array of cost entries)
    if (data.manaCost > 0) {
        lua_newtable(L); // cost entry
        lua_pushnumber(L, data.powerType);
        lua_setfield(L, -2, "type");
        lua_pushnumber(L, data.manaCost);
        lua_setfield(L, -2, "cost");
    
        lua_pushstring(L, data.powerType < 7 ? kLuaPowerNames[data.powerType] : "MANA");
        lua_setfield(L, -2, "name");
        lua_rawseti(L, -2, 1); // outer[1] = entry
    }
    return 1;
}

// --- GetSpellInfo / GetSpellTexture ---
// GetSpellInfo(spellIdOrName) -> name, rank, icon, castTime, minRange, maxRange, spellId
static int lua_GetSpellInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }

    uint32_t spellId = 0;
    if (lua_isnumber(L, 1)) {
        spellId = static_cast<uint32_t>(lua_tonumber(L, 1));
    } else if (lua_isstring(L, 1)) {
        const char* name = lua_tostring(L, 1);
        if (!name || !*name) { return luaReturnNil(L); }
        spellId = highestKnownRankByName(gh, name);
    }

    if (spellId == 0) { return luaReturnNil(L); }
    std::string name = gh->getSpellName(spellId);
    if (name.empty()) { return luaReturnNil(L); }

    lua_pushstring(L, name.c_str());                        // 1: name
    const std::string& rank = gh->getSpellRank(spellId);
    lua_pushstring(L, rank.c_str());                        // 2: rank
    std::string iconPath = gh->getSpellIconPath(spellId);
    if (!iconPath.empty()) lua_pushstring(L, iconPath.c_str());
    else lua_pushnil(L);                                     // 3: icon texture path
    // The client's order, which this did not follow.
    //
    //   name, rank, icon, cost, isFunnel, powerType, castTime, minRange, maxRange
    //
    // Seven values were returned and only the first three were in the right
    // places. Everything asking for a cost got a cast time in milliseconds,
    // everything asking for a cast time got the spell id - so a spell read as
    // taking several thousand seconds to cast - and the two ranges were nil.
    // The interface unpacks all nine in one line in multicastactionbarframe,
    // and any addon reading a spell does the same.
    //
    // isFunnel is the one value with nothing behind it here. False is right
    // for every spell but a handful of warlock drains, and it is what the
    // callers branch on rather than display.
    auto spellData = gh->getSpellData(spellId);
    lua_pushnumber(L, spellData.manaCost);                   // 4: cost
    lua_pushboolean(L, 0);                                   // 5: isFunnel
    lua_pushnumber(L, spellData.powerType);                  // 6: powerType
    lua_pushnumber(L, spellData.castTimeMs);                 // 7: castTime (ms)
    lua_pushnumber(L, spellData.minRange);                   // 8: minRange (yards)
    lua_pushnumber(L, spellData.maxRange);                   // 9: maxRange (yards)
    return 9;
}

// GetSpellTexture(spellIdOrName) -> icon texture path string
static int lua_GetSpellTexture(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    if (game::isActiveExpansion("tbc") && bookSlotBelowOne(L))
        return luaL_error(L, "GetSpellTexture(): Invalid spell slot");

    const uint32_t spellId = spellIdForCall(L, gh);
    if (spellId == 0) { return luaReturnNil(L); }
    std::string iconPath = gh->getSpellIconPath(spellId);
    if (!iconPath.empty()) lua_pushstring(L, iconPath.c_str());
    else lua_pushnil(L);
    return 1;
}

// GetItemInfo(itemId) -> name, link, quality, iLevel, reqLevel, class, subclass, maxStack, equipSlot, texture, vendorPrice

static int lua_GetSpellLink(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }

    const uint32_t spellId = spellIdForCall(L, gh);
    if (spellId == 0) { return luaReturnNil(L); }
    std::string name = gh->getSpellName(spellId);
    if (name.empty()) { return luaReturnNil(L); }
    const std::string link = game::spellChatLink(spellId, name);
    lua_pushstring(L, link.c_str());
    return 1;
}

/// CancelUnitBuff(unit, indexOrName [, filterOrRank]) - both forms.
///
/// The interface uses both and this took only the index, through
/// luaL_checknumber - so the two name forms raised on the spot rather than
/// cancelling anything. The possess bar cancels the aura holding the player in
/// a vehicle by name, and /cancelaura passes what was typed; both were an
/// error message where an aura should have dropped.
///
/// The third argument is a filter for the index form and a rank for the name
/// form, which is how WoW spells it. Only HELPFUL is answerable - a debuff
/// cannot be cancelled - so the filter decides nothing here beyond confirming
/// buffs are what is being counted.
static int lua_CancelUnitBuff(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    std::string uidStr(luaL_optstring(L, 1, "player"));
    toLowerInPlace(uidStr);
    if (uidStr != "player") return 0;   // only one's own auras can be cancelled

    const auto& auras = gh->getPlayerAuras();
    const bool byName = lua_isstring(L, 2) && !lua_isnumber(L, 2);

    if (byName) {
        std::string want(lua_tostring(L, 2));
        toLowerInPlace(want);
        for (const auto& a : auras) {
            if (a.isEmpty() || (a.flags & 0x80) != 0) continue;
            std::string have = gh->getSpellName(a.spellId);
            toLowerInPlace(have);
            if (have == want) { gh->cancelAura(a.spellId); return 0; }
        }
        return 0;
    }

    const int index = static_cast<int>(luaL_optnumber(L, 2, 0));
    if (index < 1) return 0;
    int buffCount = 0;
    for (const auto& a : auras) {
        if (a.isEmpty()) continue;
        if ((a.flags & 0x80) != 0) continue;   // debuffs are not cancellable
        if (++buffCount == index) { gh->cancelAura(a.spellId); return 0; }
    }
    return 0;
}

// CastSpellByID(spellId) - cast spell by numeric ID
static int lua_CastSpellByID(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    uint32_t spellId = static_cast<uint32_t>(luaL_checknumber(L, 1));
    if (spellId == 0) return 0;
    // The second argument names who it is cast on, and this is the click-cast
    // path: SECURE_ACTIONS.spell does CastSpellByID(spellID, unit) with the
    // unit off the button that was clicked. Ignoring it cast on whatever was
    // targeted instead, so healing a party member through their frame hit the
    // current target.
    uint64_t target = 0;
    if (const char* unit = luaL_optstring(L, 2, nullptr)) {
        std::string u(unit);
        toLowerInPlace(u);
        target = resolveUnitGuid(gh, u);
    }
    if (target == 0 && gh->hasTarget()) target = gh->getTargetGuid();
    gh->castSpell(spellId, target);
    return 0;
}

static int lua_IsUsableSpell(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushboolean(L, 0); lua_pushboolean(L, 0); return 2; }

    uint32_t spellId = 0;
    if (lua_isnumber(L, 1)) {
        spellId = static_cast<uint32_t>(lua_tonumber(L, 1));
    } else if (lua_isstring(L, 1)) {
        const char* name = lua_tostring(L, 1);
        if (!name || !*name) { lua_pushboolean(L, 0); lua_pushboolean(L, 0); return 2; }
        std::string nameLow(name);
        toLowerInPlace(nameLow);
        for (uint32_t sid : gh->getKnownSpells()) {
            std::string sn = gh->getSpellName(sid);
            toLowerInPlace(sn);
            if (sn == nameLow) { spellId = sid; break; }
        }
    }

    if (spellId == 0 || !gh->getKnownSpells().count(spellId)) {
        lua_pushboolean(L, 0);
        lua_pushboolean(L, 0);
        return 2;
    }

    const auto usability = game::spellUsability(*gh, spellId);
    lua_pushboolean(L, usability.usable);
    lua_pushboolean(L, usability.noPower);
    return 2;
}


void registerSpellLuaAPI(lua_State* L) {
    static const struct { const char* name; lua_CFunction func; } api[] = {
                {"GetTotemInfo",     lua_GetTotemInfo},
                // DestroyTotem(slot) - pull one down early. The totem bar's
                // right-click, and the slot is all the request carries.
                {"DestroyTotem", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int slot = static_cast<int>(luaL_optnumber(L, 1, 0));
            // The bar counts from one and the request from zero.
            if (gh && slot >= 1) gh->destroyTotem(slot - 1);
            return 0;
        }},
                {"GetTotemTimeLeft", lua_GetTotemTimeLeft},
                {"SpellStopCasting", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const bool wasCasting = gh && gh->isCasting();
            if (gh) gh->cancelCast();
            lua_pushboolean(L, wasCasting);
            return 1;
        }},
                {"SpellStopTargeting", [](lua_State* L) -> int {
            // No AoE reticle, but the item cursor a stone or an oil arms is a
            // kind of targeting and escape has to put it down.
            auto* gh = getGameHandler(L);
            const bool wasTargeting = gh && (gh->isAwaitingItemTarget() || gh->isAwaitingGroundTarget());
            if (gh && gh->isAwaitingItemTarget()) gh->cancelItemTargeting();
            if (gh) gh->cancelGroundTarget();
            lua_pushboolean(L, wasTargeting);
            return 1;
        }},
                {"SpellIsTargeting", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushboolean(L, gh && (gh->isAwaitingItemTarget() || gh->isAwaitingGroundTarget()));
            return 1;
        }},
                {"IsSpellInRange",    lua_IsSpellInRange},
                {"UnitBuff",          lua_UnitBuff},
                {"UnitDebuff",        lua_UnitDebuff},
                {"UnitAura",          lua_UnitAuraGeneric},
                // The 1.12 names. A vanilla interface reads its buffs through
                // these and through nothing else.
                {"GetPlayerBuff",             lua_GetPlayerBuff},
                {"GetPlayerBuffTexture",      lua_GetPlayerBuffTexture},
                {"GetPlayerBuffName",         lua_GetPlayerBuffName},
                {"GetPlayerBuffTimeLeft",     lua_GetPlayerBuffTimeLeft},
                {"GetPlayerBuffApplications", lua_GetPlayerBuffApplications},
                {"GetPlayerBuffDispelType",   lua_GetPlayerBuffDispelType},
                {"CancelPlayerBuff",          lua_CancelPlayerBuff},
                {"UnitCastingInfo",   lua_UnitCastingInfo},
                {"UnitChannelInfo",   lua_UnitChannelInfo},
                {"CastSpellByName",   lua_CastSpellByName},
                {"CastSpellByID",       lua_CastSpellByID},
                {"IsSpellKnown",      lua_IsSpellKnown},
                {"PlayerHasSpells", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushboolean(L, gh && !gh->getSpellBookTabs().empty());
            return 1;
        }},
                {"GetNumSpellTabs",   lua_GetNumSpellTabs},
                {"GetSpellTabInfo",   lua_GetSpellTabInfo},
                {"GetSpellBookItemInfo", lua_GetSpellBookItemInfo},
                {"GetSpellBookItemName", lua_GetSpellBookItemName},
                {"GetSpellName",      lua_GetSpellName},
                {"SpellTargetUnit",   lua_SpellTargetUnit},
                {"SpellTargetItem",   lua_SpellTargetItem},
                {"DropItemOnUnit",    lua_DropItemOnUnit},
                {"SetMultiCastSpell", lua_SetMultiCastSpell},
                {"CastSpell",         lua_CastSpell},
                {"IsPassiveSpell",    lua_IsPassiveSpell},
                {"IsHarmfulSpell", [](lua_State* L) -> int {
            return spellDispositionAnswer(L, game::spellclass::SpellDisposition::Harmful);
        }},
                {"IsHelpfulSpell", [](lua_State* L) -> int {
            return spellDispositionAnswer(L, game::spellclass::SpellDisposition::Helpful);
        }},
                {"IsHarmfulItem", [](lua_State* L) -> int {
            return itemDispositionAnswer(L, game::spellclass::SpellDisposition::Harmful);
        }},
                {"IsHelpfulItem", [](lua_State* L) -> int {
            return itemDispositionAnswer(L, game::spellclass::SpellDisposition::Helpful);
        }},
                // 1.12's name for the same question - renamed at 2.0, and
                // spellbookframe.lua calls it twice from
                // SpellButton_UpdateButton, which runs every time the book is
                // drawn. Unbound, opening the spellbook raised.
                {"IsSpellPassive",    lua_IsPassiveSpell},
                {"IsSelectedSpell",   lua_IsSelectedSpell},
                {"GetSpellAutocast",  lua_GetSpellAutocast},
                {"GetKnownSlotFromHighestRankSlot", lua_GetKnownSlotFromHighestRankSlot},
                {"UpdateSpells",      lua_UpdateSpells},
                {"GetSpellCooldown",  lua_GetSpellCooldown},
                {"GetSpellPowerCost", lua_GetSpellPowerCost},
                {"GetSpellDescription", lua_GetSpellDescription},
                {"GetEnchantInfo",     lua_GetEnchantInfo},
                {"GetSpellInfo",      lua_GetSpellInfo},
                {"GetSpellTexture",   lua_GetSpellTexture},
                {"GetSpellLink",         lua_GetSpellLink},
                {"IsUsableSpell",        lua_IsUsableSpell},
                {"CancelUnitBuff",      lua_CancelUnitBuff},
                {"HasTarget",         lua_HasTarget},
                {"TargetUnit",        lua_TargetUnit},
                {"ClearTarget",       lua_ClearTarget},
                {"FocusUnit",         lua_FocusUnit},
                {"ClearFocus",        lua_ClearFocus},
                {"AssistUnit",        lua_AssistUnit},
                {"TargetLastTarget",  lua_TargetLastTarget},
                {"TargetNearestEnemy",  lua_TargetNearestEnemy},
                {"TargetNearestFriend", lua_TargetNearestFriend},
                {"GetRaidTargetIndex",  lua_GetRaidTargetIndex},
                {"SetRaidTarget",       lua_SetRaidTarget},
                {"IsPlayerSpell", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            uint32_t spellId = static_cast<uint32_t>(luaL_checknumber(L, 1));
            lua_pushboolean(L, gh && gh->getKnownSpells().count(spellId) ? 1 : 0);
            return 1;
        }},
                {"IsSpellOverlayed", [](lua_State* L) -> int {
            (void)L; lua_pushboolean(L, 0); return 1; // No proc overlay tracking
        }},
                {"IsCurrentSpell", [](lua_State* L) -> int {
            // A name as well as an id: native 2.4.3 IsCurrentSpell("Golden
            // Gryphon") answers nil while riding it, and does not raise.
            auto* gh = getGameHandler(L);
            const uint32_t current = gh ? gh->getCurrentCastSpellId() : 0;
            bool answer = false;
            if (lua_type(L, 1) == LUA_TSTRING)
                answer = current != 0 && gh->getSpellName(current) == lua_tostring(L, 1);
            else
                answer = current != 0 && current == static_cast<uint32_t>(luaL_checknumber(L, 1));
            lua_pushboolean(L, answer ? 1 : 0);
            return 1;
        }},
                {"IsAutoRepeatSpell", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const char* name = luaL_optstring(L, 1, "");
            const uint32_t active = gh ? gh->getAutoRepeatSpellId() : 0;
            lua_pushboolean(L, active != 0 && gh->getSpellName(active) == name);
            return 1;
        }},
                {"CastShapeshiftForm", [](lua_State* L) -> int {
            // CastShapeshiftForm(index) - cast the spell for the given form slot
            auto* gh = getGameHandler(L);
            int index = static_cast<int>(luaL_checknumber(L, 1));
            if (!gh || index < 1) return 0;
            const auto forms = game::playerShapeshiftForms(*gh);
            if (static_cast<size_t>(index) > forms.size()) return 0;
            gh->castSpell(forms[static_cast<size_t>(index) - 1].spellId, 0);
            return 0;
        }},
                {"CancelShapeshiftForm", [](lua_State* L) -> int {
            // /cancelform. Native 2.4.3: CMSG_CANCEL_AURA for Cat Form or
            // Shadowform, nothing in Battle Stance.
            if (auto* gh = getGameHandler(L)) gh->cancelShapeshiftForm();
            return 0;
        }},
                // GetShapeshiftFormCooldown is defined in the bootstrap, over
                // GetShapeshiftFormInfo and GetSpellCooldown, because those are
                // in two different files and it needs both. A stub here would
                // be dead - the bootstrap runs after these are registered - and
                // framexml_lua_override_check exists to catch exactly that.
                {"GetShapeshiftForm", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            // The place of the form on the stance bar, as 2.4.3 answers it:
            // 1 in Stealth, whose form id is 30.
            lua_pushnumber(L, gh ? game::currentShapeshiftFormIndex(*gh) : 0);
            return 1;
        }},
                {"GetNumShapeshiftForms", [](lua_State* L) -> int {
            // Return count based on player class
            auto* gh = getGameHandler(L);
            if (!gh) { return luaReturnZero(L); }
            // Only the forms this character has actually learned, from the
            // one list GetShapeshiftFormInfo and CastShapeshiftForm also read.
            // The bar walks 1..this and asks the other two about each index, so
            // a count taken from a different list describes one form and casts
            // another.
            lua_pushnumber(L, static_cast<double>(game::playerShapeshiftForms(*gh).size()));
            return 1;
        }},
    };
    for (const auto& [name, func] : api) {
        lua_pushcfunction(L, func);
        lua_setglobal(L, name);
    }
}

} // namespace wowee::addons
