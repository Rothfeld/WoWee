// TBC 2.4.3 Environmental Audio Shard 3: WorldStateZoneSounds.dbc overrides,
// AreaTable/WMOAreaTable audio inheritance, and live world-state transitions.

#include <catch_amalgamated.hpp>

#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/music_manager.hpp"
#include "audio/sound_entries.hpp"
#include "game/zone_manager.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

using namespace wowee;

namespace {

struct TbcAssets {
    pipeline::AssetManager assets;
    pipeline::DBCLayout layouts;
    bool ready = false;

    TbcAssets() {
        (void)audio::AudioEngine::instance();
        (void)audio::SoundEntryTable::instance();

        namespace fs = std::filesystem;
        std::vector<std::string> candidates;
        if (const char* envPath = std::getenv("WOWEE_TBC_DATA_DIR"); envPath && *envPath) {
            candidates.emplace_back(envPath);
        }
        candidates.emplace_back("/workspace/azerothcore/client/wowee-data/expansions/tbc");
        candidates.emplace_back("/workspace/azerothcore/client/wowee-data");

        const fs::path layoutPath = fs::path(WOWEE_SOURCE_DIR) / "Data" / "expansions" / "tbc" / "dbc_layouts.json";
        if (fs::exists(layoutPath) && layouts.loadFromJson(layoutPath.string())) {
            pipeline::setActiveDBCLayout(&layouts);
        }

        for (const auto& dir : candidates) {
            if (fs::exists(dir) && assets.initialize(dir)) {
                assets.setExpansionDataPath(dir);
                if (auto dbc = assets.loadDBC("WorldStateZoneSounds.dbc"); dbc && dbc->isLoaded()) {
                    ready = true;
                    break;
                }
            }
        }
    }

    ~TbcAssets() {
        audio::SoundEntryTable::instance().reset();
        audio::AudioEngine::instance().setPlaybackObserver({});
        audio::AudioEngine::instance().setAssetManager(nullptr);
        audio::AudioEngine::instance().shutdown();
        pipeline::setActiveDBCLayout(nullptr);
        assets.shutdown();
    }
};

TbcAssets& tbcEnv() {
    static TbcAssets instance;
    if (!instance.ready) {
        SKIP("TBC data directory with WorldStateZoneSounds.dbc not available");
    }
    return instance;
}

double renderRms(uint32_t frames) {
    std::vector<float> pcm(static_cast<size_t>(frames) * 2u, 0.0f);
    REQUIRE(audio::AudioEngine::instance().renderMixFrames(pcm.data(), frames) == frames);
    double sumSq = 0.0;
    for (float s : pcm) sumSq += static_cast<double>(s) * static_cast<double>(s);
    return pcm.empty() ? 0.0 : std::sqrt(sumSq / static_cast<double>(pcm.size()));
}

} // namespace

