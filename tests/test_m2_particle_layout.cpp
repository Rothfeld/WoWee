#include <catch_amalgamated.hpp>
#include "pipeline/m2_loader.hpp"
#include <cstring>
#include <vector>

TEST_CASE("TBC and Wrath particle headers preserve emitter shape", "[m2][particle]") {
    for (const uint32_t version : {260u, 264u}) {
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
        bytes[emitterOffset + (legacy ? 0x2a : 0x29)] = 1;
        bytes[emitterOffset + (legacy ? 0x2c : 0x2d)] = 1;
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
