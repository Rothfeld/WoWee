// SpellChainEffects beam rules measured against build 8606
// (_experiments/20261118T1328Z.hydross_beam_scroll in the workspace root).
#include <catch_amalgamated.hpp>

#include "rendering/chain_effect.hpp"

#include <cmath>

using wowee::rendering::chainComboStrands;
using wowee::rendering::chainNoiseRadius;
using wowee::rendering::chainTexCoord;
using wowee::rendering::ChainJoints;
using wowee::rendering::chainJointOffsetAt;
using wowee::rendering::chainJointRadius;

namespace {
// Repeats of the texture over a beam at time t.
float repeats(float length, float scale, float textureLength) {
    return std::abs(chainTexCoord(length, length, scale, textureLength, 0.0f) -
                    chainTexCoord(0.0f, length, scale, textureLength, 0.0f));
}
// Texture cycles a second passing a fixed point (the temporal frequency).
float hertz(float scale, float textureLength) {
    return std::abs(chainTexCoord(3.0f, 10.0f, scale, textureLength, 1.0f) -
                    chainTexCoord(3.0f, 10.0f, scale, textureLength, 0.0f));
}
// Beam lengths a second the pattern moves, + toward the target.
float travel(float length, float scale, float textureLength) {
    const float s0 = 0.3f * length;
    const float u = chainTexCoord(s0, length, scale, textureLength, 0.0f);
    // where the same u is one second later
    const float du_ds = chainTexCoord(1.0f, length, scale, textureLength, 1.0f) -
                        chainTexCoord(0.0f, length, scale, textureLength, 1.0f);
    const float s1 = s0 + (u - chainTexCoord(s0, length, scale, textureLength, 1.0f)) / du_ds;
    return (s1 - s0) / length;
}
}  // namespace

TEST_CASE("ManaBeam (-2) repeats twice and runs 4 Hz toward the caster at any length", "[chain]") {
    for (float length : {8.0f, 28.0f, 47.0f}) {
        CHECK(repeats(length, -2.0f, 2.0f) == Catch::Approx(2.0f));
        CHECK(travel(length, -2.0f, 2.0f) == Catch::Approx(-2.0f));
    }
    CHECK(hertz(-2.0f, 2.0f) == Catch::Approx(4.0f));
}

TEST_CASE("WhiteBeam (1.66) repeats TextureLength times, not TexCoordScale", "[chain]") {
    // Native at 28 yd: 2.1 repeats, 3.34 Hz, 1.59 lengths/s toward the target.
    CHECK(repeats(28.0f, 1.66f, 2.0f) == Catch::Approx(2.0f));
    CHECK(hertz(1.66f, 2.0f) == Catch::Approx(3.32f));
    CHECK(travel(28.0f, 1.66f, 2.0f) == Catch::Approx(1.66f));
}

TEST_CASE("A zero TexCoordScale beam stands still", "[chain]") {
    CHECK(hertz(0.0f, 1.5f) == Catch::Approx(0.0f));
    CHECK(repeats(20.0f, 0.0f, 1.5f) == Catch::Approx(1.5f));
}

TEST_CASE("Combo names further rows, with ID bits 7 and 8 in the entry's last byte", "[chain]") {
    // Chain 215 (Sunshatter, Black Temple door channel).
    const std::string sunshatter("S\x01\x01\x13Q\x01\x01\x03Q\x01\x01\x13Q\x01\x01\x13Q\x01\x01\x13"
                                 "Y\x01\x01\x03Y\x01\x01\x03Y\x01\x01\x03", 32);
    CHECK(chainComboStrands(sunshatter) == std::vector<uint32_t>{211, 209, 209, 209, 209, 217, 217, 217});
    // Chain 237 (Blades of Azzinoth's Tear of Azzinoth Summon Channel).
    CHECK(chainComboStrands(std::string("o\x01\x01\x03o\x01\x01\x13", 8)) == std::vector<uint32_t>{239, 239});
    // Chain 302: bit 8.
    CHECK(chainComboStrands(std::string("-\x01\x01" "1", 4)) == std::vector<uint32_t>{301});
    // Chain 48: below 128.
    CHECK(chainComboStrands(std::string("1\x01\x01\x01", 4)) == std::vector<uint32_t>{49});
    CHECK(chainComboStrands("").empty());
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
