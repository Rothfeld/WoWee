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

TEST_CASE("weather updates state and fires WEATHER_CHANGED without chat lines", "[replay]") {
    Session s;
    std::vector<std::pair<std::string, std::vector<std::string>>> events;
    s.handler().setAddonEventCallback(
        [&events](const std::string& name, const std::vector<std::string>& args) {
            if (name == "WEATHER_CHANGED") events.emplace_back(name, args);
        });
    s.receive(weather(3, 0.3f));
    CHECK(s.handler().isRaining());
    s.receive(weather(5, 0.9f));
    CHECK(s.handler().isRaining());
    s.receive(weather(0, 0.0f));
    CHECK_FALSE(s.handler().isRaining());
    s.receive(weather(1, 0.5f));
    CHECK_FALSE(s.handler().isRaining());
    s.receive(weather(0, 0.0f));
    CHECK_FALSE(s.handler().isRaining());
    CHECK(chatLines(s).empty());
    CHECK(events.size() == 5);
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
    CHECK(s.handler().isPlayerFeigningDeath());

    s.receive(UpdateObject().values(Session::kPlayerGuid,
                                    {{UF::UNIT_FIELD_FLAGS_2, 0}, {UF::UNIT_DYNAMIC_FLAGS, 0}}));
    CHECK_FALSE(s.handler().isPlayerDead());
    CHECK_FALSE(s.handler().isPlayerFeigningDeath());
    CHECK(std::count(events.begin(), events.end(), "PLAYER_DEAD") == 0);
    CHECK(std::count(events.begin(), events.end(), "PLAYER_ALIVE") == 0);
}

TEST_CASE("the dead flag at zero health is still a death", "[replay]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 300}, {UF::UNIT_FIELD_MAXHEALTH, 300}});
    s.receive(UpdateObject().values(Session::kPlayerGuid,
                                    {{UF::UNIT_FIELD_HEALTH, 0}, {UF::UNIT_DYNAMIC_FLAGS, 0x20}}));
    CHECK(s.handler().isPlayerDead());
    CHECK_FALSE(s.handler().isPlayerFeigningDeath());
}

// vmangos's Unit::SetFeignDeath sets the dead flag alone.
TEST_CASE("the dead flag at full health is Feign Death without its flags2 bit", "[replay]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 300}, {UF::UNIT_FIELD_MAXHEALTH, 300}});
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_DYNAMIC_FLAGS, 0x20}}));
    CHECK_FALSE(s.handler().isPlayerDead());
    CHECK(s.handler().isPlayerFeigningDeath());
}

TEST_CASE("killed while feigning stays dead when the feign ends", "[replay]") {
    Session s;
    std::vector<std::string> events;
    s.handler().setAddonEventCallback(
        [&events](const std::string& name, const std::vector<std::string>&) { events.push_back(name); });
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 300}, {UF::UNIT_FIELD_MAXHEALTH, 300}});
    s.receive(UpdateObject().values(Session::kPlayerGuid,
                                    {{UF::UNIT_FIELD_FLAGS_2, 0x1}, {UF::UNIT_DYNAMIC_FLAGS, 0x20}}));
    CHECK(s.handler().isPlayerFeigningDeath());
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_HEALTH, 0}}));
    CHECK_FALSE(s.handler().isPlayerFeigningDeath());
    s.receive(UpdateObject().values(Session::kPlayerGuid,
                                    {{UF::UNIT_FIELD_FLAGS_2, 0}, {UF::UNIT_DYNAMIC_FLAGS, 0}}));
    CHECK(s.handler().isPlayerDead());
    CHECK_FALSE(s.handler().isPlayerFeigningDeath());
    CHECK(std::count(events.begin(), events.end(), "PLAYER_ALIVE") == 0);
}

// Arcatraz Warder Corpse (entry 21304, aura 29266 Permanent Feign Death) spawns
// alive with UNIT_DYNFLAG_DEAD + UNIT_FLAG2_FEIGN_DEATH. Killing it sets health=0
// and strips the feign aura (clearing UNIT_DYNFLAG_DEAD) in the same VALUES block.
// Clearing UNIT_DYNFLAG_DEAD at health==0 must NOT fire npcRespawnCallback.
TEST_CASE("killing a feigning NPC keeps it dead instead of standing up", "[replay][corpse]") {
    Session s;
    constexpr uint64_t kNpcGuid = 0xf130005338596e3full;
    int deathCalls = 0;
    bool lastAlreadyDead = false;
    int respawnCalls = 0;
    s.handler().setNpcDeathCallback([&](uint64_t guid, bool alreadyDead) {
        if (guid == kNpcGuid) {
            ++deathCalls;
            lastAlreadyDead = alreadyDead;
        }
    });
    s.handler().setNpcRespawnCallback([&](uint64_t guid) {
        if (guid == kNpcGuid) ++respawnCalls;
    });

    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 300}, {UF::UNIT_FIELD_MAXHEALTH, 300}});
    s.receive(UpdateObject().create(kNpcGuid, wowee::game::ObjectType::UNIT, {
        {UF::UNIT_FIELD_DISPLAYID, 19993},
        {UF::UNIT_FIELD_HEALTH, 10000},
        {UF::UNIT_FIELD_MAXHEALTH, 10000},
        {UF::UNIT_FIELD_FLAGS_2, 0x1},
        {UF::UNIT_DYNAMIC_FLAGS, 0x20},
    }));
    CHECK(deathCalls == 1);
    CHECK(lastAlreadyDead);
    CHECK(respawnCalls == 0);

    // Kill update: health -> 0, feign aura stripped (flags2 -> 0, dynFlags -> 0).
    s.receive(UpdateObject().values(kNpcGuid, {
        {UF::UNIT_FIELD_HEALTH, 0},
        {UF::UNIT_FIELD_FLAGS_2, 0},
        {UF::UNIT_DYNAMIC_FLAGS, 0},
    }));
    CHECK(respawnCalls == 0);
    CHECK(lastAlreadyDead);

    // Contrast: a feigning NPC that cancels Feign Death while alive DOES stand up.
    constexpr uint64_t kLivingFeignGuid = 0xf130005338596e40ull;
    int livingRespawnCalls = 0;
    s.handler().setNpcRespawnCallback([&](uint64_t guid) {
        if (guid == kLivingFeignGuid) ++livingRespawnCalls;
    });
    s.receive(UpdateObject().create(kLivingFeignGuid, wowee::game::ObjectType::UNIT, {
        {UF::UNIT_FIELD_DISPLAYID, 19993},
        {UF::UNIT_FIELD_HEALTH, 10000},
        {UF::UNIT_FIELD_MAXHEALTH, 10000},
        {UF::UNIT_FIELD_FLAGS_2, 0x1},
        {UF::UNIT_DYNAMIC_FLAGS, 0x20},
    }));
    s.receive(UpdateObject().values(kLivingFeignGuid, {
        {UF::UNIT_FIELD_FLAGS_2, 0},
        {UF::UNIT_DYNAMIC_FLAGS, 0},
    }));
    CHECK(livingRespawnCalls == 1);
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
wowee::network::Packet nameResponse(uint64_t guid, const std::string& name,
                                    uint32_t race = 3, uint32_t gender = 0, uint32_t cls = 1) {
    wowee::network::Packet p(
        wowee::game::wireOpcode(wowee::game::Opcode::SMSG_NAME_QUERY_RESPONSE));
    p.writeUInt64(guid); p.writeString(name); p.writeString("");
    p.writeUInt32(race); p.writeUInt32(gender); p.writeUInt32(cls); p.writeUInt8(0);
    return p;
}