TEST_CASE("WorldStateZoneSounds.dbc loads all 18 TBC rules and resolves Isle of Quel'Danas & WMO rooms",
          "[audio][tbc][worldstate_zone_sounds]") {
    auto& env = tbcEnv();
    game::ZoneManager zones;
    zones.initialize();
    zones.setUseOriginalSoundtrack(false);
    zones.enrichFromDBC(&env.assets);

    const auto& rules = zones.getWorldStateZoneSounds();
    REQUIRE(rules.size() == 18);
    for (const auto& r : rules) {
        CHECK(r.worldStateId == 3426u);
        CHECK(r.worldStateValue <= 2u);
        CHECK((r.areaId == 4080u || r.wmoAreaId != 0u));
        CHECK((r.zoneMusicId == 394u || r.zoneMusicId == 395u || r.zoneMusicId == 396u));
    }

    // SoundEntries 12292 (Zone-QuelDanasStage02 -> ZoneMusic 395) omits ".mp3"
    // on "Sunwell\\SW_FelEnergyWalkUni01"; verify all 6 tracks are recovered.
    const auto* stage2Set = zones.getZoneMusicSet(395u);
    REQUIRE(stage2Set != nullptr);
    CHECK(stage2Set->dayTracks.size() == 6);
    CHECK(zones.zoneMusicSetContainsTrack(
        395u, "Sound\\Music\\ZoneMusic\\Sunwell\\SW_FelEnergyWalkUni01.mp3"));

    // Outdoor Isle of Quel'Danas (4080) and its sub-areas (e.g. 4085 Shattered
    // Sun Staging Area, 4086 Sun's Reach Sanctum, 4087 Sun's Reach Harbor)
    // inherit 4080's WorldStateZoneSounds override, while 4092 (The Dead Scar)
    // keeps its own ZoneMusic 300.
    constexpr std::array<uint32_t, 5> kInheritedSubAreas = {4080u, 4085u, 4086u, 4087u, 4088u};
    constexpr std::array<std::pair<uint32_t, uint32_t>, 4> kPhaseExpected = {{
        {0u, 394u}, // Stage 1
        {1u, 395u}, // Stage 2
        {2u, 396u}, // Stage 3
        {3u, 402u}, // Stage 4 falls back to AreaTable[4080].ZoneMusic = 402
    }};

    for (const auto& [phase, expectedMusic] : kPhaseExpected) {
        zones.setWorldState(3426u, phase);
        for (uint32_t areaId : kInheritedSubAreas) {
            INFO("phase=" << phase << " areaId=" << areaId);
            CHECK(zones.getAreaZoneMusicId(areaId) == expectedMusic);
            CHECK(zones.getAreaAmbienceId(areaId) == 351u);
        }
        CHECK(zones.getAreaZoneMusicId(4092u) == 300u);
        CHECK(zones.getAreaAmbienceId(4092u) == 363u);
    }

    // All 5 WMOAreaTable rooms in WorldStateZoneSounds.dbc:
    //   40329 -> (4468, 0, 22066)
    //   40331 -> (4468, 0, 22068)
    //   40353 -> (4495, 0, 22078)
    //   43205 -> (4435, 0, 23823)
    //   43245 -> (5139, 0, 23852)
    struct WmoTarget {
        uint32_t expectedWmoAreaId;
        uint32_t root;
        uint32_t nameSet;
        uint32_t group;
    };
    constexpr std::array<WmoTarget, 5> kWmoTargets = {{
        {40329u, 4468u, 0u, 22066u},
        {40331u, 4468u, 0u, 22068u},
        {40353u, 4495u, 0u, 22078u},
        {43205u, 4435u, 0u, 23823u},
        {43245u, 5139u, 0u, 23852u},
    }};
    for (const auto& [phase, expectedMusic] : kPhaseExpected) {
        zones.setWorldState(3426u, phase);
        for (const auto& t : kWmoTargets) {
            INFO("phase=" << phase << " wmoAreaId=" << t.expectedWmoAreaId);
            CHECK(zones.getWmoAreaTableId(t.root, t.nameSet, t.group) == t.expectedWmoAreaId);
            const auto sounds = zones.getWmoAreaSounds(t.root, t.nameSet, t.group);
            CHECK(sounds.wmoAreaId == t.expectedWmoAreaId);
            CHECK(sounds.zoneMusicId == expectedMusic);
            CHECK(sounds.ambienceId == 50u);
            CHECK(sounds.soundProviderPrefId == 75u);
        }
    }

    // enrichFromDBC reload must preserve live server world states.
    zones.setWorldState(3426u, 2u);
    zones.enrichFromDBC(&env.assets);
    CHECK(zones.getWorldStateValue(3426u) == 2u);
    CHECK(zones.getAreaZoneMusicId(4080u) == 396u);
}

