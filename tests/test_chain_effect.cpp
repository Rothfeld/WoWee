// SpellChainEffects beam rules measured against build 8606
// (_experiments/20261118T1328Z.hydross_beam_scroll in the workspace root).
#include <catch_amalgamated.hpp>

#include "rendering/chain_effect.hpp"
#include "rendering/spell_launch_origin.hpp"
#include "tbc_data_test_util.hpp"

#include <cmath>
#include <cstring>
#include <unordered_set>

using wowee::rendering::CastBeamTiming;
using wowee::rendering::castBeamTiming;
using wowee::rendering::chainBlendSupported;
using wowee::rendering::chainComboStrands;
using wowee::rendering::chainEndTaper;
using wowee::rendering::chainEndTexV;
using wowee::rendering::chainNoiseRadius;
using wowee::rendering::chainNoiseRedrawsPerFrame;
using wowee::rendering::chainNoiseStep;
using wowee::rendering::chainPulse;
using wowee::rendering::chainSegmentCount;
using wowee::rendering::chainTexCoord;
using wowee::rendering::chainUsesFlicker;
using wowee::rendering::chainUsesPulse;
using wowee::rendering::chainWaveOffset;
using wowee::rendering::ChainJoints;
using wowee::rendering::chainJointOffsetAt;
using wowee::rendering::chainJointRadius;
using wowee::rendering::chainResample;

namespace {
// Copies of the texture over a beam at time t.
float repeats(float length, float scale) {
    return std::abs(chainTexCoord(length, length, scale, 0.0f) - chainTexCoord(0.0f, length, scale, 0.0f));
}
// Texture copies a second passing a fixed point (the temporal frequency).
float hertz(float scale) {
    return std::abs(chainTexCoord(3.0f, 10.0f, scale, 1.0f) - chainTexCoord(3.0f, 10.0f, scale, 0.0f));
}
// Beam lengths a second the pattern moves, + toward the target.
float travel(float length, float scale) {
    const float s0 = 0.3f * length;
    const float u = chainTexCoord(s0, length, scale, 0.0f);
    // where the same u is one second later
    const float du_ds = chainTexCoord(1.0f, length, scale, 1.0f) - chainTexCoord(0.0f, length, scale, 1.0f);
    const float s1 = s0 + (u - chainTexCoord(s0, length, scale, 1.0f)) / du_ds;
    return (s1 - s0) / length;
}
}  // namespace

TEST_CASE("ManaBeam (-2): one copy over the beam, 2 lengths/s toward the caster at any length", "[chain]") {
    // Native chain 4 at 24 yd (_experiments/20261118T221857Z.chain_rate):
    // longest period = beam length at 2.0 Hz, two-wave crests at 4.0 Hz.
    for (float length : {8.0f, 24.0f, 47.0f}) {
        CHECK(repeats(length, -2.0f) == Catch::Approx(1.0f));
        CHECK(travel(length, -2.0f) == Catch::Approx(-2.0f));
    }
    CHECK(hertz(-2.0f) == Catch::Approx(2.0f));
}

TEST_CASE("SoulBeam chain 30 (+2) and WhiteBeam (1.66) run toward the target", "[chain]") {
    // Chain 30 native: 2.0 Hz, 2 lengths/s toward the target. WhiteBeam at
    // 28 yd: crests (two per copy) 3.34 Hz, 1.59 lengths/s.
    CHECK(repeats(25.0f, 2.0f) == Catch::Approx(1.0f));
    CHECK(travel(25.0f, 2.0f) == Catch::Approx(2.0f));
    CHECK(2.0f * hertz(1.66f) == Catch::Approx(3.32f));
    CHECK(travel(28.0f, 1.66f) == Catch::Approx(1.66f));
}

TEST_CASE("A zero TexCoordScale beam stands still", "[chain]") {
    CHECK(hertz(0.0f) == Catch::Approx(0.0f));
    CHECK(repeats(20.0f, 0.0f) == Catch::Approx(1.0f));
}

