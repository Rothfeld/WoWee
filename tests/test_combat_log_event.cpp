// The arguments Blizzard_CombatLog is handed for a combat event.
//
// That addon indexes them by position and does arithmetic on some: SWING_DAMAGE
// subtracts the overkill from the amount, so a layout that is merely close is
// not a wrong line in the log, it is an error that takes the handler down and
// leaves the window empty. These cases pin the positions.
#include <catch_amalgamated.hpp>
#include "game/combat_log_event.hpp"

using namespace wowee::game;
using T = CombatTextEntry;

namespace {
constexpr uint64_t kPlayer = 0x1;
constexpr uint64_t kCreature = 0xF130000000000002ULL;

CombatLogEventInput hit(T::Type type, uint32_t spell = 0) {
    CombatLogEventInput in;
    in.type = type;
    in.amount = 42;
    in.spellId = spell;
    in.srcGuid = kPlayer;
    in.dstGuid = kCreature;
    in.playerGuid = kPlayer;
    in.srcName = "Alpha";
    in.dstName = "Orc";
    in.spellName = spell ? "Fireball" : "";
    in.timestamp = 1700000000;
    return in;
}
}  // namespace

TEST_CASE("a swing carries the source, the destination and a full damage suffix", "[combatlog]") {
    const auto a = buildCombatLogEvent(hit(T::MELEE_DAMAGE));
    REQUIRE(a.size() == 17);
    CHECK(a[0] == "1700000000");
    CHECK(a[1] == "SWING_DAMAGE");
    CHECK(a[2] == "0x0000000000000001");
    CHECK(a[3] == "Alpha");
    CHECK(a[5] == "0xF130000000000002");
    CHECK(a[6] == "Orc");
    CHECK(a[8] == "42");   // amount
    CHECK(a[9] == "0");    // overkill, which the parser subtracts
}

TEST_CASE("a spell hit puts its prefix before the damage", "[combatlog]") {
    const auto a = buildCombatLogEvent(hit(T::SPELL_DAMAGE, 133));
    REQUIRE(a.size() == 20);
    CHECK(a[1] == "SPELL_DAMAGE");
    CHECK(a[8] == "133");
    CHECK(a[9] == "Fireball");
    CHECK(a[11] == "42");
}

TEST_CASE("a crit is flagged and an ordinary hit is not", "[combatlog]") {
    CHECK(buildCombatLogEvent(hit(T::CRIT_DAMAGE, 133))[17] == "1");
    CHECK(buildCombatLogEvent(hit(T::SPELL_DAMAGE, 133))[17] == kEventNil);
}

TEST_CASE("heals, periodic ticks and energize are the events the parser knows", "[combatlog]") {
    CHECK(buildCombatLogEvent(hit(T::HEAL, 1))[1] == "SPELL_HEAL");
    CHECK(buildCombatLogEvent(hit(T::PERIODIC_HEAL, 1))[1] == "SPELL_PERIODIC_HEAL");
    CHECK(buildCombatLogEvent(hit(T::PERIODIC_DAMAGE, 1))[1] == "SPELL_PERIODIC_DAMAGE");
    auto energize = hit(T::ENERGIZE, 1);
    energize.powerType = 3;
    const auto a = buildCombatLogEvent(energize);
    CHECK(a[1] == "SPELL_ENERGIZE");
    CHECK(a[11] == "42");   // amount
    CHECK(a[12] == "3");    // power type
}

TEST_CASE("a miss names how it missed, and a block says how much", "[combatlog]") {
    const auto dodge = buildCombatLogEvent(hit(T::DODGE));
    CHECK(dodge[1] == "SWING_MISSED");
    CHECK(dodge[8] == "DODGE");
    const auto block = buildCombatLogEvent(hit(T::BLOCK, 1));
    CHECK(block[1] == "SPELL_MISSED");
    CHECK(block.back() == "42");
}

