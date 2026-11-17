// Corpse fade-out ahead of removal (ledger #75c), rendering/unit_fade.hpp.
#include <catch_amalgamated.hpp>

#include "rendering/unit_fade.hpp"

using wowee::rendering::fadeOutOpacity;
using wowee::rendering::kUnitFadeSeconds;

TEST_CASE("a despawning corpse fades from its current opacity to zero", "[unit-fade]") {
    CHECK(fadeOutOpacity(1.0f, 0.0f, kUnitFadeSeconds) == 1.0f);
    CHECK(fadeOutOpacity(1.0f, kUnitFadeSeconds / 2, kUnitFadeSeconds) == Catch::Approx(0.5f));
    CHECK(fadeOutOpacity(1.0f, kUnitFadeSeconds, kUnitFadeSeconds) == 0.0f);
    CHECK(fadeOutOpacity(1.0f, 2 * kUnitFadeSeconds, kUnitFadeSeconds) == 0.0f);
    // A corpse still fading in, or a stealth-translucent one, starts lower.
    CHECK(fadeOutOpacity(0.4f, kUnitFadeSeconds / 2, kUnitFadeSeconds) == Catch::Approx(0.2f));
    CHECK(fadeOutOpacity(1.0f, 0.1f, 0.0f) == 0.0f);
}

TEST_CASE("fade-out and fade-in share one duration", "[unit-fade]") {
    CHECK(kUnitFadeSeconds == 0.5f);
}