TEST_CASE("Combo decodes exact 9-bit row IDs: bit 0 in b3&0x10, bit 7 in b3&0x02, bit 8 in b3&0x20", "[chain]") {
    // Chain 215 (Sunshatter, Black Temple door channel): 211, 208, 209x3, 216x3.
    const std::string sunshatter("S\x01\x01\x13Q\x01\x01\x03Q\x01\x01\x13Q\x01\x01\x13Q\x01\x01\x13"
                                 "Y\x01\x01\x03Y\x01\x01\x03Y\x01\x01\x03", 32);
    CHECK(chainComboStrands(sunshatter, 215) == std::vector<uint32_t>{211, 208, 209, 209, 209, 216, 216, 216});
    // Chain 237 (Blades of Azzinoth's Tear of Azzinoth Summon Channel): 238, 239.
    CHECK(chainComboStrands(std::string("o\x01\x01\x03o\x01\x01\x13", 8), 237) == std::vector<uint32_t>{238, 239});
    // Chain 292 (Fel Crystal Visual, even 0x124): '%' (0x25) + 0x21 (bit 0 = 0, bit 8 = 1) -> 292.
    CHECK(chainComboStrands(std::string("%\x01\x01!", 4), 292) == std::vector<uint32_t>{292});
    // Chain 314 (Fel Consumption, even 0x13a): ';' (0x3b) + 0x21 -> 314, 314.
    CHECK(chainComboStrands(std::string(";\x01\x01!;\x01\x01!", 8), 314) == std::vector<uint32_t>{314, 314});
    // Chain 377 (Parhelion Plaza gazebo): '{' (0x7b) + 0x21 -> 378 (even), '{' + 0x31 -> 379 (odd).
    CHECK(chainComboStrands(std::string("{\x01\x01!{\x01\x01" "1", 8), 377) == std::vector<uint32_t>{378, 379});
    // Chain 411 (Open Portal from Shattrath, odd 0x19b): 0x1b + 0x33 -> 411.
    CHECK(chainComboStrands(std::string("\x1b\x01\x01" "3", 4), 411) == std::vector<uint32_t>{411});
    // Chain 48: '1' (0x31) + 0x01 (bit 0 = 0) -> 48.
    CHECK(chainComboStrands(std::string("1\x01\x01\x01", 4), 48) == std::vector<uint32_t>{48});
    CHECK(chainComboStrands("", 1).empty());
}

TEST_CASE("Wave offset vanishes at both endpoints and peaks inside", "[chain]") {
    // Parhelion Plaza chain 377: WaveHeight=1.70, WaveFreq=0.1571, WaveSpeed=-2.0.
    CHECK(chainWaveOffset(0.0f, 15.0f, 1.70f, 0.1571f, -2.0f, 0.4f) == Catch::Approx(0.0f).margin(1e-6f));
    CHECK(chainWaveOffset(15.0f, 15.0f, 1.70f, 0.1571f, -2.0f, 0.4f) == Catch::Approx(0.0f).margin(1e-6f));
    // At midpoint envelope == 1.0, full sine amplitude applies.
    CHECK(chainWaveOffset(5.0f, 10.0f, 2.0f, 0.0f, -1.5707963f, 1.0f) == Catch::Approx(2.0f).margin(1e-4f));
    CHECK(chainWaveOffset(5.0f, 10.0f, 0.0f, 1.0f, 2.0f, 0.5f) == 0.0f);
}

TEST_CASE("Flicker requires Flags 0x10 and Pulse requires Flags 0x20; BlendModes 2/3/4/6 supported", "[chain]") {
    // Fel Lightning (chain 297, Flags 0x46) and Forked Lightning (chain 95, Flags 0x46):
    // non-zero flicker durations without 0x10 must NOT flicker.
    CHECK_FALSE(chainUsesFlicker(0x46u, 0.37f, 0.45f));
    CHECK_FALSE(chainUsesPulse(0x46u, 150.0f));
    // Parhelion Plaza primary strand (chain 377, Flags 0x48): neither pulses nor flickers.
    CHECK_FALSE(chainUsesFlicker(0x48u, 0.66f, 0.55f));
    CHECK_FALSE(chainUsesPulse(0x48u, -5.5f));
    // Parhelion Plaza combo strands (378/379, Flags 0x68): pulse enabled, flicker off.
    CHECK(chainUsesPulse(0x68u, -15.5f));
    CHECK_FALSE(chainUsesFlicker(0x68u, 0.66f, 0.55f));
    // Open Portal from Shattrath (chain 411, Flags 0x36): both 0x10 and 0x20 set.
    CHECK(chainUsesFlicker(0x36u, 0.50f, 0.65f));
    CHECK(chainUsesPulse(0x36u, 100.0f));

    CHECK(chainBlendSupported(2));
    CHECK(chainBlendSupported(3));
    CHECK(chainBlendSupported(4));
    CHECK(chainBlendSupported(6));
    CHECK_FALSE(chainBlendSupported(0));
    CHECK_FALSE(chainBlendSupported(5));
}