TEST_CASE("the part of a hit that did not land is not a line of its own", "[combatlog]") {
    CHECK(buildCombatLogEvent(hit(T::ABSORB, 1)).empty());
    CHECK(buildCombatLogEvent(hit(T::RESIST, 1)).empty());
    auto whole = hit(T::RESIST, 1);
    whole.amount = 0;
    CHECK(buildCombatLogEvent(whole)[1] == "SPELL_MISSED");
}

TEST_CASE("experience and honour have no combat log line", "[combatlog]") {
    CHECK(buildCombatLogEvent(hit(T::XP_GAIN)).empty());
    CHECK(buildCombatLogEvent(hit(T::HONOR_GAIN)).empty());
}

TEST_CASE("environmental damage has no source and names its kind", "[combatlog]") {
    auto in = hit(T::ENVIRONMENTAL);
    in.srcGuid = 0;
    in.srcName.clear();
    in.powerType = 2;
    const auto a = buildCombatLogEvent(in);
    CHECK(a[1] == "ENVIRONMENTAL_DAMAGE");
    CHECK(a[2] == "0x0000000000000000");
    CHECK(a[3] == kEventNil);
    CHECK(a[8] == "Falling");
}

TEST_CASE("the player's own events are flagged as theirs", "[combatlog]") {
    const auto a = buildCombatLogEvent(hit(T::MELEE_DAMAGE));
    CHECK(a[4] == std::to_string(combat_log::kMine | combat_log::kFriendly |
                                 combat_log::kControlPlayer | combat_log::kTypePlayer));
    CHECK(a[7] == std::to_string(combat_log::kOutsider | combat_log::kHostile |
                                 combat_log::kControlNpc | combat_log::kTypeNpc));
}

TEST_CASE("TBC damage has no overkill and its heal no overhealing", "[combatlog]") {
    auto swing = hit(T::MELEE_DAMAGE);
    swing.layout = CombatLogLayout::Tbc;
    const auto a = buildCombatLogEvent(swing);
    REQUIRE(a.size() == 16);
    CHECK(a[8] == "42");   // amount
    CHECK(a[9] == "1");    // school, where 3.3.5 has the overkill

    auto heal = hit(T::CRIT_HEAL, 1);
    heal.layout = CombatLogLayout::Tbc;
    const auto h = buildCombatLogEvent(heal);
    REQUIRE(h.size() == 13);
    CHECK(h[11] == "42");
    CHECK(h[12] == "1");   // critical straight after the amount
}

TEST_CASE("TBC misses carry no amount", "[combatlog]") {
    auto block = hit(T::BLOCK, 1);
    block.layout = CombatLogLayout::Tbc;
    CHECK(buildCombatLogEvent(block).back() == "BLOCK");
}

TEST_CASE("normal ranged weapon attacks emit RANGE_DAMAGE and RANGE_MISSED", "[combatlog]") {
    auto shot = hit(T::SPELL_DAMAGE, 75);
    shot.spellName = "Auto Shot";
    shot.normalRangedAttack = true;
    const auto dmg = buildCombatLogEvent(shot);
    REQUIRE(dmg.size() == 20);
    CHECK(dmg[1] == "RANGE_DAMAGE");
    CHECK(dmg[8] == "75");

    auto missed = hit(T::MISS, 75);
    missed.spellName = "Auto Shot";
    missed.normalRangedAttack = true;
    const auto m = buildCombatLogEvent(missed);
    CHECK(m[1] == "RANGE_MISSED");
    CHECK(m.back() == "MISS");
}

