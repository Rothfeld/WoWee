// The animation a model shows for one it lacks: its playable lookup first,
// AnimationData.dbc's Fallback chain past it.
//
//   WOWEE_TBC_DATA_DIR=/path/to/client/wowee-data/expansions/tbc build/bin/test_playable_animation
#include <catch_amalgamated.hpp>

#include "pipeline/dbc_loader.hpp"
#include "pipeline/m2_loader.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/melee_swing_queue.hpp"
#include "rendering/animation/playable_animation.hpp"
#include "rendering/animation/sequence_variation.hpp"
#include "tbc_data_test_util.hpp"

#include <algorithm>
#include <map>
#include <set>

using namespace wowee;
namespace A = rendering::anim;

namespace {

// AnimationData.dbc, 2.4.3 layout: ID, Name, WeaponFlags, BodyFlags, unknown, Flags, Fallback.
struct AnimationData {
    std::map<uint32_t, uint32_t> fallback, flags;
};

std::optional<rendering::PlayableStandIn> standIn(const pipeline::M2Model& model, uint32_t id,
                                                  const AnimationData& data) {
    return rendering::resolvePlayableStandIn(
        id, model.playableAnimations,
        [&](uint32_t want) {
            return std::any_of(model.sequences.begin(), model.sequences.end(),
                               [want](const auto& sequence) { return sequence.id == want; });
        },
        [&](uint32_t want) -> std::optional<uint32_t> {
            const auto it = data.fallback.find(want);
            if (it == data.fallback.end()) return std::nullopt;
            return it->second;
        },
        [&](uint32_t want) { return data.flags.count(want) ? data.flags.at(want) : 0u; });
}

AnimationData loadAnimationData() {
    const auto raw = test::readTbcData("dbfilesclient/animationdata.dbc");
    pipeline::DBCFile dbc;
    REQUIRE(dbc.load(raw));
    AnimationData data;
    for (uint32_t row = 0; row < dbc.getRecordCount(); ++row) {
        data.flags[dbc.getUInt32(row, 0)] = dbc.getUInt32(row, 5);
        data.fallback[dbc.getUInt32(row, 0)] = dbc.getUInt32(row, 6);
    }
    return data;
}

pipeline::M2Model withSequences(std::initializer_list<uint32_t> ids) {
    pipeline::M2Model model;
    for (const uint32_t id : ids) {
        pipeline::M2Sequence sequence{};
        sequence.id = id;
        sequence.duration = 1000;
        model.sequences.push_back(sequence);
    }
    return model;
}

}  // namespace

TEST_CASE("The playable lookup decides where it disagrees with AnimationData", "[animation][assets]") {
    const auto data = loadAnimationData();
    // NetherDragon.m2 has ReadyUnarmed but no StandWound or parry. The DBC
    // chain ParryUnarmed -> StandWound -> ReadyUnarmed; its lookup and native
    // 8606 (`.npc playemote 39` on JAB Nether Serpent 21329): Stand.
    const auto serpent = pipeline::M2Loader::load(test::readTbcData("creature/netherdragon/netherdragon.m2"));
    REQUIRE(serpent.playableAnimations.size() == 203);
    CHECK(data.fallback.at(A::PARRY_UNARMED) == A::STAND_WOUND);
    CHECK(data.fallback.at(A::STAND_WOUND) == A::READY_UNARMED);
    for (const uint32_t id : {A::PARRY_UNARMED, A::SHIELD_BLOCK, A::DODGE, A::STAND_WOUND}) {
        const auto shown = standIn(serpent, id, data);
        REQUIRE(shown);
        CHECK(shown->animationId == A::STAND);
        CHECK_FALSE(shown->reversed);
        CHECK_FALSE(shown->frozenAtEnd);
    }
    // Wendigo.m2 (Ice Thistle Yeti) has no ready stance at all.
    const auto yeti = pipeline::M2Loader::load(test::readTbcData("creature/wendigo/wendigo.m2"));
    CHECK(standIn(yeti, A::READY_UNARMED, data)->animationId == A::STAND);
    CHECK(standIn(yeti, A::READY_2H, data)->animationId == A::STAND);
    CHECK(standIn(yeti, A::ATTACK_2H, data)->animationId == A::ATTACK_1H);
}

