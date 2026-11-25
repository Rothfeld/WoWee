#include <catch_amalgamated.hpp>

#include "game_replay.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/combat_sound_manager.hpp"
#include "audio/sound_entries.hpp"
#include "audio/spell_sound_manager.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "rendering/spell_visual_system.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using namespace wowee;

std::string findTbcDataPath() {
    if (const char* env = std::getenv("WOWEE_TBC_DATA_DIR"); env && *env) {
        if (std::filesystem::exists(env)) return env;
    }
    static constexpr const char* kCandidates[] = {
        "/workspace/azerothcore/client/wowee-data/expansions/tbc",
        "Data/expansions/tbc",
        "../Data/expansions/tbc",
    };
    for (const char* c : kCandidates) {
        std::filesystem::path p(c);
        if (std::filesystem::exists(p / "dbc.MPQ") || std::filesystem::exists(p / "common.MPQ")) {
            return p.string();
        }
    }
    return {};
}

std::string findTbcLayoutJsonPath() {
#ifdef WOWEE_SOURCE_DIR
    std::filesystem::path srcLayout =
        std::filesystem::path(WOWEE_SOURCE_DIR) / "Data" / "expansions" / "tbc" / "dbc_layouts.json";
    if (std::filesystem::exists(srcLayout)) return srcLayout.string();
#endif
    if (std::filesystem::exists("Data/expansions/tbc/dbc_layouts.json")) {
        return "Data/expansions/tbc/dbc_layouts.json";
    }
    return {};
}

struct TbcAudioFixture {
    pipeline::AssetManager assets;
    pipeline::DBCLayout layout;
    std::vector<audio::AudioEngine::PlaybackEvent> events;
    bool ready = false;

    TbcAudioFixture() {
        const std::string dataDir = findTbcDataPath();
        const std::string layoutPath = findTbcLayoutJsonPath();
        if (dataDir.empty() || layoutPath.empty()) return;
        if (!assets.initialize(dataDir)) return;
        if (!layout.loadFromJson(layoutPath)) return;
        pipeline::setActiveDBCLayout(&layout);

        auto& eng = audio::AudioEngine::instance();
        eng.shutdown();
        if (!eng.initializeHeadless(44100, 2)) return;
        eng.setMasterVolume(1.0f);
        eng.setAssetManager(&assets);
        eng.setListenerPosition(glm::vec3(0.0f, 0.0f, 0.0f));
        audio::SoundEntryTable::instance().reset();
        if (!audio::SoundEntryTable::instance().ensureLoaded(&assets)) return;

        eng.setPlaybackObserver([this](const audio::AudioEngine::PlaybackEvent& ev) {
            events.push_back(ev);
        });
        ready = true;
    }

    ~TbcAudioFixture() {
        auto& eng = audio::AudioEngine::instance();
        eng.setPlaybackObserver({});
        eng.shutdown();
    }

    static double mixRms(uint32_t frames = 2048) {
        std::vector<float> pcm(static_cast<size_t>(frames) * 2u, 0.0f);
        const uint32_t got = audio::AudioEngine::instance().renderMixFrames(pcm.data(), frames);
        if (got == 0) return 0.0;
        const size_t n = static_cast<size_t>(got) * 2u;
        double sumSq = 0.0;
        for (size_t i = 0; i < n; ++i) sumSq += static_cast<double>(pcm[i]) * static_cast<double>(pcm[i]);
        return std::sqrt(sumSq / static_cast<double>(n));
    }
};

TbcAudioFixture& sharedFixture() {
    static TbcAudioFixture fix;
    return fix;
}