TEST_CASE("Full-corpus AreaTable (1,643) and WMOAreaTable (16,234) audio resolution has zero dangling IDs",
          "[audio][tbc][worldstate_zone_sounds]") {
    auto& env = tbcEnv();
    game::ZoneManager zones;
    zones.initialize();
    zones.setUseOriginalSoundtrack(false);
    zones.enrichFromDBC(&env.assets);

    auto areaDbc = env.assets.loadDBC("AreaTable.dbc");
    auto wmoDbc = env.assets.loadDBC("WMOAreaTable.dbc");
    auto spDbc = env.assets.loadDBC("SoundProviderPreferences.dbc");
    REQUIRE(areaDbc != nullptr);
    REQUIRE(wmoDbc != nullptr);
    REQUIRE(spDbc != nullptr);
    CHECK(areaDbc->getRecordCount() == 1643u);
    CHECK(wmoDbc->getRecordCount() == 16234u);

    std::unordered_set<uint32_t> validSpIds;
    for (uint32_t i = 0; i < spDbc->getRecordCount(); ++i) {
        validSpIds.insert(spDbc->getUInt32(i, 0));
    }

    for (uint32_t i = 0; i < areaDbc->getRecordCount(); ++i) {
        const uint32_t areaId = areaDbc->getUInt32(i, 0);
        if (areaId == 0) continue;
        const uint32_t musicId = zones.getAreaZoneMusicId(areaId);
        if (musicId != 0) {
            const auto* set = zones.getZoneMusicSet(musicId);
            REQUIRE(set != nullptr);
            CHECK((!set->dayTracks.empty() || !set->nightTracks.empty()));
        }
        const uint32_t introId = zones.getAreaIntroMusicId(areaId);
        if (introId != 0) {
            const auto* intro = zones.getZoneIntroMusic(introId);
            REQUIRE(intro != nullptr);
            CHECK_FALSE(intro->tracks.empty());
        }
        const uint32_t ambId = zones.getAreaAmbienceId(areaId);
        if (ambId != 0) {
            const std::string day = zones.getAmbienceSound(ambId, false);
            const std::string night = zones.getAmbienceSound(ambId, true);
            CHECK((!day.empty() || !night.empty()));
        }
        const uint32_t spId = zones.getAreaSoundProviderPrefId(areaId);
        if (spId != 0) CHECK(validSpIds.count(spId) == 1u);
        const uint32_t spUwId = zones.getAreaUnderwaterSoundProviderPrefId(areaId);
        if (spUwId != 0) CHECK(validSpIds.count(spUwId) == 1u);
    }

    for (uint32_t i = 0; i < wmoDbc->getRecordCount(); ++i) {
        const uint32_t root = wmoDbc->getUInt32(i, 1);
        const uint32_t nameSet = wmoDbc->getUInt32(i, 2);
        const uint32_t group = wmoDbc->getUInt32(i, 3);
        const auto sounds = zones.getWmoAreaSounds(root, nameSet, group);
        if (sounds.zoneMusicId != 0) {
            const auto* set = zones.getZoneMusicSet(sounds.zoneMusicId);
            REQUIRE(set != nullptr);
            CHECK((!set->dayTracks.empty() || !set->nightTracks.empty()));
        }
        if (sounds.introSoundId != 0) {
            const auto* intro = zones.getZoneIntroMusic(sounds.introSoundId);
            REQUIRE(intro != nullptr);
            CHECK_FALSE(intro->tracks.empty());
        }
        if (sounds.ambienceId != 0) {
            const std::string day = zones.getAmbienceSound(sounds.ambienceId, false);
            const std::string night = zones.getAmbienceSound(sounds.ambienceId, true);
            CHECK((!day.empty() || !night.empty()));
        }
        if (sounds.soundProviderPrefId != 0) {
            CHECK(validSpIds.count(sounds.soundProviderPrefId) == 1u);
        }
        if (sounds.soundProviderPrefUnderwaterId != 0) {
            CHECK(validSpIds.count(sounds.soundProviderPrefUnderwaterId) == 1u);
        }
    }

    // Duplicate (root=556, nameSet=1, group=-1/2480/2481) in WMOAreaTable:
    // higher-ID TBC record 35968..35970 ("Stonehearth Outpost", areaId=2958)
    // must win over stale record 32862..32864 (areaId=0).
    CHECK(zones.getWmoAreaId(556u, 1u, UINT32_MAX) == 2958u);
    CHECK(zones.getWmoAreaId(556u, 1u, 2480u) == 2958u);
    CHECK(zones.getWmoAreaId(556u, 1u, 2481u) == 2958u);
    CHECK(zones.getWmoAreaName(556u, 1u, UINT32_MAX) == "Stonehearth Outpost");
}