TEST_CASE("spell, periodic, heal, interrupt and environmental events emit real school masks", "[combatlog]") {
    // TBC Shadow Word: Pain (589, Shadow = 32) periodic tick
    auto swp = hit(T::PERIODIC_DAMAGE, 589);
    swp.layout = CombatLogLayout::Tbc;
    swp.spellName = "Shadow Word: Pain";
    swp.spellSchoolMask = 32;
    swp.damageSchoolMask = 32;
    const auto swpArgs = buildCombatLogEvent(swp);
    REQUIRE(swpArgs.size() == 19);
    CHECK(swpArgs[1] == "SPELL_PERIODIC_DAMAGE");
    CHECK(swpArgs[8] == "589");
    CHECK(swpArgs[10] == "32");  // prefix spellSchool
    CHECK(swpArgs[12] == "32");  // suffix damageSchool

    // TBC Immolate (348, Fire = 4) periodic tick when wire damage school overrides or matches
    auto imm = hit(T::PERIODIC_DAMAGE, 348);
    imm.layout = CombatLogLayout::Tbc;
    imm.spellName = "Immolate";
    imm.spellSchoolMask = 4;
    imm.damageSchoolMask = 4;
    const auto immArgs = buildCombatLogEvent(imm);
    REQUIRE(immArgs.size() == 19);
    CHECK(immArgs[10] == "4");
    CHECK(immArgs[12] == "4");

    // TBC Renew (139, Holy = 2) periodic heal
    auto renew = hit(T::PERIODIC_HEAL, 139);
    renew.layout = CombatLogLayout::Tbc;
    renew.spellName = "Renew";
    renew.spellSchoolMask = 2;
    const auto renewArgs = buildCombatLogEvent(renew);
    REQUIRE(renewArgs.size() == 13);
    CHECK(renewArgs[1] == "SPELL_PERIODIC_HEAL");
    CHECK(renewArgs[10] == "2");

    // SPELL_INTERRUPT carries both the interrupt's school and the interrupted spell's school
    auto intr = hit(T::INTERRUPT, 116); // Frostbolt (Frost = 16) interrupted by Counterspell (2139, Arcane = 64)
    intr.layout = CombatLogLayout::Tbc;
    intr.spellName = "Frostbolt";
    intr.spellSchoolMask = 16;
    intr.extraSpellId = 2139;
    intr.extraSpellName = "Counterspell";
    intr.extraSpellSchoolMask = 64;
    const auto intrArgs = buildCombatLogEvent(intr);
    REQUIRE(intrArgs.size() == 14);
    CHECK(intrArgs[1] == "SPELL_INTERRUPT");
    CHECK(intrArgs[8] == "2139");
    CHECK(intrArgs[10] == "64");
    CHECK(intrArgs[11] == "116");
    CHECK(intrArgs[13] == "16");

    // ENVIRONMENTAL_DAMAGE Lava (type 3) defaults to Fire (4), Falling (type 2) to Physical (1)
    auto lava = hit(T::ENVIRONMENTAL);
    lava.layout = CombatLogLayout::Tbc;
    lava.srcGuid = 0;
    lava.srcName.clear();
    lava.powerType = 3;
    const auto lavaArgs = buildCombatLogEvent(lava);
    REQUIRE(lavaArgs.size() == 17);
    CHECK(lavaArgs[8] == "Lava");
    CHECK(lavaArgs[10] == "4");
}

