// The emitter-only model and pose rules CharacterRenderer uses to run a
// character's authored emitters through M2Renderer (character_emitters.hpp).
#include <catch_amalgamated.hpp>

#include "rendering/character_emitters.hpp"

using namespace wowee;

namespace {
// Shaped like creature/wisp/wisp.m2: emitters on bones 30-41, type-0 textures.
pipeline::M2Model wispLike() {
    pipeline::M2Model m;
    m.name = "Creature\\Wisp\\Wisp.m2";
    m.vertices.resize(400);
    m.indices.resize(1200);
    m.batches.resize(3);
    m.collisionVertices.resize(8);
    m.collisionIndices.resize(36);
    m.collisionNormals.resize(12);
    m.bones.resize(42);
    m.sequences.resize(5);
    m.globalSequenceDurations = {1000, 2000};
    m.textures.push_back({.type = 0, .flags = 0, .filename = "CREATURE\\WISP\\DUST3.BLP"});
    m.particleEmitters.resize(11);
    m.particleEmitters[0].bone = 30;
    m.particleEmitters[0].texture = 0;
    return m;
}
}  // namespace

TEST_CASE("the emitter-only copy keeps what emitters read and nothing drawable", "[character-emitters]") {
    const auto source = wispLike();
    REQUIRE(rendering::hasEffectEmitters(source));
    const auto effects = rendering::emitterOnlyModel(source);
    CHECK(effects.vertices.empty());
    CHECK(effects.indices.empty());
    CHECK(effects.batches.empty());
    CHECK(effects.collisionVertices.empty());
    CHECK(effects.collisionIndices.empty());
    CHECK(effects.collisionNormals.empty());
    CHECK_FALSE(effects.isValid());
    // Emitters index bones and textures; their tracks are per sequence.
    CHECK(effects.particleEmitters.size() == 11);
    CHECK(effects.particleEmitters[0].bone == 30);
    CHECK(effects.bones.size() == 42);
    CHECK(effects.sequences.size() == 5);
    CHECK(effects.globalSequenceDurations == source.globalSequenceDurations);
    CHECK(effects.textures.size() == 1);
    CHECK(effects.name == source.name);
    CHECK(rendering::hasEffectEmitters(effects));
}

TEST_CASE("a model without emitters gets no emitter instance", "[character-emitters]") {
    pipeline::M2Model plain;
    plain.vertices.resize(3);
    CHECK_FALSE(rendering::hasEffectEmitters(plain));
    plain.ribbonEmitters.resize(1);
    CHECK(rendering::hasEffectEmitters(plain));
}

TEST_CASE("emitters fade and hide with their owner", "[character-emitters]") {
    CHECK(rendering::characterEmitterFade(true, 1.0f, 1.0f) == 1.0f);
    CHECK(rendering::characterEmitterFade(true, 0.5f, 0.5f) == 0.25f);  // fading in / camera fade
    CHECK(rendering::characterEmitterFade(false, 1.0f, 1.0f) == 0.0f);  // first person, hidden
    CHECK(rendering::characterEmitterFade(true, 0.0f, 1.0f) == 0.0f);
}

TEST_CASE("each character model gets its own stable emitter model id", "[character-emitters]") {
    const auto a = rendering::characterEmitterModelId("Creature\\Wisp\\Wisp.m2", 7);
    CHECK(a == rendering::characterEmitterModelId("Creature\\Wisp\\Wisp.m2", 7));
    CHECK(a != rendering::characterEmitterModelId("Creature\\Wisp\\Wisp.m2", 8));
    CHECK(a != rendering::characterEmitterModelId("Creature\\FireElemental\\FireElemental.m2", 7));
}
