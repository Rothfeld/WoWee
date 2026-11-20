#include <catch_amalgamated.hpp>
#include "pipeline/m2_loader.hpp"
#include "tbc_data_test_util.hpp"
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
        // Pre-264 head/tail is +0x2C (+0x2D is zero in all TBC data);
        // Wrath keeps it at +0x2D.
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
    // So does 0x4000: Zul'jin's claw wisps (0x4429) ride the claw when its
    // wielder jumps; Shaman_Frost (0x209) stays where it was born.
    CHECK(particleRenderTransform(0x4429,movedModel,currentBone) * particleBirthTransform(0x4429,model,birthBone) * origin == glm::vec4(13,2,0,1));
    CHECK(particleRenderTransform(0x209,movedModel,currentBone) * particleBirthTransform(0x209,model,birthBone) * origin == glm::vec4(10,2,0,1));
}

TEST_CASE("Births inside a step sit where the moving emitter was at that instant", "[m2][particle]") {
    using namespace wowee::rendering;
    // DR_Draenei_HoloRunes02: 33 sparks/s from a bone sliding along the floor
    // line. At 8 fps all four births of a step shared one point: beads.
    const auto previous = glm::translate(glm::mat4(1), glm::vec3(0, 0, 0));
    const auto current = glm::translate(glm::mat4(1), glm::vec3(0.25f, 0, 0));
    const glm::vec3 local(0, 1, 0);
    const float dt = 0.125f;
    CHECK(particleBirthPosition(previous, current, local, 0.0f, dt) == glm::vec3(0.25f, 1, 0));
    CHECK(particleBirthPosition(previous, current, local, dt, dt) == glm::vec3(0, 1, 0));
    const auto middle = particleBirthPosition(previous, current, local, dt * 0.5f, dt);
    CHECK(middle.x == Catch::Approx(0.125f));
    // A still emitter, or a zero step, places every birth at its pose.
    CHECK(particleBirthPosition(current, current, local, 0.06f, dt) == glm::vec3(0.25f, 1, 0));
    CHECK(particleBirthPosition(previous, current, local, 0.0f, 0.0f) == glm::vec3(0.25f, 1, 0));
}

TEST_CASE("Attached particle motion retains the scaled and rotated basis", "[m2][particle]") {
    using namespace wowee::rendering;
    const auto model = glm::scale(glm::rotate(glm::mat4(1),glm::radians(90.0f),glm::vec3(0,1,0)),glm::vec3(2));
    const auto bone = glm::translate(glm::mat4(1),glm::vec3(1,2,3));
    const glm::vec4 localPosition(0,0,0,1);
    const glm::vec4 localVelocity(0,0,3,0);
    const float gravity = 4;
    const float seconds = .5f;
    const auto expected = model * bone * (localPosition + localVelocity * seconds + glm::vec4(0,0,-.5f*gravity*seconds*seconds,0));
    for (const uint32_t flags : {0x89u,0x39u,0x4429u}) {
        const auto birth = particleBirthTransform(flags,model,bone);
        const auto render = particleRenderTransform(flags,model,bone);
        CHECK(particleWorldVelocityBasis(flags, birth, false) == glm::mat3(birth));
        const auto acceleration = particleGravityDirection(flags,model,false)*gravity;
        const auto simulated = birth*localPosition + birth*localVelocity*seconds + glm::vec4(acceleration*(.5f*seconds*seconds),0);
        const auto actual = render*simulated;
        CHECK(actual.x == Catch::Approx(expected.x).margin(.00001f));
        CHECK(actual.y == Catch::Approx(expected.y).margin(.00001f));
        CHECK(actual.z == Catch::Approx(expected.z).margin(.00001f));
    }
}