TEST_CASE("aura, party kill, and unit death subevents match TBC 2.4.3 Blizzard_CombatLog positions", "[combatlog]") {
    constexpr std::string_view kObjectNone = "2147483648"; // 0x80000000u (COMBATLOG_OBJECT_NONE)

    CombatLogEventInput buff;
    buff.layout = CombatLogLayout::Tbc;
    buff.subevent = CombatLogSubevent::AuraApplied;
    buff.spellId = 1243;
    buff.spellName = "Power Word: Fortitude";
    buff.spellSchoolMask = 2;
    buff.powerType = 0; // BUFF
    buff.srcGuid = 0;
    buff.dstGuid = kPlayer;
    buff.playerGuid = kPlayer;
    buff.dstName = "Alpha";
    buff.timestamp = 1700000000;
    const auto buffArgs = buildCombatLogEvent(buff);
    REQUIRE(buffArgs.size() == 12);
    CHECK(buffArgs[1] == "SPELL_AURA_APPLIED");
    CHECK(buffArgs[2] == "0x0000000000000000");
    CHECK(buffArgs[3] == kEventNil);
    CHECK(buffArgs[4] == kObjectNone);
    CHECK(buffArgs[5] == "0x0000000000000001");
    CHECK(buffArgs[6] == "Alpha");
    CHECK(buffArgs[8] == "1243");
    CHECK(buffArgs[9] == "Power Word: Fortitude");
    CHECK(buffArgs[10] == "2");
    CHECK(buffArgs[11] == "BUFF");

    auto dose = buff;
    dose.subevent = CombatLogSubevent::AuraAppliedDose;
    dose.spellId = 7386;
    dose.spellName = "Sunder Armor";
    dose.spellSchoolMask = 1;
    dose.powerType = 1; // DEBUFF
    dose.amount = 2;
    const auto doseArgs = buildCombatLogEvent(dose);
    REQUIRE(doseArgs.size() == 13);
    CHECK(doseArgs[1] == "SPELL_AURA_APPLIED_DOSE");
    CHECK(doseArgs[8] == "7386");
    CHECK(doseArgs[10] == "1");
    CHECK(doseArgs[11] == "DEBUFF");
    CHECK(doseArgs[12] == "2");

    auto refresh = dose;
    refresh.subevent = CombatLogSubevent::AuraRefresh;
    const auto refreshArgs = buildCombatLogEvent(refresh);
    REQUIRE(refreshArgs.size() == 12);
    CHECK(refreshArgs[1] == "SPELL_AURA_REFRESH");
    CHECK(refreshArgs[11] == "DEBUFF");

    auto removed = dose;
    removed.subevent = CombatLogSubevent::AuraRemoved;
    const auto removedArgs = buildCombatLogEvent(removed);
    REQUIRE(removedArgs.size() == 12);
    CHECK(removedArgs[1] == "SPELL_AURA_REMOVED");
    CHECK(removedArgs[11] == "DEBUFF");

    CombatLogEventInput pk;
    pk.layout = CombatLogLayout::Tbc;
    pk.subevent = CombatLogSubevent::PartyKill;
    pk.srcGuid = kPlayer;
    pk.srcName = "Alpha";
    pk.dstGuid = kCreature;
    pk.dstName = "Stonetusk Boar";
    pk.playerGuid = kPlayer;
    pk.timestamp = 1700000000;
    const auto pkArgs = buildCombatLogEvent(pk);
    REQUIRE(pkArgs.size() == 8);
    CHECK(pkArgs[1] == "PARTY_KILL");
    CHECK(pkArgs[2] == "0x0000000000000001");
    CHECK(pkArgs[3] == "Alpha");
    CHECK(pkArgs[5] == "0xF130000000000002");
    CHECK(pkArgs[6] == "Stonetusk Boar");

    CombatLogEventInput died;
    died.layout = CombatLogLayout::Tbc;
    died.subevent = CombatLogSubevent::UnitDied;
    died.srcGuid = kPlayer; // Even if populated, UNIT_DIED forces nil/COMBATLOG_OBJECT_NONE source
    died.dstGuid = kCreature;
    died.dstName = "Stonetusk Boar";
    died.playerGuid = kPlayer;
    died.timestamp = 1700000000;
    const auto diedArgs = buildCombatLogEvent(died);
    REQUIRE(diedArgs.size() == 8);
    CHECK(diedArgs[1] == "UNIT_DIED");
    CHECK(diedArgs[2] == "0x0000000000000000");
    CHECK(diedArgs[3] == kEventNil);
    CHECK(diedArgs[4] == kObjectNone);
    CHECK(diedArgs[5] == "0xF130000000000002");
    CHECK(diedArgs[6] == "Stonetusk Boar");

    auto destroyed = died;
    destroyed.subevent = CombatLogSubevent::UnitDestroyed;
    const auto destroyedArgs = buildCombatLogEvent(destroyed);
    REQUIRE(destroyedArgs.size() == 8);
    CHECK(destroyedArgs[1] == "UNIT_DESTROYED");
    CHECK(destroyedArgs[4] == kObjectNone);
}
