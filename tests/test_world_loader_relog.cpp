#include <catch_amalgamated.hpp>
#include "core/world_loader.hpp"

using wowee::core::WorldLoader;

TEST_CASE("WorldLoader::shouldCleanUpPreviousMap distinguishes relog from cross-map transfer",
          "[world_loader][relog][gameobject]") {
    SECTION("First login and relog after performLeaveWorld do not wipe incoming spawn queues") {
        REQUIRE_FALSE(WorldLoader::shouldCleanUpPreviousMap(WorldLoader::kNoLoadedMapId));
    }

    SECTION("Cross-map transfers from any loaded map ID tear down the previous map") {
        REQUIRE(WorldLoader::shouldCleanUpPreviousMap(0u));    // Eastern Kingdoms
        REQUIRE(WorldLoader::shouldCleanUpPreviousMap(1u));    // Kalimdor
        REQUIRE(WorldLoader::shouldCleanUpPreviousMap(530u));  // Outland
        REQUIRE(WorldLoader::shouldCleanUpPreviousMap(532u));  // Karazhan
    }
}
