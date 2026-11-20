// Server packets through GameHandler's own dispatch. See game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"
#include "addons/lua_api_registrations.hpp"
#include "game/entity.hpp"
#include "game/game_utils.hpp"
#include "game/packed_time.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

using replay::Session;
using replay::UpdateObject;
using wowee::game::UF;

namespace {

std::shared_ptr<wowee::game::Unit> playerUnit(Session& s) {
    return std::dynamic_pointer_cast<wowee::game::Unit>(
        s.handler().getEntityManager().getEntity(Session::kPlayerGuid));
}

constexpr uint32_t kAutoShot = 75;
constexpr uint64_t kMob = 0xF130000000000123ull;
constexpr uint64_t kOtherHunter = 0x0000000000000077ull;

// A hunter in the world with a mob to shoot.
void huntMob(Session& s) {
    // This packet-only fixture has no asset manager. Supply the two hunter
    // spells used by its cast tests so prerequisite validation can resolve them.
    s.handler().spellNameCacheRef()[75].name = "Auto Shot";
    s.handler().spellNameCacheRef()[3044].name = "Arcane Shot";
    // Arcane Shot's Spell.dbc global cooldown: StartRecoveryCategory 133,
    // StartRecoveryTime 1500 - the GCD now comes from these fields.
    s.handler().spellNameCacheRef()[3044].startRecoveryCategory = 133;
    s.handler().spellNameCacheRef()[3044].startRecoveryMs = 1500;
    s.handler().spellNameCacheLoadedRef() = true;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181}, {UF::UNIT_FIELD_LEVEL, 9}});
    s.receive(UpdateObject().create(kMob, wowee::game::ObjectType::UNIT,
                                    {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_LEVEL, 8}}));
}

// The castCount CMSG_CAST_SPELL carried on the latest press, its first byte.
uint8_t lastCastCount(const Session& s) {
    auto casts = s.sent().of(wowee::game::Opcode::CMSG_CAST_SPELL);
    REQUIRE_FALSE(casts.empty());
    return casts.back().getData().at(0);
}

// Spell::SendCastResult: castCount, spellId, result.
wowee::network::Packet castFailed(uint8_t castCount, uint32_t spellId, uint8_t result) {
    wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_CAST_FAILED));
    p.writeUInt8(castCount);
    p.writeUInt32(spellId);
    p.writeUInt8(result);
    return p;
}

// Player::SendAutoRepeatCancel: the shooter's packed guid, to everyone nearby.
wowee::network::Packet autoRepeatCancelled(uint64_t shooter) {
    wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_CANCEL_AUTO_REPEAT));
    p.writePackedGuid(shooter);
    return p;
}

constexpr uint8_t kNotReady = 67;
constexpr uint8_t kOutOfRange = 97;

}  // namespace

TEST_CASE("the session speaks WotLK", "[replay]") {
    Session s;
    CHECK(wowee::game::isActiveExpansion("wotlk"));
    CHECK_FALSE(wowee::game::isPreWotlk());
}

TEST_CASE("the player's create block makes the player's unit", "[replay]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181},
                   {UF::UNIT_FIELD_MAXHEALTH, 181},
                   {UF::UNIT_FIELD_LEVEL, 9}});

    auto unit = playerUnit(s);
    REQUIRE(unit);
    CHECK(unit->getHealth() == 181);
    CHECK(unit->getMaxHealth() == 181);
    CHECK(unit->getLevel() == 9);
}

TEST_CASE("a values block changes only the fields it carries", "[replay]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181},
                   {UF::UNIT_FIELD_MAXHEALTH, 181},
                   {UF::UNIT_FIELD_LEVEL, 9}});
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_HEALTH, 65}}));

    auto unit = playerUnit(s);
    REQUIRE(unit);
    CHECK(unit->getHealth() == 65);
    CHECK(unit->getMaxHealth() == 181);
}

// AzerothCore's Player::SetRestState and RemoveRestState set and clear
// PLAYER_FLAGS_RESTING (0x20).
TEST_CASE("resting follows the resting player flag", "[replay]") {
    Session s;
    s.spawnPlayer({{UF::PLAYER_FLAGS, 0x20}});
    CHECK(s.handler().isPlayerResting());

    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::PLAYER_FLAGS, 0}}));
    CHECK_FALSE(s.handler().isPlayerResting());
}

// Player::SendInitialPacketsBeforeAddToMap: the packed game time, the speed,
// and a zero added in 3.1.2.
TEST_CASE("the login time sets the game clock", "[replay]") {
    Session s;
    wowee::game::WowDate now;
    now.hour = 13;
    now.minute = 30;
    wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_LOGIN_SETTIMESPEED));
    p.writeUInt32(wowee::game::packWowPackedTime(now));
    p.writeFloat(0.01666667f);
    p.writeUInt32(0);
    s.receive(p);

    CHECK(s.handler().getGameTime() == Catch::Approx(13.5f).margin(0.01f));
}

TEST_CASE("a refused Auto Shot press stops the repeat it started", "[replay]") {
    Session s;
    huntMob(s);
    s.handler().castSpell(kAutoShot, kMob);
    REQUIRE(s.handler().getAutoRepeatSpellId() == kAutoShot);

    s.receive(castFailed(lastCastCount(s), kAutoShot, kOutOfRange));
    CHECK(s.handler().getAutoRepeatSpellId() == 0);
}

TEST_CASE("a refused second Auto Shot press leaves the first one repeating", "[replay]") {
    Session s;
    huntMob(s);
    s.handler().castSpell(kAutoShot, kMob);
    s.handler().castSpell(kAutoShot, kMob);

    s.receive(castFailed(lastCastCount(s), kAutoShot, kNotReady));
    CHECK(s.handler().getAutoRepeatSpellId() == kAutoShot);
}

// Unit::InterruptSpell sends it for whoever stopped shooting.
TEST_CASE("only the player's own auto repeat cancel stops Auto Shot", "[replay]") {
    Session s;
    huntMob(s);
    s.handler().castSpell(kAutoShot, kMob);

    s.receive(autoRepeatCancelled(kOtherHunter));
    CHECK(s.handler().getAutoRepeatSpellId() == kAutoShot);

    s.receive(autoRepeatCancelled(Session::kPlayerGuid));
    CHECK(s.handler().getAutoRepeatSpellId() == 0);
}

constexpr uint32_t kArcaneShot = 3044;

TEST_CASE("a refused cast takes back the GCD it started", "[replay]") {
    Session s;
    huntMob(s);
    s.handler().castSpell(kArcaneShot, kMob);
    REQUIRE(s.handler().isGCDActive());

    s.receive(castFailed(lastCastCount(s), kArcaneShot, kOutOfRange));
    CHECK_FALSE(s.handler().isGCDActive());
}

TEST_CASE("a press refused during the GCD leaves the GCD running", "[replay]") {
    Session s;
    huntMob(s);
    s.handler().castSpell(kArcaneShot, kMob);
    s.handler().castSpell(kArcaneShot, kMob);

    s.receive(castFailed(lastCastCount(s), kArcaneShot, kNotReady));
    CHECK(s.handler().isGCDActive());
}

