// Corpse fade-out ahead of removal (ledger #75c), rendering/unit_fade.hpp.
#include <catch_amalgamated.hpp>

#include "rendering/unit_fade.hpp"

using wowee::rendering::appendUnitRenderSteps;
using wowee::rendering::attachedSpellVisualFade;
using wowee::rendering::creatureSpawnFadeDuration;
using wowee::rendering::effectiveUnitOpacity;
using wowee::rendering::fadeInOpacity;
using wowee::rendering::fadeOutOpacity;
using wowee::rendering::kKitOpacityTransitionSeconds;
using wowee::rendering::kitOpacityTransitionDuration;
using wowee::rendering::kitTransitionOpacity;
using wowee::rendering::kUnitDespawnFadeOutSeconds;
using wowee::rendering::kUnitFadeSeconds;
using wowee::rendering::kUnitSpawnFadeInSeconds;

TEST_CASE("a despawning corpse fades from its current opacity to zero", "[unit-fade]") {
    CHECK(fadeOutOpacity(1.0f, 0.0f, kUnitFadeSeconds) == 1.0f);
    CHECK(fadeOutOpacity(1.0f, kUnitFadeSeconds / 2, kUnitFadeSeconds) == Catch::Approx(0.5f));
    CHECK(fadeOutOpacity(1.0f, kUnitFadeSeconds, kUnitFadeSeconds) == 0.0f);
    CHECK(fadeOutOpacity(1.0f, 2 * kUnitFadeSeconds, kUnitFadeSeconds) == 0.0f);
    // A corpse still fading in, or a stealth-translucent one, starts lower.
    CHECK(fadeOutOpacity(0.4f, kUnitFadeSeconds / 2, kUnitFadeSeconds) == Catch::Approx(0.2f));
    CHECK(fadeOutOpacity(1.0f, 0.1f, 0.0f) == 0.0f);
}

TEST_CASE("a newly spawned unit fades in smoothly to its target display opacity", "[unit-fade]") {
    CHECK(kUnitSpawnFadeInSeconds == 2.0f);
    CHECK(kUnitDespawnFadeOutSeconds == 1.5f);
    CHECK(fadeInOpacity(1.0f, 0.0f, kUnitSpawnFadeInSeconds) == 0.0f);
    CHECK(fadeInOpacity(1.0f, 0.5f, kUnitSpawnFadeInSeconds) == Catch::Approx(0.0625f));
    CHECK(fadeInOpacity(1.0f, 1.0f, kUnitSpawnFadeInSeconds) == Catch::Approx(0.25f));
    CHECK(fadeInOpacity(1.0f, 1.5f, kUnitSpawnFadeInSeconds) == Catch::Approx(0.5625f));
    CHECK(fadeInOpacity(1.0f, 2.0f, kUnitSpawnFadeInSeconds) == 1.0f);
    CHECK(fadeInOpacity(1.0f, 3.0f, kUnitSpawnFadeInSeconds) == 1.0f);
    // Translucent displays (e.g. spirit models) cap at their authored display alpha.
    CHECK(fadeInOpacity(0.5f, 1.0f, kUnitSpawnFadeInSeconds) == Catch::Approx(0.125f));
    CHECK(fadeInOpacity(0.5f, 2.0f, kUnitSpawnFadeInSeconds) == Catch::Approx(0.5f));
    CHECK(fadeInOpacity(0.5f, 0.0f, 0.0f) == Catch::Approx(0.5f));
}

TEST_CASE("fade-out and fade-in share one duration", "[unit-fade]") {
    CHECK(kUnitFadeSeconds == 0.5f);
}

TEST_CASE("in-place creature display swaps do not fade in from zero", "[unit-fade][shapeshift]") {
    // Alive display swap (Serpent Form IN / OUT) and death display revert
    // (Druid of the Fang dying while transformed, even if originally spawned via CREATE_OBJECT2)
    // appear immediately at full display opacity on TBC 2.4.3 build 8606.
    CHECK(creatureSpawnFadeDuration(true, false) == 0.0f);
    CHECK(creatureSpawnFadeDuration(true, true) == 0.0f);
    CHECK(fadeInOpacity(1.0f, 0.0f, creatureSpawnFadeDuration(true, true)) == 1.0f);
    CHECK(fadeInOpacity(0.4f, 0.0f, creatureSpawnFadeDuration(true, false)) == Catch::Approx(0.4f));

    // Initial spawns retain 2.0 s (CREATE_OBJECT2) and 0.5 s (streaming into view).
    CHECK(creatureSpawnFadeDuration(false, true) == kUnitSpawnFadeInSeconds);
    CHECK(creatureSpawnFadeDuration(false, false) == kUnitFadeSeconds);
}

