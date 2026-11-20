// TBC 2.4.3 Overpower / reactive ability / combo-point finisher prerequisite
// and usability parity tests.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"
#include "game/spell_prerequisites.hpp"
#include "game/tbc_cast_results.hpp"

#include <string>
#include <vector>

using replay::Session;
using replay::UpdateObject;
using wowee::game::ObjectType;
using wowee::game::Opcode;
using wowee::game::SpellPrerequisite;
using wowee::game::spellPrerequisite;
using wowee::game::spellUsability;
using wowee::game::wireOpcode;
using wowee::network::Packet;
using UF = wowee::game::UF;

namespace {

constexpr uint64_t kMobA = 0xF130000000001001ull;
constexpr uint64_t kMobB = 0xF130000000001002ull;

constexpr uint32_t kOverpowerR4 = 11585u;
constexpr uint32_t kRevengeR1   = 6572u;
constexpr uint32_t kVictoryRush = 34428u;
constexpr uint32_t kExecuteR1   = 5308u;
constexpr uint32_t kEviscerate  = 2098u;
constexpr uint32_t kRipR1       = 1079u;
constexpr uint32_t kPWShield    = 17u;

Packet buildComboPoints(uint64_t target, uint8_t points) {
    Packet p(wireOpcode(Opcode::SMSG_UPDATE_COMBO_POINTS));
    p.writePackedGuid(target);
    p.writeUInt8(points);
    return p;
}

Packet buildTbcCastFailed(uint32_t spellId, uint8_t tbcResult, uint8_t castCount) {
    Packet p(wireOpcode(Opcode::SMSG_CAST_FAILED));
    p.writeUInt32(spellId);
    p.writeUInt8(tbcResult);
    p.writeUInt8(castCount);
    return p;
}

void seedSpells(Session& s) {
    auto& cache = s.handler().spellNameCacheRef();
    // Exact TBC 2.4.3 Spell.dbc attributes for Overpower, reactive abilities, and finishers.
    cache[kOverpowerR4].name = "Overpower";
    cache[kOverpowerR4].attr = 0x00250010u;
    cache[kOverpowerR4].attrEx = 0x48100200u; // FINISHING_MOVE_DAMAGE | COMBO_ON_BLOCK

    cache[kRevengeR1].name = "Revenge";
    cache[kRevengeR1].attr = 0x00050010u;
    cache[kRevengeR1].attrEx = 0x08000200u;
    cache[kRevengeR1].casterAuraState = 1u; // AURA_STATE_DEFENSE

    cache[kVictoryRush].name = "Victory Rush";
    cache[kVictoryRush].attr = 0x00050010u;
    cache[kVictoryRush].attrEx = 0x08000200u;
    cache[kVictoryRush].casterAuraState = 10u; // AURA_STATE_WARRIOR_VICTORY_RUSH

    cache[kExecuteR1].name = "Execute";
    cache[kExecuteR1].attr = 0x00050110u;
    cache[kExecuteR1].attrEx = 0x08000200u;
    cache[kExecuteR1].targetAuraState = 2u; // AURA_STATE_HEALTHLESS_20_PERCENT

    cache[kEviscerate].name = "Eviscerate";
    cache[kEviscerate].attr = 0x00050010u;
    cache[kEviscerate].attrEx = 0x00100200u; // FINISHING_MOVE_DAMAGE only

    cache[kRipR1].name = "Rip";
    cache[kRipR1].attr = 0x00040010u;
    cache[kRipR1].attrEx = 0x00100200u; // FINISHING_MOVE_DAMAGE only

    cache[kPWShield].name = "Power Word: Shield";
    cache[kPWShield].attr = 0x00010000u;
    cache[kPWShield].targetAuraStateNot = 18u; // AURA_STATE_WEAKENED_SOUL

    s.handler().spellNameCacheLoadedRef() = true;
    for (uint32_t spellId : {kOverpowerR4, kRevengeR1, kVictoryRush, kExecuteR1, kEviscerate, kRipR1, kPWShield}) {
        Packet learned(wireOpcode(Opcode::SMSG_LEARNED_SPELL));
        learned.writeUInt32(spellId);
        s.receive(learned);
    }
}

void spawnCombatScene(Session& s) {
    seedSpells(s);
    s.receive(UpdateObject().tbc().create(Session::kPlayerGuid, ObjectType::PLAYER,
                                          {{UF::UNIT_FIELD_HEALTH, 2000},
                                           {UF::UNIT_FIELD_MAXHEALTH, 2000},
                                           {UF::UNIT_FIELD_LEVEL, 70}},
                                          true));
    s.receive(UpdateObject().tbc().create(kMobA, ObjectType::UNIT,
                                          {{UF::UNIT_FIELD_HEALTH, 1000},
                                           {UF::UNIT_FIELD_MAXHEALTH, 1000},
                                           {UF::UNIT_FIELD_LEVEL, 70}}));
    s.receive(UpdateObject().tbc().create(kMobB, ObjectType::UNIT,
                                          {{UF::UNIT_FIELD_HEALTH, 1000},
                                           {UF::UNIT_FIELD_MAXHEALTH, 1000},
                                           {UF::UNIT_FIELD_LEVEL, 70}}));
    s.handler().setTargetGuidRaw(kMobA);
}

} // namespace

