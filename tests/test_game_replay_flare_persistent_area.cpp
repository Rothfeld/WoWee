#include <catch_amalgamated.hpp>

#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "rendering/spell_visual_system.hpp"

#include <cstdlib>
#include <filesystem>

namespace {

std::filesystem::path resolveTbcDataDir() {
    if (const char* env = std::getenv("WOWEE_TBC_DATA_DIR"); env && *env) {
        std::filesystem::path p(env);
        if (std::filesystem::is_directory(p)) return p;
    }
    const std::filesystem::path fallback("/workspace/azerothcore/client/wowee-data/expansions/tbc");
    if (std::filesystem::is_directory(fallback)) return fallback;
    return {};
}

struct ScopedDbcLayout {
    wowee::pipeline::DBCLayout layout;
    const wowee::pipeline::DBCLayout* prev = nullptr;

    bool init() {
        prev = wowee::pipeline::getActiveDBCLayout();
        const auto path = std::filesystem::path(WOWEE_SOURCE_DIR) / "Data/expansions/tbc/dbc_layouts.json";
        if (!layout.loadFromJson(path.string())) return false;
        wowee::pipeline::setActiveDBCLayout(&layout);
        return true;
    }

    ~ScopedDbcLayout() {
        wowee::pipeline::setActiveDBCLayout(prev);
    }
};

}  // namespace