wowee::network::Packet tbcMonsterChatPacket(wowee::game::ChatType type,
                                            uint64_t senderGuid,
                                            const std::string& senderName,
                                            uint64_t targetGuid,
                                            const std::string& targetName,
                                            const std::string& message) {
    wowee::network::Packet p(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_MESSAGECHAT));
    p.writeUInt8(static_cast<uint8_t>(type));
    p.writeUInt32(0); // LANG_UNIVERSAL
    p.writeUInt64(senderGuid);
    p.writeUInt32(0);
    p.writeUInt32(static_cast<uint32_t>(senderName.size() + 1));
    p.writeString(senderName);
    p.writeUInt64(targetGuid);
    const bool isPlayer = (targetGuid != 0) && ((targetGuid >> 48) == 0x0000);
    if (targetGuid != 0 && !isPlayer && type != wowee::game::ChatType::WHISPER_FOREIGN) {
        p.writeUInt32(static_cast<uint32_t>(targetName.size() + 1));
        p.writeString(targetName);
    }
    p.writeUInt32(static_cast<uint32_t>(message.size() + 1));
    p.writeString(message);
    p.writeUInt8(0);
    return p;
}

}  // namespace

TEST_CASE("TBC monster and raid-boss chat expands $-tokens and delivers arg1..arg5", "[chat][tbc][bossemote]") {
    using wowee::game::ChatType;
    constexpr uint64_t kBossGuid = 0xF13000624E596E69ULL;
    constexpr uint64_t kRemotePlayerGuid = 0x0000000000000055ULL;
    constexpr uint64_t kNpcTargetGuid = 0xF130000064000001ULL;
    constexpr uint64_t kPetTargetGuid = 0xF140000064000001ULL;

    Session s("tbc");
    // Local player: Human(1) Rogue(4) Male(0)
    const uint32_t localBytes0 = (1u) | (4u << 8) | (0u << 16);
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_LEVEL, 70}, {UF::UNIT_FIELD_BYTES_0, localBytes0}});
    s.receive(nameResponse(Session::kPlayerGuid, "Horserichard", /*race=*/1, /*gender=*/0, /*cls=*/4));

    struct FiredEvent {
        std::string name;
        std::vector<std::string> args;
    };
    std::vector<FiredEvent> events;
    s.handler().setAddonEventCallback([&](const std::string& name, const std::vector<std::string>& args) {
        if (name.rfind("CHAT_MSG_", 0) == 0) {
            events.push_back({name, args});
        }
    });

    SECTION("RAID_BOSS_EMOTE targeting local player expands all $-tokens and %s without double-sub") {
        s.receive(tbcMonsterChatPacket(
            ChatType::RAID_BOSS_EMOTE, kBossGuid, "Grand Warlock Alythess",
            Session::kPlayerGuid, "",
            "%s directs Conflagration at $n. ($N/$c/$C/$r/$R/$gboy:girl;)$bRun!"));
        REQUIRE(events.size() == 1);
        CHECK(events[0].name == "CHAT_MSG_RAID_BOSS_EMOTE");
        REQUIRE(events[0].args.size() >= 8);
        CHECK(events[0].args[0] ==
              "Grand Warlock Alythess directs Conflagration at Horserichard. (Horserichard/rogue/Rogue/human/Human/boy)\nRun!");
        CHECK(events[0].args[1] == "Grand Warlock Alythess");
        CHECK(events[0].args[4] == "Horserichard");
    }

    SECTION("RAID_BOSS_EMOTE targeting uncached remote player waits for NAME_QUERY_RESPONSE") {
        s.receive(tbcMonsterChatPacket(
            ChatType::RAID_BOSS_EMOTE, kBossGuid, "Grand Warlock Alythess",
            kRemotePlayerGuid, "",
            "Alythess directs Conflagration at $n ($c $r $gman:woman;)."));
        // Deferred until name query resolves.
        CHECK(events.empty());
        CHECK(s.sent().of(wowee::game::Opcode::CMSG_NAME_QUERY).size() == 1);

        // Blood Elf(10) Female(1) Paladin(2)
        s.receive(nameResponse(kRemotePlayerGuid, "Horserichy", /*race=*/10, /*gender=*/1, /*cls=*/2));
        REQUIRE(events.size() == 1);
        CHECK(events[0].name == "CHAT_MSG_RAID_BOSS_EMOTE");
        CHECK(events[0].args[0] == "Alythess directs Conflagration at Horserichy (paladin blood elf woman).");
        CHECK(events[0].args[1] == "Grand Warlock Alythess");
        CHECK(events[0].args[4] == "Horserichy");
    }

    SECTION("MONSTER_SAY/YELL/EMOTE/WHISPER/PARTY and RAID_BOSS_WHISPER expand $-tokens") {
        const std::pair<ChatType, std::string> cases[] = {
            {ChatType::MONSTER_SAY, "CHAT_MSG_MONSTER_SAY"},
            {ChatType::MONSTER_PARTY, "CHAT_MSG_MONSTER_PARTY"},
            {ChatType::MONSTER_YELL, "CHAT_MSG_MONSTER_YELL"},
            {ChatType::MONSTER_WHISPER, "CHAT_MSG_MONSTER_WHISPER"},
            {ChatType::MONSTER_EMOTE, "CHAT_MSG_MONSTER_EMOTE"},
            {ChatType::RAID_BOSS_WHISPER, "CHAT_MSG_RAID_BOSS_WHISPER"},
        };
        for (const auto& [ctype, evName] : cases) {
            events.clear();
            s.receive(tbcMonsterChatPacket(
                ctype, kBossGuid, "Alythess", Session::kPlayerGuid, "", "Hello $n ($c)"));
            REQUIRE(events.size() == 1);
            CHECK(events[0].name == evName);
            CHECK(events[0].args[0] == "Hello Horserichard (rogue)");
            CHECK(events[0].args[4] == "Horserichard");
        }
    }

    SECTION("Creature target uses wire targetName; pet target parses cleanly; zero target leaves $-tokens raw") {
        s.receive(tbcMonsterChatPacket(
            ChatType::RAID_BOSS_EMOTE, kBossGuid, "Alythess",
            kNpcTargetGuid, "TargetNPC", "Boss targets $n!"));
        s.receive(tbcMonsterChatPacket(
            ChatType::RAID_BOSS_EMOTE, kBossGuid, "Alythess",
            kPetTargetGuid, "", "Pet target $n!"));
        s.receive(tbcMonsterChatPacket(
            ChatType::RAID_BOSS_EMOTE, kBossGuid, "Alythess",
            0, "", "Zero target $n and $N!"));

        REQUIRE(events.size() == 3);
        CHECK(events[0].args[0] == "Boss targets TargetNPC!");
        CHECK(events[0].args[4] == "TargetNPC");
        CHECK(events[1].args[0] == "Pet target !");
        CHECK(events[1].args[4].empty());
        CHECK(events[2].args[0] == "Zero target $n and $N!");
        CHECK(events[2].args[4].empty());
    }
}

