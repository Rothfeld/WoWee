// SpellChainEffects beam rules measured against build 8606
// (_experiments/20261118T1328Z.hydross_beam_scroll in the workspace root).
#include <catch_amalgamated.hpp>

#include "rendering/chain_effect.hpp"

#include <cmath>

using wowee::rendering::chainComboStrands;
using wowee::rendering::chainNoiseRadius;
using wowee::rendering::chainTexCoord;

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

TEST_CASE("Combo names further rows by its entries' first byte", "[chain]") {
    // Chain 215 (Sunshatter, Black Temple door channel).
    const std::string combo("S\x01\x01\x13Q\x01\x01\x03Q\x01\x01\x13Q\x01\x01\x13Q\x01\x01\x13"
                            "Y\x01\x01\x03Y\x01\x01\x03Y\x01\x01\x03", 32);
    CHECK(chainComboStrands(combo) == std::vector<uint32_t>{83, 81, 81, 81, 81, 89, 89, 89});
    CHECK(chainComboStrands("").empty());
}

TEST_CASE("Noise strays by NoiseScale of the beam length", "[chain]") {
    CHECK(chainNoiseRadius(0.01f, 28.0f) == Catch::Approx(0.28f));
    CHECK(chainNoiseRadius(0.0f, 28.0f) == 0.0f);
}