namespace {

constexpr uint64_t kPet = 0xF140000000000051ull;

// Player::PetSpellInitialize up to the spell count: guid, family, duration,
// react and command states, flags, the ten action bar slots.
wowee::network::Packet petSpells(uint64_t pet) {
    wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_PET_SPELLS));
    p.writeUInt64(pet);
    p.writeUInt16(0);
    p.writeUInt32(0);
    p.writeUInt8(1);
    p.writeUInt8(1);
    p.writeUInt16(0);
    for (int i = 0; i < 10; ++i) p.writeUInt32(0);
    p.writeUInt8(0);
    return p;
}

bool petCanBeAbandoned(Session& s) {
    lua_State* L = luaL_newstate();
    lua_pushlightuserdata(L, &s.handler());
    lua_setfield(L, LUA_REGISTRYINDEX, "wowee_game_handler");
    wowee::addons::registerUnitLuaAPI(L);
    REQUIRE(luaL_dostring(L, "return PetCanBeAbandoned()") == 0);
    const bool result = lua_toboolean(L, -1) != 0;
    lua_close(L);
    return result;
}

}  // namespace

namespace {

constexpr uint64_t kWolf = 0xF130000000000321ull;

// Unit::SendAttackStateUpdate for one swing of a single physical sub-damage.
wowee::network::Packet swing(uint64_t attacker, uint64_t victim, uint32_t damage, uint8_t victimState,
                             uint32_t hitInfo = 0x2 /* HITINFO_AFFECTS_VICTIM */, uint32_t blocked = 0) {
    wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_ATTACKERSTATEUPDATE));
    p.writeUInt32(hitInfo);
    p.writePackedGuid(attacker);
    p.writePackedGuid(victim);
    p.writeUInt32(damage);
    p.writeUInt32(0);
    p.writeUInt8(1);
    p.writeUInt32(1);  // physical
    p.writeFloat(static_cast<float>(damage));
    p.writeUInt32(damage);
    p.writeUInt8(victimState);
    p.writeUInt32(0);
    p.writeUInt32(0);
    if (hitInfo & 0x2000) p.writeUInt32(blocked);  // HITINFO_BLOCK
    return p;
}

std::vector<wowee::game::CombatTextEntry::Type> combatTextTypes(Session& s) {
    std::vector<wowee::game::CombatTextEntry::Type> types;
    for (const auto& e : s.handler().getCombatText()) types.push_back(e.type);
    return types;
}

}  // namespace

// Pet::CreateBaseAtTamed and Pet::LoadPetFromDB set UNIT_CAN_BE_ABANDONED
// (0x02) in UNIT_FIELD_BYTES_2 byte 2 on a hunter's beast, and nothing else
// sets it.
TEST_CASE("PetCanBeAbandoned is true for a tamed pet only", "[replay]") {
    for (const bool tamed : {true, false}) {
        Session s;
        s.spawnPlayer({{UF::UNIT_FIELD_LEVEL, 10}});
        s.receive(UpdateObject().create(
            kPet, wowee::game::ObjectType::UNIT,
            {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_BYTES_2, tamed ? 0x03u << 16 : 0u}}));
        s.receive(petSpells(kPet));
        REQUIRE(s.handler().getPetGuid() == kPet);

        CHECK(petCanBeAbandoned(s) == tamed);
    }
}

// WorldPackets::Misc::Weather: the WeatherState, its intensity, and whether
// it changes at once. Weather::GetWeatherState picks the state.
TEST_CASE("weather states read as what the server means", "[replay]") {
    using wowee::game::WeatherKind;
    struct Case { uint32_t state; WeatherKind kind; bool raining; bool snowing; };
    const Case cases[] = {
        {0, WeatherKind::NONE, false, false},
        {1, WeatherKind::FOG, false, false},
        {3, WeatherKind::RAIN, true, false},
        {4, WeatherKind::RAIN, true, false},
        {5, WeatherKind::RAIN, true, false},
        {6, WeatherKind::SNOW, false, true},
        {7, WeatherKind::SNOW, false, true},
        {8, WeatherKind::SNOW, false, true},
        {22, WeatherKind::SANDSTORM, false, false},
        {41, WeatherKind::SANDSTORM, false, false},
        {42, WeatherKind::SANDSTORM, false, false},
        {86, WeatherKind::THUNDERSTORM, true, false},
        {90, WeatherKind::RAIN, true, false},
        {106, WeatherKind::SNOW, false, true},
    };
    for (const Case& c : cases) {
        CAPTURE(c.state);
        Session s;
        wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_WEATHER));
        p.writeUInt32(c.state);
        p.writeFloat(0.5f);
        p.writeUInt8(0);
        s.receive(p);

        CHECK(s.handler().getWeatherKind() == c.kind);
        CHECK(s.handler().isRaining() == c.raining);
        CHECK(s.handler().isSnowing() == c.snowing);
    }
}

namespace {

wowee::network::Packet weather(uint32_t state, float intensity) {
    wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_WEATHER));
    p.writeUInt32(state);
    p.writeFloat(intensity);
    p.writeUInt8(0);
    return p;
}

std::vector<std::string> chatLines(Session& s) {
    std::vector<std::string> lines;
    for (const auto& m : s.handler().getChatHistory()) lines.push_back(m.message);
    return lines;
}

}  // namespace

TEST_CASE("weather is announced once per kind, and clears only after weather", "[replay]") {
    Session s;
    s.receive(weather(3, 0.3f));
    s.receive(weather(5, 0.9f));
    s.receive(weather(0, 0.0f));
    s.receive(weather(1, 0.5f));
    s.receive(weather(0, 0.0f));
    CHECK(chatLines(s) == std::vector<std::string>{"It begins to rain.", "The weather clears."});
}

TEST_CASE("faint rain is not rain", "[replay]") {
    Session s;
    s.receive(weather(4, 0.01f));
    CHECK_FALSE(s.handler().isRaining());
}

// AuraEffect::HandleFeignDeath sets UNIT_FLAG2_FEIGN_DEATH and
// UNIT_DYNFLAG_DEAD together, and removes them together.
TEST_CASE("Feign Death is not a death", "[replay]") {
    Session s;
    std::vector<std::string> events;
    s.handler().setAddonEventCallback(
        [&events](const std::string& name, const std::vector<std::string>&) { events.push_back(name); });
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 300}, {UF::UNIT_FIELD_MAXHEALTH, 300}});

    s.receive(UpdateObject().values(Session::kPlayerGuid,
                                    {{UF::UNIT_FIELD_FLAGS_2, 0x1}, {UF::UNIT_DYNAMIC_FLAGS, 0x20}}));
    CHECK_FALSE(s.handler().isPlayerDead());

    s.receive(UpdateObject().values(Session::kPlayerGuid,
                                    {{UF::UNIT_FIELD_FLAGS_2, 0}, {UF::UNIT_DYNAMIC_FLAGS, 0}}));
    CHECK_FALSE(s.handler().isPlayerDead());
    CHECK(std::count(events.begin(), events.end(), "PLAYER_DEAD") == 0);
    CHECK(std::count(events.begin(), events.end(), "PLAYER_ALIVE") == 0);
}