TEST_CASE("TBC sent chat shows only what the server echoes", "[chat][tbc]") {
    using wowee::game::ChatType;
    Session s("tbc");
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_LEVEL, 2}});
    s.receive(nameResponse(Session::kPlayerGuid, "Graulbert"));
    int bubbles = 0;
    s.handler().setChatBubbleCallback([&](uint64_t guid, const std::string& text, ChatType type) {
        CHECK(guid == Session::kPlayerGuid);
        CHECK(text == "bubblecheck");
        CHECK(type == ChatType::SAY);
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
    s.handler().setGlobalStringLookup([](const char* name) -> std::string {
        static const std::vector<std::pair<std::string, std::string>> strings = {
            {"ERR_SET_LOOT_FREEFORALL", "Looting changed to Free for All."},
            {"ERR_SET_LOOT_ROUNDROBIN", "Looting changed to Round Robin."},
            {"ERR_SET_LOOT_MASTER", "Looting changed to Master Looter."},
            {"ERR_SET_LOOT_GROUP", "Looting changed to Group Loot."},
            {"ERR_SET_LOOT_NBG", "Looting set to Need Before Greed."},
            {"ERR_SET_LOOT_THRESHOLD_S", "Loot threshold set to %s."},
            {"ERR_NEW_LOOT_MASTER_S", "%s is now the loot master."},
            {"ITEM_QUALITY2_DESC", "Uncommon"},
            {"ITEM_QUALITY3_DESC", "Rare"},
            {"OPT_OUT_LOOT_TOGGLE_ON", "You are now passing on random loot."},
            {"OPT_OUT_LOOT_TOGGLE_OFF", "You are now eligible for random loot."},
        };
        for (const auto& [key, text] : strings) {
            if (key == name) return text;
        }
        return {};
    });
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

TEST_CASE("Cheap Shot and other INITIATE_COMBAT_POST_CAST spells start melee autoattack on SMSG_SPELL_GO",
          "[replay][strike-autoattack][cheapshot]") {
    using wowee::game::Opcode;

    auto makeTbcSpellGo = [](uint64_t caster, uint32_t spellId, uint64_t target) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_SPELL_GO));
        p.writePackedGuid(caster);
        p.writePackedGuid(caster);
        p.writeUInt32(spellId);
        p.writeUInt16(0);      // castFlags
        p.writeUInt32(0);      // timestamp
        p.writeUInt8(1);       // hitCount
        p.writeUInt64(target); // hitTarget[0]
        p.writeUInt8(0);       // missCount
        p.writeUInt32(0x02u);  // TARGET_FLAG_UNIT
        p.writePackedGuid(target);
        return p;
    };

    SECTION("Cheap Shot (attrEx2=0x00100000) defers auto-attack until SMSG_SPELL_GO") {
        Session s("tbc");
        huntMob(s);
        constexpr uint32_t cheapShot = 1833;
        auto& entry = s.handler().spellNameCacheRef()[cheapShot];
        entry.name = "Cheap Shot";
        entry.attr = 0x00270010u;
        entry.attrEx = 0x08040000u;
        entry.attrEx2 = 0x00100000u;
        s.handler().spellNameCacheLoadedRef() = true;

        s.handler().castSpell(cheapShot, kMob);
        REQUIRE(s.sent().of(Opcode::CMSG_CAST_SPELL).size() == 1);
        // Not started before server confirmation (so failed out-of-stealth cast never swings).
        CHECK_FALSE(s.handler().hasAutoAttackIntent());
        CHECK(s.sent().of(Opcode::CMSG_ATTACKSWING).empty());

        s.receive(makeTbcSpellGo(Session::kPlayerGuid, cheapShot, kMob));
        CHECK(s.handler().hasAutoAttackIntent());
        CHECK(s.sent().of(Opcode::CMSG_ATTACKSWING).size() == 1);
    }

    SECTION("Heroic Strike (ON_NEXT_SWING_NO_DAMAGE) starts auto-attack on cast send") {
        Session s("tbc");
        huntMob(s);
        constexpr uint32_t heroicStrike = 78;
        auto& entry = s.handler().spellNameCacheRef()[heroicStrike];
        entry.name = "Heroic Strike";
        entry.attr = 0x00050014u;
        entry.attrEx = 0x08000000u;
        entry.attrEx2 = 0x00000000u;
        s.handler().spellNameCacheLoadedRef() = true;

        s.handler().castSpell(heroicStrike, kMob);
        REQUIRE(s.sent().of(Opcode::CMSG_CAST_SPELL).size() == 1);
        CHECK(s.handler().hasAutoAttackIntent());
        CHECK(s.sent().of(Opcode::CMSG_ATTACKSWING).size() == 1);
    }

    SECTION("Sap, Gouge, Blind, and Pick Pocket never start auto-attack even on SMSG_SPELL_GO") {
        struct CcCase { uint32_t id; const char* name; uint32_t attr0; uint32_t attrEx; };
        const CcCase cases[] = {
            {6770u, "Sap",         0x00370010u, 0x08040520u},
            {1776u, "Gouge",       0x00150010u, 0x08040000u},
            {2094u, "Blind",       0x40110010u, 0x00000000u},
            {921u,  "Pick Pocket", 0x00030010u, 0x00221020u},
        };
        for (const auto& cc : cases) {
            CAPTURE(cc.id, cc.name);
            Session s("tbc");
            huntMob(s);
            auto& entry = s.handler().spellNameCacheRef()[cc.id];
            entry.name = cc.name;
            entry.attr = cc.attr0;
            entry.attrEx = cc.attrEx;
            entry.attrEx2 = 0u;
            s.handler().spellNameCacheLoadedRef() = true;

            s.handler().castSpell(cc.id, kMob);
            s.receive(makeTbcSpellGo(Session::kPlayerGuid, cc.id, kMob));
            CHECK_FALSE(s.handler().hasAutoAttackIntent());
            CHECK(s.sent().of(Opcode::CMSG_ATTACKSWING).empty());
        }
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

TEST_CASE("TBC MSG_LIST_STABLED_PETS parses uint32 loyalty and all pets without desync", "[stable][tbc]") {
    Session s("tbc");
    using wowee::game::Opcode;
    constexpr uint64_t npcGuid = 0xF130002700000123ULL;

    std::vector<std::string> events;
    s.handler().setAddonEventCallback([&](const std::string& e, const std::vector<std::string>&) {
        events.push_back(e);
    });

    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::MSG_LIST_STABLED_PETS));
    p.writeUInt64(npcGuid);
    p.writeUInt8(3); // 1 active + 2 stabled
    p.writeUInt8(2); // 2 purchased stable slots
    // Pet 1: active wolf (slot 1)
    p.writeUInt32(101);
    p.writeUInt32(705);
    p.writeUInt32(12);
    p.writeString("Timber");
    p.writeUInt32(6); // Loyalty Level 6 (Best Friend)
    p.writeUInt8(1);  // active
    // Pet 2: stabled boar (slot 2)
    p.writeUInt32(102);
    p.writeUInt32(708);
    p.writeUInt32(11);
    p.writeString("Porkchop");
    p.writeUInt32(3); // Loyalty Level 3 (Submissive)
    p.writeUInt8(2);  // first stable slot
    // Pet 3: stabled bear (slot 3)
    p.writeUInt32(103);
    p.writeUInt32(1128);
    p.writeUInt32(14);
    p.writeString("Misha");
    p.writeUInt32(1); // Loyalty Level 1 (Rebellious)
    p.writeUInt8(3);  // second stable slot

    s.receive(p);

    CHECK(s.handler().isStableWindowOpen());
    CHECK(s.handler().getStableMasterGuid() == npcGuid);
    CHECK(s.handler().getStableSlots() == 2);
    const auto& pets = s.handler().getStabledPets();
    REQUIRE(pets.size() == 3);

    CHECK(pets[0].petNumber == 101);
    CHECK(pets[0].entry == 705);
    CHECK(pets[0].level == 12);
    CHECK(pets[0].name == "Timber");
    CHECK(pets[0].loyalty == 6);
    CHECK(pets[0].slot == 1);
    CHECK(pets[0].isActive);

    CHECK(pets[1].petNumber == 102);
    CHECK(pets[1].entry == 708);
    CHECK(pets[1].level == 11);
    CHECK(pets[1].name == "Porkchop");
    CHECK(pets[1].loyalty == 3);
    CHECK(pets[1].slot == 2);
    CHECK_FALSE(pets[1].isActive);

    CHECK(pets[2].petNumber == 103);
    CHECK(pets[2].entry == 1128);
    CHECK(pets[2].level == 14);
    CHECK(pets[2].name == "Misha");
    CHECK(pets[2].loyalty == 1);
    CHECK(pets[2].slot == 3);
    CHECK_FALSE(pets[2].isActive);

    CHECK(events == std::vector<std::string>{"PET_STABLE_SHOW", "PET_STABLE_UPDATE"});
}