TEST_CASE("Multi-column WorldStateZoneSounds overrides and live AudioCoordinator re-evaluation work without zone crossing",
          "[audio][tbc][worldstate_zone_sounds]") {
    auto& env = tbcEnv();
    auto& engine = audio::AudioEngine::instance();
    engine.shutdown();
    REQUIRE(engine.initializeHeadless(48000, 2));
    engine.setAssetManager(&env.assets);

    game::ZoneManager zones;
    zones.initialize();
    zones.setUseOriginalSoundtrack(false);
    zones.enrichFromDBC(&env.assets);

    SECTION("Synthetic 4-column override covers IntroMusic, ZoneMusic, SoundAmbience, and SoundProviderPref") {
        // Attach a custom override on parent area 4080 (Isle of Quel'Danas) watched on worldState 9901=7,
        // and verify child sub-area 4085 (Shattered Sun Staging Area, parent=4080, own=0) inherits all 4
        // overridden columns while child sub-area 4092 (The Dead Scar, own ZoneMusic=300, Ambience=363)
        // keeps its own area-specific columns unless 4092 itself is overridden.
        const uint32_t stormwindIntro = zones.getAreaIntroMusicId(1519u);
        const uint32_t stormwindAmbience = zones.getAreaAmbienceId(1519u);
        REQUIRE(stormwindIntro != 0u);
        REQUIRE(stormwindAmbience != 0u);
        REQUIRE(stormwindAmbience != 351u);
        zones.addWorldStateZoneSoundOverride(game::ZoneManager::WorldStateZoneSoundOverride{
            .worldStateId = 9901u,
            .worldStateValue = 7u,
            .areaId = 4080u,
            .wmoAreaId = 0u,
            .zoneIntroMusicId = stormwindIntro,
            .zoneMusicId = 395u,
            .soundAmbienceId = stormwindAmbience,
            .soundProviderPrefId = 75u,
        });

        CHECK_FALSE(zones.setWorldState(9999u, 1u)); // unwatched world state
        CHECK(zones.setWorldState(9901u, 7u));       // watched world state!

        // Direct resolution on 4080 receives all 4 overridden columns (including SoundAmbience).
        const auto parentResolved = zones.resolveZoneAudio(0u, 0u, 0u, false, /*areaId=*/4080u, /*zoneId=*/4080u);
        CHECK(parentResolved.zoneIntroMusicId == stormwindIntro);
        CHECK(parentResolved.zoneMusicId == 395u);
        CHECK(parentResolved.soundAmbienceId == stormwindAmbience);
        CHECK(parentResolved.soundProviderPrefId == 75u);

        // Sub-area 4085 has own ZoneMusic=0, IntroSound=0, SoundProviderPref=0 (inherits 4080's
        // overrides) and own AmbienceID=351 in AreaTable.dbc.
        const auto resolved = zones.resolveZoneAudio(0u, 0u, 0u, false, /*areaId=*/4085u, /*zoneId=*/4080u);
        CHECK(resolved.zoneIntroMusicId == stormwindIntro);
        CHECK(resolved.zoneMusicId == 395u);
        CHECK(resolved.soundAmbienceId == 351u);
        CHECK(resolved.soundProviderPrefId == 75u);

        const auto deadScar = zones.resolveZoneAudio(0u, 0u, 0u, false, /*areaId=*/4092u, /*zoneId=*/4080u);
        CHECK(deadScar.zoneMusicId == 300u);
        CHECK(deadScar.soundAmbienceId == 363u);
        CHECK(deadScar.zoneIntroMusicId == stormwindIntro);
        CHECK(deadScar.soundProviderPrefId == 75u);
    }

    SECTION("Live worldState 3426 transitions immediately switch active music set without moving") {
        audio::AudioCoordinator ac;
        REQUIRE(ac.initialize());
        ac.initializeEarlyAssets(&env.assets);

        audio::ZoneAudioContext ctx;
        ctx.zoneManager = &zones;
        ctx.hasTile = true;
        ctx.areaId = 4085u;       // Shattered Sun Staging Area (child of 4080)
        ctx.tileZoneId = 4080u;
        ctx.serverZoneId = 4080u;
        ctx.gameTimeHours = 12.0f;

        const auto advanceSeconds = [&](float totalSec) {
            constexpr float kStep = 0.25f;
            for (float elapsed = 0.0f; elapsed < totalSec; elapsed += kStep) {
                ctx.deltaTime = kStep;
                ac.updateZoneAudio(ctx);
            }
        };

        // Stage 0 -> ZoneMusic 394 (render 6s at 48kHz past soft MP3 intro pads)
        zones.setWorldState(3426u, 0u);
        advanceSeconds(0.5f);
        REQUIRE(ac.getMusicManager() != nullptr);
        CHECK(zones.zoneMusicSetContainsTrack(394u, ac.getMusicManager()->getCurrentTrack()));
        CHECK(renderRms(48000 * 6) > 1e-5);

        // Advance past 4s switch cooldown; stay at exact same coordinates & areaId!
        advanceSeconds(4.5f);

        // Live SMSG_UPDATE_WORLD_STATE: 3426 = 1 -> ZoneMusic 395
        CHECK(zones.setWorldState(3426u, 1u));
        advanceSeconds(2.5f); // completes smooth 0.9s fade-out + 0.9s fade-in
        CHECK(zones.zoneMusicSetContainsTrack(395u, ac.getMusicManager()->getCurrentTrack()));
        CHECK(renderRms(48000 * 6) > 1e-5);

        advanceSeconds(2.5f);

        // Live SMSG_UPDATE_WORLD_STATE: 3426 = 2 -> ZoneMusic 396
        CHECK(zones.setWorldState(3426u, 2u));
        advanceSeconds(2.5f);
        CHECK(zones.zoneMusicSetContainsTrack(396u, ac.getMusicManager()->getCurrentTrack()));
        CHECK(renderRms(48000 * 6) > 1e-5);

        advanceSeconds(2.5f);

        // Live SMSG_UPDATE_WORLD_STATE: 3426 = 3 -> base ZoneMusic 402
        CHECK(zones.setWorldState(3426u, 3u));
        advanceSeconds(2.5f);
        CHECK(zones.zoneMusicSetContainsTrack(402u, ac.getMusicManager()->getCurrentTrack()));
        CHECK(renderRms(48000 * 6) > 1e-5);

        ac.shutdown();
    }

    engine.shutdown();
}
