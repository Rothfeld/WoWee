// Spell and melee range against a large unit, as native 2.4.3 measures it:
// 3D, centre to centre, with both units' UNIT_FIELD_COMBATREACH. Measured on
// pair 2 from a human rogue (reach 1.5) at a stunned Mottled Boar whose combat
// reach was set to Onyxia's 23.4 (_experiments/20261118T170000Z.melee_range_pair2
// in the workspace). See spellclass::checkSpellRange.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"

#include <bit>
#include <string>
#include <vector>

using replay::Session;
using replay::UpdateObject;
using wowee::game::ObjectType;
using wowee::game::Opcode;
using wowee::game::spellclass::SpellRangeCheck;
using UF = wowee::game::UF;
namespace spellclass = wowee::game::spellclass;

namespace {

constexpr uint64_t kBoar = 0xF130000C1A0012A1ull;
constexpr uint32_t kSinisterStrike = 26862;
constexpr uint32_t kThrow = 2764;
constexpr uint32_t kBlind = 2094;

struct Observed {
    std::vector<std::string> errors;
    std::vector<std::string> events;  // "UNIT_SPELLCAST_FAILED Sinister Strike"
};

// A human rogue and a boar with Onyxia's reach at the origin, the spells'
// SpellRange rows (2 Combat Range, 74 Ranged Weapon, 7 Ten yards) and the
// GlobalStrings lines the refusals print.
void rogueAtBoar(Session& s, Observed& seen) {
    auto& cache = s.handler().spellNameCacheRef();
    for (const auto& [id, name] : {std::pair{kSinisterStrike, "Sinister Strike"},
                                   std::pair{kThrow, "Throw"}, std::pair{kBlind, "Blind"}}) {
        cache[id].name = name;
        cache[id].implicitTargetsA[0] = spellclass::kImplicitTargetEnemy;
    }
    cache[kSinisterStrike].maxRange = 5.0f;
    cache[kThrow].maxRange = 30.0f;
    cache[kBlind].maxRange = 10.0f;
    s.handler().spellNameCacheLoadedRef() = true;
    s.handler().setSpellDataResolver([](uint32_t id) {
        wowee::game::GameHandler::SpellDataInfo info;
        if (id == kSinisterStrike) { info.maxRange = 5.0f; info.rangeFlags = spellclass::kSpellRangeFlagMelee; }
        if (id == kThrow) { info.maxRange = 30.0f; info.rangeFlags = spellclass::kSpellRangeFlagRanged; }
        if (id == kBlind) info.maxRange = 10.0f;
        return info;
    });
    s.handler().setGlobalStringLookup([](const char* name) {
        const std::string n(name);
        if (n == "ERR_SPELL_OUT_OF_RANGE") return std::string("Out of range.");
        if (n == "SPELL_FAILED_TOO_CLOSE") return std::string("Target too close");
        if (n == "ERR_BADATTACKPOS") return std::string("You are too far away!");
        return std::string();
    });
    s.handler().setUIErrorCallback([&seen](const std::string& msg) { seen.errors.push_back(msg); });
    s.handler().setAddonEventCallback([&seen](const std::string& name, const std::vector<std::string>& args) {
        if (name.rfind("UNIT_SPELLCAST_", 0) == 0 && args.size() > 1) seen.events.push_back(name + " " + args[1]);
    });
    s.receive(UpdateObject().tbc().create(Session::kPlayerGuid, ObjectType::PLAYER,
                                          {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100},
                                           {UF::UNIT_FIELD_LEVEL, 40},
                                           {UF::UNIT_FIELD_COMBATREACH, std::bit_cast<uint32_t>(1.5f)}},
                                          true));
    s.receive(UpdateObject().tbc().create(kBoar, ObjectType::UNIT,
                                          {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100},
                                           {UF::UNIT_FIELD_LEVEL, 1},
                                           {UF::UNIT_FIELD_COMBATREACH, std::bit_cast<uint32_t>(23.4f)}}));
    s.handler().setTarget(kBoar);
}

size_t casts(const Session& s) { return s.sent().of(Opcode::CMSG_CAST_SPELL).size(); }

}  // namespace

