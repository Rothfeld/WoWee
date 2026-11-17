// Whether an M2 batch is alpha tested, which decides the pipeline it draws on.
//
// A batch whose texture carries no alpha channel was forced onto the cutout
// pipeline whenever its blend mode was 2 or higher. For alpha blend that is
// reasonable - blending by an alpha that is 1 everywhere draws solid, so
// cutting out at least keeps the artist's silhouette.
//
// For the additive modes it destroys the effect. Additive does not use alpha
// for transparency: black is what disappears, because adding zero changes
// nothing. Glow cards are authored exactly that way - an opaque texture,
// bright in the middle and black at the edges.
//
// The oracle is the model and the texture Orgrimmar's bonfire ships.
// orcpvpbonfirelarge material 0 is blend mode 4, and its texture
// GENERICGLOW_ALPHA_128.BLP reports alphaDepth 0 in its own header - named for
// an alpha it does not have. Forced to cutout, every texel passed the test and
// the flame rendered as a flat opaque grey disc behind the logs.
#include <catch_amalgamated.hpp>

#include "rendering/m2_blend_mode.hpp"

using wowee::rendering::m2BatchNeedsAlphaTest;
using wowee::rendering::m2BlendIsAdditive;
using wowee::rendering::m2UseAuthoredCull;

TEST_CASE("authored culling respects the material, transform and pass", "[m2]") {
    // The Storm Peaks snow trees in the Icecrown recording use flags=0,
    // blend 1 for their needles and blend 0 for their trunks.
    CHECK(m2UseAuthoredCull(true, false, true, true, 0, 1));
    CHECK(m2UseAuthoredCull(true, false, true, true, 0, 0));
    CHECK(m2UseAuthoredCull(true, false, true, true, 0x01, 1));
    CHECK_FALSE(m2UseAuthoredCull(false, false, true, true, 0, 1));
    CHECK_FALSE(m2UseAuthoredCull(true, true, true, true, 0, 1));
    CHECK_FALSE(m2UseAuthoredCull(true, false, false, true, 0, 1));
    CHECK_FALSE(m2UseAuthoredCull(true, false, true, false, 0, 1));
    // Grizzly Hills and Silverpine have foliage materials with the two-sided
    // flag set; other material bits must not mask it.
    CHECK_FALSE(m2UseAuthoredCull(true, false, true, true, 0x04, 1));
    CHECK_FALSE(m2UseAuthoredCull(true, false, true, true, 0x05, 1));
    CHECK_FALSE(m2UseAuthoredCull(true, false, true, true, 0, 2));
    CHECK_FALSE(m2UseAuthoredCull(true, false, true, true, 0, 3));
    CHECK_FALSE(m2UseAuthoredCull(true, false, true, true, 0, 256));
}

TEST_CASE("Authored effect meshes retain their material draw pass", "[m2]") {
    using wowee::rendering::m2BatchIsTransparent;
    // AshenvaleWisps has an opaque shrub-textured mesh and a separate emitter.
    for (uint8_t blend = 0; blend <= 6; ++blend) {
        CHECK(m2BatchIsTransparent(blend, true, true) == (blend >= 2));
        CHECK(m2BatchIsTransparent(blend, false, true) == (blend >= 2));
        CHECK(m2BatchIsTransparent(blend, true, false));
    }
}

TEST_CASE("the Orgrimmar bonfire's glow card is not alpha tested", "[m2]") {
    // Blend mode 4, no alpha channel: the case that showed it.
    CHECK_FALSE(m2BatchNeedsAlphaTest(4, false));
    // And it stays untested if the texture does happen to carry alpha.
    CHECK_FALSE(m2BatchNeedsAlphaTest(4, true));
}

TEST_CASE("no additive batch is alpha tested", "[m2]") {
    // Black is the transparent colour for these, whatever the texture holds.
    for (uint8_t mode : {uint8_t{3}, uint8_t{4}}) {
        INFO("blend mode " << int(mode));
        CHECK(m2BlendIsAdditive(mode));
        CHECK_FALSE(m2BatchNeedsAlphaTest(mode, false));
        CHECK_FALSE(m2BatchNeedsAlphaTest(mode, true));
    }
}

TEST_CASE("alpha key is tested where there is alpha to key on", "[m2]") {
    CHECK(m2BatchNeedsAlphaTest(1, true));
    // And not where there is none. Every texel passes a test against an alpha
    // that is 1 everywhere, so the test decides nothing - but it still puts
    // the batch on the cutout pipeline and its alpha-to-coverage. Tirisfal's
    // canopy trunks are drawn alpha-keyed over a DXT1 texture with no
    // punch-through block in it; the one of the seven whose trunk is plain
    // opaque is the one that renders right.
    CHECK_FALSE(m2BatchNeedsAlphaTest(1, false));
}

TEST_CASE("a blended batch with no alpha still falls back to cutout",
          "[m2]") {
    // The reason the rule existed. Blending by an alpha that is 1 everywhere
    // draws the quad solid, so testing gives the silhouette a chance.
    CHECK(m2BatchNeedsAlphaTest(2, false));
    // With a real alpha channel it blends, as authored.
    CHECK_FALSE(m2BatchNeedsAlphaTest(2, true));
}

TEST_CASE("opaque is never tested", "[m2]") {
    CHECK_FALSE(m2BatchNeedsAlphaTest(0, true));
    CHECK_FALSE(m2BatchNeedsAlphaTest(0, false));
    CHECK_FALSE(m2BlendIsAdditive(0));
    CHECK_FALSE(m2BlendIsAdditive(1));
    CHECK_FALSE(m2BlendIsAdditive(2));
}

TEST_CASE("the modulate modes keep the old fallback", "[m2]") {
    // They cover what is behind rather than adding to it, so a missing alpha
    // is the same problem it is for blending.
    CHECK(m2BatchNeedsAlphaTest(5, false));
    CHECK(m2BatchNeedsAlphaTest(6, false));
    CHECK_FALSE(m2BatchNeedsAlphaTest(5, true));
    CHECK_FALSE(m2BatchNeedsAlphaTest(6, true));
}