TEST_CASE("Playable lookup flags: 3 frozen on the last frame, 1 backwards", "[animation][assets]") {
    const auto data = loadAnimationData();
    const auto human = pipeline::M2Loader::load(test::readTbcData("character/human/male/humanmale.m2"));
    const auto dead = standIn(human, A::DEAD, data);
    REQUIRE(dead);
    CHECK(dead->animationId == A::DEATH);
    CHECK(dead->frozenAtEnd);
    const auto lootHold = standIn(human, A::LOOT_HOLD, data);
    REQUIRE(lootHold);
    CHECK(lootHold->animationId == A::LOOT);
    CHECK(lootHold->frozenAtEnd);
    const auto lootUp = standIn(human, A::LOOT_UP, data);
    REQUIRE(lootUp);
    CHECK(lootUp->animationId == A::LOOT);
    CHECK(lootUp->reversed);
    CHECK_FALSE(lootUp->frozenAtEnd);
    const auto gnoll = pipeline::M2Loader::load(test::readTbcData("creature/gnoll/gnoll.m2"));
    const auto backwards = standIn(gnoll, A::WALK_BACKWARDS, data);
    REQUIRE(backwards);
    CHECK(backwards->animationId == A::WALK);
    CHECK(backwards->reversed);
}

TEST_CASE("Without a lookup entry the AnimationData chain and flags decide", "[animation][assets]") {
    const auto data = loadAnimationData();
    // Wrath-style model: no playable lookup.
    const auto model = withSequences({A::STAND, A::DEATH, A::WALK, A::READY_UNARMED});
    const auto dead = standIn(model, A::DEAD, data);
    REQUIRE(dead);
    CHECK(dead->animationId == A::DEATH);
    CHECK(dead->frozenAtEnd);
    const auto backwards = standIn(model, A::WALK_BACKWARDS, data);
    REQUIRE(backwards);
    CHECK(backwards->animationId == A::WALK);
    CHECK(backwards->reversed);
    // SwimBackwards -> Swim -> Walk: backwards only on the direct Fallback.
    const auto swimBack = standIn(model, A::SWIM_BACKWARDS, data);
    REQUIRE(swimBack);
    CHECK(swimBack->animationId == A::WALK);
    CHECK_FALSE(swimBack->reversed);
    CHECK(standIn(model, A::READY_1H, data)->animationId == A::READY_UNARMED);
    // A 2.4.3 lookup has 203 entries; StealthRun (223) is past it.
    auto tbcStyle = model;
    tbcStyle.playableAnimations.resize(203);
    for (uint16_t id = 0; id < 203; ++id) tbcStyle.playableAnimations[id] = {id, 0};
    CHECK(standIn(tbcStyle, 223, data)->animationId == A::WALK);  // StealthRun -> StealthWalk -> Walk
}

TEST_CASE("A lookup naming nothing the model has gives no stand-in", "[animation]") {
    // Be_Broom01.m2 maps everything to Closed (147), which it lacks.
    auto model = withSequences({A::STAND});
    model.playableAnimations.assign(203, {147, 0});
    const AnimationData none;
    CHECK_FALSE(standIn(model, A::READY_UNARMED, none));
}

TEST_CASE("DragonAzurgoz (Madrigosa) lacks DEAD (6) and holds the final frame of DEATH (1)", "[animation][assets]") {
    const auto data = loadAnimationData();
    const auto dragon = pipeline::M2Loader::load(test::readTbcData("creature/dragon/dragonazurgoz.m2"));
    const auto hasSeq = [&](uint32_t id) {
        return std::any_of(dragon.sequences.begin(), dragon.sequences.end(),
                           [id](const auto& s) { return s.id == id; });
    };
    REQUIRE(hasSeq(A::DEATH));
    REQUIRE_FALSE(hasSeq(A::DEAD));
    REQUIRE_FALSE(hasSeq(A::STUN));

    const auto dead = standIn(dragon, A::DEAD, data);
    REQUIRE(dead);
    CHECK(dead->animationId == A::DEATH);
    CHECK(dead->frozenAtEnd);
    CHECK_FALSE(dead->reversed);

    // Regression: STUN (14) on DragonAzurgoz falls back to STAND (0), so a
    // feign-dead dragon carrying UNIT_FLAG_STUNNED must never enter STUN.
    const auto stun = standIn(dragon, A::STUN, data);
    REQUIRE(stun);
    CHECK(stun->animationId == A::STAND);
}

