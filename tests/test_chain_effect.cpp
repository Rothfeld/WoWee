// SpellChainEffects beam rules measured against build 8606
// (_experiments/20261118T1328Z.hydross_beam_scroll in the workspace root).
#include <catch_amalgamated.hpp>

#include "rendering/chain_effect.hpp"
#include "tbc_data_test_util.hpp"

#include <cmath>
#include <cstring>

using wowee::rendering::CastBeamTiming;
using wowee::rendering::castBeamTiming;
using wowee::rendering::chainComboStrands;
using wowee::rendering::chainNoiseRadius;
using wowee::rendering::chainNoiseStep;
using wowee::rendering::chainPulse;
using wowee::rendering::chainTexCoord;
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

TEST_CASE("Combo names further rows, with ID bits 7 in the entry's last byte; 0x20 copies the row", "[chain]") {
    // Chain 215 (Sunshatter, Black Temple door channel).
    const std::string sunshatter("S\x01\x01\x13Q\x01\x01\x03Q\x01\x01\x13Q\x01\x01\x13Q\x01\x01\x13"
                                 "Y\x01\x01\x03Y\x01\x01\x03Y\x01\x01\x03", 32);
    CHECK(chainComboStrands(sunshatter, 215) == std::vector<uint32_t>{211, 209, 209, 209, 209, 217, 217, 217});
    // Chain 237 (Blades of Azzinoth's Tear of Azzinoth Summon Channel).
    CHECK(chainComboStrands(std::string("o\x01\x01\x03o\x01\x01\x13", 8), 237) == std::vector<uint32_t>{239, 239});
    // Chain 292 (Fel Crystal Visual): '%' with 0x21 spells 293 but natively
    // draws a second 292 strand and no green 293 lightning.
    CHECK(chainComboStrands(std::string("%\x01\x01!", 4), 292) == std::vector<uint32_t>{292});
    // Chain 314 (Fel Consumption): two more SoulBeam strands, no Beam_Purple 315.
    CHECK(chainComboStrands(std::string(";\x01\x01!;\x01\x01!", 8), 314) == std::vector<uint32_t>{314, 314});
    // Chain 48: below 128.
    CHECK(chainComboStrands(std::string("1\x01\x01\x01", 4), 48) == std::vector<uint32_t>{49});
    CHECK(chainComboStrands("", 1).empty());
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
}