network::Packet buildTbcSpellStartPacket(uint64_t casterGuid,
                                         uint32_t spellId,
                                         uint32_t castTimeMs,
                                         uint64_t targetGuid) {
    network::Packet pkt(game::wireOpcode(game::Opcode::SMSG_SPELL_START));
    pkt.writePackedGuid(casterGuid);
    pkt.writePackedGuid(casterGuid);
    pkt.writeUInt32(spellId);
    pkt.writeUInt8(1);        // castCount
    pkt.writeUInt16(0x0002u); // CAST_FLAG_UNKNOWN2
    pkt.writeUInt32(castTimeMs);
    pkt.writeUInt16(targetGuid != 0 ? 0x0002u : 0x0000u); // TARGET_FLAG_UNIT or SELF
    if (targetGuid != 0) pkt.writePackedGuid(targetGuid);
    return pkt;
}

network::Packet buildTbcSpellGoPacket(uint64_t casterGuid,
                                      uint32_t spellId,
                                      const std::vector<uint64_t>& hitTargets) {
    network::Packet pkt(game::wireOpcode(game::Opcode::SMSG_SPELL_GO));
    pkt.writePackedGuid(casterGuid);
    pkt.writePackedGuid(casterGuid);
    pkt.writeUInt32(spellId);
    pkt.writeUInt16(0x0100u); // CAST_FLAG_UNKNOWN9
    pkt.writeUInt32(1000u);   // timestamp
    pkt.writeUInt8(static_cast<uint8_t>(hitTargets.size()));
    for (uint64_t t : hitTargets) pkt.writeUInt64(t);
    pkt.writeUInt8(0); // missCount
    const uint64_t primary = hitTargets.empty() ? 0u : hitTargets.front();
    pkt.writeUInt16(primary != 0 ? 0x0002u : 0x0000u);
    if (primary != 0) pkt.writePackedGuid(primary);
    return pkt;
}

} // namespace

TEST_CASE("Shard B: TBC SpellVisual.dbc col 10 MissileSound and col 11 AnimEventSoundID load exact rows",
          "[audio][tbc][spells]") {
    auto& fix = sharedFixture();
    if (!fix.ready) SKIP("TBC data not present");
    pipeline::setActiveDBCLayout(&fix.layout);

    rendering::SpellVisualSystem svs;
    svs.setAssetManager(&fix.assets);

    // TBC 2.4.3 SpellVisual.dbc has 473 positive MissileSound rows (4 -1 sentinels excluded)
    // and 15 positive AnimEventSoundID rows (10 -1 sentinels excluded).
    CHECK(svs.loadedMissileSoundCount() == 473);
    CHECK(svs.loadedAnimEventSoundCount() == 15);

    // Canonical class & ranged SpellVisual.MissileSound mappings
    CHECK(svs.missileSoundId(13) == 3013u);   // Frostbolt -> MissileIceLoop
    CHECK(svs.missileSoundId(67) == 3011u);   // Fireball -> MissileFireLoop
    CHECK(svs.missileSoundId(64) == 3015u);   // Shadow Bolt -> MissileShadowLoop
    CHECK(svs.missileSoundId(3299) == 3013u); // Arcane Shot -> MissileIceLoop
    CHECK(svs.missileSoundId(7955) == 3014u); // Aimed Shot -> MissileNatureLoop
    CHECK(svs.missileSoundId(8844) == 4222u); // Hurl Trident -> ArrowLoop
    CHECK(svs.missileSoundId(9336) == 4222u); // Throw Dagger -> ArrowLoop

    // Sentinel -1 rows in col 11 must resolve to 0 rather than 0xFFFFFFFF
    CHECK(svs.animEventSoundId(6739) == 0u);
    CHECK(svs.animEventSoundId(7671) == 0u);

    // Canonical SpellVisual.AnimEventSoundID mappings
    CHECK(svs.animEventSoundId(91) == 1142u);   // Herb Gathering -> HerbalismSearch
    CHECK(svs.animEventSoundId(93) == 1143u);   // Mining -> Mining Impact
    CHECK(svs.animEventSoundId(9732) == 963u);  // Zul'Aman Crate Burst -> Mace2H_ArmorWood
}