TEST_CASE("Pulse: Fel Consumption's lit window runs from the target to the caster and comes again", "[chain]") {
    // Chain 314: -13 yd/s, 3.808 on, 1.8 fade, over 24 yd.
    const float speed = -13.0f, on = 3.808f, fade = 1.8f, length = 24.0f;
    const float period = (length + on + 2.0f * fade) / 13.0f;
    // Before the window enters, the beam is dark; it enters at the target end.
    CHECK(chainPulse(23.9f, length, speed, on, fade, 0.0f) == Catch::Approx(0.0f).margin(0.07f));
    const float t = 3.0f / 13.0f;  // head 1.2 yd in from the target
    CHECK(chainPulse(length - 1.0f, length, speed, on, fade, t) == Catch::Approx(1.0f));
    CHECK(chainPulse(1.0f, length, speed, on, fade, t) == 0.0f);
    // Mid-way the lit stretch is OnLength long with fades on both sides.
    const float mid = (fade + 14.0f) / 13.0f;  // head 14 yd from the target, s = 10
    CHECK(chainPulse(10.0f, length, speed, on, fade, mid) == Catch::Approx(1.0f));
    CHECK(chainPulse(10.0f + on, length, speed, on, fade, mid) == Catch::Approx(1.0f));
    CHECK(chainPulse(10.0f + on + fade / 2, length, speed, on, fade, mid) == Catch::Approx(0.5f));
    CHECK(chainPulse(10.0f - fade / 2, length, speed, on, fade, mid) == Catch::Approx(0.5f));
    CHECK(chainPulse(3.0f, length, speed, on, fade, mid) == 0.0f);
    // One period later the same.
    CHECK(chainPulse(10.0f, length, speed, on, fade, mid + period) == Catch::Approx(1.0f).margin(1e-3f));
    // A positive speed starts at the caster.
    CHECK(chainPulse(1.0f, length, 13.0f, on, fade, t) == Catch::Approx(1.0f));
}

TEST_CASE("Noise keeps JointSmoothness of its offset each frame", "[chain]") {
    // Chain 4 (0.75): 0.75 frame to frame; stationary rms 0.38 of the draw's.
    CHECK(chainNoiseStep(1.0f, 0.0f, 0.75f) == Catch::Approx(0.75f));
    CHECK(chainNoiseStep(0.0f, 1.0f, 0.75f) == Catch::Approx(0.25f));
    CHECK(chainNoiseStep(0.4f, -1.0f, 0.0f) == Catch::Approx(-1.0f));
    const float var = 0.25f * 0.25f / (1.0f - 0.75f * 0.75f);
    CHECK(std::sqrt(var) == Catch::Approx(0.378f).margin(1e-3f));
}

TEST_CASE("Noise strays by NoiseScale of the beam length", "[chain]") {
    CHECK(chainNoiseRadius(0.01f, 28.0f) == Catch::Approx(0.28f));
    CHECK(chainNoiseRadius(0.0f, 28.0f) == 0.0f);
}

TEST_CASE("Per-frame noise smoothing requires Flags 0x4; Flags 0x8 Shahraz black strands hold full jagged noise", "[chain]") {
    // Chain Lightning (1, Flags 0x4), Blue Beam (4, Flags 0x4), Drain Life (8, Flags 0x44): redraw per frame.
    CHECK(chainNoiseRedrawsPerFrame(0x4u));
    CHECK(chainNoiseRedrawsPerFrame(0x44u));
    CHECK(chainNoiseRedrawsPerFrame(0x6cu));
    // Mother Shahraz black strands (173/176/177/179, Flags 0x8): hold full-amplitude jagged noise.
    CHECK_FALSE(chainNoiseRedrawsPerFrame(0x8u));
    CHECK_FALSE(chainNoiseRedrawsPerFrame(0x0u));
}

