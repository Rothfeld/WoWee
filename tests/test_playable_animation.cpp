// The animation a model shows for one it lacks: its playable lookup first,
// AnimationData.dbc's Fallback chain past it.
//
//   WOWEE_TBC_DATA_DIR=/path/to/client/wowee-data/expansions/tbc build/bin/test_playable_animation
#include <catch_amalgamated.hpp>

#include "pipeline/dbc_loader.hpp"
#include "pipeline/m2_loader.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/playable_animation.hpp"
#include "tbc_data_test_util.hpp"

#include <algorithm>
#include <map>

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