TEST_CASE("TBC SMSG_STABLE_RESULT handles exact StableResultCode enum and refreshes list", "[stable][tbc]") {
    Session s("tbc");
    using wowee::game::Opcode;
    constexpr uint64_t npcGuid = 0xF130002700000123ULL;

    // Open stable window with 0 slots
    wowee::network::Packet listPkt(wowee::game::wireOpcode(Opcode::MSG_LIST_STABLED_PETS));
    listPkt.writeUInt64(npcGuid);
    listPkt.writeUInt8(0);
    listPkt.writeUInt8(0);
    s.receive(listPkt);
    REQUIRE(s.handler().isStableWindowOpen());
    REQUIRE(s.handler().getStableSlots() == 0);

    const auto sendResult = [&](uint8_t code) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_STABLE_RESULT));
        p.writeUInt8(code);
        s.receive(p);
    };

    // 0x01 = STABLE_ERR_MONEY -> no list refresh
    sendResult(0x01);
    CHECK(s.sent().of(Opcode::MSG_LIST_STABLED_PETS).empty());
    CHECK(s.handler().getStableSlots() == 0);

    // 0x06 = STABLE_ERR_STABLE -> no list refresh
    sendResult(0x06);
    CHECK(s.sent().of(Opcode::MSG_LIST_STABLED_PETS).empty());

    // 0x08 = STABLE_SUCCESS_STABLE -> re-queries MSG_LIST_STABLED_PETS
    sendResult(0x08);
    CHECK(s.sent().of(Opcode::MSG_LIST_STABLED_PETS).size() == 1);

    // 0x09 = STABLE_SUCCESS_UNSTABLE / swap -> re-queries MSG_LIST_STABLED_PETS
    sendResult(0x09);
    CHECK(s.sent().of(Opcode::MSG_LIST_STABLED_PETS).size() == 2);

    // 0x0A = STABLE_SUCCESS_BUY_SLOT -> increments slots & re-queries MSG_LIST_STABLED_PETS
    sendResult(0x0A);
    CHECK(s.handler().getStableSlots() == 1);
    CHECK(s.sent().of(Opcode::MSG_LIST_STABLED_PETS).size() == 3);
}

