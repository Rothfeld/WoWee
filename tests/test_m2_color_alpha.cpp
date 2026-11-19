#include <catch_amalgamated.hpp>

#include "test_support.hpp"

#include "pipeline/m2_loader.hpp"
#include "rendering/m2_track_sampler.hpp"

#include <filesystem>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// The peasant lumberjack carry model (HumanMalePeasantWood) contains TWO wood
// bundle submeshes; M2 color-alpha animation keeps exactly one visible per
// animation (the second only appears while the bundle is dropped during the
// Death sequence). If the loader stops parsing these tracks, the character
// renderer draws both bundles at once - the "carrying two bundles of wood"
// bug. Both the WotLK (v264, array-of-arrays tracks) and vanilla/Turtle
// (v256, flat tracks with ranges) encodings must produce usable tracks.

using wowee::pipeline::M2Loader;
using wowee::pipeline::M2Model;

namespace {


// First alpha key of the color track for the given sequence index, or -1.
float firstAlpha(const M2Model& model, size_t colorIdx, size_t seqIdx) {
    if (colorIdx >= model.colorAlphaTracks.size()) return -1.0f;
    const auto& track = model.colorAlphaTracks[colorIdx];
    if (seqIdx >= track.sequences.size()) return -1.0f;
    if (track.sequences[seqIdx].floatValues.empty()) return -1.0f;
    return track.sequences[seqIdx].floatValues[0];
}

} // namespace

TEST_CASE("M2 track sampling respects discrete and linear interpolation", "[m2][track]") {
    wowee::pipeline::M2AnimationTrack track;
    track.sequences.resize(1);
    track.sequences[0].timestamps = {0, 100};
    track.sequences[0].floatValues = {0.0f, 1.0f};

    track.interpolationType = 0;
    CHECK(wowee::rendering::m2_track::sampleFloat(
              track, 0, 50.0f, 0.0f, {}, -1.0f) == Catch::Approx(0.0f));
    CHECK(wowee::rendering::m2_track::sampleFloat(
              track, 0, 100.0f, 0.0f, {}, -1.0f) == Catch::Approx(1.0f));

    track.interpolationType = 1;
    CHECK(wowee::rendering::m2_track::sampleFloat(
              track, 0, 50.0f, 0.0f, {}, -1.0f) == Catch::Approx(0.5f));
}

TEST_CASE("M2 global tracks use their independent wrapped clock", "[m2][track]") {
    wowee::pipeline::M2AnimationTrack track;
    track.interpolationType = 1;
    track.globalSequence = 0;
    track.sequences.resize(1);
    track.sequences[0].timestamps = {0, 1000};
    track.sequences[0].floatValues = {0.0f, 1.0f};

    CHECK(wowee::rendering::m2_track::sampleFloat(
              track, 7, 900.0f, 1250.0f, {1000}, -1.0f) == Catch::Approx(0.25f));
}

TEST_CASE("Sharpened Blade sparkle preserves authored pulse timing", "[m2][transparency]") {
    auto data = wowee::test::readFile("Data/spell/enchantments/sparkle_a.m2");
    if (data.empty()) {
        SUCCEED("model asset not extracted; optional real-asset coverage skipped");
        return;
    }
    M2Model model = M2Loader::load(data);
    REQUIRE(model.globalSequenceDurations.size() >= 1);
    CHECK(model.globalSequenceDurations[0] == 4000);
    REQUIRE(model.textureWeightLookup.size() == 1);
    CHECK(model.textureWeightLookup[0] == 0);
    REQUIRE(model.textureWeightTracks.size() == 1);

    const auto& pulse = model.textureWeightTracks[0];
    CHECK(pulse.interpolationType == 1);
    CHECK(pulse.globalSequence == 0);
    REQUIRE(pulse.sequences.size() == 1);
    CHECK(pulse.sequences[0].timestamps ==
          std::vector<uint32_t>{0, 333, 833, 3500, 4000});
    REQUIRE(pulse.sequences[0].floatValues.size() == 5);

    // The glint is briefly on, fades out, stays absent for most of the cycle,
    // then fades back in. Sampling this track prevents a permanent saturated
    // card and restores the original once-per-four-seconds cadence.
    CHECK(wowee::rendering::m2_track::sampleFloat(
              pulse, 0, 0.0f, 333.0f, model.globalSequenceDurations, -1.0f)
          == Catch::Approx(1.0f));
    CHECK(wowee::rendering::m2_track::sampleFloat(
              pulse, 0, 0.0f, 833.0f, model.globalSequenceDurations, -1.0f)
          == Catch::Approx(0.0f));
    CHECK(wowee::rendering::m2_track::sampleFloat(
              pulse, 0, 0.0f, 2000.0f, model.globalSequenceDurations, -1.0f)
          == Catch::Approx(0.0f));
}