TEST_CASE("TBC Overpower does not fail with NoComboPoints when inactive and lights up on target dodge",
          "[replay][tbc][overpower]") {
    Session s("tbc");
    spawnCombatScene(s);

    std::vector<std::string> events;
    s.handler().setAddonEventCallback([&](const std::string& ev, const std::vector<std::string>&) {
        events.push_back(ev);
    });

    // Before target dodges: greyed out on bar, but NOT blocked as NoComboPoints.
    CHECK_FALSE(spellUsability(s.handler(), kOverpowerR4).usable);
    CHECK(spellPrerequisite(s.handler(), kOverpowerR4, kMobA) == SpellPrerequisite::Ready);

    // Casting while inactive sends CMSG_CAST_SPELL so the server can reject with
    // SPELL_FAILED_CASTER_AURASTATE ("You can't do that yet").
    s.handler().castSpell(kOverpowerR4, kMobA);
    auto casts = s.sent().of(Opcode::CMSG_CAST_SPELL);
    REQUIRE(casts.size() == 1);

    // Server replies with TBC SPELL_FAILED_CASTER_AURASTATE (0x14 -> normalized 22).
    s.receive(buildTbcCastFailed(kOverpowerR4, 0x14, 1));
    CHECK(std::find(events.begin(), events.end(), "UNIT_SPELLCAST_FAILED") != events.end());

    // Target dodges -> server sends SMSG_UPDATE_COMBO_POINTS(kMobA, 1).
    events.clear();
    s.receive(buildComboPoints(kMobA, 1));
    CHECK(std::find(events.begin(), events.end(), "SPELL_UPDATE_USABLE") != events.end());
    CHECK(spellUsability(s.handler(), kOverpowerR4).usable);

    // Switching to another target that did not dodge greys Overpower out again.
    s.handler().setTargetGuidRaw(kMobB);
    CHECK_FALSE(spellUsability(s.handler(), kOverpowerR4).usable);

    // Switching back to the dodged target restores usability; clearing expires it.
    s.handler().setTargetGuidRaw(kMobA);
    CHECK(spellUsability(s.handler(), kOverpowerR4).usable);
    s.receive(buildComboPoints(kMobA, 0));
    CHECK_FALSE(spellUsability(s.handler(), kOverpowerR4).usable);
}

TEST_CASE("TBC reactive abilities track caster and target aura states",
          "[replay][tbc][overpower]") {
    Session s("tbc");
    spawnCombatScene(s);

    // Inactive initially; none trigger NoComboPoints.
    CHECK_FALSE(spellUsability(s.handler(), kRevengeR1).usable);
    CHECK_FALSE(spellUsability(s.handler(), kVictoryRush).usable);
    CHECK_FALSE(spellUsability(s.handler(), kExecuteR1).usable);
    CHECK(spellUsability(s.handler(), kPWShield).usable);
    CHECK(spellPrerequisite(s.handler(), kRevengeR1, kMobA) == SpellPrerequisite::Ready);
    CHECK(spellPrerequisite(s.handler(), kVictoryRush, kMobA) == SpellPrerequisite::Ready);
    CHECK(spellPrerequisite(s.handler(), kExecuteR1, kMobA) == SpellPrerequisite::Ready);

    // Set caster AURA_STATE_DEFENSE (bit 0) and AURA_STATE_WARRIOR_VICTORY_RUSH (bit 9).
    s.receive(UpdateObject().tbc().values(Session::kPlayerGuid,
                                          {{UF::UNIT_FIELD_AURASTATE, (1u << 0) | (1u << 9)}}));
    CHECK(spellUsability(s.handler(), kRevengeR1).usable);
    CHECK(spellUsability(s.handler(), kVictoryRush).usable);

    // Set target AURA_STATE_HEALTHLESS_20_PERCENT (bit 1) and AURA_STATE_WEAKENED_SOUL (bit 17).
    s.receive(UpdateObject().tbc().values(kMobA,
                                          {{UF::UNIT_FIELD_AURASTATE, (1u << 1) | (1u << 17)}}));
    CHECK(spellUsability(s.handler(), kExecuteR1).usable);
    CHECK_FALSE(spellUsability(s.handler(), kPWShield).usable);
}

TEST_CASE("TBC Rogue Eviscerate and Druid Rip require combo points on target",
          "[replay][tbc][overpower]") {
    Session s("tbc");
    spawnCombatScene(s);

    // 0 combo points: unusable and blocked client-side as NoComboPoints.
    CHECK_FALSE(spellUsability(s.handler(), kEviscerate).usable);
    CHECK_FALSE(spellUsability(s.handler(), kRipR1).usable);
    CHECK(spellPrerequisite(s.handler(), kEviscerate, kMobA) == SpellPrerequisite::NoComboPoints);
    CHECK(spellPrerequisite(s.handler(), kRipR1, kMobA) == SpellPrerequisite::NoComboPoints);

    s.handler().castSpell(kEviscerate, kMobA);
    s.handler().castSpell(kRipR1, kMobA);
    CHECK(s.sent().of(Opcode::CMSG_CAST_SPELL).empty());

    // 1+ combo point on kMobA: usable and Ready on kMobA, still blocked on kMobB.
    s.receive(buildComboPoints(kMobA, 2));
    CHECK(spellUsability(s.handler(), kEviscerate).usable);
    CHECK(spellUsability(s.handler(), kRipR1).usable);
    CHECK(spellPrerequisite(s.handler(), kEviscerate, kMobA) == SpellPrerequisite::Ready);
    CHECK(spellPrerequisite(s.handler(), kRipR1, kMobA) == SpellPrerequisite::Ready);
    CHECK(spellPrerequisite(s.handler(), kEviscerate, kMobB) == SpellPrerequisite::NoComboPoints);

    s.handler().castSpell(kEviscerate, kMobA);
    CHECK(s.sent().of(Opcode::CMSG_CAST_SPELL).size() == 1);
}