TEST_CASE("stealth and SpellVisualKit opacity eases cubically over 1 s both ways", "[unit-fade][stealth]") {
    // Entering Stealth (SpellVisualKit 312, CharProc 14 opacity = 0.3).
    CHECK(kKitOpacityTransitionSeconds == 1.0f);
    CHECK(kitTransitionOpacity(1.0f, 0.3f, 0.0f) == Catch::Approx(1.0f));
    CHECK(kitTransitionOpacity(1.0f, 0.3f, 0.5f) == Catch::Approx(1.0f + (0.3f - 1.0f) * 0.125f));
    CHECK(kitTransitionOpacity(1.0f, 0.3f, 1.0f) == Catch::Approx(0.3f));
    CHECK(kitTransitionOpacity(1.0f, 0.3f, 2.0f) == Catch::Approx(0.3f));

    // Leaving Stealth back to 1.0 uses the same cubic curve.
    CHECK(kitTransitionOpacity(0.3f, 1.0f, 0.0f) == Catch::Approx(0.3f));
    CHECK(kitTransitionOpacity(0.3f, 1.0f, 0.5f) == Catch::Approx(0.3f + (1.0f - 0.3f) * 0.125f));
    CHECK(kitTransitionOpacity(0.3f, 1.0f, 1.0f) == Catch::Approx(1.0f));

    // Combined opacity multiplies display/spawn alpha, close-camera fade, and kit opacity.
    CHECK(effectiveUnitOpacity(1.0f, 1.0f, 0.3f) == Catch::Approx(0.3f));
    CHECK(effectiveUnitOpacity(1.0f, 0.5f, 0.3f) == Catch::Approx(0.15f));
    CHECK(effectiveUnitOpacity(0.5f, 1.0f, 0.5f) == Catch::Approx(0.25f));
    CHECK(effectiveUnitOpacity(1.0f, 0.0f, 0.3f) == 0.0f);
}

TEST_CASE("already-stealthed unit entering view applies kit opacity immediately without becoming opaque first",
          "[unit-fade][stealth]") {
    // Initial aura sync on a newly created instance (or visual transfer across a
    // display swap) uses a 0 s kit opacity duration, while runtime aura changes
    // retain the 1.0 s cubic ease-in.
    CHECK(kitOpacityTransitionDuration(/*initialAppearanceSync=*/true) == 0.0f);
    CHECK(kitOpacityTransitionDuration(/*initialAppearanceSync=*/false) == kKitOpacityTransitionSeconds);

    const float initialKitOpacity =
        kitTransitionOpacity(1.0f, 0.3f, 0.0f, kitOpacityTransitionDuration(/*initialAppearanceSync=*/true));
    CHECK(initialKitOpacity == Catch::Approx(0.3f));

    // As the unit fades in over kUnitFadeSeconds (0.5 s), effective opacity rises
    // monotonically from 0.0 -> 0.3 and never peaks near 1.0 (opaque).
    float prev = 0.0f;
    for (float t : {0.0f, 0.1f, 0.25f, 0.4f, 0.5f, 0.75f, 1.0f}) {
        const float spawnAlpha = fadeInOpacity(1.0f, t, kUnitFadeSeconds);
        const float eff = effectiveUnitOpacity(spawnAlpha, 1.0f, initialKitOpacity);
        CHECK(eff >= prev - 1e-6f);
        CHECK(eff <= 0.3f + 1e-6f);
        prev = eff;
    }
    CHECK(prev == Catch::Approx(0.3f));
}

TEST_CASE("faded unit schedules whole-unit depth prepass before color across body and attachments", "[unit-fade][stealth]") {
    // Root character (10) with helm (11), shoulders (12, 13), weapons (14, 15), enchant (16).
    const std::vector<uint32_t> unit = {10, 11, 12, 13, 14, 15, 16};

    std::vector<std::pair<int, uint32_t>> solidSteps;
    appendUnitRenderSteps(solidSteps, unit, /*includeSolid=*/true, /*includeBlended=*/false);
    REQUIRE(solidSteps.size() == unit.size());
    for (size_t i = 0; i < unit.size(); ++i) {
        CHECK(solidSteps[i] == std::pair<int, uint32_t>{0, unit[i]});
    }

    std::vector<std::pair<int, uint32_t>> blendedSteps;
    appendUnitRenderSteps(blendedSteps, unit, /*includeSolid=*/false, /*includeBlended=*/true);
    REQUIRE(blendedSteps.size() == unit.size() * 2);
    // Every member runs its depth prepass (-1) before any member runs color (1),
    // and no member is scheduled more than once per pass.
    for (size_t i = 0; i < unit.size(); ++i) {
        CHECK(blendedSteps[i] == std::pair<int, uint32_t>{-1, unit[i]});
        CHECK(blendedSteps[unit.size() + i] == std::pair<int, uint32_t>{1, unit[i]});
    }
}