TEST_CASE("the dead flag at zero health is still a death", "[replay]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 300}, {UF::UNIT_FIELD_MAXHEALTH, 300}});
    s.receive(UpdateObject().values(Session::kPlayerGuid,
                                    {{UF::UNIT_FIELD_HEALTH, 0}, {UF::UNIT_DYNAMIC_FLAGS, 0x20}}));
    CHECK(s.handler().isPlayerDead());
}

// vmangos's Unit::SetFeignDeath sets the dead flag alone.
TEST_CASE("the dead flag at full health is Feign Death without its flags2 bit", "[replay]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 300}, {UF::UNIT_FIELD_MAXHEALTH, 300}});
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_DYNAMIC_FLAGS, 0x20}}));
    CHECK_FALSE(s.handler().isPlayerDead());
}

TEST_CASE("killed while feigning stays dead when the feign ends", "[replay]") {
    Session s;
    std::vector<std::string> events;
    s.handler().setAddonEventCallback(
        [&events](const std::string& name, const std::vector<std::string>&) { events.push_back(name); });
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 300}, {UF::UNIT_FIELD_MAXHEALTH, 300}});
    s.receive(UpdateObject().values(Session::kPlayerGuid,
                                    {{UF::UNIT_FIELD_FLAGS_2, 0x1}, {UF::UNIT_DYNAMIC_FLAGS, 0x20}}));
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_HEALTH, 0}}));
    s.receive(UpdateObject().values(Session::kPlayerGuid,
                                    {{UF::UNIT_FIELD_FLAGS_2, 0}, {UF::UNIT_DYNAMIC_FLAGS, 0}}));
    CHECK(s.handler().isPlayerDead());
    CHECK(std::count(events.begin(), events.end(), "PLAYER_ALIVE") == 0);
}

// A player created while feigning, as after a relog or a teleport mid-feign.
TEST_CASE("a player created feigning is alive, and stays so when it ends", "[replay]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 300},
                   {UF::UNIT_FIELD_MAXHEALTH, 300},
                   {UF::UNIT_FIELD_FLAGS_2, 0x1},
                   {UF::UNIT_DYNAMIC_FLAGS, 0x20}});
    CHECK_FALSE(s.handler().isPlayerDead());
    s.receive(UpdateObject().values(Session::kPlayerGuid,
                                    {{UF::UNIT_FIELD_FLAGS_2, 0}, {UF::UNIT_DYNAMIC_FLAGS, 0}}));
    CHECK_FALSE(s.handler().isPlayerDead());
}

TEST_CASE("a wolf's hit shows its damage", "[replay]") {
    using T = wowee::game::CombatTextEntry;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 300}, {UF::UNIT_FIELD_MAXHEALTH, 300}});
    s.receive(swing(kWolf, Session::kPlayerGuid, 12, 1));
    REQUIRE(s.handler().getCombatText().size() == 1);
    CHECK(s.handler().getCombatText()[0].type == T::MELEE_DAMAGE);
    CHECK(s.handler().getCombatText()[0].amount == 12);
}

TEST_CASE("a dodged swing shows a dodge", "[replay]") {
    using T = wowee::game::CombatTextEntry;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 300}, {UF::UNIT_FIELD_MAXHEALTH, 300}});
    s.receive(swing(Session::kPlayerGuid, kWolf, 0, 2));
    CHECK(combatTextTypes(s) == std::vector<T::Type>{T::DODGE});
}

TEST_CASE("a parried wolf shows a parry", "[replay]") {
    using T = wowee::game::CombatTextEntry;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 300}, {UF::UNIT_FIELD_MAXHEALTH, 300}});
    s.receive(swing(kWolf, Session::kPlayerGuid, 0, 3));
    CHECK(combatTextTypes(s) == std::vector<T::Type>{T::PARRY});
}

// Unit::CalcDamageInfo sends an evade as a miss with VICTIMSTATE_EVADES.
TEST_CASE("an evading wolf shows an evade, not a miss", "[replay]") {
    using T = wowee::game::CombatTextEntry;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 300}, {UF::UNIT_FIELD_MAXHEALTH, 300}});
    s.receive(swing(Session::kPlayerGuid, kWolf, 0, 6, 0x2 | 0x10 | 0x200000));
    CHECK(combatTextTypes(s) == std::vector<T::Type>{T::EVADE});
}

TEST_CASE("a partly blocked hit shows its damage and the block", "[replay]") {
    using T = wowee::game::CombatTextEntry;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 300}, {UF::UNIT_FIELD_MAXHEALTH, 300}});
    s.receive(swing(kWolf, Session::kPlayerGuid, 7, 1, 0x2 | 0x2000, 5));
    CHECK(combatTextTypes(s) == std::vector<T::Type>{T::MELEE_DAMAGE, T::BLOCK});
}

TEST_CASE("a fully blocked hit shows only the block", "[replay]") {
    using T = wowee::game::CombatTextEntry;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 300}, {UF::UNIT_FIELD_MAXHEALTH, 300}});
    s.receive(swing(kWolf, Session::kPlayerGuid, 0, 5, 0x2 | 0x2000, 12));
    CHECK(combatTextTypes(s) == std::vector<T::Type>{T::BLOCK});
}

namespace {

constexpr uint32_t kRaptorStrike = 2973;

// Spell::SendSpellStart for the player's own spell on no particular target.
wowee::network::Packet spellStart(uint8_t castCount, uint32_t spellId, uint32_t castTimeMs) {
    wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_SPELL_START));
    p.writePackedGuid(Session::kPlayerGuid);
    p.writePackedGuid(Session::kPlayerGuid);
    p.writeUInt8(castCount);
    p.writeUInt32(spellId);
    p.writeUInt32(0);  // cast flags
    p.writeUInt32(castTimeMs);
    p.writeUInt32(0);  // target flags: none
    return p;
}

}  // namespace

// Spell::SendCastResult writes the press's castCount; the server's own casts,
// such as each Auto Shot after the first, have none and send 0.
TEST_CASE("a press never goes out with castCount 0", "[replay]") {
    Session s;
    huntMob(s);
    for (int i = 0; i < 255; ++i) s.handler().castSpell(kArcaneShot, kMob);
    s.handler().castSpell(kAutoShot, kMob);
    CHECK(lastCastCount(s) != 0);

    s.receive(castFailed(0, kAutoShot, kOutOfRange));
    CHECK(s.handler().getAutoRepeatSpellId() == kAutoShot);
}