TEST_CASE("Shard B: SpellMissile starts 3D MissileSound in flight and stops immediately on impact",
          "[audio][tbc][spells]") {
    auto& fix = sharedFixture();
    if (!fix.ready) SKIP("TBC data not present");
    pipeline::setActiveDBCLayout(&fix.layout);

    auto& eng = audio::AudioEngine::instance();
    eng.shutdown();
    REQUIRE(eng.initializeHeadless(44100, 2));
    eng.setAssetManager(&fix.assets);
    eng.setListenerPosition(glm::vec3(0.0f, 0.0f, 0.0f));
    fix.events.clear();
    eng.setPlaybackObserver([&fix](const audio::AudioEngine::PlaybackEvent& ev) {
        fix.events.push_back(ev);
    });

    rendering::SpellVisualSystem svs;
    svs.setAssetManager(&fix.assets);

    // Immediate launch: Frostbolt visual 13 (MissileIceLoop 3013), 20 yd at 25 yd/s (0.8 s flight)
    REQUIRE(svs.playSpellMissile(13, glm::vec3(2.0f, 0.0f, 0.0f), glm::vec3(22.0f, 0.0f, 0.0f),
                                 /*speed=*/25.0f, /*launchDelay=*/0.0f, /*impactAtEnd=*/false));
    REQUIRE(fix.events.size() == 1);
    CHECK(fix.events.back().started);
    CHECK(fix.events.back().spatial);
    CHECK(eng.getActiveSoundCount() == 1);
    CHECK(TbcAudioFixture::mixRms(2048) > 1e-4);

    // Mid-flight at t = 0.30 s: missile still airborne and MissileSound still active
    svs.update(0.10f);
    svs.update(0.20f);
    CHECK(eng.getActiveSoundCount() == 1);

    // Arrive after remaining 0.60 s of flight, then land on next frame: MissileSound stops immediately
    svs.update(0.65f);
    svs.update(0.016f);
    CHECK(eng.getActiveSoundCount() == 0);

    // Delayed release launch (launchDelay = 0.25 s): silent until hand release, then stops on landing
    fix.events.clear();
    REQUIRE(svs.playSpellMissile(67, glm::vec3(2.0f, 0.0f, 0.0f), glm::vec3(14.5f, 0.0f, 0.0f),
                                 /*speed=*/25.0f, /*launchDelay=*/0.25f, /*impactAtEnd=*/false));
    CHECK(eng.getActiveSoundCount() == 0);
    svs.update(0.15f);
    CHECK(eng.getActiveSoundCount() == 0);
    svs.update(0.12f); // crosses launchDelay = 0.25 s and spawns missile
    CHECK(eng.getActiveSoundCount() == 1);
    CHECK(TbcAudioFixture::mixRms(1024) > 1e-4);
    svs.update(0.60f); // completes remaining flight after release
    svs.update(0.016f);
    CHECK(eng.getActiveSoundCount() == 0);
}

TEST_CASE("Shard B: AnimEventSoundID fires on gather Precast cycles and stops on cancel",
          "[audio][tbc][spells]") {
    auto& fix = sharedFixture();
    if (!fix.ready) SKIP("TBC data not present");
    pipeline::setActiveDBCLayout(&fix.layout);

    auto& eng = audio::AudioEngine::instance();
    eng.shutdown();
    REQUIRE(eng.initializeHeadless(44100, 2));
    eng.setAssetManager(&fix.assets);
    eng.setListenerPosition(glm::vec3(0.0f, 0.0f, 0.0f));
    fix.events.clear();
    eng.setPlaybackObserver([&fix](const audio::AudioEngine::PlaybackEvent& ev) {
        fix.events.push_back(ev);
    });

    rendering::SpellVisualSystem svs;
    svs.setAssetManager(&fix.assets);

    // Mining (spell 2575 -> visual 93): PrecastKit 166 has SoundID=0, AnimEventSoundID=1143 (Mining Impact)
    svs.playSpellVisualPrecast(93, glm::vec3(1.0f, 0.0f, 0.0f), /*castTimeMs=*/3200,
                               /*attachInstanceId=*/42, /*spellId=*/2575);
    CHECK(fix.events.empty()); // No fake PrecastArcane on start

    svs.update(0.75f); // Mid-swing cycle fires Mining Impact (1143)
    REQUIRE(fix.events.size() == 1);
    CHECK(fix.events.back().started);
    CHECK(TbcAudioFixture::mixRms(2048) > 1e-4);

    // Cancelling precast stops further AnimEventSoundID swings
    svs.cancelPrecastVisuals(42, 2575);
    fix.events.clear();
    svs.update(2.0f);
    CHECK(fix.events.empty());

    // Cast-only AnimEventSoundID (Zul'Aman Crate Burst visual 9732 -> Sound 963 Mace2H_ArmorWood)
    svs.playCastKit(9732, glm::vec3(1.0f, 0.0f, 0.0f), 42, {}, false, 0.0f);
    REQUIRE(fix.events.size() == 1);
    CHECK(fix.events.back().started);
}