TEST_CASE("IsSpellInRange takes both combat reaches", "[replay][melee-range]") {
    Session s("tbc");
    Observed seen;
    rogueAtBoar(s, seen);
    auto& gh = s.handler();
    // Native: Sinister Strike 1 at 26.231 yards, 0 at 26.236; Throw the opposite.
    gh.setPosition(26.231f, 0.0f, 0.0f);
    CHECK(gh.unitSpellRange(kSinisterStrike, kBoar) == SpellRangeCheck::InRange);
    CHECK(gh.unitSpellRange(kThrow, kBoar) == SpellRangeCheck::TooClose);
    CHECK(gh.unitWithinMeleeReach(kBoar) == true);
    gh.setPosition(26.236f, 0.0f, 0.0f);
    CHECK(gh.unitSpellRange(kSinisterStrike, kBoar) == SpellRangeCheck::OutOfRange);
    CHECK(gh.unitSpellRange(kThrow, kBoar) == SpellRangeCheck::InRange);
    CHECK(gh.unitWithinMeleeReach(kBoar) == false);
    // 3D: 26.2 yards away along the ground and 1 yard up is 26.22, in; 2 up
    // is 26.28, out.
    gh.setPosition(26.2f, 0.0f, 1.0f);
    CHECK(gh.unitSpellRange(kSinisterStrike, kBoar) == SpellRangeCheck::InRange);
    gh.setPosition(26.2f, 0.0f, 2.0f);
    CHECK(gh.unitSpellRange(kSinisterStrike, kBoar) == SpellRangeCheck::OutOfRange);
    // Blind (10 yards): in at 34.849, out at 34.950. Throw out past 54.9.
    gh.setPosition(34.849f, 0.0f, 0.0f);
    CHECK(gh.unitSpellRange(kBlind, kBoar) == SpellRangeCheck::InRange);
    gh.setPosition(34.95f, 0.0f, 0.0f);
    CHECK(gh.unitSpellRange(kBlind, kBoar) == SpellRangeCheck::OutOfRange);
    gh.setPosition(54.964f, 0.0f, 0.0f);
    CHECK(gh.unitSpellRange(kThrow, kBoar) == SpellRangeCheck::OutOfRange);
}

TEST_CASE("Sinister Strike reaches a large unit from its melee reach", "[replay][melee-range]") {
    Session s("tbc");
    Observed seen;
    rogueAtBoar(s, seen);
    auto& gh = s.handler();
    // Native: sent and hit at 20.06 and 26.16 yards - this client refused
    // anything past 8.
    gh.setPosition(20.0f, 0.0f, 0.0f);
    gh.castSpell(kSinisterStrike, kBoar);
    CHECK(casts(s) == 1);
    CHECK(seen.errors.empty());
}

TEST_CASE("out of range is refused before sending, as the original does", "[replay][melee-range]") {
    Session s("tbc");
    Observed seen;
    rogueAtBoar(s, seen);
    auto& gh = s.handler();
    // Native at 26.67 yards: "Out of range.", UNIT_SPELLCAST_FAILED, nothing sent.
    gh.setPosition(26.67f, 0.0f, 0.0f);
    gh.castSpell(kSinisterStrike, kBoar);
    CHECK(casts(s) == 0);
    CHECK(seen.errors == std::vector<std::string>{"Out of range."});
    CHECK(seen.events == std::vector<std::string>{"UNIT_SPELLCAST_FAILED Sinister Strike"});
    // Throw at 20.06: "Target too close"; at 56.03: "Out of range."; 40 is sent.
    gh.setPosition(20.06f, 0.0f, 0.0f);
    gh.castSpell(kThrow, kBoar);
    gh.setPosition(56.03f, 0.0f, 0.0f);
    gh.castSpell(kThrow, kBoar);
    CHECK(casts(s) == 0);
    CHECK(seen.errors == std::vector<std::string>{"Out of range.", "Target too close", "Out of range."});
    gh.setPosition(40.0f, 0.0f, 0.0f);
    gh.castSpell(kThrow, kBoar);
    CHECK(casts(s) == 1);
}

TEST_CASE("an attack out of reach says so at once and every four seconds", "[replay][melee-range]") {
    Session s("tbc");
    Observed seen;
    rogueAtBoar(s, seen);
    auto& gh = s.handler();
    // Native at 26.16 yards: swings land, no error.
    gh.setPosition(26.16f, 0.0f, 0.0f);
    gh.startAutoAttack(kBoar);
    CHECK(seen.errors.empty());
    gh.stopAutoAttack();
    // At 26.47: its own error, then the server's 70 ms later, then one every
    // 4.00 to 4.02 seconds after the server's.
    gh.setPosition(26.47f, 0.0f, 0.0f);
    gh.startAutoAttack(kBoar);
    CHECK(seen.errors.size() == 1);
    s.receive(wowee::network::Packet(wowee::game::wireOpcode(Opcode::SMSG_ATTACKSWING_NOTINRANGE)));
    CHECK(seen.errors.size() == 2);
    gh.updateAutoAttack(3.9f);
    CHECK(seen.errors.size() == 2);
    gh.updateAutoAttack(0.2f);
    CHECK(seen.errors.size() == 3);
    CHECK(seen.errors.back() == "You are too far away!");
}