TEST_CASE("WotLK peasant wood model parses per-animation color alpha", "[m2][color]") {
    auto data = wowee::test::readFile("Data/creature/humanmalepeasant/humanmalepeasantwood.m2");
    if (data.empty()) {
        SUCCEED("model asset not extracted; optional real-asset coverage skipped");
        return;
    }
    M2Model model = M2Loader::load(data);
    REQUIRE(model.version >= 264);

    REQUIRE(model.colorAlphas.size() == 2);
    REQUIRE(model.colorAlphaTracks.size() == 2);

    // At-rest values: primary bundle opaque, alternate bundle hidden.
    CHECK(model.colorAlphas[0] == Catch::Approx(1.0f).margin(0.01f));
    CHECK(model.colorAlphas[1] == Catch::Approx(0.0f).margin(0.01f));

    // Sequence 0 is Walk: the carried bundle shows, the dropped one does not.
    CHECK(firstAlpha(model, 0, 0) == Catch::Approx(1.0f).margin(0.01f));
    CHECK(firstAlpha(model, 1, 0) == Catch::Approx(0.0f).margin(0.01f));

    // The final sequence is Death, where the bundles swap (drop animation).
    const size_t deathSeq = model.sequences.size() - 1;
    REQUIRE(model.sequences[deathSeq].id == 1);
    const auto& dropTrack = model.colorAlphaTracks[1].sequences[deathSeq];
    REQUIRE_FALSE(dropTrack.floatValues.empty());
    CHECK(dropTrack.floatValues.back() == Catch::Approx(1.0f).margin(0.01f));
}

TEST_CASE("Vanilla peasant wood model parses color alpha ranges", "[m2][color]") {
    auto data = wowee::test::readFile(
        "Data/expansions/turtle/overlay/creature/humanmalepeasant/HumanMalePeasantWood.m2");
    if (data.empty()) {
        SUCCEED("turtle overlay asset not extracted; optional real-asset coverage skipped");
        return;
    }
    M2Model model = M2Loader::load(data);
    REQUIRE(model.version < 264);

    REQUIRE(model.colorAlphas.size() == 2);
    REQUIRE(model.colorAlphaTracks.size() == 2);

    // Vanilla stores one flat 1→0 (and 0→1) key pair shared across sequences.
    // The at-rest bake must still resolve: bundle 0 visible, bundle 1 hidden.
    CHECK(model.colorAlphas[0] == Catch::Approx(1.0f).margin(0.01f));
    CHECK(model.colorAlphas[1] == Catch::Approx(0.0f).margin(0.01f));

    // Both color tracks are step-interpolated, so the hidden bundle stays at
    // exactly 0 for the whole first key span (the renderer culls on <= 0.01).
    REQUIRE(model.colorAlphaTracks[1].interpolationType == 0);
    CHECK(firstAlpha(model, 1, 0) == Catch::Approx(0.0f).margin(0.01f));
}