TEST_CASE("sustained unit-attached spell visuals inherit owner effective opacity", "[unit-fade][spell-visual]") {
    // Unliving Soldier (18498, display 17920 CreatureModelAlpha = 160/255) with
    // Phase In (33422, StateKit 6887 CharProc 14 = 0.50): Ritual_Arcane_PreCast_Base.m2
    // fades in from 0 -> 0.3137 over kUnitSpawnFadeInSeconds (2.0 s).
    const float displayAlpha = 160.0f / 255.0f;
    const float kitOpacity = 0.5f;
    const float effEarly = effectiveUnitOpacity(fadeInOpacity(displayAlpha, 0.5f, kUnitSpawnFadeInSeconds), 1.0f, kitOpacity);
    const float effSteady = effectiveUnitOpacity(fadeInOpacity(displayAlpha, 2.0f, kUnitSpawnFadeInSeconds), 1.0f, kitOpacity);
    CHECK(attachedSpellVisualFade(/*sustainedUnitVisual=*/true, /*ownerRoomHidden=*/false, effEarly) ==
          Catch::Approx(displayAlpha * 0.0625f * kitOpacity));
    CHECK(attachedSpellVisualFade(/*sustainedUnitVisual=*/true, /*ownerRoomHidden=*/false, effSteady) ==
          Catch::Approx(displayAlpha * kitOpacity));

    // Shadowform (16592, StateKit 3129 CharProc 14 = 0.65): Shadow_Precast_Med_Base.m2 scales by 0.65.
    CHECK(attachedSpellVisualFade(/*sustainedUnitVisual=*/true, /*ownerRoomHidden=*/false, 0.65f) ==
          Catch::Approx(0.65f));

    // One-shot Cast/Impact effects stay at full alpha unless room-hidden.
    CHECK(attachedSpellVisualFade(/*sustainedUnitVisual=*/false, /*ownerRoomHidden=*/false, 0.3f) == 1.0f);
    CHECK(attachedSpellVisualFade(/*sustainedUnitVisual=*/false, /*ownerRoomHidden=*/true, 0.3f) == 0.0f);
    CHECK(attachedSpellVisualFade(/*sustainedUnitVisual=*/true, /*ownerRoomHidden=*/true, 0.65f) == 0.0f);
}

TEST_CASE("newly spawned dynamic GameObjects fade in over 1.5s while static GOs appear immediately",
          "[unit-fade][gameobject][fishing]") {
    using namespace wowee::rendering;
    CHECK(gameObjectSpawnFadeDuration(/*newlySpawned=*/false) == Catch::Approx(0.0f));
    CHECK(gameObjectSpawnFadeDuration(/*newlySpawned=*/true) == Catch::Approx(kGameObjectFadeInSeconds));
    CHECK(kGameObjectFadeInSeconds == Catch::Approx(1.5f));

    CHECK(fadeInOpacity(1.0f, 0.0f, kGameObjectFadeInSeconds) == Catch::Approx(0.0f));
    CHECK(fadeInOpacity(1.0f, 0.75f, kGameObjectFadeInSeconds) == Catch::Approx(0.25f));
    CHECK(fadeInOpacity(1.0f, 1.5f, kGameObjectFadeInSeconds) == Catch::Approx(1.0f));
    CHECK(fadeOutOpacity(0.5f, 0.75f, kGameObjectFadeSeconds) == Catch::Approx(0.25f));
    CHECK(fadeOutOpacity(0.5f, 1.5f, kGameObjectFadeSeconds) == Catch::Approx(0.0f));
}

TEST_CASE("GM invisibility 37800 and Stealth 1784 kit opacities apply immediately on initial remote spawn",
          "[unit-fade][stealth][sidedisc6]") {
    // GM invisibility (.gm visible off -> Spell 37800 Transparency (50%), SpellVisual 8739,
    // StateKit 3450 CharProc 14 = 0.50) and Rogue Stealth (Spell 1784, SpellVisual 184,
    // StateKit 312 CharProc 14 = 0.30) must initialize at target opacity (0 s duration)
    // when spawnOnlinePlayer runs syncUnitAuraVisuals on creation.
    const float initDuration = kitOpacityTransitionDuration(/*initialAppearanceSync=*/true);
    CHECK(initDuration == 0.0f);
    const float gmInvisKit = kitTransitionOpacity(1.0f, 0.5f, 0.0f, initDuration);
    const float stealthKit = kitTransitionOpacity(1.0f, 0.3f, 0.0f, initDuration);
    CHECK(effectiveUnitOpacity(1.0f, 1.0f, gmInvisKit) == Catch::Approx(0.5f));
    CHECK(effectiveUnitOpacity(1.0f, 1.0f, stealthKit) == Catch::Approx(0.3f));
}
