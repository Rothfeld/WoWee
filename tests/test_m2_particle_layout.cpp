#include <catch_amalgamated.hpp>
#include "pipeline/m2_loader.hpp"
#include <cstring>
#include <vector>
#include "rendering/m2_track_sampler.hpp"

TEST_CASE("Hidden portal mesh opacity differs from animated fade-in", "[m2][particle]") {
    wowee::pipeline::M2AnimationTrack track;
    using wowee::rendering::m2_track::isAlwaysZero;
    CHECK_FALSE(isAlwaysZero(track));
    track.sequences.resize(1);
    CHECK_FALSE(isAlwaysZero(track));
    track.sequences[0].floatValues = {0};
    CHECK(isAlwaysZero(track));
    track.sequences[0].floatValues = {0, 0, 0};
    CHECK(isAlwaysZero(track));
    track.sequences[0].floatValues = {0, 1, 0};
    CHECK_FALSE(isAlwaysZero(track));
    track.sequences[0].floatValues = {0};
    track.sequences.resize(2);
    track.sequences[1].floatValues = {1};
    CHECK_FALSE(isAlwaysZero(track));
}

TEST_CASE("TBC and Wrath particle headers preserve emitter shape", "[m2][particle]") {
    for (const uint32_t version : {256u, 260u, 263u, 264u}) {
        const bool legacy = version < 264;
        constexpr uint32_t emitterOffset = 512;
        std::vector<uint8_t> bytes(emitterOffset + (legacy ? 504 : 476), 0);
        std::memcpy(bytes.data(), "MD20", 4);
        const auto put32 = [&](size_t offset, uint32_t value) {
            std::memcpy(bytes.data() + offset, &value, sizeof(value));
        };
        put32(4, version);
        put32(legacy ? 0x13c : 0x128, 1);
        put32(legacy ? 0x140 : 0x12c, emitterOffset);
        // Original LootFX legacy header: 04 00 01 00 at +0x28.
        bytes[emitterOffset + 0x28] = 4;
        bytes[emitterOffset + (version < 263 ? 0x2a : 0x29)] = 1;
        // Particle type and head/tail are separate bytes in every version.
        bytes[emitterOffset + 0x2c] = 0;
        bytes[emitterOffset + 0x2d] = 1;
        const float tailLength = 3.0f;
        std::memcpy(bytes.data() + emitterOffset + (legacy ? 0x17c : 0x15c),
                    &tailLength, sizeof(tailLength));
        const auto model = wowee::pipeline::M2Loader::load(bytes);
        REQUIRE(model.particleEmitters.size() == 1);
        CHECK(model.particleEmitters[0].blendingType == 4);
        CHECK(model.particleEmitters[0].emitterType == 1);
        CHECK(model.particleEmitters[0].headOrTail == 1);
        CHECK(model.particleEmitters[0].tailLength == Catch::Approx(3));
    }
}

#include "rendering/particle_emission.hpp"
#include <glm/gtc/matrix_transform.hpp>

TEST_CASE("Living portal particles follow their rotating emitter bone", "[m2][particle]") {
    using wowee::rendering::particleRenderTransform;
    const glm::mat4 model = glm::translate(glm::mat4(1), glm::vec3(10, 20, 30));
    const glm::vec3 center(0, 0, 2.737066f);
    const glm::mat4 bone = glm::translate(glm::mat4(1), center)
        * glm::rotate(glm::mat4(1), glm::radians(90.0f), glm::vec3(1, 0, 0))
        * glm::translate(glm::mat4(1), -center);
    // One half-second-old InstancePortal particle: radius contracts while
    // the current bone rotates its position and velocity around the portal.
    const float radius = 4.1666665f - 3.3333333f * 0.5f;
    const glm::vec4 position(center + glm::vec3(0, radius, 0), 1);
    const glm::vec4 velocity(0, -3.3333333f, 0, 0);
    const glm::mat4 attached = particleRenderTransform(0x39, model, bone);
    const glm::vec4 world = attached * position;
    CHECK(world.x == Catch::Approx(10));
    CHECK(world.y == Catch::Approx(20));
    CHECK(world.z == Catch::Approx(30 + center.z + radius));
    CHECK((attached * velocity).z == Catch::Approx(-3.3333333f));
    // Summon_Ritual uses 0x19: already-world-space particles must not be
    // translated or rotated again when the emitter changes its pose.
    CHECK(particleRenderTransform(0x19, model, bone) * position == position);
    CHECK(particleRenderTransform(0x19, model, bone) * velocity == velocity);
}
TEST_CASE("Flat spherical emitters spread across their area and travel radially", "[m2][particle]") {
    using wowee::rendering::sphericalParticleEmission;
    const auto horizontal = sphericalParticleEmission(2, 4, 0, 0, 1, 0x429);
    CHECK(horizontal.offset.y == Catch::Approx(4));
    CHECK(horizontal.offset.z == 0);
    CHECK(horizontal.direction.y == Catch::Approx(1));
    CHECK(horizontal.direction.z == 0);
    const auto vertical = sphericalParticleEmission(2, 4, 0, 0, 1, 0x529);
    CHECK(vertical.offset == horizontal.offset);
    CHECK(vertical.direction == glm::vec3(0, 0, 1));
    const auto side = sphericalParticleEmission(2, 4, 0, 1.57079632679f, 1, 0x429);
    CHECK(side.offset.x == Catch::Approx(4));
    CHECK(side.offset.z == 0);
}

TEST_CASE("Summoning portal emits a ring rather than filling its center", "[m2][particle]") {
    using wowee::rendering::sphericalParticleEmission;
    for (const float sample : {0.0f, 0.5f, 1.0f}) {
        for (const float angle : {-3.14159265f, -1.57079633f, 0.0f, 1.57079633f}) {
            const auto emission = sphericalParticleEmission(1.8055556f, 1.8055556f,
                angle, 0, sample, 0x19);
            CHECK(emission.offset.x == Catch::Approx(0));
            CHECK(glm::length(emission.offset) == Catch::Approx(1.8055556f));
            CHECK(glm::dot(emission.offset, emission.direction) > 0);
        }
    }
    CHECK(glm::length(sphericalParticleEmission(2, 4, 0, 0, 0, 0).offset) == Catch::Approx(2));
    CHECK(glm::length(sphericalParticleEmission(2, 4, 0, 0, .5f, 0).offset) == Catch::Approx(3));
}