TEST_CASE("Brutallus.m2 AttackUnarmedOff (117) stands in as AttackOff (87) and draws both M2 variations",
          "[animation][assets][brutallus]") {
    const auto data = loadAnimationData();
    const auto brutallus = pipeline::M2Loader::load(test::readTbcData("creature/brutallus/brutallus.m2"));
    const auto offStandIn = standIn(brutallus, A::ATTACK_UNARMED_OFF, data);
    REQUIRE(offStandIn);
    CHECK(offStandIn->animationId == A::ATTACK_OFF);
    CHECK_FALSE(offStandIn->reversed);
    CHECK_FALSE(offStandIn->frozenAtEnd);
    CHECK(rendering::standInAllowsVariationDraw(*offStandIn));

    std::mt19937 rng(42);
    std::set<int> drawnIndices;
    for (int i = 0; i < 64; ++i) {
        const auto v = rendering::drawSequenceVariation(brutallus.sequences, offStandIn->animationId, rng);
        REQUIRE(v);
        drawnIndices.insert(v->index);
    }
    // Brutallus.m2 authors two AttackOff (87) sequences (indices 12 and 15, frequency 16383 each).
    CHECK(drawnIndices == std::set<int>{12, 15});

    // Meanwhile Wendigo.m2 ReadyUnarmed -> Stand, reversed Walk, and frozen Death do not roll variations.
    const auto yeti = pipeline::M2Loader::load(test::readTbcData("creature/wendigo/wendigo.m2"));
    const auto yetiReady = standIn(yeti, A::READY_UNARMED, data);
    REQUIRE(yetiReady);
    CHECK(yetiReady->animationId == A::STAND);
    CHECK_FALSE(rendering::standInAllowsVariationDraw(*yetiReady));
    CHECK_FALSE(rendering::standInAllowsVariationDraw({A::WALK, true, false}));
    CHECK_FALSE(rendering::standInAllowsVariationDraw({A::DEATH, false, true}));
}

TEST_CASE("Krakken.m2 DEATH (1) transitions to authored looping DEAD (6) on completion",
          "[animation][assets][lurker]") {
    const auto krakken = pipeline::M2Loader::load(test::readTbcData("creature/krakken/krakken.m2"));
    CHECK(rendering::deathTransitionsToAuthoredDeadLoop(A::DEATH, krakken.sequences));
    CHECK_FALSE(rendering::deathTransitionsToAuthoredDeadLoop(A::STAND, krakken.sequences));
    CHECK_FALSE(rendering::deathTransitionsToAuthoredDeadLoop(A::DEAD, krakken.sequences));

    // Models without an authored DEAD (6) sequence hold the final frame of DEATH.
    const auto dragon = pipeline::M2Loader::load(test::readTbcData("creature/dragon/dragonazurgoz.m2"));
    CHECK_FALSE(rendering::deathTransitionsToAuthoredDeadLoop(A::DEATH, dragon.sequences));
    const auto human = pipeline::M2Loader::load(test::readTbcData("character/human/male/humanmale.m2"));
    CHECK_FALSE(rendering::deathTransitionsToAuthoredDeadLoop(A::DEATH, human.sequences));

    // Zero-duration DEAD sequence must not trigger a loop transition.
    pipeline::M2Model zeroDead = withSequences({A::DEATH});
    pipeline::M2Sequence emptyDead{};
    emptyDead.id = A::DEAD;
    emptyDead.duration = 0;
    zeroDead.sequences.push_back(emptyDead);
    CHECK_FALSE(rendering::deathTransitionsToAuthoredDeadLoop(A::DEATH, zeroDead.sequences));
}

TEST_CASE("EyeStalkOfKathune.m2 ChannelCastDirected (124) collapses to STAND (0) while TitanFemale SpellCastDirected (53) resolves to SpellCast (32)",
          "[animation][assets][eyestalk]") {
    const auto data = loadAnimationData();
    auto resolvesPastStand = [&](const pipeline::M2Model& model, uint32_t animId) {
        return rendering::animationResolvesPastStand(
            animId, model.playableAnimations,
            [&](uint32_t id) {
                return std::any_of(model.sequences.begin(), model.sequences.end(),
                                   [id](const auto& s) { return s.id == id && s.duration > 0; });
            },
            [&](uint32_t id) -> std::optional<uint32_t> {
                const auto it = data.fallback.find(id);
                if (it == data.fallback.end()) return std::nullopt;
                return it->second;
            },
            [&](uint32_t id) -> uint32_t {
                const auto it = data.flags.find(id);
                return it == data.flags.end() ? 0u : it->second;
            });
    };

    const auto eyeStalk = pipeline::M2Loader::load(test::readTbcData("creature/eyestalkofkathune/eyestalkofkathune.m2"));
    // ChannelCastDirected (124) is absent and falls back to STAND (0); do not
    // override ReadyUnarmed (25) combat sway with static Stand (0).
    CHECK_FALSE(resolvesPastStand(eyeStalk, A::CHANNEL_CAST_DIRECTED));
    CHECK(resolvesPastStand(eyeStalk, A::READY_UNARMED));
    CHECK(resolvesPastStand(eyeStalk, A::STAND));

    // TitanFemale lacks SpellCastDirected (53) but resolves it to SpellCast (32), not STAND (0).
    const auto titanFemale = pipeline::M2Loader::load(test::readTbcData("creature/titanfemale/titanfemale.m2"));
    CHECK(resolvesPastStand(titanFemale, A::SPELL_CAST_DIRECTED));
}