TEST_CASE("Stable client packets match TBC mangos-tbc wire layouts", "[stable][tbc]") {
    Session s("tbc");
    using wowee::game::Opcode;
    constexpr uint64_t npcGuid = 0xF130002700000456ULL;

    wowee::network::Packet listPkt(wowee::game::wireOpcode(Opcode::MSG_LIST_STABLED_PETS));
    listPkt.writeUInt64(npcGuid);
    listPkt.writeUInt8(0);
    listPkt.writeUInt8(2);
    s.receive(listPkt);

    // CMSG_STABLE_PET: exactly 8 bytes (uint64 npcGuid only, no trailing slot byte)
    s.handler().stablePet();
    {
        auto pkts = s.sent().of(Opcode::CMSG_STABLE_PET);
        REQUIRE(pkts.size() == 1);
        CHECK(pkts[0].getSize() == 8);
        CHECK(pkts[0].readUInt64() == npcGuid);
    }

    // CMSG_UNSTABLE_PET: 12 bytes (uint64 npcGuid + uint32 petNumber)
    s.handler().unstablePet(42);
    {
        auto pkts = s.sent().of(Opcode::CMSG_UNSTABLE_PET);
        REQUIRE(pkts.size() == 1);
        CHECK(pkts[0].getSize() == 12);
        CHECK(pkts[0].readUInt64() == npcGuid);
        CHECK(pkts[0].readUInt32() == 42);
    }

    // CMSG_BUY_STABLE_SLOT: 8 bytes (uint64 npcGuid)
    s.handler().buyStableSlot();
    {
        auto pkts = s.sent().of(Opcode::CMSG_BUY_STABLE_SLOT);
        REQUIRE(pkts.size() == 1);
        CHECK(pkts[0].getSize() == 8);
        CHECK(pkts[0].readUInt64() == npcGuid);
    }

    // CMSG_STABLE_SWAP_PET: 12 bytes (uint64 npcGuid + uint32 petNumber)
    s.handler().stableSwapPet(99);
    {
        auto pkts = s.sent().of(Opcode::CMSG_STABLE_SWAP_PET);
        REQUIRE(pkts.size() == 1);
        CHECK(pkts[0].getSize() == 12);
        CHECK(pkts[0].readUInt64() == npcGuid);
        CHECK(pkts[0].readUInt32() == 99);
    }
}