TEST_CASE("Legacy collision boxes accept reversed authored corners", "[m2][bounds]") {
    std::vector<uint8_t> bytes(332, 0);
    std::memcpy(bytes.data(), "MD20", 4);
    const uint32_t version = 262;
    std::memcpy(bytes.data() + 4, &version, 4);
    const float corners[]{0.30556f, 0.30556f, 2.03128f, -0.30556f, -0.30556f, 0.0f};
    std::memcpy(bytes.data() + 208, corners, sizeof(corners));
    const auto model = M2Loader::load(bytes);
    CHECK(model.boundMin.z == 0.0f);
    CHECK(model.boundMax.z == Catch::Approx(2.03128f));
    CHECK(model.boundMin.x == Catch::Approx(-0.30556f));
    CHECK(model.boundMax.x == Catch::Approx(0.30556f));
}

TEST_CASE("Legacy particles preserve zero and tiny lifecycle endpoints", "[m2][particles]") {
    std::vector<uint8_t> bytes(332 + 504, 0);
    std::memcpy(bytes.data(), "MD20", 4);
    const uint32_t version = 260, count = 1, offset = 332;
    std::memcpy(bytes.data() + 4, &version, 4);
    std::memcpy(bytes.data() + 316, &count, 4);
    std::memcpy(bytes.data() + 320, &offset, 4);
    const float midpoint = 0.5f;
    std::memcpy(bytes.data() + offset + 0x14c, &midpoint, 4);
    const float scales[]{0.0f, 0.05555556f, 0.00002777778f};
    std::memcpy(bytes.data() + offset + 0x15c, scales, sizeof(scales));
    const auto model = M2Loader::load(bytes);
    REQUIRE(model.particleEmitters.size() == 1);
    const auto& curve = model.particleEmitters[0].particleScale;
    REQUIRE(curve.floatValues.size() == 3);
    CHECK(curve.floatValues[0] == 0.0f);
    CHECK(curve.floatValues[1] == Catch::Approx(scales[1]));
    CHECK(curve.floatValues[2] == Catch::Approx(scales[2]));
}

TEST_CASE("Legacy tracks retain inclusive closing and single keys", "[m2][track]") {
    std::vector<uint8_t> bytes(676, 0);
    std::memcpy(bytes.data(), "MD20", 4);
    const auto put = [&](size_t offset, uint32_t value) {
        std::memcpy(bytes.data() + offset, &value, 4);
    };
    put(4, 263);
    put(28, 2); put(32, 540); // Two animation windows.
    put(544, 1000); put(548, 1200);
    put(612, 2000); put(616, 2100);
    put(52, 1); put(56, 332); // One TBC bone.
    put(348, 0xffff0001); // Linear translation, no global sequence.
    put(352, 2); put(356, 444); // Two inclusive ranges.
    put(360, 4); put(364, 460); // Four timestamps.
    put(368, 4); put(372, 476); // Four vector keys.
    put(444, 0); put(448, 2);
    put(452, 3); put(456, 3);
    const uint32_t times[]{1000, 1100, 1200, 2000};
    const float keys[]{0,0,0, 1,0,0, 0,0,0, 5,0,0};
    std::memcpy(bytes.data() + 460, times, sizeof(times));
    std::memcpy(bytes.data() + 476, keys, sizeof(keys));
    const auto model = M2Loader::load(bytes);
    REQUIRE(model.bones.size() == 1);
    const auto& track = model.bones[0].translation;
    REQUIRE(track.sequences.size() == 2);
    CHECK(track.sequences[0].timestamps == std::vector<uint32_t>{1000, 1100, 1200});
    CHECK(track.sequences[0].timeOffset == 1000);
    REQUIRE(track.sequences[1].vec3Values.size() == 1);
    CHECK(track.sequences[1].vec3Values[0].x == 5.0f);
    CHECK(wowee::rendering::m2_track::sampleVec3(track, 0, 200, 0, {}, {}).x == 0.0f);
}