TEST_CASE("Every third joint is minor, every twelfth major (Akil'zon's Electrical Storm 279)", "[chain]") {
    const ChainJoints j{.count = 27, .perMinor = 3, .minorPerMajor = 4, .radius = 0.07f, .minorScale = 3.7f, .majorScale = 8.45f};
    CHECK(chainJointRadius(j, 1) == Catch::Approx(0.07f));
    CHECK(chainJointRadius(j, 3) == Catch::Approx(0.259f));
    CHECK(chainJointRadius(j, 12) == Catch::Approx(0.5915f));
    CHECK(chainJointRadius(j, 24) == Catch::Approx(0.5915f));
    CHECK(chainJointRadius(j, 27) == Catch::Approx(0.259f));
    const ChainJoints plain{.count = 27, .radius = 0.145f};  // Sunshatter 215: no minor/major
    CHECK(chainJointRadius(plain, 12) == Catch::Approx(0.145f));
}

TEST_CASE("Joint offsets are joined by straight lines and vanish at the ends", "[chain]") {
    const std::vector<float> offsets{1.0f, -1.0f, 0.5f};  // joints at 1/4, 2/4, 3/4
    CHECK(chainJointOffsetAt(offsets, 0.0f) == Catch::Approx(0.0f));
    CHECK(chainJointOffsetAt(offsets, 0.125f) == Catch::Approx(0.5f));
    CHECK(chainJointOffsetAt(offsets, 0.25f) == Catch::Approx(1.0f));
    CHECK(chainJointOffsetAt(offsets, 0.375f) == Catch::Approx(0.0f));
    CHECK(chainJointOffsetAt(offsets, 0.75f) == Catch::Approx(0.5f));
    CHECK(chainJointOffsetAt(offsets, 1.0f) == Catch::Approx(0.0f));
    CHECK(chainJointOffsetAt(std::vector<float>{}, 0.5f) == 0.0f);
}

TEST_CASE("A new segment count keeps the beam's bends where they were", "[chain]") {
    // Ends 0, one bend of 1 at the middle of 4 segments.
    const std::vector<float> offsets{0.0f, 0.5f, 1.0f, 0.5f, 0.0f};
    const auto more = chainResample(offsets, 9);
    CHECK(more.front() == 0.0f);
    CHECK(more.back() == 0.0f);
    CHECK(more[4] == Catch::Approx(1.0f));
    CHECK(more[2] == Catch::Approx(0.5f));
    CHECK(chainResample(offsets, 5) == offsets);
}

TEST_CASE("CastKit beam timing uses SpellChainEffects SegDuration and SegDelay", "[chain]") {
    // Chain Heal (row 2): SegDuration 2000 ms, SegDelay 200 ms.
    const auto ch0 = castBeamTiming(2000, 200, 1.0f, 0, true);
    CHECK(ch0.delay == Catch::Approx(0.2f));
    CHECK(ch0.duration == Catch::Approx(2.0f));
    const auto ch1 = castBeamTiming(2000, 200, 1.0f, 1, true);
    CHECK(ch1.delay == Catch::Approx(0.4f));
    CHECK(ch1.duration == Catch::Approx(2.0f));

    // Chain Lightning (row 1): SegDuration 1000 ms, SegDelay 300 ms.
    const auto cl0 = castBeamTiming(1000, 300, 1.0f, 0, true);
    CHECK(cl0.delay == Catch::Approx(0.3f));
    CHECK(cl0.duration == Catch::Approx(1.0f));

    // Forked Lightning (row 95): SegDuration 1000 ms, SegDelay 0 ms, fan-out (not chained).
    const auto fl = castBeamTiming(1000, 0, 1.0f, 2, false);
    CHECK(fl.delay == Catch::Approx(0.0f));
    CHECK(fl.duration == Catch::Approx(1.0f));

    // When SegDuration is 0, fall back to the kit model/spell duration.
    const auto fb = castBeamTiming(0, 0, 1.5f, 0, false);
    CHECK(fb.delay == Catch::Approx(0.0f));
    CHECK(fb.duration == Catch::Approx(1.5f));
}

