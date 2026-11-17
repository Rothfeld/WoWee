#include <catch_amalgamated.hpp>
#include "pipeline/m2_loader.hpp"
#include <cstring>
#include <vector>
#include "rendering/m2_track_sampler.hpp"

TEST_CASE("TBC texture coordinate combos preserve secondary UV and environment mapping", "[m2][material]") {
    std::vector<uint8_t> bytes(518, 0);
    std::memcpy(bytes.data(), "MD20", 4);
    const auto put32 = [&](size_t at, uint32_t value) {
        std::memcpy(bytes.data() + at, &value, sizeof(value));
    };
    put32(4, 260);
    put32(156, 3);
    put32(160, 512);
    const int16_t coordinates[] = {0, 1, -1};
    std::memcpy(bytes.data() + 512, coordinates, sizeof(coordinates));
    const auto model = wowee::pipeline::M2Loader::load(bytes);
    REQUIRE(model.textureCoordLookup.size() == 3);
    CHECK(model.textureCoordLookup == std::vector<int16_t>{0, 1, -1});
}

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
    for (const uint32_t version : {256u, 260u, 261u, 262u, 263u, 264u}) {
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
        bytes[emitterOffset + (version < 262 ? 0x2a : 0x29)] = 1;
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

TEST_CASE("Version 262 emitters read byte blend and emitter type", "[m2][particle]") {
    // GnomereganVent.m2 (version 262), emitter 0, bytes +0x28..+0x2B: 02 01 00 00.
    // Read as uint16 pairs this was blend 258 and type 0, a point emitter.
    constexpr uint32_t emitterOffset = 512;
    std::vector<uint8_t> bytes(emitterOffset + 504, 0);
    std::memcpy(bytes.data(), "MD20", 4);
    const auto put32 = [&](size_t offset, uint32_t value) {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    };
    put32(4, 262);
    put32(0x13c, 1);
    put32(0x140, emitterOffset);
    const uint8_t header[] = {2, 1, 0, 0};
    std::memcpy(bytes.data() + emitterOffset + 0x28, header, sizeof(header));
    const auto model = wowee::pipeline::M2Loader::load(bytes);
    REQUIRE(model.particleEmitters.size() == 1);
    CHECK(model.particleEmitters[0].blendingType == 2);
    CHECK(model.particleEmitters[0].emitterType == 1);
}

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
    // Lighting is independent of attachment: 0x19 also has NoTrail.
    CHECK(particleRenderTransform(0x19, model, bone) == attached);
}
TEST_CASE("Trailing emitters preserve birth pose independent of emitter shape", "[m2][particle]") {
    using namespace wowee::rendering;
    const auto model = glm::translate(glm::mat4(1), glm::vec3(10, 0, 0));
    const auto birthBone = glm::translate(glm::mat4(1), glm::vec3(0, 2, 0));
    const auto currentBone = glm::translate(glm::mat4(1), glm::vec3(0, 4, 0));
    const glm::vec4 origin(0,0,0,1);
    for (const uint32_t flags : {0x29u, 0x229u}) {
        const auto render = particleRenderTransform(flags, model, currentBone);
        CHECK(render * particleBirthTransform(flags,model,birthBone) * origin == glm::vec4(10,2,0,1));
        CHECK(render * particleBirthTransform(flags,model,currentBone) * origin == glm::vec4(10,4,0,1));
        const auto movedModel = glm::translate(model, glm::vec3(3,0,0));
        CHECK(particleRenderTransform(flags,movedModel,currentBone) * particleBirthTransform(flags,model,birthBone) * origin == glm::vec4(10,2,0,1));
    }
    // ModelSpace keeps the model's movement, but never today's emitter pose.
    const auto movedModel = glm::translate(model, glm::vec3(3,0,0));
    CHECK(particleRenderTransform(0x89,movedModel,currentBone) * particleBirthTransform(0x89,model,birthBone) * origin == glm::vec4(13,2,0,1));
}

TEST_CASE("Particle gravity retains the authored scaled and rotated basis", "[m2][particle]") {
    using namespace wowee::rendering;
    const auto model = glm::scale(glm::rotate(glm::mat4(1),glm::radians(90.0f),glm::vec3(0,1,0)),glm::vec3(2));
    const auto bone = glm::translate(glm::mat4(1),glm::vec3(1,2,3));
    const glm::vec4 localPosition(0,0,0,1);
    const glm::vec4 localVelocity(0,0,3,0);
    const float gravity = 4;
    const float seconds = .5f;
    const auto expected = model * bone * (localPosition + localVelocity * seconds + glm::vec4(0,0,-.5f*gravity*seconds*seconds,0));
    for (const uint32_t flags : {0x29u,0x89u,0x39u}) {
        const auto birth = particleBirthTransform(flags,model,bone);
        const auto render = particleRenderTransform(flags,model,bone);
        const auto acceleration = particleGravityDirection(birth)*gravity;
        const auto simulated = birth*localPosition + birth*localVelocity*seconds + glm::vec4(acceleration*(.5f*seconds*seconds),0);
        const auto actual = render*simulated;
        CHECK(actual.x == Catch::Approx(expected.x).margin(.00001f));
        CHECK(actual.y == Catch::Approx(expected.y).margin(.00001f));
        CHECK(actual.z == Catch::Approx(expected.z).margin(.00001f));
    }
}

TEST_CASE("Particle widths follow authored matrix placement scale", "[m2][particle]") {
    using wowee::rendering::particlePlacementScale;
    // Exodar MODD: DR_DRAENEI_HOLORUNES02_BIG at -4108.62,-11523.8.
    const float authoredScale = 0.4504f;
    const auto placement = glm::translate(glm::mat4(1), glm::vec3(-4108.62f, -11523.8f, -135.668f))
        * glm::rotate(glm::mat4(1), 1.37f, glm::normalize(glm::vec3(1, 2, 3)))
        * glm::scale(glm::mat4(1), glm::vec3(authoredScale));
    CHECK(particlePlacementScale(placement) == Catch::Approx(authoredScale));
    CHECK(0.611111f * particlePlacementScale(placement) == Catch::Approx(0.2752444f));
    CHECK(particlePlacementScale(glm::mat4(1)) == Catch::Approx(1));
    CHECK(particlePlacementScale(glm::scale(glm::mat4(1), glm::vec3(6.18096f)))
        == Catch::Approx(6.18096f));
    CHECK(particlePlacementScale(glm::scale(glm::mat4(1), glm::vec3(-2))) == Catch::Approx(2));
    CHECK(particlePlacementScale(glm::scale(glm::mat4(1), glm::vec3(0))) == 0);
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