TEST_CASE("Legacy sparse tracks sample within their animation window", "[m2][track]") {
    using wowee::pipeline::M2AnimationTrack;
    using namespace wowee::rendering::m2_track;
    M2AnimationTrack track;
    track.interpolationType = 1;
    track.sequences.resize(1);
    auto& keys = track.sequences[0];
    keys.timeOffset = 30000;
    keys.timestamps = {3333, 31000, 32000};
    keys.vec3Values = {{0,0,0}, {1,0,0}, {2,0,0}};
    keys.floatValues = {0, 1, 2};
    keys.quatValues = {glm::quat(1,0,0,0), glm::quat(1,0,0,0), glm::quat(0,0,1,0)};
    CHECK(sampleVec3(track, 0, 2000, 0, {}, {}).x == 2.0f);
    CHECK(sampleFloat(track, 0, 2000, 0, {}, 0) == 2.0f);
    CHECK(sampleQuat(track, 0, 2000, 0, {}).y == Catch::Approx(1));
    CHECK(sampleFloat(track, 0, 0, 0, {}, 0) == Catch::Approx((30000.0f - 3333) / (31000 - 3333)));
}

TEST_CASE("Legacy M2Color parses animated RGB tracks for outer flame tinting", "[m2][color]") {
    // Flamestrike_Impact_Base.m2 color[0] animates from pale peach
    // (0.929, 0.741, 0.671) at 0 ms to vivid red (0.922, 0.020, 0.020) at
    // 1133 ms; reading only key 0 left the outer flame column pale yellow.
    constexpr uint32_t seqOfs = 332;
    constexpr uint32_t colorOfs = seqOfs + 68;     // 56 bytes in v260
    constexpr uint32_t tsOfs = colorOfs + 56;
    constexpr uint32_t rgbOfs = tsOfs + 8;
    std::vector<uint8_t> bytes(rgbOfs + 24, 0);
    std::memcpy(bytes.data(), "MD20", 4);
    const auto put32 = [&](size_t at, uint32_t v) { std::memcpy(bytes.data() + at, &v, 4); };
    const auto put16 = [&](size_t at, uint16_t v) { std::memcpy(bytes.data() + at, &v, 2); };
    put32(4, 260);
    put32(0x1C, 1); put32(0x20, seqOfs);           // 1 sequence [0..12000]
    put32(seqOfs + 8, 12000);
    put32(0x54, 1); put32(0x58, colorOfs);         // nColors = 1, ofsColors (v260 header)
    put16(colorOfs + 0, 1);                        // linear interpolation
    put16(colorOfs + 2, 0xFFFF);                   // no global sequence
    put32(colorOfs + 12, 2); put32(colorOfs + 16, tsOfs);
    put32(colorOfs + 20, 2); put32(colorOfs + 24, rgbOfs);
    const uint32_t times[2] = {0, 1133};
    const float keys[6] = {0.929f, 0.741f, 0.671f, 0.922f, 0.020f, 0.020f};
    std::memcpy(bytes.data() + tsOfs, times, sizeof(times));
    std::memcpy(bytes.data() + rgbOfs, keys, sizeof(keys));

    const M2Model model = M2Loader::load(bytes);
    REQUIRE(model.colorRGB.size() == 1);
    REQUIRE(model.colorRGBTracks.size() == 1);
    CHECK(model.colorRGB[0].g == Catch::Approx(0.741f));
    const auto& track = model.colorRGBTracks[0];
    const auto start = wowee::rendering::m2_track::sampleVec3(track, 0, 0.0f, 0.0f, {}, glm::vec3(1.0f));
    const auto mid = wowee::rendering::m2_track::sampleVec3(track, 0, 566.5f, 0.0f, {}, glm::vec3(1.0f));
    const auto red = wowee::rendering::m2_track::sampleVec3(track, 0, 1133.0f, 0.0f, {}, glm::vec3(1.0f));
    CHECK(start.g == Catch::Approx(0.741f));
    CHECK(mid.g == Catch::Approx(0.3805f).margin(0.001f));
    CHECK(red.r == Catch::Approx(0.922f));
    CHECK(red.g == Catch::Approx(0.020f));
    CHECK(red.b == Catch::Approx(0.020f));
}