TEST_CASE("TBC 2.4.3 SpellVisualSystem defers Flare PersistentAreaKit until Missile_Flare lands and fades over 2.0 s",
          "[flare][spell_visual][tbc]") {
    const auto dataDir = resolveTbcDataDir();
    if (dataDir.empty()) {
        SKIP("TBC 2.4.3 data directory not available locally");
    }

    ScopedDbcLayout scopedLayout;
    REQUIRE(scopedLayout.init());

    wowee::pipeline::AssetManager assets;
    assets.setExpansionDataPath(dataDir.string());
    REQUIRE(assets.initialize(dataDir.string()));

    wowee::rendering::SpellVisualSystem svs;
    svs.setAssetManager(&assets);

    // SpellVisual.HasMissile (col 6): Flare (10387) and Blue Smoke Flare (10391)
    // have missiles, whereas Volley (10384, HasMissile=0, MissileModel=1603) does not.
    CHECK(svs.hasSpellMissile(10387u));
    CHECK(svs.hasSpellMissile(10391u));
    CHECK_FALSE(svs.hasSpellMissile(10384u));

    const glm::vec3 casterPos(-6000.0f, -3248.0f, 87.3f);
    const glm::vec3 flareDest(-6000.0f, -3263.0f, 86.16f);  // ~15 yd flight at 5.0 yd/s -> ~3.0 s
    constexpr uint64_t kDynGuid1 = 0xF100000000000007ull;
    constexpr uint64_t kDynGuid2 = 0xF100000000000008ull;

    SECTION("Order A: SMSG_SPELL_GO missile precedes DynamicObject creation") {
        REQUIRE(svs.playSpellMissile(10387u, casterPos, flareDest, /*speed=*/5.0f,
                                     /*launchDelay=*/0.6f, /*impactAtEnd=*/false,
                                     /*areaImpactAtEnd=*/true));

        // CMaNGOS spawns both DynamicObjects ~50-200 ms after SMSG_SPELL_GO.
        svs.update(0.10f);
        svs.playPersistentArea(kDynGuid1, 10387u, flareDest, 10.0f);
        svs.playPersistentArea(kDynGuid2, 10387u, flareDest, 10.0f);

        // Neither ground Flare_State_Base instance may spawn while Missile_Flare is in flight.
        CHECK(svs.pendingPersistentAreaCount() == 2u);
        CHECK(svs.activePersistentAreaCount() == 0u);
        CHECK(svs.activePersistentAreaInstanceCount() == 0u);

        // Advance mid-flight (t = 2.0 s < ~3.0 s landing): still deferred.
        for (int step = 0; step < 38; ++step) {
            svs.update(0.05f);
        }
        CHECK(svs.pendingPersistentAreaCount() == 2u);
        CHECK(svs.activePersistentAreaCount() == 0u);

        // Advance past landing (~3.2 s total): both Flare_State_Base instances ignite on arrival.
        for (int step = 0; step < 30; ++step) {
            svs.update(0.05f);
        }
        CHECK(svs.pendingPersistentAreaCount() == 0u);
        CHECK(svs.activePersistentAreaCount() == 2u);
        CHECK(svs.activePersistentAreaInstanceCount() == 2u);

        // On DynamicObject removal at 20 s, both instances fade over 2.0 s before retiring.
        svs.stopPersistentArea(kDynGuid1);
        svs.stopPersistentArea(kDynGuid2);
        CHECK(svs.activePersistentAreaCount() == 0u);
        CHECK(svs.fadingPersistentAreaCount() == 2u);

        svs.update(1.00f);
        CHECK(svs.fadingPersistentAreaCount() == 2u);

        svs.update(1.05f);
        CHECK(svs.fadingPersistentAreaCount() == 0u);
    }

    SECTION("Order B: DynamicObject arrives slightly before SMSG_SPELL_GO in same server tick") {
        svs.playPersistentArea(kDynGuid1, 10387u, flareDest, 10.0f);
        svs.playPersistentArea(kDynGuid2, 10387u, flareDest, 10.0f);
        CHECK(svs.activePersistentAreaCount() == 2u);

        svs.update(0.05f);
        REQUIRE(svs.playSpellMissile(10387u, casterPos, flareDest, /*speed=*/5.0f,
                                     /*launchDelay=*/0.6f, /*impactAtEnd=*/false,
                                     /*areaImpactAtEnd=*/true));

        // Premature instances are immediately deferred onto the newly launched missile.
        CHECK(svs.activePersistentAreaCount() == 0u);
        CHECK(svs.pendingPersistentAreaCount() == 2u);

        for (int step = 0; step < 70; ++step) {
            svs.update(0.05f);
        }
        CHECK(svs.pendingPersistentAreaCount() == 0u);
        CHECK(svs.activePersistentAreaCount() == 2u);
        CHECK(svs.activePersistentAreaInstanceCount() == 2u);
    }

    SECTION("Mid-flight DynamicObject cancellation never spawns an orphaned persistent flare") {
        REQUIRE(svs.playSpellMissile(10387u, casterPos, flareDest, /*speed=*/5.0f,
                                     /*launchDelay=*/0.6f, /*impactAtEnd=*/false,
                                     /*areaImpactAtEnd=*/true));
        svs.playPersistentArea(kDynGuid1, 10387u, flareDest, 10.0f);
        CHECK(svs.pendingPersistentAreaCount() == 1u);

        svs.stopPersistentArea(kDynGuid1);
        CHECK(svs.pendingPersistentAreaCount() == 0u);

        for (int step = 0; step < 70; ++step) {
            svs.update(0.05f);
        }
        CHECK(svs.activePersistentAreaCount() == 0u);
    }

    SECTION("Observer walking into range of an existing Flare spawns immediately and CharProc 9 deduplicates") {
        // No missile in flight -> immediate ground spawn.
        svs.playPersistentArea(kDynGuid1, 10387u, flareDest, 10.0f);
        svs.playPersistentArea(kDynGuid2, 10387u, flareDest, 10.0f);
        CHECK(svs.pendingPersistentAreaCount() == 0u);
        CHECK(svs.activePersistentAreaCount() == 2u);
        CHECK(svs.activePersistentAreaInstanceCount() == 2u);

        // Hurricane (16914 -> SV 9489, kit 9354 with CharProc 9 + LightningStorm_Cloud_State)
        // deduplicates co-located multi-effect DynamicObjects into 1 primary persistent area.
        constexpr uint64_t kHurr1 = 0xF100000000000011ull;
        constexpr uint64_t kHurr2 = 0xF100000000000012ull;
        const glm::vec3 hurrPos(-6020.0f, -3260.0f, 86.16f);
        svs.playPersistentArea(kHurr1, 9489u, hurrPos, 10.0f);
        svs.playPersistentArea(kHurr2, 9489u, hurrPos, 10.0f);
        CHECK(svs.activePersistentAreaCount() == 3u);
        CHECK(svs.activePersistentAreaInstanceCount() == 3u);
    }
}
