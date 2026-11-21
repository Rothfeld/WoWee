#include "catch_amalgamated.hpp"
#include "ui/screen_vignette.hpp"

using wowee::ui::computeLowHealthVignetteAlpha;
using wowee::ui::kLowHealthVignetteThreshold;
using wowee::ui::shouldTriggerDamageFlash;

TEST_CASE("Full-HP melee hit never triggers red screen-edge vignette", "[vignette]") {
    SECTION("50000 -> 49966 (99.93% HP) produces zero damage flash even if legacy enabled") {
        CHECK_FALSE(shouldTriggerDamageFlash(true, false, 50000, 49966, 50000));
        CHECK_FALSE(shouldTriggerDamageFlash(false, false, 50000, 49966, 50000));
        CHECK(computeLowHealthVignetteAlpha(true, false, 49966, 50000, 1.0) == 0);
        CHECK(computeLowHealthVignetteAlpha(true, false, 50000, 50000, 1.0) == 0);
    }

    SECTION("Mid-HP hit above 20% produces zero vignette") {
        CHECK_FALSE(shouldTriggerDamageFlash(true, false, 50000, 25000, 50000));
        CHECK_FALSE(shouldTriggerDamageFlash(true, false, 50000, 10001, 50000));
        CHECK(computeLowHealthVignetteAlpha(true, false, 10001, 50000, 0.2) == 0);
        CHECK(computeLowHealthVignetteAlpha(true, false, 10000, 50000, 0.2) == 0);
    }

    SECTION("Critically low HP (<= 20%) activates low-health vignette when alive") {
        CHECK(shouldTriggerDamageFlash(true, false, 12000, 9000, 50000));
        CHECK_FALSE(shouldTriggerDamageFlash(false, false, 12000, 9000, 50000));
        CHECK_FALSE(shouldTriggerDamageFlash(true, true, 12000, 9000, 50000));
        CHECK_FALSE(shouldTriggerDamageFlash(true, false, 9000, 9000, 50000));

        const int alphaLow = computeLowHealthVignetteAlpha(true, false, 5000, 50000, 0.2);
        CHECK(alphaLow > 0);
        CHECK(alphaLow <= 90);
        CHECK(computeLowHealthVignetteAlpha(false, false, 5000, 50000, 0.2) == 0);
        CHECK(computeLowHealthVignetteAlpha(true, true, 5000, 50000, 0.2) == 0);
        CHECK(computeLowHealthVignetteAlpha(true, false, 0, 50000, 0.2) == 0);
        CHECK(kLowHealthVignetteThreshold == Catch::Approx(0.20f));
    }
}