TEST_CASE("Shipped TBC SpellChainEffects.dbc carries Chain Heal 2000 ms / 200 ms and Chain Lightning 1000 ms / 300 ms",
          "[chain][tbc]") {
    const auto bytes = wowee::test::readTbcData("dbfilesclient/spellchaineffects.dbc");
    REQUIRE(bytes.size() >= 20);
    uint32_t nrec = 0, nfields = 0, recSize = 0;
    std::memcpy(&nrec, bytes.data() + 4, 4);
    std::memcpy(&nfields, bytes.data() + 8, 4);
    std::memcpy(&recSize, bytes.data() + 12, 4);
    REQUIRE(recSize == 173);
    REQUIRE(20 + static_cast<size_t>(nrec) * recSize <= bytes.size());

    auto findRow = [&](uint32_t targetId) -> std::pair<uint32_t, uint32_t> {
        for (uint32_t i = 0; i < nrec; ++i) {
            const uint8_t* r = bytes.data() + 20 + static_cast<size_t>(i) * recSize;
            uint32_t id = 0;
            std::memcpy(&id, r, 4);
            if (id == targetId) {
                uint32_t segDur = 0, segDel = 0;
                std::memcpy(&segDur, r + 5 * 4, 4);
                std::memcpy(&segDel, r + 6 * 4, 4);
                return {segDur, segDel};
            }
        }
        FAIL("SpellChainEffects row not found: " << targetId);
        return {0, 0};
    };

    const auto [clDur, clDel] = findRow(1);   // Chain Lightning
    CHECK(clDur == 1000);
    CHECK(clDel == 300);

    const auto [chDur, chDel] = findRow(2);   // Chain Heal
    CHECK(chDur == 2000);
    CHECK(chDel == 200);

    const auto [flDur, flDel] = findRow(95);  // Forked Lightning
    CHECK(flDur == 1000);
    CHECK(flDel == 0);

    const auto [spDur, spDel] = findRow(220); // Spite
    CHECK(spDur == 6499);
    CHECK(spDel == 300);

    const auto [felDur, felDel] = findRow(297); // Fel Lightning (Sunblade Protector 46480)
    CHECK(felDur == 1000);
    CHECK(felDel == 0);
}