TEST_CASE("party member stats survive instance exit and refresh on worldport ack",
          "[party][tbc][raid][worldport]") {
    using wowee::game::Opcode;
    using wowee::game::wireOpcode;

    for (const bool raid : {false, true}) {
        INFO((raid ? "raid" : "party"));
        Session s("tbc");
        constexpr uint64_t memberGuid = 96;
        joinGroup(s, memberGuid, raid);
        // Joining a group requests full stats for remote members.
        CHECK(s.sent().of(Opcode::CMSG_REQUEST_PARTY_MEMBER_STATS).size() == 1);

        // Teammate is in view inside an instance with mana, level, PvP, and >65535 HP.
        const uint16_t power1 = wowee::game::fieldIndex(UF::UNIT_FIELD_POWER1);
        const uint16_t maxPower1 = wowee::game::fieldIndex(UF::UNIT_FIELD_MAXPOWER1);
        const uint16_t bytes0 = wowee::game::fieldIndex(UF::UNIT_FIELD_BYTES_0);
        const uint16_t unitFlags = wowee::game::fieldIndex(UF::UNIT_FIELD_FLAGS);
        const uint16_t playerFlags = wowee::game::fieldIndex(UF::PLAYER_FLAGS);
        REQUIRE(power1 != 0xFFFF);
        REQUIRE(maxPower1 != 0xFFFF);
        s.receive(UpdateObject().tbc().create(memberGuid, wowee::game::ObjectType::PLAYER, {
            {UF::UNIT_FIELD_HEALTH, 10000000},
            {UF::UNIT_FIELD_MAXHEALTH, 10000000},
            {UF::UNIT_FIELD_LEVEL, 70},
            {UF::UNIT_FIELD_BYTES_0, 0x00000805}, // powerType 0 (mana), mage, undead
            {UF::UNIT_FIELD_FLAGS, 0x00001000},   // UNIT_FLAG_PVP
            {UF::PLAYER_FLAGS, 0x00000002},       // PLAYER_FLAGS_AFK
        }));
        s.receive(UpdateObject().tbc().valuesAt(memberGuid, {
            {power1, 7420},
            {maxPower1, 9800},
            {bytes0, 0x00000805},
            {unitFlags, 0x00001000},
            {playerFlags, 0x00000002},
        }));

        // Mirrored into GroupMember while in view.
        {
            const auto& m = s.handler().getPartyData().members.front();
            CHECK(m.curHealth == 10000000u);
            CHECK(m.maxHealth == 10000000u);
            CHECK(m.powerType == 0u);
            CHECK(m.curPower == 7420u);
            CHECK(m.maxPower == 9800u);
            CHECK(m.level == 70u);
            CHECK(m.isOnline == 1u);
            CHECK((m.onlineStatus & 0x0002u) != 0); // PVP
            CHECK((m.onlineStatus & 0x0040u) != 0); // AFK
        }

        // Local player exits the instance: SMSG_NEW_WORLD clears entities,
        // MSG_MOVE_WORLDPORT_ACK re-requests party member stats.
        wowee::network::Packet newWorld(wireOpcode(Opcode::SMSG_NEW_WORLD));
        newWorld.writeUInt32(1);        // Kalimdor
        newWorld.writeFloat(-8415.7f);
        newWorld.writeFloat(1502.44f);
        newWorld.writeFloat(30.68f);
        newWorld.writeFloat(5.49f);
        s.receive(newWorld);
        CHECK_FALSE(s.handler().getEntityManager().hasEntity(memberGuid));

        s.handler().sendPendingWorldportAck();
        const auto requestsAfterAck = s.sent().of(Opcode::CMSG_REQUEST_PARTY_MEMBER_STATS).size();
        CHECK(requestsAfterAck == 2);

        // Even if a delta SMSG_PARTY_MEMBER_STATS (flags=2, CUR_HP only, uint16)
        // arrives before SMSG_PARTY_MEMBER_STATS_FULL, maxHealth must not remain
        // 32-bit 10000000 while curHealth is 16-bit 38528 (0.38% HP / red portrait).
        const uint16_t low16 = static_cast<uint16_t>(10000000u & 0xFFFFu);
        wowee::network::Packet delta(wireOpcode(Opcode::SMSG_PARTY_MEMBER_STATS));
        delta.writePackedGuid(memberGuid);
        delta.writeUInt32(0x0002u); // GROUP_UPDATE_FLAG_CUR_HP only
        delta.writeUInt16(low16);
        s.receive(delta);
        {
            const auto& m = s.handler().getPartyData().members.front();
            CHECK(m.curHealth == low16);
            CHECK(m.maxHealth == low16);
            CHECK(m.curPower == 7420u);
            CHECK(m.maxPower == 9800u);
        }
        CHECK(s.sent().of(Opcode::CMSG_REQUEST_PARTY_MEMBER_STATS).size() == 3);
    }
}

TEST_CASE("TBC SMSG_PARTY_MEMBER_STATS_FULL parses all wire fields and fires pet/pvp events",
          "[party][tbc]") {
    using wowee::game::Opcode;
    using wowee::game::wireOpcode;

    Session s("tbc");
    constexpr uint64_t memberGuid = 96;
    constexpr uint64_t petGuid = 0xF140000000000123ULL;
    joinGroup(s, memberGuid, false);

    std::vector<std::pair<std::string, std::string>> events;
    s.handler().setAddonEventCallback(
        [&](const std::string& e, const std::vector<std::string>& args) {
            events.emplace_back(e, args.empty() ? std::string() : args.front());
        });

    // Exact CMaNGOS-TBC HandleRequestPartyMemberStatsOpcode layout with pet (0x7FFFFFFF).
    wowee::network::Packet full(wireOpcode(Opcode::SMSG_PARTY_MEMBER_STATS_FULL));
    full.writePackedGuid(memberGuid);
    full.writeUInt32(0x7FFFFFFFu);
    full.writeUInt16(0x0003u);  // ONLINE | PVP
    full.writeUInt16(6120u);    // CUR_HP
    full.writeUInt16(6800u);    // MAX_HP
    full.writeUInt8(1u);        // POWER_TYPE = Rage
    full.writeUInt16(450u);     // CUR_POWER (45 rage in tenths)
    full.writeUInt16(1000u);    // MAX_POWER (100 rage in tenths)
    full.writeUInt16(70u);      // LEVEL
    full.writeUInt16(3429u);    // ZONE (Ruins of Ahn'Qiraj)
    full.writeUInt16(static_cast<uint16_t>(int16_t(-8429)));
    full.writeUInt16(static_cast<uint16_t>(int16_t(1512)));
    full.writeUInt64(0x1ull);   // AURAS: slot 0
    full.writeUInt16(25389u);   // Power Word: Fortitude
    full.writeUInt8(0x09u);     // aura flags
    full.writeUInt64(petGuid);  // PET_GUID
    full.writeString("Wolf");   // PET_NAME
    full.writeUInt16(1234u);    // PET_MODEL_ID
    full.writeUInt16(2400u);    // PET_CUR_HP
    full.writeUInt16(2500u);    // PET_MAX_HP
    full.writeUInt8(2u);        // PET_POWER_TYPE = Focus
    full.writeUInt16(80u);      // PET_CUR_POWER
    full.writeUInt16(100u);     // PET_MAX_POWER
    full.writeUInt64(0ull);     // PET_AURAS
    s.receive(full);

    const auto& m = s.handler().getPartyData().members.front();
    CHECK(m.hasPartyStats);
    CHECK(m.isOnline == 1u);
    CHECK(m.onlineStatus == 0x0003u);
    CHECK(m.curHealth == 6120u);
    CHECK(m.maxHealth == 6800u);
    CHECK(m.powerType == 1u);
    CHECK(m.curPower == 450u);
    CHECK(m.maxPower == 1000u);
    CHECK(m.level == 70u);
    CHECK(m.zoneId == 3429u);
    CHECK(m.posX == -8429);
    CHECK(m.posY == 1512);
    CHECK(m.petGuid == petGuid);

    const auto fired = [&](const char* event, const char* unit) {
        return std::find(events.begin(), events.end(),
                         std::make_pair(std::string(event), std::string(unit))) != events.end();
    };
    CHECK(fired("UNIT_LEVEL", "party1"));
    CHECK(fired("UNIT_HEALTH", "party1"));
    CHECK(fired("UNIT_MAXHEALTH", "party1"));
    CHECK(fired("UNIT_DISPLAYPOWER", "party1"));
    CHECK(fired("UNIT_RAGE", "party1"));
    CHECK(fired("UNIT_MAXRAGE", "party1"));
    CHECK(fired("UNIT_AURA", "party1"));
    CHECK(fired("UNIT_FACTION", "party1"));
    CHECK(fired("UNIT_PET", "party1"));
}