TEST_CASE("SalamanderMale.m2 (Majordomo Executus) ShuffleLeft/ShuffleRight/EmoteTalk stand-ins draw all three Stand variations including talking sequence 4",
          "[animation][assets][majordomo]") {
    const auto data = loadAnimationData();
    const auto salamander = pipeline::M2Loader::load(test::readTbcData("creature/salamander/salamandermale.m2"));

    const auto hasSeq = [&](const pipeline::M2Model& m, uint32_t id) {
        return std::any_of(m.sequences.begin(), m.sequences.end(),
                           [id](const auto& s) { return s.id == id && s.duration > 0; });
    };
    const auto resolvesPlayable = [&](const pipeline::M2Model& m, uint32_t id) {
        return rendering::animationResolvesToPlayable(
            id, m.playableAnimations,
            [&](uint32_t want) { return hasSeq(m, want); },
            [&](uint32_t want) -> std::optional<uint32_t> {
                const auto it = data.fallback.find(want);
                return it == data.fallback.end() ? std::nullopt : std::optional<uint32_t>(it->second);
            },
            [&](uint32_t want) -> uint32_t {
                const auto it = data.flags.find(want);
                return it == data.flags.end() ? 0u : it->second;
            });
    };

    REQUIRE_FALSE(hasSeq(salamander, A::SHUFFLE_LEFT));
    REQUIRE_FALSE(hasSeq(salamander, A::SHUFFLE_RIGHT));
    REQUIRE_FALSE(hasSeq(salamander, A::EMOTE_TALK));
    CHECK(resolvesPlayable(salamander, A::SHUFFLE_LEFT));
    CHECK(resolvesPlayable(salamander, A::SHUFFLE_RIGHT));
    CHECK(resolvesPlayable(salamander, A::EMOTE_TALK));

    for (const uint32_t req : {A::SHUFFLE_LEFT, A::SHUFFLE_RIGHT, A::EMOTE_TALK}) {
        const auto shown = standIn(salamander, req, data);
        REQUIRE(shown);
        CHECK(shown->animationId == A::STAND);
        CHECK_FALSE(shown->reversed);
        CHECK_FALSE(shown->frozenAtEnd);
        // One-shot conversation turn shuffle / talk emote rolls weighted Stand variations.
        CHECK(rendering::standInAllowsVariationDraw(*shown, req, /*loop=*/false));
        // Looping shuffle keeps primary Stand without rolling fidgets.
        CHECK_FALSE(rendering::standInAllowsVariationDraw(*shown, req, /*loop=*/true));
    }

    std::mt19937 rng(12018);
    std::set<int> drawnIndices;
    const auto shuffleStandIn = standIn(salamander, A::SHUFFLE_RIGHT, data);
    REQUIRE(shuffleStandIn);
    for (int i = 0; i < 64; ++i) {
        const auto v = rendering::drawSequenceVariation(salamander.sequences, shuffleStandIn->animationId, rng);
        REQUIRE(v);
        drawnIndices.insert(v->index);
    }
    // SalamanderMale.m2 authors three equal-weight (10922) Stand sequences: 2 (4s), 3 (4s), and 4 (3s jaw/arm talking).
    CHECK(drawnIndices == std::set<int>{2, 3, 4});

    // Combat one-shots and ready loops that fall back to Stand must still hold primary Stand.
    const auto serpent = pipeline::M2Loader::load(test::readTbcData("creature/netherdragon/netherdragon.m2"));
    const auto parryStandIn = standIn(serpent, A::PARRY_UNARMED, data);
    REQUIRE(parryStandIn);
    CHECK(parryStandIn->animationId == A::STAND);
    CHECK_FALSE(rendering::standInAllowsVariationDraw(*parryStandIn, A::PARRY_UNARMED, /*loop=*/false));
    CHECK_FALSE(rendering::standInAllowsVariationDraw(*parryStandIn, A::READY_UNARMED, /*loop=*/true));

    // Doodad models mapping ShuffleLeft to an absent target (147 Closed) are not playable.
    auto broom = withSequences({A::STAND});
    broom.playableAnimations.assign(203, {147, 0});
    CHECK_FALSE(resolvesPlayable(broom, A::SHUFFLE_LEFT));
}