TEST_CASE("a refused second press leaves the queued swing ability lit", "[replay]") {
    Session s;
    huntMob(s);
    auto& cache = s.handler().spellNameCacheRef();
    cache[kRaptorStrike].attr = 0x4;  // SPELL_ATTR0_ON_NEXT_SWING
    s.handler().spellNameCacheLoadedRef() = true;

    s.handler().castSpell(kRaptorStrike, kMob);
    const uint8_t queued = lastCastCount(s);
    s.receive(spellStart(queued, kRaptorStrike, 0));
    REQUIRE(s.handler().getActiveActionSpellId() == kRaptorStrike);

    s.handler().castSpell(kRaptorStrike, kMob);
    s.receive(castFailed(lastCastCount(s), kRaptorStrike, kNotReady));
    CHECK(s.handler().getActiveActionSpellId() == kRaptorStrike);

    s.receive(castFailed(queued, kRaptorStrike, kOutOfRange));
    CHECK(s.handler().getActiveActionSpellId() == 0);
}

namespace {

bool petCanBeRenamed(Session& s) {
    lua_State* L = luaL_newstate();
    lua_pushlightuserdata(L, &s.handler());
    lua_setfield(L, LUA_REGISTRYINDEX, "wowee_game_handler");
    wowee::addons::registerSocialLuaAPI(L);
    REQUIRE(luaL_dostring(L, "return PetCanBeRenamed()") == 0);
    const bool result = lua_toboolean(L, -1) != 0;
    lua_close(L);
    return result;
}

}  // namespace

TEST_CASE("PvP menu sets explicit preference independently of active flag", "[replay][pvp]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_FLAGS, 0x1000}, {UF::PLAYER_FLAGS, 0}});
    lua_State* L = luaL_newstate();
    lua_pushlightuserdata(L, &s.handler());
    lua_setfield(L, LUA_REGISTRYINDEX, "wowee_game_handler");
    wowee::addons::registerSocialLuaAPI(L);
    REQUIRE(luaL_dostring(L, "return GetPVPDesired()") == 0);
    CHECK(lua_tonumber(L, -1) == 0);
    REQUIRE(luaL_dostring(L, "SetPVP(1); SetPVP(1); SetPVP(nil)") == 0);
    const auto packets = s.sent().of(wowee::game::Opcode::CMSG_TOGGLE_PVP);
    REQUIRE(packets.size() == 3);
    CHECK(packets[0].getData() == std::vector<uint8_t>{1});
    CHECK(packets[1].getData() == std::vector<uint8_t>{1});
    CHECK(packets[2].getData() == std::vector<uint8_t>{0});
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::PLAYER_FLAGS, 0x200}}));
    REQUIRE(luaL_dostring(L, "return GetPVPDesired()") == 0);
    CHECK(lua_tonumber(L, -1) == 1);
    lua_close(L);
}

TEST_CASE("PvP flag updates notify Blizzard portrait listeners", "[replay][pvp]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_FLAGS, 0}});
    bool notified = false;
    s.handler().setAddonEventCallback([&](const std::string& event, const std::vector<std::string>& args) {
        if (event == "UNIT_FACTION" && args == std::vector<std::string>{"player"}) notified = true;
    });
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_FLAGS, 0x1000}}));
    CHECK(notified);
}

// Pet::LoadPetFromDB sets UNIT_CAN_BE_RENAMED (0x01) beside
// UNIT_CAN_BE_ABANDONED only on a pet never named, and
// WorldSession::HandlePetRename clears it.
TEST_CASE("PetCanBeRenamed follows the pet's rename flag", "[replay]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_LEVEL, 10}});
    s.receive(UpdateObject().create(kPet, wowee::game::ObjectType::UNIT,
                                    {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_BYTES_2, 0x03u << 16}}));
    s.receive(petSpells(kPet));
    CHECK(petCanBeRenamed(s));

    s.receive(UpdateObject().values(kPet, {{UF::UNIT_FIELD_BYTES_2, 0x02u << 16}}));
    CHECK_FALSE(petCanBeRenamed(s));
}

namespace {

// WorldObject::SendSpellMiss: the spell, the caster's guid whole, an unknown
// byte, one target, its guid whole, and how it missed (SpellMissInfo).
wowee::network::Packet spellMiss(uint64_t caster, uint64_t victim, uint32_t spellId, uint8_t missInfo) {
    wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_SPELLLOGMISS));
    p.writeUInt32(spellId);
    p.writeUInt64(caster);
    p.writeUInt8(0);
    p.writeUInt32(1);
    p.writeUInt64(victim);
    p.writeUInt8(missInfo);
    return p;
}

std::vector<wowee::game::CombatTextEntry::Type> spellMissText(uint64_t caster, uint64_t victim,
                                                               uint8_t missInfo) {
    Session s;
    huntMob(s);
    // Resists and immunities show only on the target (fctSpellMechanics).
    s.handler().setTarget(kMob);
    s.receive(spellMiss(caster, victim, kArcaneShot, missInfo));
    std::vector<wowee::game::CombatTextEntry::Type> types;
    for (const auto& e : s.handler().getCombatText()) types.push_back(e.type);
    return types;
}

}  // namespace

namespace {

// SpellMissInfo, as every server family numbers it.
const std::vector<std::pair<uint8_t, wowee::game::CombatTextEntry::Type>>& missTypes() {
    using T = wowee::game::CombatTextEntry;
    static const std::vector<std::pair<uint8_t, T::Type>> table = {
        {1, T::MISS},   {2, T::RESIST}, {3, T::DODGE},   {4, T::PARRY},  {5, T::BLOCK},
        {6, T::EVADE},  {7, T::IMMUNE}, {8, T::IMMUNE},  {9, T::DEFLECT}, {10, T::ABSORB},
    };
    return table;
}

}  // namespace

TEST_CASE("a spell miss shows what the server says happened", "[replay]") {
    for (const auto& [missInfo, type] : missTypes()) {
        CAPTURE(missInfo);
        // The player's own misses, against the target, where every kind shows.
        CHECK(spellMissText(Session::kPlayerGuid, kMob, missInfo) ==
              std::vector<wowee::game::CombatTextEntry::Type>{type});
    }
}

namespace {

// Spell::SendSpellGo with no hits and one miss: caster and unit packed, the
// cast count, spell, flags and time, no hit targets, one miss target whole
// with its SpellMissInfo, and an empty target mask.
wowee::network::Packet spellGoMiss(uint64_t victim, uint32_t spellId, uint8_t missInfo) {
    wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_SPELL_GO));
    p.writePackedGuid(Session::kPlayerGuid);
    p.writePackedGuid(Session::kPlayerGuid);
    p.writeUInt8(1);
    p.writeUInt32(spellId);
    p.writeUInt32(0);
    p.writeUInt32(0);
    p.writeUInt8(0);
    p.writeUInt8(1);
    p.writeUInt64(victim);
    p.writeUInt8(missInfo);
    p.writeUInt32(0);
    return p;
}

}  // namespace