TEST_CASE("out-of-range and destroy of in-view party member mirror stats and re-request",
          "[party][tbc]") {
    using wowee::game::Opcode;
    using wowee::game::wireOpcode;

    for (const bool destroy : {false, true}) {
        INFO((destroy ? "SMSG_DESTROY_OBJECT" : "OUT_OF_RANGE_OBJECTS"));
        Session s("tbc");
        constexpr uint64_t memberGuid = 77;
        joinGroup(s, memberGuid, false);
        REQUIRE(s.sent().of(Opcode::CMSG_REQUEST_PARTY_MEMBER_STATS).size() == 1);

        const uint16_t power1 = wowee::game::fieldIndex(UF::UNIT_FIELD_POWER1);
        const uint16_t maxPower1 = wowee::game::fieldIndex(UF::UNIT_FIELD_MAXPOWER1);
        s.receive(UpdateObject().tbc().create(memberGuid, wowee::game::ObjectType::PLAYER, {
            {UF::UNIT_FIELD_HEALTH, 4100},
            {UF::UNIT_FIELD_MAXHEALTH, 5200},
            {UF::UNIT_FIELD_LEVEL, 64},
            {UF::UNIT_FIELD_BYTES_0, 0x00000504}, // powerType 0 (mana)
        }));
        s.receive(UpdateObject().tbc().valuesAt(memberGuid, {
            {power1, 3150},
            {maxPower1, 4800},
        }));

        wowee::network::Packet despawn(wireOpcode(destroy ? Opcode::SMSG_DESTROY_OBJECT
                                                          : Opcode::SMSG_UPDATE_OBJECT));
        if (destroy) {
            despawn.writeUInt64(memberGuid);
            despawn.writeUInt8(0);
        } else {
            despawn.writeUInt32(1);
            despawn.writeUInt8(static_cast<uint8_t>(wowee::game::UpdateType::OUT_OF_RANGE_OBJECTS));
            despawn.writeUInt32(1);
            despawn.writePackedGuid(memberGuid);
        }
        s.receive(despawn);

        CHECK_FALSE(s.handler().getEntityManager().hasEntity(memberGuid));
        const auto& m = s.handler().getPartyData().members.front();
        CHECK(m.curHealth == 4100u);
        CHECK(m.maxHealth == 5200u);
        CHECK(m.curPower == 3150u);
        CHECK(m.maxPower == 4800u);
        CHECK(m.level == 64u);
        CHECK(s.sent().of(Opcode::CMSG_REQUEST_PARTY_MEMBER_STATS).size() == 2);
    }
}

TEST_CASE("Tab and TargetNearest* stay silent with no candidates and preserve existing target",
          "[target][tab][tbc]") {
    Session s("tbc");
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_LEVEL, 70}});

    int uiErrors = 0;
    s.handler().setAddonEventCallback(
        [&](const std::string& e, const std::vector<std::string>&) {
            if (e == "UI_ERROR_MESSAGE") ++uiErrors;
        });

    const size_t chatBefore = s.handler().getChatHistory().size();

    // No candidates anywhere: every nearest/last target helper stays silent.
    s.handler().tabTarget();
    s.handler().targetEnemy(false);
    s.handler().targetEnemy(true);
    s.handler().targetFriend(false);
    s.handler().targetNearestEnemyPlayer(false);
    s.handler().targetNearestFriendPlayer(false);
    s.handler().targetNearestPartyMember(false);
    s.handler().targetNearestRaidMember(false);
    s.handler().targetLastTarget();
    s.handler().targetLastEnemy();
    s.handler().targetLastFriend();

    CHECK(s.handler().getTargetGuid() == 0);
    CHECK(s.handler().getChatHistory().size() == chatBefore);
    CHECK(uiErrors == 0);

    // Existing target is preserved when Tab finds no enemies in range.
    s.handler().setTarget(Session::kPlayerGuid);
    REQUIRE(s.handler().getTargetGuid() == Session::kPlayerGuid);
    s.handler().tabTarget();
    s.handler().targetEnemy(true);
    CHECK(s.handler().getTargetGuid() == Session::kPlayerGuid);
    CHECK(s.handler().getChatHistory().size() == chatBefore);
    CHECK(uiErrors == 0);

    // When a living hostile creature IS in front and in range, Tab selects it cleanly.
    constexpr uint64_t kEnemyGuid = 0xF130000010000001ULL;
    s.receive(UpdateObject().tbc().create(kEnemyGuid, wowee::game::ObjectType::UNIT, {
        {UF::UNIT_FIELD_HEALTH, 100},
        {UF::UNIT_FIELD_MAXHEALTH, 100},
        {UF::UNIT_FIELD_LEVEL, 70},
        {UF::UNIT_FIELD_FACTIONTEMPLATE, 14},  // Monster (hostile)
    }));
    auto enemy = s.handler().getEntityManager().getEntity(kEnemyGuid);
    REQUIRE(enemy);
    enemy->setPosition(10.0f, 0.0f, 0.0f, 0.0f);
    s.handler().tabTarget();
    CHECK(s.handler().getTargetGuid() == kEnemyGuid);
    CHECK(s.handler().getChatHistory().size() == chatBefore);
    CHECK(uiErrors == 0);
}