TEST_CASE("Shard B: StateKit aura handles stop immediately on aura fade, dispel, or unit death",
          "[audio][tbc][spells]") {
    auto& fix = sharedFixture();
    if (!fix.ready) SKIP("TBC data not present");
    pipeline::setActiveDBCLayout(&fix.layout);

    auto& eng = audio::AudioEngine::instance();
    eng.shutdown();
    REQUIRE(eng.initializeHeadless(44100, 2));
    eng.setAssetManager(&fix.assets);
    eng.setListenerPosition(glm::vec3(0.0f, 0.0f, 0.0f));
    fix.events.clear();
    eng.setPlaybackObserver([&fix](const audio::AudioEngine::PlaybackEvent& ev) {
        fix.events.push_back(ev);
    });

    rendering::SpellVisualSystem svs;
    svs.setAssetManager(&fix.assets);

    // Druid Cyclone (visual 218 -> StateKit 363 -> Sound 10045 Cyclone_StateLoop, Flags 0x200)
    REQUIRE(svs.syncAuraVisuals(100, {218}, /*observerConcealed=*/false, /*unitDead=*/false));
    CHECK(eng.getActiveSoundCount() == 1);
    CHECK(TbcAudioFixture::mixRms(2048) > 1e-4);

    // Transferring instance (e.g. morph/model swap) preserves the stoppable aura handle
    svs.transferUnitVisuals(100, 101);
    CHECK(eng.getActiveSoundCount() == 1);

    // Aura fades / dispelled -> stops handle immediately
    REQUIRE(svs.syncAuraVisuals(101, {}, /*observerConcealed=*/false, /*unitDead=*/false));
    CHECK(eng.getActiveSoundCount() == 0);

    // Paladin Divine Shield (visual 154 -> StateKit 252 -> Sound 39) stops immediately on unitDead
    REQUIRE(svs.syncAuraVisuals(101, {154}, /*observerConcealed=*/false, /*unitDead=*/false));
    CHECK(eng.getActiveSoundCount() == 1);
    svs.syncAuraVisuals(101, {154}, /*observerConcealed=*/false, /*unitDead=*/true);
    CHECK(eng.getActiveSoundCount() == 0);
}