TEST_CASE("Detached particle gravity ignores the emitter bone's rotation", "[m2][particle]") {
    using namespace wowee::rendering;
    // Core Hound drool: the head bone is pitched, the drops fall straight down.
    const auto model = glm::scale(glm::rotate(glm::mat4(1),glm::radians(30.0f),glm::vec3(0,0,1)),glm::vec3(2));
    for (const uint32_t flags : {0x29u,0x89u}) {
        const auto down = particleGravityDirection(flags,model,false);
        const glm::vec3 expected(0,0,-1);
        CHECK(down.x == Catch::Approx(expected.x).margin(.00001f));
        CHECK(down.y == Catch::Approx(expected.y).margin(.00001f));
        CHECK(down.z == Catch::Approx(expected.z).margin(.00001f));
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

TEST_CASE("Sphere emitters: elevation above the horizontal plane, no negative radius", "[m2][particle]") {
    using wowee::rendering::sphericalParticleEmission;
    // Shadow Nova (shadow_nova_area.m2 #0: vertical 0, horizontal pi,
    // radius 9.72, 0x2209): every birth on the ground plane, moving outward.
    for (const float azimuth : {-3.14159265f, -1.0f, 0.0f, 2.0f, 3.14159265f}) {
        const auto nova = sphericalParticleEmission(9.722f, 9.722f, 0, azimuth, 0.5f, 0x2209);
        CHECK(std::abs(nova.offset.z) < 1e-5f);
        CHECK(glm::length(nova.offset) == Catch::Approx(9.722f));
        CHECK(std::abs(nova.direction.z) < 1e-5f);
        CHECK(glm::dot(nova.direction, nova.offset) > 0);
    }
    // Etherial.m2 Death #20 (radius -1.11 to -0.83): no offset, no velocity;
    // its gravity alone draws the falling lines.
    const auto death = sphericalParticleEmission(-1.111f, -0.833f, 0, 2.0f, 0.3f, 0x400);
    CHECK(death.offset == glm::vec3(0));
    CHECK(death.direction == glm::vec3(0));
    // Etherial.m2 Death #21 (radius 0, 0x100): straight up.
    CHECK(sphericalParticleEmission(0, 0, 3.0f, 0, 0.5f, 0x100).direction == glm::vec3(0, 0, 1));
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

TEST_CASE("Particle sprites keep authored lifecycle scale above one yard", "[m2][particle]") {
    using wowee::rendering::particleSpriteHalfWidth;
    // GnomereganVent emitter 0 (0x200) scales 2.77778 / 4.44444 / 8.33333.
    CHECK(particleSpriteHalfWidth(0x200u, 8.33333f, 1.0f) == Catch::Approx(8.33333f));
    CHECK(particleSpriteHalfWidth(0x200u, 2.77778f, 1.0f) == Catch::Approx(2.77778f));
    CHECK(particleSpriteHalfWidth(0x0u, 0.0000278f, 1.0f) == Catch::Approx(0.0000278f));
}

TEST_CASE("Emitters with flag 0x20 or NoTrail size their sprites by placement", "[m2][particle]") {
    using wowee::rendering::particleSpriteHalfWidth;
    // LootFX.m2 over GnomeBigGear (sparkle scale 3.52): star #3 (0x229, mid
    // scale 0.0833) grows with it, flare #0 (0x101, 0.1389) stays authored.
    CHECK(particleSpriteHalfWidth(0x229u, 0.0833f, 3.52f) == Catch::Approx(0.2932f).margin(0.0001f));
    CHECK(particleSpriteHalfWidth(0x101u, 0.1389f, 3.52f) == Catch::Approx(0.1389f));
    // Light of Elune (LARGE), GO size 5, feralaslightblue.m2 #0 (0x1).
    CHECK(particleSpriteHalfWidth(0x1u, 0.1389f, 5.0f) == Catch::Approx(0.1389f));
    // Mark of Kael'Thas, GO size 5, flames 0x521.
    CHECK(particleSpriteHalfWidth(0x521u, 0.5556f, 5.0f) == Catch::Approx(2.778f));
    // Time Rift (display scale 4): ring #1 (0x1219) and toon smoke #3 (0x219)
    // ride their bones without 0x20 and grow with the creature.
    CHECK(particleSpriteHalfWidth(0x1219u, 2.461f, 4.0f) == Catch::Approx(9.844f));
    CHECK(particleSpriteHalfWidth(0x219u, 2.908f, 4.0f) == Catch::Approx(11.632f));
}

TEST_CASE("XYQuad sprites span their emitter bone's placed X and Y", "[m2][particle]") {
    // Time Rift ring bones: a 90 degree turn about Y under the creature's
    // placement turns the quad's X axis down and keeps Y.
    const glm::mat4 placed = glm::scale(glm::mat4(1.0f), glm::vec3(4.0f)) *
        glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0, 1, 0));
    const auto [x, y] = wowee::rendering::particleQuadAxes(placed);
    CHECK(x.z == Catch::Approx(-1.0f));
    CHECK(glm::length(x) == Catch::Approx(1.0f));
    CHECK(y.y == Catch::Approx(1.0f));
}

TEST_CASE("Legacy emitters keep authored all-zero lifecycle alpha", "[m2][particle]") {
    // G_ExplosiveTrap emitter 3: additive, colour alpha bytes 0/0/0.
    constexpr uint32_t emitterOffset = 512;
    std::vector<uint8_t> bytes(emitterOffset + 504, 0);
    std::memcpy(bytes.data(), "MD20", 4);
    const auto put32 = [&](size_t offset, uint32_t value) {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    };
    put32(4, 260);
    put32(0x13c, 1);
    put32(0x140, emitterOffset);
    bytes[emitterOffset + 0x28] = 4;
    for (int c = 0; c < 3; ++c) put32(emitterOffset + 0x150 + 4 * c, 0x00ffffffu);
    const auto model = wowee::pipeline::M2Loader::load(bytes);
    REQUIRE(model.particleEmitters.size() == 1);
    CHECK(model.particleEmitters[0].particleAlpha.floatValues == std::vector<float>{0, 0, 0});
    CHECK(model.particleEmitters[0].particleColor.vec3Values[1] == glm::vec3(1));
}

TEST_CASE("Legacy texture transforms follow the flipbook array", "[m2][material]") {
    // TBC header: weights +0x64, flipbooks +0x6C, transforms +0x74,
    // replaceable lookup +0x7C. BlackrockLavaFalls01: 0 flipbooks, 6 transforms.
    for (const uint32_t version : {256u, 260u, 263u}) {
        constexpr uint32_t transformOffset = 512;
        std::vector<uint8_t> bytes(transformOffset + 2 * 84, 0);
        std::memcpy(bytes.data(), "MD20", 4);
        const auto put32 = [&](size_t offset, uint32_t value) {
            std::memcpy(bytes.data() + offset, &value, sizeof(value));
        };
        put32(4, version);
        put32(0x74, 2);
        put32(0x78, transformOffset);
        put32(0x7C, 1);
        put32(0x80, transformOffset);
        const auto model = wowee::pipeline::M2Loader::load(bytes);
        CHECK(model.textureTransforms.size() == 2);
    }
}

TEST_CASE("Legacy emitters carry authored cell ranges and post-tail fields", "[m2][particle]") {
    // ShadowSnare_High_Base #2: 16 tiles, head (0,7,1), decay (8,16,1), midpoint 0.5.
    constexpr uint32_t e = 512;
    std::vector<uint8_t> bytes(e + 504, 0);
    std::memcpy(bytes.data(), "MD20", 4);
    const auto put32 = [&](size_t offset, uint32_t value) { std::memcpy(bytes.data() + offset, &value, 4); };
    const auto putf = [&](size_t offset, float value) { std::memcpy(bytes.data() + offset, &value, 4); };
    put32(4, 260);
    put32(0x13c, 1);
    put32(0x140, e);
    putf(e + 0x14C, 0.5f);
    const int16_t cells[10] = {0, 7, 1, 8, 16, 1, 2, 3, 4, 5};
    std::memcpy(bytes.data() + e + 0x168, cells, sizeof(cells));
    putf(e + 0x17C, 0.1f);   // tailLength
    putf(e + 0x180, 10.0f);  // twinkleSpeed
    putf(e + 0x184, 1.0f);   // twinklePercent
    putf(e + 0x194, 0.2f);   // drag
    putf(e + 0x198, 3.0f);   // spin (GnomereganVent)
    putf(e + 0x1C0, 6.3886f);  // windTime (Blacksmith_Smoke #0)
    const auto model = wowee::pipeline::M2Loader::load(bytes);
    REQUIRE(model.particleEmitters.size() == 1);
    const auto& em = model.particleEmitters[0];
    CHECK(em.legacyCells);
    CHECK(em.headCells.end == 7);
    CHECK(em.decayCells.start == 8);
    CHECK(em.decayCells.end == 16);
    CHECK(em.tailCells.start == 2);
    CHECK(em.tailDecayCells.end == 5);
    CHECK(em.tailLength == Catch::Approx(0.1f));
    CHECK(em.twinkleSpeed == Catch::Approx(10.0f));
    CHECK(em.drag == Catch::Approx(0.2f));
    CHECK(em.spin == Catch::Approx(3.0f));
    CHECK(em.windTime == Catch::Approx(6.3886f));
}

TEST_CASE("Particle cells follow authored head and decay ranges", "[m2][particle]") {
    using wowee::rendering::particleCell;
    wowee::pipeline::M2ParticleEmitter em{};
    em.legacyCells = true;
    em.midPoint = 0.5f;
    em.headCells = {0, 7, 1};
    em.decayCells = {8, 16, 1};
    CHECK(particleCell(em, 0.0f, false, 16) == 0);
    CHECK(particleCell(em, 0.49f, false, 16) == 7);
    CHECK(particleCell(em, 0.5f, false, 16) == 8);
    CHECK(particleCell(em, 0.75f, false, 16) == 12);
    CHECK(particleCell(em, 0.999f, false, 16) == 0);  // cell 16 wraps on 16 tiles
    // Repeat: GiantInsectSwarm (0,1,5) on 2 tiles alternates five times.
    em.headCells = {0, 1, 5};
    CHECK(particleCell(em, 0.0f, false, 2) == 0);
    CHECK(particleCell(em, 0.06f, false, 2) == 1);
    CHECK(particleCell(em, 0.11f, false, 2) == 0);
    // Descending: ShadowWordSilence_Breath (31,16).
    em.headCells = {31, 16, 1};
    CHECK(particleCell(em, 0.0f, false, 64) == 31);
    CHECK(particleCell(em, 0.499f, false, 64) == 16);
    // Untiled textures always show cell 0.
    CHECK(particleCell(em, 0.3f, false, 1) == 0);
    // Tail sprite uses the tail ranges.
    em.tailCells = {4, 5, 1};
    CHECK(particleCell(em, 0.0f, true, 16) == 4);
}

TEST_CASE("Particle blend modes follow the M2Blend table", "[m2][particle]") {
    using wowee::rendering::particleBlendPipeline;
    using wowee::rendering::ParticleBlendPipeline;
    // EntanglingRoots_State #1 (1, PARTROCK, 8-bit alpha): alpha-key cutout.
    CHECK(particleBlendPipeline(1) == ParticleBlendPipeline::Alpha);
    CHECK(particleBlendPipeline(2) == ParticleBlendPipeline::Alpha);
    CHECK(particleBlendPipeline(4) == ParticleBlendPipeline::Additive);
    CHECK(particleBlendPipeline(3) == ParticleBlendPipeline::Additive);
    // UnstableAffliction_Impact_Chest #1 (5, Clouds8x8ModFire): multiply.
    CHECK(particleBlendPipeline(5) == ParticleBlendPipeline::Mod);
    CHECK(particleBlendPipeline(6) == ParticleBlendPipeline::Mod2x);
}

TEST_CASE("Legacy emitters stop births where their EnabledIn track turns off", "[m2][particle]") {
    // ThunderClap_Cast_Base (Sonic Burst cast kit): Stand 667-5600, EnabledIn
    // keys 667 -> 1, 833 -> 0, discrete, at emitter +0x1DC.
    constexpr uint32_t e = 512;
    constexpr uint32_t seq = e + 504;
    constexpr uint32_t ranges = seq + 68, times = ranges + 8, keys = times + 8;
    std::vector<uint8_t> bytes(keys + 2, 0);
    std::memcpy(bytes.data(), "MD20", 4);
    const auto put32 = [&](size_t offset, uint32_t value) { std::memcpy(bytes.data() + offset, &value, 4); };
    put32(4, 260);
    put32(0x1C, 1);
    put32(0x20, seq);
    put32(seq + 4, 667);
    put32(seq + 8, 5600);
    put32(0x13c, 1);
    put32(0x140, e);
    const uint32_t track = e + 0x1DC;
    const int16_t globalSequence = -1;
    std::memcpy(bytes.data() + track + 2, &globalSequence, 2);
    put32(track + 4, 1); put32(track + 8, ranges);
    put32(track + 12, 2); put32(track + 16, times);
    put32(track + 20, 2); put32(track + 24, keys);
    put32(ranges, 0); put32(ranges + 4, 1);
    put32(times, 667); put32(times + 4, 833);
    bytes[keys] = 1; bytes[keys + 1] = 0;

    const auto model = wowee::pipeline::M2Loader::load(bytes);
    REQUIRE(model.particleEmitters.size() == 1);
    const auto& enabled = model.particleEmitters[0].enabledIn;
    using wowee::rendering::m2_track::sampleFloat;
    CHECK(sampleFloat(enabled, 0, 0.0f, 0.0f, {}, 1.0f) == 1.0f);
    CHECK(sampleFloat(enabled, 0, 150.0f, 0.0f, {}, 1.0f) == 1.0f);
    CHECK(sampleFloat(enabled, 0, 170.0f, 0.0f, {}, 1.0f) == 0.0f);
    CHECK(sampleFloat(enabled, 0, 4000.0f, 0.0f, {}, 1.0f) == 0.0f);
}

TEST_CASE("Legacy emitters read geometry model and tumble range", "[m2][particle]") {
    // Whirlwind_State_Base.m2 (260) #0: "Spells\\WhirlwindGeoWhite.mdx", tumble 0..8 about Z.
    constexpr uint32_t emitterOffset = 512;
    std::vector<uint8_t> bytes(emitterOffset + 504 + 64, 0);
    std::memcpy(bytes.data(), "MD20", 4);
    const auto put32 = [&](size_t offset, uint32_t value) { std::memcpy(bytes.data() + offset, &value, 4); };
    const auto putf = [&](size_t offset, float value) { std::memcpy(bytes.data() + offset, &value, 4); };
    put32(4, 260);
    put32(0x13c, 1);
    put32(0x140, emitterOffset);
    const std::string name = "Spells\\WhirlwindGeoWhite.mdx";
    const uint32_t nameOffset = emitterOffset + 504;
    std::memcpy(bytes.data() + nameOffset, name.c_str(), name.size() + 1);
    put32(emitterOffset + 0x18, static_cast<uint32_t>(name.size() + 1));
    put32(emitterOffset + 0x1c, nameOffset);
    putf(emitterOffset + 0x1b0, 8.0f);
    const auto model = wowee::pipeline::M2Loader::load(bytes);
    REQUIRE(model.particleEmitters.size() == 1);
    CHECK(model.particleEmitters[0].geometryModel == name);
    CHECK(model.particleEmitters[0].tumbleMin == glm::vec3(0.0f));
    CHECK(model.particleEmitters[0].tumbleMax == glm::vec3(0.0f, 0.0f, 8.0f));
}

TEST_CASE("Pinned particle tails end at the birth point", "[m2][particle]") {
    using namespace wowee::rendering;
    // Bloodlust hand ring (0x479): 0.28 yd/s, tailLength 10 s, 0.1 yd from birth.
    const glm::vec3 velocity(0.28f, 0.0f, 0.0f);
    const glm::vec3 toBirth(-0.1f, 0.0f, 0.0f);
    const glm::vec3 pinned = particleTailVector(0x479, velocity, 10.0f, toBirth);
    CHECK(pinned.x == Catch::Approx(-0.1f));
    // Without 0x400 the tail keeps its full authored length.
    CHECK(particleTailVector(0x79, velocity, 10.0f, toBirth).x == Catch::Approx(-2.8f));
    // Acid breath drop (0x429) far from its birth: velocity tail.
    const glm::vec3 drop = particleTailVector(0x429, glm::vec3(0, 4.44f, 0), 0.1f, glm::vec3(0, -3.0f, 0));
    CHECK(drop.y == Catch::Approx(-0.444f));
    // Purge_Impact_Chest #1 (0x19, non-pinned): speed 0.556, drag 30, tailLength 4.75 s.
    // Drag-integrated tail span (1 - exp(-30*4.75))/30 = 0.0333 s keeps newborn tails tiny (< 0.02 yd).
    for (float age : {0.0f, 0.002f, 0.01f, 0.033f, 0.1f, 0.25f}) {
        const float v = 0.556f * std::exp(-30.0f * age);
        const float dist = 0.556f * particleDragTravel(30.0f, age);
        const glm::vec3 t = particleTailVector(0x19, glm::vec3(v, 0.0f, 0.0f), 4.75f,
                                               glm::vec3(-dist, 0.0f, 0.0f), 30.0f);
        CHECK(glm::length(t) < 0.02f);
    }
    // Holy_Precast_High_Base #0/#2 (Chain Heal precast ground sunburst, flags 0xb9, non-pinned):
    // #0 peaks at age=0 (drag 0.5, tailLength 0.40 s) and #2 peaks at age=0.12 s (drag 0.8, tailLength 0.35 s).
    // Both preserve their full drag-integrated radial streaks (> 0.45 yd) instead of collapsing at birth.
    const glm::vec3 ch0 = particleTailVector(0xb9, glm::vec3(-1.667f, 0.0f, 0.0f), 0.40f,
                                             glm::vec3(0.0f), 0.50f);
    CHECK(glm::length(ch0) == Catch::Approx(1.667f * particleDragTravel(0.50f, 0.40f)));
    CHECK(glm::length(ch0) > 0.55f);
    const float v2 = 1.667f * std::exp(-0.80f * 0.12f);
    const glm::vec3 ch2 = particleTailVector(0xb9, glm::vec3(-v2, 0.0f, 0.0f), 0.35f,
                                             glm::vec3(0.19f, 0.0f, 0.0f), 0.80f);
    CHECK(glm::length(ch2) == Catch::Approx(v2 * particleDragTravel(0.80f, 0.35f)));
    CHECK(glm::length(ch2) > 0.45f);
}

TEST_CASE("Shipped TBC Holy_Precast_High_Base.m2 ground sunburst emitters preserve full radial tails",
          "[m2][particle][tbc]") {
    using namespace wowee::rendering;
    const auto bytes = wowee::test::readTbcData("spells/holy_precast_high_base.m2");
    const auto model = wowee::pipeline::M2Loader::load(bytes);
    REQUIRE(model.particleEmitters.size() == 5);
    // Emitter #0 (z=0.16, peak alpha at birth) and #2 (z=0.10, 400/s ground sunburst ring).
    for (size_t idx : {size_t{0}, size_t{2}}) {
        const auto& em = model.particleEmitters[idx];
        CHECK(em.emitterType == 2);
        CHECK(em.headOrTail == 1);
        CHECK((em.flags & kParticlePinned) == 0);
        CHECK(em.tailLength == Catch::Approx(idx == 0 ? 0.40f : 0.35f));
        CHECK(em.drag == Catch::Approx(idx == 0 ? 0.50f : 0.80f));
        const glm::vec3 tail = particleTailVector(em.flags, glm::vec3(-1.667f, 0.0f, 0.0f),
                                                  em.tailLength, glm::vec3(0.0f), em.drag);
        CHECK(glm::length(tail) > 0.50f);
    }
    // Shaman Water/Air/Earth totem top flame (flags 0x9, unpinned, drag 0):
    // keeps full authored -velocity * tailLength so the flame streak reaches
    // down into the totem bowl instead of floating above the horns.
    const glm::vec3 totemTail = particleTailVector(0x9, glm::vec3(0.0f, 0.0f, 0.908f), 1.2f,
                                                   glm::vec3(0.0f, 0.0f, -0.1f), 0.0f);
    CHECK(totemTail.z == Catch::Approx(-0.908f * 1.2f));
}

TEST_CASE("Spline emitters read Bezier points and place births along them", "[m2][particle]") {
    // FlameCircle.m2 (260) #0: 13 points, four quarter-circle Bezier segments
    // of radius 2.778 with 1.533 handles.
    constexpr uint32_t emitterOffset = 512;
    constexpr float r = 2.778f, h = 1.533f;
    const std::vector<glm::vec3> circle = {
        {r, 0, 0}, {r, h, 0}, {h, r, 0}, {0, r, 0}, {-h, r, 0}, {-r, h, 0}, {-r, 0, 0},
        {-r, -h, 0}, {-h, -r, 0}, {0, -r, 0}, {h, -r, 0}, {r, -h, 0}, {r, 0, 0}};
    std::vector<uint8_t> bytes(emitterOffset + 504 + circle.size() * 12, 0);
    std::memcpy(bytes.data(), "MD20", 4);
    const auto put32 = [&](size_t offset, uint32_t value) { std::memcpy(bytes.data() + offset, &value, 4); };
    put32(4, 260);
    put32(0x13c, 1);
    put32(0x140, emitterOffset);
    put32(emitterOffset + 0x2a, 3);
    put32(emitterOffset + 0x1d4, static_cast<uint32_t>(circle.size()));
    put32(emitterOffset + 0x1d8, emitterOffset + 504);
    std::memcpy(bytes.data() + emitterOffset + 504, circle.data(), circle.size() * 12);
    const auto model = wowee::pipeline::M2Loader::load(bytes);
    REQUIRE(model.particleEmitters.size() == 1);
    CHECK(model.particleEmitters[0].emitterType == 3);
    REQUIRE(model.particleEmitters[0].splinePoints == circle);

    using wowee::rendering::splineEmissionPoint;
    // Knots at every quarter; mid-segment on the circle within Bezier error.
    CHECK(splineEmissionPoint(circle, 0.0f) == circle[0]);
    CHECK(splineEmissionPoint(circle, 0.25f) == circle[3]);
    CHECK(splineEmissionPoint(circle, 1.0f) == circle[12]);
    for (const float t : {0.125f, 0.375f, 0.625f, 0.875f})
        CHECK(glm::length(splineEmissionPoint(circle, t)) == Catch::Approx(r).epsilon(0.01));
    // A 3k + 1 count is the format: two points cannot be a Bezier path.
    put32(emitterOffset + 0x1d4, 2);
    CHECK_FALSE(wowee::pipeline::M2Loader::load(bytes).isValid());
}

TEST_CASE("Spline births spread over a held width and follow an animated one", "[m2][particle]") {
    using namespace wowee::rendering;
    wowee::pipeline::M2AnimationTrack held, swept;
    held.sequences.resize(1); held.sequences[0].floatValues = {1.0f};
    swept.sequences.resize(1); swept.sequences[0].floatValues = {0.0f, 1.0f};
    // Shadowmoon_Rune2 #0 (length 0, width 1 held): a fence round the ring.
    CHECK_FALSE(particleTrackVaries(held, 0));
    CHECK(splineBirthParameter(0.0f, 1.0f, false, false, 0.25f) == Catch::Approx(0.25f));
    // FlameCircle (length 0 held, width 0 -> 1 over Stand): the front at the width.
    CHECK(particleTrackVaries(swept, 0));
    CHECK(splineBirthParameter(0.0f, 0.4f, false, true, 0.9f) == Catch::Approx(0.4f));
    // SummonPet_Cast_Impact_Base / SummonPet_Impact_Base (both length and width animated):
    // births span the moving window [length(t), width(t)].
    CHECK(splineBirthParameter(0.9f, 1.0f, true, true, 0.5f) == Catch::Approx(0.95f));
    CHECK(splineBirthParameter(0.2f, 0.0f, true, true, 0.25f) == Catch::Approx(0.15f));
}

TEST_CASE("Spline points rotate +90 deg about Z into M2 bone space", "[m2][particle]") {
    using wowee::rendering::splinePointToBoneSpace;
    // SummonPet_Cast_Impact_Base.m2 arcs authored along X map to bone +Y (+Z_world),
    // with radial extent Y mapped to -X_bone (outward from caster center).
    const glm::vec3 peak = splinePointToBoneSpace({2.24f, 0.31f, 0.0f});
    CHECK(peak.x == Catch::Approx(-0.31f));
    CHECK(peak.y == Catch::Approx(2.24f));
    CHECK(peak.z == Catch::Approx(0.0f));
    // FlameStrike_Impact.m2 XZ billboard ribbons map into the YZ billboard plane.
    const glm::vec3 ribbon = splinePointToBoneSpace({0.93f, 0.0f, 13.15f});
    CHECK(ribbon.x == Catch::Approx(0.0f));
    CHECK(ribbon.y == Catch::Approx(0.93f));
    CHECK(ribbon.z == Catch::Approx(13.15f));
}

TEST_CASE("A step track says nothing past its last key", "[m2][particle]") {
    // Holy Ground (consecration_impact_base.m2): rate 0 at 0 ms, 200 at 67 ms,
    // no interpolation, 12.4 s Stand; the original emits no particles.
    wowee::pipeline::M2AnimationTrack rate;
    rate.interpolationType = 0;
    rate.sequences.resize(1);
    rate.sequences[0].timestamps = {0, 67};
    rate.sequences[0].floatValues = {0.0f, 200.0f};
    using wowee::rendering::m2_track::pastLastStepKey;
    CHECK_FALSE(pastLastStepKey(rate, 0, 30.0f, 0.0f, {}));
    CHECK(pastLastStepKey(rate, 0, 500.0f, 0.0f, {}));
    rate.interpolationType = 1;
    CHECK_FALSE(pastLastStepKey(rate, 0, 500.0f, 0.0f, {}));
}

TEST_CASE("Detached particles move in yards whatever the placement scale", "[m2][particle]") {
    using namespace wowee::rendering;
    // Netherspite's Void Zone: PortalofKathune.m2 at display scale 0.1, 0x229,
    // gravity -1.39 lifts its smoke 1.56 yd in 1.5 s, not a tenth of that.
    const auto model = glm::scale(glm::rotate(glm::mat4(1),glm::radians(40.0f),glm::vec3(0,0,1)),glm::vec3(0.1f));
    const auto birth = particleBirthTransform(0x229u, model, glm::mat4(1));
    const auto basis = particleWorldVelocityBasis(0x229u, birth, false);
    CHECK(glm::length(basis * glm::vec3(0,0,1.1111f)) == Catch::Approx(1.1111f));
    CHECK(glm::length(particleGravityDirection(0x229u, model, false)) == Catch::Approx(1.0f));
    // Its birth point still takes the placement: a 25 yd area is 2.5 yd.
    CHECK(glm::length(glm::vec3(birth * glm::vec4(25,0,0,1))) == Catch::Approx(2.5f));
}

TEST_CASE("Detached particles without flag 0x8 move at placement scale", "[m2][particle]") {
    using namespace wowee::rendering;
    // LootFX.m2 flare #0 (0x101, 0.5556 yd/s for 1.6 s) over GnomeBigGear,
    // sparkle scale 3.52: rises about 3 yd in the original.
    const auto model = glm::scale(glm::rotate(glm::mat4(1),glm::radians(40.0f),glm::vec3(0,0,1)),glm::vec3(3.52f));
    const auto birth = particleBirthTransform(0x101u, model, glm::mat4(1));
    const auto basis = particleWorldVelocityBasis(0x101u, birth, false);
    CHECK(glm::length(basis * glm::vec3(0,0,0.5556f)) * 1.6f == Catch::Approx(3.129f).margin(0.001f));
    CHECK(glm::length(particleGravityDirection(0x101u, model, false)) == Catch::Approx(3.52f));
    CHECK(glm::length(particleGravityDirection(0x109u, model, false)) == Catch::Approx(1.0f));
}

TEST_CASE("Map doodad and spell visual particles move at placement scale with flag 0x8", "[m2][particle]") {
    using namespace wowee::rendering;
    // Owner predicate: map doodads and spell visuals scale with placement;
    // game objects and units (externalPose) keep yards for flag 0x8.
    CHECK(particleScalesWithPlacement(/*isGameObject=*/false, /*externalPose=*/false));
    CHECK_FALSE(particleScalesWithPlacement(/*isGameObject=*/true, /*externalPose=*/false));
    CHECK_FALSE(particleScalesWithPlacement(/*isGameObject=*/false, /*externalPose=*/true));

    // Magisters' Terrace throne orb: BloodElf_PowerOrb_Green.m2, MODD scale
    // 0.358, smoke #5 0x629 at 2.222 yd/s: 0.8 yd/s in the original.
    const auto model = glm::scale(glm::rotate(glm::mat4(1),glm::radians(89.0f),glm::vec3(0,0,1)),glm::vec3(0.358f));
    const auto birth = particleBirthTransform(0x629u, model, glm::mat4(1));
    CHECK(glm::length(particleWorldVelocityBasis(0x629u, birth, true) * glm::vec3(0,0,2.222f))
          == Catch::Approx(0.7955f).margin(0.001f));
    CHECK(glm::length(particleGravityDirection(0x629u, model, true)) == Catch::Approx(0.358f));
    // The same emitter on a game object or unit keeps yards.
    CHECK(glm::length(particleWorldVelocityBasis(0x629u, birth, false) * glm::vec3(0,0,2.222f))
          == Catch::Approx(2.222f));

    // Warlock Hellfire (1949/5857): HellFire_Impact_Caster_Base.m2 #0 (0x8009)
    // attached to Base (19) scales particle ring radius with the caster's Base
    // bone scale (0.5x Gnome, 1.0x Human, 2.0x scaled) so it matches the
    // projected lava ground decal.
    for (float s : {0.5f, 1.0f, 2.0f}) {
        const auto m = glm::scale(glm::mat4(1), glm::vec3(s));
        const auto b = particleBirthTransform(0x8009u, m, glm::mat4(1));
        CHECK(glm::length(particleWorldVelocityBasis(0x8009u, b, true) * glm::vec3(5.0f, 0.0f, 0.0f))
              == Catch::Approx(5.0f * s));
        CHECK(glm::length(particleGravityDirection(0x8009u, m, true)) == Catch::Approx(s));
    }
}

TEST_CASE("Sprites turn by spin times age", "[m2][particle]") {
    // Christmas Tree (Medium) star: spin 0.5, a quarter turn 1.57 s in.
    CHECK(wowee::rendering::particleRotation(0.5f, 3.14159f) == Catch::Approx(1.5708f));
    CHECK(wowee::rendering::particleRotation(0.0f, 2.0f) == 0.0f);
}

TEST_CASE("Twinkle size stays within the authored range", "[m2][particle]") {
    using wowee::rendering::particleTwinkleScale;
    // focusedcasting_state_chest: speed 10, scale 0.1 - 1.0.
    float lo = 1.0f, hi = 0.0f;
    for (uint32_t seed = 0; seed < 2000; ++seed) {
        const float s = particleTwinkleScale(seed * 2654435761u, 0.05f, 10.0f, 0.1f, 1.0f);
        CHECK(s >= 0.1f);
        CHECK(s <= 1.0f);
        lo = std::min(lo, s); hi = std::max(hi, s);
    }
    CHECK(lo < 0.15f);
    CHECK(hi > 0.95f);
    // Same particle, same twinkle period: same size.
    CHECK(particleTwinkleScale(7, 0.01f, 10.0f, 0.1f, 1.0f) == particleTwinkleScale(7, 0.09f, 10.0f, 0.1f, 1.0f));
    // A fixed range (most emitters author 1 / 1) leaves the size alone.
    CHECK(particleTwinkleScale(7, 0.3f, 10.0f, 1.0f, 1.0f) == 1.0f);
}

TEST_CASE("Pre-WotLK playable animation lookup decides what Stand shows", "[m2][animation]") {
    // world/goober/g_cage02.m2: Open, Opened, Close, Closed and no Stand; its
    // lookup shows Closed (147) for Stand, so the doodad stands shut.
    std::vector<uint8_t> bytes(1024, 0);
    std::memcpy(bytes.data(), "MD20", 4);
    const auto put32 = [&](size_t at, uint32_t value) { std::memcpy(bytes.data() + at, &value, 4); };
    const auto put16 = [&](size_t at, uint16_t value) { std::memcpy(bytes.data() + at, &value, 2); };
    put32(4, 261);
    put32(0x2C, 1);    // playable lookup: one entry (Stand)
    put32(0x30, 600);
    put16(600, 147);
    put16(602, 0);
    const auto model = wowee::pipeline::M2Loader::load(bytes);
    REQUIRE(model.playableAnimations.size() == 1);
    CHECK(model.playableAnimations[0].fallbackId == 147);
    CHECK(model.playableAnimations[0].flags == 0);

    using wowee::pipeline::M2Sequence;
    std::vector<M2Sequence> cage(4);
    cage[0].id = 148; cage[1].id = 149; cage[2].id = 146; cage[3].id = 147;
    CHECK(wowee::pipeline::standSequenceIndex(cage, model.playableAnimations) == 3);
    // G_BrazierTroll lists Destroy first: Stand is still its Stand.
    std::vector<M2Sequence> brazier(3);
    brazier[0].id = 150; brazier[1].id = 0; brazier[2].id = 153;
    CHECK(wowee::pipeline::standSequenceIndex(brazier, {{0, 0}}) == 1);
    // Neither Stand nor a lookup (Wrath layout): the first sequence.
    CHECK(wowee::pipeline::standSequenceIndex(cage, {}) == 0);
}

TEST_CASE("Plane emitters measure azimuth from +Y so Hurricane's cloud lies horizontal", "[m2][particle]") {
    using wowee::rendering::planeParticleDirection;
    // Zero inclination stays along +Z regardless of azimuth.
    CHECK(planeParticleDirection(0.0f, 2.1f) == glm::vec3(0.0f, 0.0f, 1.0f));
    // At horizontal range 0, births fan in the emitter bone's YZ plane (X == 0).
    // LightningStorm_Cloud_State.m2 #0 (Hurricane 16914) authors vertical pi,
    // horizontal 0 on bone 3, whose rotation maps Y and Z horizontal and +X
    // straight up +Z.
    const glm::quat bone3Rot = glm::normalize(glm::quat(0.328f, -0.638f, -0.305f, -0.626f));
    for (const float inc : {-3.14159265f, -1.57079633f, -0.7f, 0.0f, 1.1f, 2.4f}) {
        const glm::vec3 localDir = planeParticleDirection(inc, 0.0f);
        CHECK(localDir.x == Catch::Approx(0.0f).margin(1e-6f));
        CHECK(glm::length(localDir) == Catch::Approx(1.0f));
        const glm::vec3 worldDir = bone3Rot * localDir;
        CHECK(std::abs(worldDir.z) < 0.06f);
    }
}

TEST_CASE("Discrete Squirt (0x8000) emitters fire keyframe bursts once and stay silent on held frames", "[m2][particle]") {
    using wowee::rendering::forEachSquirtBurst;
    // Blizzard_Impact_Base.m2 #3/#4: Stand [33..3333] (timeOffset = 33),
    // discrete rate [(33, 0), (800, 35), (933, 0)],
    // EnabledIn [(33, 0), (800, 1), (900, 0)].
    wowee::pipeline::M2AnimationTrack rate, enabled;
    rate.interpolationType = 0;
    rate.sequences.resize(1);
    rate.sequences[0].timeOffset = 33;
    rate.sequences[0].timestamps = {33, 800, 933};
    rate.sequences[0].floatValues = {0.0f, 35.0f, 0.0f};

    enabled.interpolationType = 0;
    enabled.sequences.resize(1);
    enabled.sequences[0].timeOffset = 33;
    enabled.sequences[0].timestamps = {33, 800, 900};
    enabled.sequences[0].floatValues = {0.0f, 1.0f, 0.0f};

    auto countStep = [&](float prevMs, float currMs, bool startBoundary, float dtSec) {
        uint32_t total = 0;
        forEachSquirtBurst(rate, enabled, 0, prevMs, currMs, 3300.0f, startBoundary,
                           0.0f, {}, 1.0f, dtSec,
                           [&](uint32_t n, float age, float) {
                               CHECK(age >= 0.0f);
                               CHECK(age <= dtSec);
                               total += n;
                           });
        return total;
    };

    // Initial step and falling shard phase (0 -> 760 ms local): nothing emitted.
    CHECK(countStep(0.0f, 33.0f, true, 0.033f) == 0);
    CHECK(countStep(33.0f, 760.0f, false, 0.727f) == 0);
    // Crossing local 767 ms (global 800 ms) fires all 35 particles once, even if
    // the frame ends at 880 ms (global 913 ms, past EnabledIn's 900 ms turn-off).
    CHECK(countStep(760.0f, 880.0f, false, 0.120f) == 35);
    // Remaining Stand duration and held final frame emit nothing.
    CHECK(countStep(880.0f, 3300.0f, false, 2.420f) == 0);
    CHECK(countStep(3300.0f, 3300.0f, false, 0.033f) == 0);

    // DecisiveStrike_Impact_Chest #0 disables EnabledIn at 3400 ms where key 50 sits.
    wowee::pipeline::M2AnimationTrack disabledAtKey;
    disabledAtKey.interpolationType = 0;
    disabledAtKey.sequences.resize(1);
    disabledAtKey.sequences[0].timeOffset = 3333;
    disabledAtKey.sequences[0].timestamps = {3333, 3400};
    disabledAtKey.sequences[0].floatValues = {1.0f, 0.0f};

    wowee::pipeline::M2AnimationTrack strikeRate;
    strikeRate.interpolationType = 0;
    strikeRate.sequences.resize(1);
    strikeRate.sequences[0].timeOffset = 3333;
    strikeRate.sequences[0].timestamps = {3333, 3400};
    strikeRate.sequences[0].floatValues = {0.0f, 50.0f};

    uint32_t suppressed = 0;
    forEachSquirtBurst(strikeRate, disabledAtKey, 0, 0.0f, 100.0f, 667.0f, true,
                       0.0f, {}, 1.0f, 0.100f,
                       [&](uint32_t n, float, float) { suppressed += n; });
    CHECK(suppressed == 0);
}

TEST_CASE("Plane emitter spans areaLength along Y and areaWidth along X", "[m2][particle]") {
    using wowee::rendering::planeParticleOffset;
    // elwynntallwaterfall01.m2: length=18.0 along the Y waterfall lip, width=1.37 along X.
    const glm::vec3 edge = planeParticleOffset(18.0f, 1.37f, 1.0f, 1.0f);
    CHECK(edge.x == Catch::Approx(0.685f));
    CHECK(edge.y == Catch::Approx(9.0f));
    CHECK(edge.z == Catch::Approx(0.0f));
    // Under bone 1's 66.5 deg Y rotation, the 18 yd lip stays horizontal in Y
    // rather than tilting +-8.25 yd into Z.
    const glm::mat4 rotY = glm::rotate(glm::mat4(1.0f), glm::radians(66.5f), glm::vec3(0, 1, 0));
    const glm::vec3 rotated = glm::vec3(rotY * glm::vec4(edge, 0.0f));
    CHECK(rotated.y == Catch::Approx(9.0f));
    CHECK(std::abs(rotated.z) < 0.7f);
}

TEST_CASE("Outdoor lit particles take upward ambient and diffuse lighting", "[m2][particle]") {
    using wowee::rendering::outdoorParticleLight;
    // Ammen Vale 10:12: dim cool ambient + sun diffuse from above (-lightDir.z > 0).
    const glm::vec3 lit = outdoorParticleLight(
        glm::vec3(0.25f, 0.40f, 0.53f),
        glm::vec3(0.79f, 0.63f, 0.44f),
        glm::normalize(glm::vec3(0.0f, 0.6f, -0.8f)));
    CHECK(lit.r == Catch::Approx(0.25f + 0.79f * 0.8f));
    CHECK(lit.g == Catch::Approx(0.40f + 0.63f * 0.8f));
    CHECK(lit.b == Catch::Approx(0.53f + 0.44f * 0.8f));
    // Sun below horizon contributes zero diffuse.
    const glm::vec3 night = outdoorParticleLight(
        glm::vec3(0.10f, 0.15f, 0.25f),
        glm::vec3(0.80f, 0.70f, 0.60f),
        glm::vec3(0.0f, 0.0f, 1.0f));
    CHECK(night.r == Catch::Approx(0.10f));
    CHECK(night.g == Catch::Approx(0.15f));
    CHECK(night.b == Catch::Approx(0.25f));
}

TEST_CASE("Continuous emitter stochastic accumulator averages half the authored rate", "[m2][particle]") {
    using wowee::rendering::particleContinuousEmissionIncrement;
    // d_smoke.m2 emitters 0/2 author rate=8/s, lifespan=12s.
    CHECK(particleContinuousEmissionIncrement(8.0f, 0.05f, 0.0f) == Catch::Approx(0.0f));
    CHECK(particleContinuousEmissionIncrement(8.0f, 0.05f, 0.5f) == Catch::Approx(0.2f));
    CHECK(particleContinuousEmissionIncrement(8.0f, 0.05f, 1.0f) == Catch::Approx(0.4f));

    // Over many frames at different frame rates, mean rate stays 0.5 * authored rate.
    for (const float dt : {1.0f / 30.0f, 1.0f / 60.0f}) {
        float accum = 0.0f;
        int emitted = 0;
        const int steps = static_cast<int>(1000.0f / dt);
        for (int i = 0; i < steps; ++i) {
            // Deterministic quasi-uniform sequence in [0, 1].
            const float u = static_cast<float>((i * 2654435761u) & 0xFFFFu) / 65535.0f;
            accum += particleContinuousEmissionIncrement(8.0f, dt, u);
            while (accum >= 1.0f) {
                accum -= 1.0f;
                ++emitted;
            }
        }
        const float measuredRate = static_cast<float>(emitted) / 1000.0f;
        CHECK(measuredRate == Catch::Approx(4.0f).margin(0.15f));
    }
}

TEST_CASE("Detached particle gravity stays world -Z under pitched bone attachment transforms", "[m2][particle]") {
    using namespace wowee::rendering;
    // Hand-attached fire precast (Fire_Precast_Hand.m2, flags 0x29, gravity -3.47)
    // and pitched breath effects (AcidLiquidBreath.m2, flags 0x229, gravity +2.78)
    // receive an attachment bone transform pitched 90 deg forward; detached gravity
    // must still accelerate vertically along world -Z (scaled when placement-scaled).
    const auto pitched = glm::scale(
        glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
        glm::vec3(1.5f));
    const glm::vec3 worldScaled = particleGravityDirection(0x29u, pitched, /*scalesWithPlacement=*/true);
    CHECK(worldScaled.x == Catch::Approx(0.0f).margin(1e-5f));
    CHECK(worldScaled.y == Catch::Approx(0.0f).margin(1e-5f));
    CHECK(worldScaled.z == Catch::Approx(-1.5f).margin(1e-5f));

    const glm::vec3 worldYards = particleGravityDirection(0x229u, pitched, /*scalesWithPlacement=*/false);
    CHECK(worldYards.x == Catch::Approx(0.0f).margin(1e-5f));
    CHECK(worldYards.y == Catch::Approx(0.0f).margin(1e-5f));
    CHECK(worldYards.z == Catch::Approx(-1.0f).margin(1e-5f));
}