TEST_CASE("a spell go miss shows what the server says happened", "[replay]") {
    for (const auto& [missInfo, type] : missTypes()) {
        CAPTURE(missInfo);
        Session s;
        huntMob(s);
        s.handler().setTarget(kMob);
        s.receive(spellGoMiss(kMob, kArcaneShot, missInfo));
        std::vector<wowee::game::CombatTextEntry::Type> types;
        for (const auto& e : s.handler().getCombatText()) types.push_back(e.type);
        CHECK(types == std::vector<wowee::game::CombatTextEntry::Type>{type});
    }
}

namespace {

// CMaNGOS ChatHandler::BuildChatPacket, the default body: header, target guid,
// message, chat tag - and, as SMSG_GM_MESSAGECHAT, the GM's name after it.
wowee::network::Packet chatPacket(wowee::game::ChatType type, uint64_t sender, uint64_t target,
                                  const std::string& message, bool gm = false) {
    using wowee::game::Opcode;
    wowee::network::Packet p(
        wowee::game::wireOpcode(gm ? Opcode::SMSG_GM_MESSAGECHAT : Opcode::SMSG_MESSAGECHAT));
    p.writeUInt8(static_cast<uint8_t>(type));
    p.writeUInt32(7);  // LANG_COMMON
    p.writeUInt64(sender);
    p.writeUInt32(0);
    p.writeUInt64(target);
    p.writeUInt32(static_cast<uint32_t>(message.size() + 1));
    p.writeString(message);
    p.writeUInt8(gm ? 0x04 : 0x00);  // CHAT_TAG_GM
    if (gm) {
        const std::string name = "Graulbert";
        p.writeUInt32(static_cast<uint32_t>(name.size() + 1));
        p.writeString(name);
    }
    return p;
}

// TBC SMSG_NAME_QUERY_RESPONSE: full guid, name, realm, race, gender, class, no
// declined names.
wowee::network::Packet nameResponse(uint64_t guid, const std::string& name) {
    wowee::network::Packet p(
        wowee::game::wireOpcode(wowee::game::Opcode::SMSG_NAME_QUERY_RESPONSE));
    p.writeUInt64(guid); p.writeString(name); p.writeString("");
    p.writeUInt32(3); p.writeUInt32(0); p.writeUInt32(1); p.writeUInt8(0);
    return p;
}

}  // namespace

TEST_CASE("TBC sent chat shows only what the server echoes", "[chat][tbc]") {
    using wowee::game::ChatType;
    Session s("tbc");
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_LEVEL, 2}});
    s.receive(nameResponse(Session::kPlayerGuid, "Graulbert"));
    int bubbles = 0;
    s.handler().setChatBubbleCallback([&](uint64_t guid, const std::string& text, bool yell) {
        CHECK(guid == Session::kPlayerGuid);
        CHECK(text == "bubblecheck");
        CHECK_FALSE(yell);
        ++bubbles;
    });
    auto rows = [&](ChatType type) {
        std::vector<wowee::game::MessageChatData> found;
        for (const auto& m : s.handler().getChatHistory())
            if (m.type == type) found.push_back(m);
        return found;
    };

    // A GM command goes out as SAY; ParseCommands consumes it and answers with
    // system messages only, so the speaker sees no "says" line.
    s.handler().sendChatMessage(ChatType::SAY, ".gps", "");
    CHECK(s.sent().of(wowee::game::Opcode::CMSG_MESSAGECHAT).size() == 1);
    CHECK(rows(ChatType::SAY).empty());
    CHECK(bubbles == 0);

    // Ordinary speech: nothing on send, one line and one bubble from the echo.
    s.handler().sendChatMessage(ChatType::SAY, "bubblecheck", "");
    CHECK(rows(ChatType::SAY).empty());
    s.receive(chatPacket(ChatType::SAY, Session::kPlayerGuid, 0, "bubblecheck"));
    auto said = rows(ChatType::SAY);
    REQUIRE(said.size() == 1);
    CHECK(said[0].senderName == "Graulbert");
    CHECK(said[0].message == "bubblecheck");
    CHECK(bubbles == 1);

    // The same line again is the player talking, not spam to be filtered.
    s.receive(chatPacket(ChatType::SAY, Session::kPlayerGuid, 0, "bubblecheck"));
    CHECK(rows(ChatType::SAY).size() == 2);
    CHECK(bubbles == 2);

    // Player::Whisper: the inform carries the receiver's guid as its sender.
    constexpr uint64_t kOther = 0x43;
    s.receive(nameResponse(kOther, "Graulberto"));
    s.handler().sendChatMessage(ChatType::WHISPER, "hi", "Graulberto");
    CHECK(rows(ChatType::WHISPER_INFORM).empty());
    s.receive(chatPacket(ChatType::WHISPER_INFORM, kOther, 0, "hi"));
    auto inform = rows(ChatType::WHISPER_INFORM);
    REQUIRE(inform.size() == 1);
    CHECK(inform[0].senderName == "Graulberto");
    CHECK(inform[0].message == "hi");
}

TEST_CASE("TBC GM chat appends the GM name after the ordinary body", "[chat][tbc]") {
    using wowee::game::ChatType;
    Session s("tbc");
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_LEVEL, 2}});
    s.receive(nameResponse(Session::kPlayerGuid, "Graulbert"));
    s.receive(chatPacket(ChatType::SAY, Session::kPlayerGuid, 0, "gm line", /*gm=*/true));
    std::vector<wowee::game::MessageChatData> said;
    for (const auto& m : s.handler().getChatHistory())
        if (m.type == ChatType::SAY) said.push_back(m);
    REQUIRE(said.size() == 1);
    CHECK(said[0].message == "gm line");
    CHECK(said[0].senderName == "Graulbert");
    CHECK(said[0].chatTag == 0x04);
}

TEST_CASE("TBC leadership promotion resolves menu tokens and names", "[party][tbc][promote]") {
    Session s("tbc");
    using wowee::game::Opcode;
    wowee::network::Packet roster(wowee::game::wireOpcode(Opcode::SMSG_GROUP_LIST));
    roster.writeUInt8(0); roster.writeUInt8(0);
    roster.writeUInt8(0); roster.writeUInt8(0);
    roster.writeUInt64(1);
    roster.writeUInt32(1);
    roster.writeString("Graulberto"); roster.writeUInt64(3);
    roster.writeUInt8(1); roster.writeUInt8(0); roster.writeUInt8(0);
    roster.writeUInt64(Session::kPlayerGuid);
    s.receive(roster);
    lua_State* L = luaL_newstate();
    lua_pushlightuserdata(L, &s.handler());
    lua_setfield(L, LUA_REGISTRYINDEX, "wowee_game_handler");
    wowee::addons::registerSocialLuaAPI(L);
    REQUIRE(luaL_dostring(L, "PromoteToLeader('party1', 1); PromoteToLeader('Graulberto')") == 0);
    const auto packets = s.sent().of(Opcode::CMSG_GROUP_SET_LEADER);
    REQUIRE(packets.size() == 2);
    for (auto packet : packets) {
        REQUIRE(packet.getSize() == 8);
        CHECK(packet.readUInt64() == 3);
    }
    lua_close(L);
}