TEST_CASE("All 320 shipped TBC SpellChainEffects.dbc rows have valid layout, textures, blend modes, and 0 missing Combo IDs",
          "[chain][tbc]") {
    const auto bytes = wowee::test::readTbcData("dbfilesclient/spellchaineffects.dbc");
    REQUIRE(bytes.size() >= 20);
    uint32_t nrec = 0, nfields = 0, recSize = 0, strSize = 0;
    std::memcpy(&nrec, bytes.data() + 4, 4);
    std::memcpy(&nfields, bytes.data() + 8, 4);
    std::memcpy(&recSize, bytes.data() + 12, 4);
    std::memcpy(&strSize, bytes.data() + 16, 4);
    REQUIRE(recSize == 173);
    REQUIRE(nfields == 47);
    const size_t strBase = 20 + static_cast<size_t>(nrec) * recSize;
    REQUIRE(strBase + strSize <= bytes.size());

    auto getStr = [&](uint32_t off) -> std::string_view {
        if (off >= strSize) return {};
        const char* p = reinterpret_cast<const char*>(bytes.data() + strBase + off);
        return std::string_view(p, strnlen(p, strSize - off));
    };

    std::unordered_set<uint32_t> allIds;
    allIds.reserve(nrec);
    for (uint32_t i = 0; i < nrec; ++i) {
        uint32_t id = 0;
        std::memcpy(&id, bytes.data() + 20 + static_cast<size_t>(i) * recSize, 4);
        allIds.insert(id);
    }

    size_t totalComboEntries = 0;
    for (uint32_t i = 0; i < nrec; ++i) {
        const uint8_t* r = bytes.data() + 20 + static_cast<size_t>(i) * recSize;
        uint32_t id = 0, texOff = 0, flags = 0, comboOff = 0;
        float avgSegLen = 0.0f, width = 0.0f, jointSmoothness = 0.0f;
        std::memcpy(&id, r, 4);
        std::memcpy(&avgSegLen, r + 1 * 4, 4);
        std::memcpy(&width, r + 2 * 4, 4);
        std::memcpy(&texOff, r + 7 * 4, 4);
        std::memcpy(&flags, r + 8 * 4, 4);
        std::memcpy(&jointSmoothness, r + 16 * 4, 4);
        std::memcpy(&comboOff, r + 161, 4);
        const uint8_t blendMode = r[160];

        INFO("SpellChainEffects row " << id);
        CHECK(std::isfinite(avgSegLen));
        CHECK(avgSegLen > 0.0f);
        CHECK(std::isfinite(width));
        CHECK(width > 0.0f);
        CHECK(jointSmoothness >= 0.0f);
        CHECK(jointSmoothness <= 1.0f);
        CHECK_FALSE(getStr(texOff).empty());
        CHECK(chainBlendSupported(blendMode));

        if (comboOff != 0) {
            const auto combo = getStr(comboOff);
            REQUIRE(combo.size() % 4 == 0);
            const auto strands = chainComboStrands(combo, id);
            totalComboEntries += strands.size();
            for (uint32_t targetId : strands) {
                INFO("Combo target " << targetId << " from row " << id);
                CHECK(allIds.count(targetId) == 1);
            }
            if (id == 189) {
                // Prophet Velen's Call Entropius (46818, visual 10476, kit 9601):
                // 3x 191 (BlendMode 6 ModAdd Beam_Purple), 4x 192 (cyan), 4x 193 (deep blue).
                CHECK(blendMode == 3);
                CHECK(strands == std::vector<uint32_t>{191, 191, 191, 192, 193, 193, 193, 192, 192, 192, 193});
            }
            if (id == 342) {
                // Felmyst Demonic Vapor (spell 45389): 7x 343 (pulsing purple/magenta arcing web strands).
                CHECK(strands == std::vector<uint32_t>{343, 343, 343, 343, 343, 343, 343});
            }
            if (id == 377) {
                // Parhelion Plaza crystal beam: 4x 378 + 5x 379.
                CHECK(strands == std::vector<uint32_t>{378, 378, 378, 378, 379, 379, 379, 379, 379});
            }
            if (id == 388) {
                // Shadowsword Soulbinder (spell 46442): BlendMode 4 (Mod) Beam_WebBlack + 3x 387 Beam_Purple.
                CHECK(blendMode == 4);
                CHECK(strands == std::vector<uint32_t>{387, 387, 387});
            }
        }
        if (id == 191) {
            // Prophet Velen's Call Entropius inner strand: BlendMode 6 (EGxBlend_ModAdd).
            CHECK(blendMode == 6);
            CHECK(wowee::rendering::chainBlendPipeline(blendMode) == wowee::rendering::ChainBlendPipeline::ModAdd);
        }
        if (id == 297) {
            // Fel Lightning (Sunblade Protector 46480): green ShockLightning, non-flickering (Flags 0x46).
            CHECK(flags == 0x46u);
            CHECK(r[157] == 126);
            CHECK(r[158] == 210);
            CHECK(r[159] == 0);
            CHECK(blendMode == 3);
            CHECK_FALSE(chainUsesFlicker(flags, 0.37f, 0.45f));
        }
    }
    CHECK(totalComboEntries == 245);
}