TEST_CASE("targeting an out-of-range party member persists across ticks and despawns until leaving group",
          "[party][target][tbc]") {
    using wowee::game::Opcode;
    using wowee::game::wireOpcode;

    class ConnectedSentPackets : public replay::SentPackets {
    public:
        [[nodiscard]] bool isConnected() const override { return true; }
        void update() override {}
    };

    Session s("tbc");
    wowee::game::GameHandlerTestAccess::setSocket(
        s.handler(), std::make_unique<ConnectedSentPackets>());
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_LEVEL, 70}});
    constexpr uint64_t memberGuid = 0x66;
    joinGroup(s, memberGuid, false);

    // 1. Party member is out of visibility range (not in EntityManager):
    //    clicking PartyMemberFrame1 -> setTarget(memberGuid) must survive GameHandler::update().
    REQUIRE_FALSE(s.handler().getEntityManager().hasEntity(memberGuid));
    REQUIRE(s.handler().isGroupMember(memberGuid));
    s.handler().setTarget(memberGuid);
    REQUIRE(s.handler().getTargetGuid() == memberGuid);
    s.handler().update(0.05f);
    CHECK(s.handler().getTargetGuid() == memberGuid);

    // 2. Party member comes into range and then leaves via OUT_OF_RANGE_OBJECTS or SMSG_DESTROY_OBJECT:
    //    selection must remain on the party member in both cases.
    for (const bool destroy : {false, true}) {
        INFO((destroy ? "SMSG_DESTROY_OBJECT" : "OUT_OF_RANGE_OBJECTS"));
        s.receive(UpdateObject().tbc().create(memberGuid, wowee::game::ObjectType::PLAYER, {
            {UF::UNIT_FIELD_HEALTH, 0},
            {UF::UNIT_FIELD_MAXHEALTH, 5200},
            {UF::UNIT_FIELD_LEVEL, 70},
        }));
        REQUIRE(s.handler().getEntityManager().hasEntity(memberGuid));
        s.handler().setTarget(memberGuid);
        REQUIRE(s.handler().getTargetGuid() == memberGuid);

        wowee::network::Packet despawn(wireOpcode(destroy ? Opcode::SMSG_DESTROY_OBJECT
                                                          : Opcode::SMSG_UPDATE_OBJECT));
        if (destroy) {
            despawn.writeUInt64(memberGuid);
            despawn.writeUInt8(0);
        } else {
            despawn.writeUInt32(1);
            despawn.writeUInt8(static_cast<uint8_t>(wowee::game::UpdateType::OUT_OF_RANGE_OBJECTS));
            despawn.writeUInt32(1);
            despawn.writePackedGuid(memberGuid);
        }
        s.receive(despawn);
        REQUIRE_FALSE(s.handler().getEntityManager().hasEntity(memberGuid));
        CHECK(s.handler().getTargetGuid() == memberGuid);
        s.handler().update(0.05f);
        CHECK(s.handler().getTargetGuid() == memberGuid);
    }

    // 3. Non-group entity going out of range still clears target.
    constexpr uint64_t strangerGuid = 0xF130000010000099ULL;
    s.receive(UpdateObject().tbc().create(strangerGuid, wowee::game::ObjectType::UNIT, {
        {UF::UNIT_FIELD_HEALTH, 100},
        {UF::UNIT_FIELD_MAXHEALTH, 100},
        {UF::UNIT_FIELD_LEVEL, 70},
    }));
    s.handler().setTarget(strangerGuid);
    REQUIRE(s.handler().getTargetGuid() == strangerGuid);
    {
        wowee::network::Packet oor(wireOpcode(Opcode::SMSG_UPDATE_OBJECT));
        oor.writeUInt32(1);
        oor.writeUInt8(static_cast<uint8_t>(wowee::game::UpdateType::OUT_OF_RANGE_OBJECTS));
        oor.writeUInt32(1);
        oor.writePackedGuid(strangerGuid);
        s.receive(oor);
    }
    CHECK(s.handler().getTargetGuid() == 0);

    // 4. Re-target out-of-range party member, then disband group (SMSG_GROUP_DESTROYED):
    //    once no longer in group and not in EntityManager, update() clears target.
    s.handler().setTarget(memberGuid);
    REQUIRE(s.handler().getTargetGuid() == memberGuid);
    {
        wowee::network::Packet disband(wireOpcode(Opcode::SMSG_GROUP_DESTROYED));
        s.receive(disband);
    }
    REQUIRE_FALSE(s.handler().isGroupMember(memberGuid));
    s.handler().update(0.05f);
    CHECK(s.handler().getTargetGuid() == 0);
}