TEST_CASE("TBC daze speed changes and recovery are applied and acknowledged", "[movement][tbc][speed]") {
    Session s("tbc");
    using wowee::game::Opcode;
    uint32_t counter = 10;
    for (float speed : {3.5f, 7.0f}) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_FORCE_RUN_SPEED_CHANGE));
        p.writePackedGuid(Session::kPlayerGuid);
        p.writeUInt32(counter);
        p.writeUInt8(0);
        p.writeFloat(speed);
        s.receive(p);
        CHECK(s.handler().getServerRunSpeed() == Catch::Approx(speed));
        auto acks = s.sent().of(Opcode::CMSG_FORCE_RUN_SPEED_CHANGE_ACK);
        REQUIRE_FALSE(acks.empty());
        auto ack = acks.back();
        CHECK(ack.readUInt64() == Session::kPlayerGuid);
        CHECK(ack.readUInt32() == counter);
        ack.setReadPos(ack.getSize() - 4);
        CHECK(ack.readFloat() == Catch::Approx(speed));
        ++counter;
    }
    wowee::network::Packet walk(wowee::game::wireOpcode(Opcode::SMSG_FORCE_WALK_SPEED_CHANGE));
    walk.writePackedGuid(Session::kPlayerGuid);
    walk.writeUInt32(counter);
    walk.writeFloat(2.5f);
    s.receive(walk);
    CHECK(s.handler().getServerWalkSpeed() == Catch::Approx(2.5f));
}

TEST_CASE("TBC loot setting feedback follows server changes once", "[party][tbc][loot-settings]") {
    Session s("tbc");
    auto roster = [&](uint8_t method, uint8_t threshold, uint64_t master) {
        wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_GROUP_LIST));
        p.writeUInt8(0); p.writeUInt8(0); p.writeUInt8(0); p.writeUInt8(0);
        p.writeUInt64(1); p.writeUInt32(2);
        for (const auto& [name, guid] : std::vector<std::pair<std::string, uint64_t>>{{"Graulberto", 3}, {"Edric", 5}}) {
            p.writeString(name); p.writeUInt64(guid);
            p.writeUInt8(1); p.writeUInt8(0); p.writeUInt8(0);
        }
        p.writeUInt64(Session::kPlayerGuid);
        p.writeUInt8(method); p.writeUInt64(master); p.writeUInt8(threshold); p.writeUInt8(0);
        s.receive(p);
    };
    roster(3, 2, 0);
    const auto initial = chatLines(s).size();
    roster(3, 3, 0);
    CHECK(chatLines(s).back() == "Loot threshold set to Rare.");
    roster(2, 3, 3);
    CHECK(chatLines(s).back() == "Graulberto is now the loot master.");
    roster(2, 3, 5);
    CHECK(chatLines(s).back() == "Edric is now the loot master.");
    roster(4, 3, 0);
    CHECK(chatLines(s).back() == "Looting set to Need Before Greed.");
    const auto count = chatLines(s).size();
    CHECK(count == initial + 5);
    roster(4, 3, 0);
    CHECK(chatLines(s).size() == count);
    s.handler().sendOptOutOfLoot(true);
    CHECK(chatLines(s).back() == "You are now passing on random loot.");
    s.handler().sendOptOutOfLoot(false);
    CHECK(chatLines(s).back() == "You are now eligible for random loot.");
}

TEST_CASE("TBC party stats use packed GUID without a full-update prefix", "[party][tbc]") {
    Session s("tbc");
    using wowee::game::Opcode;
    using wowee::game::wireOpcode;
    constexpr uint64_t memberGuid = 3;
    // Group::SendUpdate: TBC roster, one remote member.
    wowee::network::Packet roster(wireOpcode(Opcode::SMSG_GROUP_LIST));
    roster.writeUInt8(0); roster.writeUInt8(0);
    roster.writeUInt8(0); roster.writeUInt8(0);
    roster.writeUInt64(1);
    roster.writeUInt32(1);
    roster.writeString("Graulberto"); roster.writeUInt64(memberGuid);
    roster.writeUInt8(1); roster.writeUInt8(0); roster.writeUInt8(0);
    roster.writeUInt64(memberGuid);
    s.receive(roster);
    REQUIRE(s.handler().getPartyData().members.size() == 1);

    for (const auto opcode : {Opcode::SMSG_PARTY_MEMBER_STATS_FULL,
                              Opcode::SMSG_PARTY_MEMBER_STATS}) {
        // WorldSession::HandleRequestPartyMemberStatsOpcode / changed packet.
        wowee::network::Packet stats(wireOpcode(opcode));
        stats.writePackedGuid(memberGuid);
        stats.writeUInt32(0x7);
        stats.writeUInt16(1);
        stats.writeUInt16(417);
        stats.writeUInt16(530);
        s.receive(stats);
        const auto& member = s.handler().getPartyData().members.front();
        CHECK(member.hasPartyStats);
        CHECK(member.onlineStatus == 1);
        CHECK(member.curHealth == 417);
        CHECK(member.maxHealth == 530);
    }
}

TEST_CASE("spell attack attribute starts melee autoattack", "[replay][strike-autoattack]") {
    for (const uint32_t attributes : {0u, 0x200u}) {
        Session s;
        huntMob(s);
        constexpr uint32_t sinisterStrike = 1752;
        s.handler().spellNameCacheRef()[sinisterStrike].attrEx = attributes;
        s.handler().spellNameCacheLoadedRef() = true;
        s.handler().castSpell(sinisterStrike, kMob);
        REQUIRE(s.sent().of(wowee::game::Opcode::CMSG_CAST_SPELL).size() == 1);
        CHECK(s.handler().hasAutoAttackIntent() == (attributes != 0));
        CHECK(s.sent().of(wowee::game::Opcode::CMSG_ATTACKSWING).size() == (attributes ? 1 : 0));
    }
}

TEST_CASE("removed target notifies FrameXML after clearing selection", "[replay][target-removal]") {
    for (const bool destroy : {false, true}) {
        Session s;
        huntMob(s);
        s.handler().setTarget(kMob);
        int changes = 0;
        s.handler().setAddonEventCallback([&](const std::string& event, const std::vector<std::string>&) {
            if (event == "PLAYER_TARGET_CHANGED") {
                ++changes;
                CHECK(s.handler().getTargetGuid() == 0);
                CHECK_FALSE(s.handler().getTarget());
            }
        });
        using wowee::game::Opcode;
        wowee::network::Packet p(wowee::game::wireOpcode(destroy ? Opcode::SMSG_DESTROY_OBJECT : Opcode::SMSG_UPDATE_OBJECT));
        if (destroy) {
            p.writeUInt64(kMob);
            p.writeUInt8(0);
        } else {
            p.writeUInt32(1);
            p.writeUInt8(static_cast<uint8_t>(wowee::game::UpdateType::OUT_OF_RANGE_OBJECTS));
            p.writeUInt32(1);
            p.writePackedGuid(kMob);
        }
        s.receive(p);
        CHECK(s.handler().getTargetGuid() == 0);
        CHECK(changes == 1);
        CHECK_FALSE(s.handler().getEntityManager().hasEntity(kMob));
    }
}