TEST_CASE("Shard B: updateChannelVisual(0) stops ChannelKit sound immediately while allowing re-tick",
          "[audio][tbc][spells]") {
    auto& fix = sharedFixture();
    if (!fix.ready) SKIP("TBC data not present");
    pipeline::setActiveDBCLayout(&fix.layout);

    auto& eng = audio::AudioEngine::instance();
    eng.shutdown();
    REQUIRE(eng.initializeHeadless(44100, 2));
    eng.setAssetManager(&fix.assets);
    eng.setListenerPosition(glm::vec3(0.0f, 0.0f, 0.0f));
    fix.events.clear();
    eng.setPlaybackObserver([&fix](const audio::AudioEngine::PlaybackEvent& ev) {
        fix.events.push_back(ev);
    });

    rendering::SpellVisualSystem svs;
    svs.setAssetManager(&fix.assets);

    // Priest Mind Flay (visual 277 -> ChannelKit 430 -> Sound 3012 MissileHolyLoop)
    svs.startChannelVisual(277, glm::vec3(1.0f, 0.0f, 0.0f), /*durationMs=*/3000, /*attachInstanceId=*/55);
    CHECK(eng.getActiveSoundCount() == 1);
    CHECK(TbcAudioFixture::mixRms(1024) > 1e-4);

    // MSG_CHANNEL_UPDATE(0) stops ChannelKit sound immediately without waiting for next update() tick
    svs.updateChannelVisual(55, 0);
    CHECK(eng.getActiveSoundCount() == 0);

    // Back-to-back MSG_CHANNEL_START for the same visual restarts the channel sound cleanly
    svs.startChannelVisual(277, glm::vec3(1.0f, 0.0f, 0.0f), /*durationMs=*/3000, /*attachInstanceId=*/55);
    CHECK(eng.getActiveSoundCount() == 1);
    svs.reset();
    CHECK(eng.getActiveSoundCount() == 0);
}

TEST_CASE("Shard B: SpellHandler emits zero legacy school fallback or hardcoded melee impact sounds",
          "[audio][tbc][spells]") {
    auto& fix = sharedFixture();
    if (!fix.ready) SKIP("TBC data not present");

    replay::Session session("tbc");
    pipeline::setActiveDBCLayout(&fix.layout);

    auto& eng = audio::AudioEngine::instance();
    eng.shutdown();
    REQUIRE(eng.initializeHeadless(44100, 2));
    eng.setAssetManager(&fix.assets);

    audio::AudioCoordinator coordinator;
    coordinator.initializeEarlyAssets(&fix.assets);
    if (auto* csm = coordinator.getCombatSoundManager()) csm->initialize(&fix.assets);
    if (auto* ssm = coordinator.getSpellSoundManager()) ssm->initialize(&fix.assets);

    session.services().assetManager = &fix.assets;
    session.services().audioCoordinator = &coordinator;

    constexpr uint64_t kPlayerGuid = replay::Session::kPlayerGuid;
    constexpr uint64_t kTargetGuid = 0xF130000123000022ULL;
    session.receive(replay::UpdateObject().tbc().create(
        kPlayerGuid, game::ObjectType::PLAYER,
        {{game::UF::UNIT_FIELD_HEALTH, 500}, {game::UF::UNIT_FIELD_LEVEL, 70}}, true));
    session.receive(replay::UpdateObject().tbc().create(
        kTargetGuid, game::ObjectType::UNIT,
        {{game::UF::UNIT_FIELD_HEALTH, 500}, {game::UF::UNIT_FIELD_LEVEL, 70}}));

    fix.events.clear();
    eng.setPlaybackObserver([&fix](const audio::AudioEngine::PlaybackEvent& ev) {
        fix.events.push_back(ev);
    });

    // 1. Timed cast with PrecastKit.SoundID == 0 (Hunter Aimed Shot 19434): zero PrecastArcane
    session.receive(buildTbcSpellStartPacket(kPlayerGuid, 19434, 3000, kTargetGuid));
    CHECK(fix.events.empty());

    // 2. Instant melee abilities (Warrior Heroic Strike 78, Mortal Strike 12294, Hamstring 1715):
    //    zero hardcoded MEDIUM+FLESH weapon swing/impact injection from SpellHandler
    for (uint32_t meleeSpell : {78u, 12294u, 1715u}) {
        session.receive(buildTbcSpellGoPacket(kPlayerGuid, meleeSpell, {kTargetGuid}));
        CHECK(fix.events.empty());
    }

    // 3. Spells with silent CastKit/ImpactKit (Warrior Second Wind 29834, Death Wish 12292, Battle Stance 2457):
    //    zero CastArcane / ArcaneMissileImpact school fallback pollution
    for (uint32_t silentSpell : {29834u, 12292u, 2457u}) {
        session.receive(buildTbcSpellGoPacket(kPlayerGuid, silentSpell, {kPlayerGuid}));
        CHECK(fix.events.empty());
    }
}