TEST_CASE("Chain beam endpoints taper to a point over the first and last segment with centred V", "[chain]") {
    // Chain Lightning (AvgSegLen 2.78): 7.5 yd -> 3 segments (4 vertices), 16 yd -> 6 segments (7 vertices).
    CHECK(chainSegmentCount(7.5f, 2.78f) == 3);
    CHECK(chainSegmentCount(16.0f, 2.78f) == 6);
    // Discombobulate (AvgSegLen 0.1): 10 yd -> 100 segments.
    CHECK(chainSegmentCount(10.0f, 0.1f) == 100);
    // Short melee cast or Eye Beam (AvgSegLen 100): at least 2 segments (3 vertices) so interior width exists.
    CHECK(chainSegmentCount(2.0f, 2.78f) == 2);
    CHECK(chainSegmentCount(20.0f, 100.0f) == 2);

    // 4-vertex strip (3 segments): pinched to 0 at both ends, full width inside.
    CHECK(chainEndTaper(0, 4) == 0.0f);
    CHECK(chainEndTaper(1, 4) == 1.0f);
    CHECK(chainEndTaper(2, 4) == 1.0f);
    CHECK(chainEndTaper(3, 4) == 0.0f);

    // 3-vertex strip (minimum 2 segments): midpoint keeps full width.
    CHECK(chainEndTaper(0, 3) == 0.0f);
    CHECK(chainEndTaper(1, 3) == 1.0f);
    CHECK(chainEndTaper(2, 3) == 0.0f);

    // Tapered tips sample texture centreline v = 0.5; interior vertices keep edge 0 / 1.
    CHECK(chainEndTexV(0, 4, 0) == 0.5f);
    CHECK(chainEndTexV(0, 4, 1) == 0.5f);
    CHECK(chainEndTexV(1, 4, 0) == 0.0f);
    CHECK(chainEndTexV(1, 4, 1) == 1.0f);
    CHECK(chainEndTexV(3, 4, 0) == 0.5f);
    CHECK(chainEndTexV(3, 4, 1) == 0.5f);

    // Degenerate 2-vertex strip does not collapse to zero area.
    CHECK(chainEndTaper(0, 2) == 1.0f);
    CHECK(chainEndTaper(1, 2) == 1.0f);
    CHECK(chainEndTexV(0, 2, 0) == 0.0f);
    CHECK(chainEndTexV(1, 2, 1) == 1.0f);
}

TEST_CASE("SpellVisual.dbc MissileAttachment 0..8 maps to SpellVisualKit M2 attachment IDs", "[chain]") {
    using wowee::rendering::missileAttachmentToM2Id;
    CHECK(missileAttachmentToM2Id(0) == 20);  // Head (Illidan Eye Blast 39908, Spotlight)
    CHECK(missileAttachmentToM2Id(1) == 34);  // Chest (Blood Siphon, Containment Beam)
    CHECK(missileAttachmentToM2Id(2) == 19);  // Base (Netherbeam)
    CHECK(missileAttachmentToM2Id(3) == 21);  // SpellLeftHand (Blue Beam, Ribbon Pole)
    CHECK(missileAttachmentToM2Id(4) == 22);  // SpellRightHand (Energy Surge)
    CHECK(missileAttachmentToM2Id(5) == 17);  // Breath (Chaos Gazer Drain Life 46291)
    CHECK(missileAttachmentToM2Id(6) == 23);  // Special1 (Beholder center eye: Eyebeam, Fiery Glare)
    CHECK(missileAttachmentToM2Id(7) == 24);  // Special2 (Beholder left eye: Arcane Glare, Dark Glare)
    CHECK(missileAttachmentToM2Id(8) == 25);  // Special3 (Beholder right eye: Shadow Glare)
    CHECK(missileAttachmentToM2Id(-1) == 34); // Fallback when passed directly
}

TEST_CASE("chainBlendPipeline maps TBC SpellChainEffects EGxBlend modes to distinct pipelines", "[chain]") {
    using wowee::rendering::ChainBlendPipeline;
    using wowee::rendering::chainBlendPipeline;
    CHECK(chainBlendPipeline(2) == ChainBlendPipeline::Alpha);
    CHECK(chainBlendPipeline(3) == ChainBlendPipeline::Add);
    CHECK(chainBlendPipeline(4) == ChainBlendPipeline::Mod);
    CHECK(chainBlendPipeline(6) == ChainBlendPipeline::ModAdd);
}

TEST_CASE("StateKit channel beam fallback requires non-zero CharParamOne", "[chain]") {
    using wowee::rendering::stateKitDrawsChannelBeam;
    // Shared Bonds 41363 (SpellVisual 8244, StateKit 7030): CharParamOne = 1.0 -> draws beam.
    CHECK(stateKitDrawsChannelBeam(1.0f));
    // Illidan Cage Trap Caged 40704..40713 (SpellVisual 9227, StateKit 7847): CharParamOne = 0.0 -> no beam.
    CHECK_FALSE(stateKitDrawsChannelBeam(0.0f));
}