TEST_CASE("a player whisper is shown whatever it starts with", "[chat][tbc]") {
    // An addon whisper is LANG_ADDON (or a prefix<TAB>payload envelope); a
    // spoken whisper beginning with an addon's old prefix is still a whisper.
    using wowee::game::ChatType;
    Session s("tbc");
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_LEVEL, 2}});
    constexpr uint64_t kOther = 0x43;
    s.receive(nameResponse(kOther, "Graulberto"));
    for (const std::string text : {"oRAnge juice?", "AVR is fine", "GS_x", "\xC3\x84rger"}) {
        s.receive(chatPacket(ChatType::WHISPER, kOther, Session::kPlayerGuid, text));
    }
    std::vector<std::string> shown;
    for (const auto& m : s.handler().getChatHistory())
        if (m.type == ChatType::WHISPER) shown.push_back(m.message);
    CHECK(shown == std::vector<std::string>{"oRAnge juice?", "AVR is fine", "GS_x", "\xC3\x84rger"});
}

namespace {
// Group::SendUpdate, TBC: the player plus one remote member in subgroup 0.
void joinGroup(Session& s, uint64_t memberGuid, bool raid) {
    using wowee::game::Opcode;
    using wowee::game::wireOpcode;
    wowee::network::Packet roster(wireOpcode(Opcode::SMSG_GROUP_LIST));
    roster.writeUInt8(raid ? 1 : 0); roster.writeUInt8(0);
    roster.writeUInt8(0); roster.writeUInt8(0);
    roster.writeUInt64(1);
    roster.writeUInt32(1);
    roster.writeString("Graulberto"); roster.writeUInt64(memberGuid);
    roster.writeUInt8(1); roster.writeUInt8(0); roster.writeUInt8(0);
    roster.writeUInt64(memberGuid);
    s.receive(roster);
    REQUIRE(s.handler().getPartyData().members.size() == 1);
}
}  // namespace

TEST_CASE("an in-view group member dying refreshes their party frame, raid or not",
          "[party][tbc][raid]") {
    // Serpentshrine: the native character died in combat, in view, in a raid
    // of two. The target frame said Dead; PartyMemberFrame1 kept full health
    // and an old debuff, because the death came by UNIT_FIELD_HEALTH alone
    // and UNIT_HEALTH went out only as "raid1" in a raid. PartyMemberFrame
    // listens for "party1".
    for (const bool raid : {false, true}) {
        INFO((raid ? "raid" : "party"));
        Session s("tbc");
        constexpr uint64_t memberGuid = 3;
        joinGroup(s, memberGuid, raid);
        std::vector<std::pair<std::string, std::string>> events;
        s.handler().setAddonEventCallback(
            [&](const std::string& e, const std::vector<std::string>& args) {
                events.emplace_back(e, args.empty() ? std::string() : args.front());
            });
        s.receive(UpdateObject().tbc().create(memberGuid, wowee::game::ObjectType::PLAYER,
            {{UF::UNIT_FIELD_HEALTH, 8177932}, {UF::UNIT_FIELD_MAXHEALTH, 10000000}}));
        events.clear();
        // The killing blow: health alone, as the server sends it.
        s.receive(UpdateObject().tbc().values(memberGuid, {{UF::UNIT_FIELD_HEALTH, 0}}));
        const auto fired = [&](const char* event, const char* unit) {
            return std::find(events.begin(), events.end(),
                             std::make_pair(std::string(event), std::string(unit))) != events.end();
        };
        CHECK(fired("UNIT_HEALTH", "party1"));
        if (raid) CHECK(fired("UNIT_HEALTH", "raid1"));
        const auto& member = s.handler().getPartyData().members.front();
        CHECK(member.curHealth == 0);
        CHECK((member.onlineStatus & 0x04) != 0);
    }
}

TEST_CASE("a TBC creature's aura fields give it auras", "[replay][tbc][aura]") {
    // Lady Vashj's Magic Barrier (38112), applied by the shield generators,
    // is in her UNIT_FIELD_AURA slot 0 and nowhere else: TBC has no
    // SMSG_AURA_UPDATE. Only players' aura fields were read, so no creature
    // target showed a buff or a debuff.
    Session s("tbc");
    constexpr uint64_t vashj = 0xF1300052DC5945C1ull;
    s.receive(UpdateObject().tbc().create(vashj, wowee::game::ObjectType::UNIT,
        {{UF::OBJECT_FIELD_ENTRY, 21212}, {UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}}));
    s.receive(UpdateObject().tbc().values(vashj, {{UF::UNIT_FIELD_AURAS, 38112}}));
    const auto* auras = s.handler().getUnitAuras(vashj);
    REQUIRE(auras);
    REQUIRE(!auras->empty());
    CHECK(auras->front().spellId == 38112u);
    CHECK((auras->front().flags & 0x80) == 0);   // slot 0: a buff
    // Removal is the slot going back to zero.
    s.receive(UpdateObject().tbc().values(vashj, {{UF::UNIT_FIELD_AURAS, 0}}));
    CHECK(s.handler().getUnitAuras(vashj)->front().isEmpty());
}

TEST_CASE("a refreshed aura's duration does not pass to the next aura in its slot",
          "[replay][tbc][aura]") {
    // Reliquary of Souls: Fixate 41295 is re-cast every few seconds, each
    // time with SMSG_UPDATE_AURA_DURATION and no field change. When it left
    // the slot, the next aura there (one whose duration the server never
    // sends) took that old duration and showed "0 s".
    Session s("tbc");
    s.receive(UpdateObject().tbc().create(Session::kPlayerGuid, wowee::game::ObjectType::PLAYER,
        {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}}, true));
    s.handler().spellNameCacheLoadedRef() = true;
    const auto duration = [&](uint8_t slot, uint32_t ms) {
        wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_UPDATE_AURA_DURATION));
        p.writeUInt8(slot);
        p.writeUInt32(ms);
        s.receive(p);
    };
    duration(0, 10000);
    s.receive(UpdateObject().tbc().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_AURAS, 41295}}));
    REQUIRE(s.handler().getPlayerAuras().at(0).spellId == 41295u);
    CHECK(s.handler().getPlayerAuras().at(0).durationMs == 10000);
    duration(0, 10000);   // refresh: same spell, fields unchanged
    s.receive(UpdateObject().tbc().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_AURAS, 0}}));
    s.receive(UpdateObject().tbc().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_AURAS, 41294}}));
    REQUIRE(s.handler().getPlayerAuras().at(0).spellId == 41294u);
    CHECK(s.handler().getPlayerAuras().at(0).durationMs == -1);
    // A replacement whose duration did come first still keeps it (Hydross).
    duration(0, 30000);
    s.receive(UpdateObject().tbc().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_AURAS, 38216}}));
    CHECK(s.handler().getPlayerAuras().at(0).durationMs == 30000);
}

TEST_CASE("TBC extra-aura info ignores non-visible passive slots >= 56", "[replay][tbc][aura]") {
    // CMaNGOS TBC assigns passive auras (Magic Resistance 822, Gemcutting 28875,
    // Arcane Affinity 28877, Shadow Resistance 20579) theoretical slots >= 56
    // (MAX_AURAS) and still sends SMSG_SET_EXTRA_AURA_INFO for them. They must
    // not enter playerAuras_ or trigger PLAYER_AURAS_CHANGED.
    Session s("tbc");
    s.receive(UpdateObject().tbc().create(Session::kPlayerGuid, wowee::game::ObjectType::PLAYER,
        {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}}, true));
    int auraEvents = 0;
    s.handler().setAddonEventCallback([&](const std::string& e, const std::vector<std::string>&) {
        if (e == "PLAYER_AURAS_CHANGED") ++auraEvents;
    });
    const auto sendExtra = [&](uint8_t slot, uint32_t spellId, int32_t maxDur, uint32_t remDur) {
        wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_SET_EXTRA_AURA_INFO_OBSOLETE));
        p.writePackedGuid(Session::kPlayerGuid);
        p.writeUInt8(slot);
        p.writeUInt32(spellId);
        p.writeUInt32(static_cast<uint32_t>(maxDur));
        p.writeUInt32(remDur);
        s.receive(p);
    };
    sendExtra(56, 822, -1, 0);
    sendExtra(57, 28875, -1, 0);
    sendExtra(58, 28877, -1, 0);
    sendExtra(59, 20579, -1, 0);
    CHECK(s.handler().getPlayerAuras().size() <= 56u);
    CHECK(auraEvents == 0);

    // Visible slot (< 56) still records duration/caster metadata and notifies UI.
    sendExtra(0, 26992, 600000, 600000);
    REQUIRE(!s.handler().getPlayerAuras().empty());
    CHECK(s.handler().getPlayerAuras().at(0).spellId == 26992u);
    CHECK(s.handler().getPlayerAuras().at(0).durationMs == 600000);
    CHECK(auraEvents == 1);
}

TEST_CASE("every power in a values block is announced, in field order", "[replay][tbc][power]") {
    // A night elf druid shifting into Cat Form (pair 13, CMaNGOS TBC
    // Player::Regenerate and the form's power change in one block): mana,
    // rage and energy move and the display power becomes energy. 2.4.3 fired
    // UNIT_MANA, UNIT_RAGE, UNIT_ENERGY, UNIT_DISPLAYPOWER for "player".
    Session s("tbc");
    s.receive(UpdateObject().tbc().create(Session::kPlayerGuid, wowee::game::ObjectType::PLAYER,
        {{UF::UNIT_FIELD_HEALTH, 4254}, {UF::UNIT_FIELD_LEVEL, 70}, {UF::UNIT_FIELD_BYTES_0, 0x00000B04}},
        true));
    std::vector<std::pair<std::string, std::string>> events;
    s.handler().setAddonEventCallback([&](const std::string& e, const std::vector<std::string>& args) {
        if (e.rfind("UNIT_", 0) == 0)
            events.emplace_back(e, args.empty() ? std::string() : args.front());
    });
    const uint16_t power1 = wowee::game::fieldIndex(UF::UNIT_FIELD_POWER1);
    const uint16_t bytes0 = wowee::game::fieldIndex(UF::UNIT_FIELD_BYTES_0);
    REQUIRE(power1 != 0xFFFF);
    s.receive(UpdateObject().tbc().valuesAt(Session::kPlayerGuid,
        {{power1, 383}, {static_cast<uint16_t>(power1 + 1), 63}, {static_cast<uint16_t>(power1 + 3), 0},
         {bytes0, 0x03000B04}}));
    using Event = std::pair<std::string, std::string>;
    CHECK(events == std::vector<Event>{{"UNIT_MANA", "player"}, {"UNIT_RAGE", "player"},
                                       {"UNIT_ENERGY", "player"}, {"UNIT_DISPLAYPOWER", "player"}});
    const auto* player = s.handler().getEntityManager().getEntity(Session::kPlayerGuid).get();
    REQUIRE(player);
    const auto& unit = static_cast<const wowee::game::Unit&>(*player);
    CHECK(unit.getPowerByType(0) == 383);
    CHECK(unit.getPowerByType(1) == 63);
    CHECK(unit.getPowerType() == 3);
}

TEST_CASE("party stats announce the member's power by name when it moves", "[party][tbc][power]") {
    // A level 10 mage regenerating out of sight (pair 13, Group::UpdatePlayerOutOfRange
    // every five seconds with GROUP_UPDATE_FLAG_CUR_POWER alone). 2.4.3 fired
    // UNIT_MANA party1 for each, and on the member's relog, where every flag
    // is sent, only for the values that changed.
    Session s("tbc");
    constexpr uint64_t memberGuid = 3;
    joinGroup(s, memberGuid, false);
    std::vector<std::pair<std::string, std::string>> events;
    s.handler().setAddonEventCallback([&](const std::string& e, const std::vector<std::string>& args) {
        events.emplace_back(e, args.empty() ? std::string() : args.front());
    });
    using Event = std::pair<std::string, std::string>;
    const auto stats = [&](wowee::game::Opcode opcode, uint32_t flags, uint16_t power) {
        wowee::network::Packet p(wowee::game::wireOpcode(opcode));
        p.writePackedGuid(memberGuid);
        p.writeUInt32(flags);
        if (flags & 0x01) p.writeUInt16(3);
        if (flags & 0x02) p.writeUInt16(187);
        if (flags & 0x04) p.writeUInt16(187);
        if (flags & 0x08) p.writeUInt8(0);
        if (flags & 0x10) p.writeUInt16(power);
        if (flags & 0x20) p.writeUInt16(456);
        if (flags & 0x40) p.writeUInt16(10);
        events.clear();
        s.receive(p);
    };
    using wowee::game::Opcode;
    stats(Opcode::SMSG_PARTY_MEMBER_STATS_FULL, 0x7F, 380);
    CHECK(std::count(events.begin(), events.end(), Event{"UNIT_MANA", "party1"}) == 1);
    CHECK(std::count(events.begin(), events.end(), Event{"UNIT_MAXMANA", "party1"}) == 1);
    stats(Opcode::SMSG_PARTY_MEMBER_STATS, 0x10, 404);
    CHECK(events == std::vector<Event>{{"UNIT_MANA", "party1"}});
    CHECK(s.handler().getPartyData().members.front().curPower == 404);
    // Everything sent again, nothing changed but the mana.
    stats(Opcode::SMSG_PARTY_MEMBER_STATS, 0x7F, 20);
    CHECK(events == std::vector<Event>{{"UNIT_MANA", "party1"}});
    stats(Opcode::SMSG_PARTY_MEMBER_STATS, 0x7F, 20);
    CHECK(events.empty());
}
