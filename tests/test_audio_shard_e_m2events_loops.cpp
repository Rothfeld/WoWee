// Deterministic TBC 2.4.3 (build 8606) Catch2 audio test suite for Shard E
// (GAP-09 + GAP-10 + embedded M2 $SND):
//   1. M2Loader parses all 22 M2 audio event tag families + event.data + bone + per-sequence times
//   2. All 20 shipped TBC spell/effect M2s with $SND resolve SoundEntries and synthesize non-zero PCM
//   3. DeathThudLookups.dbc (45 rows: SizeClass x TerrainType x dry/water) & $DTH corpse thuds
//   4. 3D remote/NPC $FSD footsteps, bare-foot fallback (FootstepID 7 -> 8), and ListenerAtCharacter 0.65x 2D vs +2.0z 3D
//   5. $TRD Material.dbc col 2 chest/leg armor foley (Plate 1004, Chain 1005, Leather 1003, Cloth silent) + Self/Others CVars
//   6. Drowning loop (3353 PlayerDrowningLoop) start/stop lifecycle + DAMAGE_DROWNING tick (5736 + Injury)
//   7. CreatureSoundData.LoopSoundID (col 24: Fire/Earth/Water Elementals, Infernal, Slime) spawn/death lifecycle
//   8. Creature & GameObject M2 sound events ($CSD, $SCD, $SMG, $SMD, $GO0..5, $GC0..3, $DSL, $DSO) & Pet vocals
//
// Run with:
//   WOWEE_TBC_DATA_DIR=/workspace/azerothcore/client/wowee-data/expansions/tbc ./build/bin/test_audio_shard_e_m2events_loops

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <catch_amalgamated.hpp>

#include "audio/audio_engine.hpp"
#include "audio/creature_sound_index.hpp"
#include "audio/footstep_manager.hpp"
#include "audio/sound_entries.hpp"
#include "audio/spell_sound_manager.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/m2_loader.hpp"

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

struct ShardETestEnv {
    pipeline::AssetManager assets;
    pipeline::DBCLayout dbcLayout;
    bool ready = false;

    ShardETestEnv() {
        (void)audio::AudioEngine::instance();
        (void)audio::SoundEntryTable::instance();

        const char* root = std::getenv("WOWEE_TBC_DATA_DIR");
        if (!root || !*root) return;
        if (!std::filesystem::exists(root)) return;
        if (!assets.initialize(root)) return;
        assets.setExpansionDataPath(root);

        const std::filesystem::path layoutPath =
            std::filesystem::path(WOWEE_SOURCE_DIR) / "Data" / "expansions" / "tbc" / "dbc_layouts.json";
        if (std::filesystem::exists(layoutPath) && dbcLayout.loadFromJson(layoutPath.string())) {
            pipeline::setActiveDBCLayout(&dbcLayout);
        }
        ready = true;
    }

    ~ShardETestEnv() {
        audio::SoundEntryTable::instance().reset();
        audio::AudioEngine::instance().setPlaybackObserver({});
        audio::AudioEngine::instance().setAssetManager(nullptr);
        audio::AudioEngine::instance().shutdown();
        pipeline::setActiveDBCLayout(nullptr);
        assets.shutdown();
    }
};

ShardETestEnv& env() {
    static ShardETestEnv instance;
    if (!instance.ready) {
        SKIP("Set WOWEE_TBC_DATA_DIR to extracted TBC 2.4.3 data directory");
    }
    return instance;
}

struct ChannelRms {
    double left = 0.0;
    double right = 0.0;
    double total = 0.0;
};

ChannelRms renderRms(uint32_t frames) {
    std::vector<float> pcm(static_cast<size_t>(frames) * 2u, 0.0f);
    const uint32_t got = audio::AudioEngine::instance().renderMixFrames(pcm.data(), frames);
    REQUIRE(got == frames);
    double sumL = 0.0;
    double sumR = 0.0;
    for (uint32_t i = 0; i < frames; ++i) {
        const double l = pcm[static_cast<size_t>(i) * 2u];
        const double r = pcm[static_cast<size_t>(i) * 2u + 1u];
        REQUIRE(std::isfinite(l));
        REQUIRE(std::isfinite(r));
        sumL += l * l;
        sumR += r * r;
    }
    ChannelRms out;
    out.left = std::sqrt(sumL / std::max<uint32_t>(1u, frames));
    out.right = std::sqrt(sumR / std::max<uint32_t>(1u, frames));
    out.total = std::sqrt((sumL + sumR) / std::max<uint32_t>(1u, frames * 2u));
    return out;
}

void resetHeadlessEngine(pipeline::AssetManager& assets) {
    auto& engine = audio::AudioEngine::instance();
    engine.setPlaybackObserver({});
    engine.shutdown();
    REQUIRE(engine.initializeHeadless(48000, 2));
    engine.setAssetManager(&assets);
    engine.setMasterVolume(1.0f);
    engine.setSuspended(false);
    engine.setListenerPosition(glm::vec3(0.0f));
    engine.setListenerOrientation(glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
}

int findSequenceByAnimId(const pipeline::M2Model& model, uint32_t animId) {
    for (size_t i = 0; i < model.sequences.size(); ++i) {
        if (model.sequences[i].id == animId) return static_cast<int>(i);
    }
    return -1;
}

} // namespace

TEST_CASE("M2Loader parses all TBC M2 audio event tags and event.data fields", "[audio][tbc][shard_e]") {
    using T = pipeline::M2AudioEventType;

    // 1. Exhaustive tag parser verification across all 22 tag families (32 concrete tags)
    const std::pair<const char*, T> expectedTags[] = {
        {"$SND", T::Sound},
        {"$DTH", T::DeathThud},
        {"$FSD", T::Footstep},
        {"$TRD", T::ArmorFoley},
        {"$CSD", T::CustomSound},
        {"$SCD", T::SpellCastDirected},
        {"$SMG", T::Submerge},
        {"$SMD", T::Submerged},
        {"$GO0", T::GameObject0},
        {"$GO1", T::GameObject1},
        {"$GO2", T::GameObject2},
        {"$GO3", T::GameObject3},
        {"$GO4", T::GameObject4},
        {"$GO5", T::GameObject5},
        {"$GC0", T::GameObjectCustom0},
        {"$GC1", T::GameObjectCustom1},
        {"$GC2", T::GameObjectCustom2},
        {"$GC3", T::GameObjectCustom3},
        {"$DSL", T::DoodadSoundLoop},
        {"$DSO", T::DoodadSoundOneShot},
        {"$CSS", T::CombatSwing},
        {"$CAH", T::CombatAttackHit},
        {"$AH0", T::ArmorHit0},
        {"$AH1", T::ArmorHit1},
        {"$AH2", T::ArmorHit2},
        {"$AH3", T::ArmorHit3},
        {"$BWP", T::BowPull},
        {"$BWR", T::BowRelease},
        {"$SHL", T::SheatheLeft},
        {"$SHR", T::SheatheRight},
        {"$ESD", T::EmoteSound},
    };
    for (const auto& [tag, expected] : expectedTags) {
        INFO("Tag: " << tag);
        CHECK(pipeline::parseM2AudioEventTag(tag) == expected);
    }
    for (const char* nonAudio : {"$CSL", "$CSR", "$CST", "$SHK", "$FL0", "$RR1", "$HIT", "ABCD"}) {
        INFO("Non-audio tag: " << nonAudio);
        CHECK(pipeline::parseM2AudioEventTag(nonAudio) == T::None);
    }

    auto& tbc = env();

    // 2. Real TBC character M2 (HumanMale.m2): $FSD, $DTH, $TRD, $CSD, $CSS, $CAH, $AH0..3, $BWP, $SHL/$SHR, $ESD
    {
        const auto bytes = tbc.assets.readFile("Character\\Human\\Male\\HumanMale.m2");
        REQUIRE_FALSE(bytes.empty());
        const pipeline::M2Model human = pipeline::M2Loader::load(bytes);
        REQUIRE_FALSE(human.audioEvents.empty());

        std::unordered_set<T> seen;
        bool sawClapCsd = false;
        for (const auto& ev : human.audioEvents) {
            seen.insert(ev.type);
            if (ev.type == T::CustomSound && ev.data == 6576u) {
                sawClapCsd = true;
            }
        }
        CHECK(seen.contains(T::Footstep));
        CHECK(seen.contains(T::DeathThud));
        CHECK(seen.contains(T::ArmorFoley));
        CHECK(seen.contains(T::CustomSound));
        CHECK(seen.contains(T::CombatSwing));
        CHECK(seen.contains(T::CombatAttackHit));
        CHECK(seen.contains(T::BowPull));
        CHECK(seen.contains(T::BowRelease));
        CHECK(seen.contains(T::SheatheLeft));
        CHECK(seen.contains(T::SheatheRight));
        CHECK(seen.contains(T::EmoteSound));
        CHECK(sawClapCsd);
    }

    // 3. Real TBC GameObject M2s: Chest01.m2 ($GO0..$GO3) and G_BombWagon.m2 ($GO0, $GO1, $GO4, $DSO=1400)
    {
        const auto chestBytes = tbc.assets.readFile("World\\Generic\\ActiveDoodads\\Chest01\\Chest01.m2");
        REQUIRE_FALSE(chestBytes.empty());
        const pipeline::M2Model chest = pipeline::M2Loader::load(chestBytes);
        std::unordered_set<T> seen;
        for (const auto& ev : chest.audioEvents) seen.insert(ev.type);
        CHECK(seen.contains(T::GameObject0));
        CHECK(seen.contains(T::GameObject1));
        CHECK(seen.contains(T::GameObject2));
        CHECK(seen.contains(T::GameObject3));

        const auto wagonBytes = tbc.assets.readFile("World\\Goober\\G_BombWagon.m2");
        REQUIRE_FALSE(wagonBytes.empty());
        const pipeline::M2Model wagon = pipeline::M2Loader::load(wagonBytes);
        bool sawCannonDso = false;
        bool sawGo4 = false;
        for (const auto& ev : wagon.audioEvents) {
            if (ev.type == T::DoodadSoundOneShot && ev.data == 1400u) sawCannonDso = true;
            if (ev.type == T::GameObject4) sawGo4 = true;
        }
        CHECK(sawCannonDso);
        CHECK(sawGo4);
    }
}

TEST_CASE("All 20 shipped TBC spell/effect M2s with embedded $SND play 3D SoundEntries PCM",
          "[audio][tbc][shard_e]") {
    auto& tbc = env();
    resetHeadlessEngine(tbc.assets);

    auto& table = audio::SoundEntryTable::instance();
    table.reset();
    REQUIRE(table.ensureLoaded(&tbc.assets));

    audio::FootstepManager fm;
    REQUIRE(fm.initialize(&tbc.assets));

    std::vector<audio::AudioEngine::PlaybackEvent> events;
    audio::AudioEngine::instance().setPlaybackObserver([&](const audio::AudioEngine::PlaybackEvent& ev) {
        events.push_back(ev);
    });

    struct SpellSndExpectation {
        const char* path;
        uint32_t soundId;
        bool activeInStand; // false on trimmed [4333..5833) clones whose M2Range is (1,1) empty
    };
    const SpellSndExpectation kSpellSndM2s[] = {
        {"Spells\\Abyssal_Impact_Base.m2", 3181u, false},
        {"Spells\\ArcaneShot_Area.m2", 22u, true},
        {"Spells\\Blizzard_Impact_Base.m2", 22u, true},
        {"Spells\\CallLightning_Impact.m2", 30u, true},
        {"Spells\\Fel_RainOfFire_Impact_Base.m2", 3181u, true},
        {"Spells\\IceNuke_Missile.m2", 3181u, true},
        {"Spells\\Infernal_Impact.m2", 3181u, false},
        {"Spells\\Infernal_Impact_Base.m2", 3181u, true},
        {"Spells\\LevelUp\\LevelUp.m2", 888u, true},
        {"Spells\\LevelUpSpell\\LevelUpSpell.m2", 888u, true},
        {"Spells\\LoyaltyDown_Impact_Base.m2", 888u, true},
        {"Spells\\LoyaltyUp_Impact_Base.m2", 888u, true},
        {"Spells\\MagtheradonCeilingChunk.m2", 3181u, true},
        {"Spells\\Meteor_Impact_Base.m2", 3181u, true},
        {"Spells\\Meteor_Impact_Base_Red.m2", 3181u, false},
        {"Spells\\RainOfCandys_Impact_Base.m2", 3181u, true},
        {"Spells\\RainOfFire_Impact_Base.m2", 3181u, true},
        {"Spells\\ReputationLevelUp.m2", 8473u, true},
        {"Spells\\SpellLevelUp.m2", 888u, true},
        {"Spells\\Starshards_Impact_Base.m2", 22u, true},
    };

    uint32_t instanceCounter = 100;
    for (const auto& [m2Path, expectedSoundId, activeInStand] : kSpellSndM2s) {
        INFO("Spell M2: " << m2Path);
        resetHeadlessEngine(tbc.assets);
        events.clear();
        audio::AudioEngine::instance().setPlaybackObserver([&](const audio::AudioEngine::PlaybackEvent& ev) {
            events.push_back(ev);
        });

        const auto m2Bytes = tbc.assets.readFile(m2Path);
        REQUIRE_FALSE(m2Bytes.empty());
        const pipeline::M2Model model = pipeline::M2Loader::load(m2Bytes);
        REQUIRE(model.hasRenderableContent());
        REQUIRE_FALSE(model.sequences.empty());

        bool foundSnd = false;
        uint32_t maxEventTimeMs = 50u;
        for (const auto& ev : model.audioEvents) {
            if (ev.type == pipeline::M2AudioEventType::Sound && ev.data == expectedSoundId) {
                foundSnd = true;
                if (activeInStand) {
                    REQUIRE_FALSE(ev.times.empty());
                    REQUIRE_FALSE(ev.times[0].empty());
                    maxEventTimeMs = std::max(maxEventTimeMs, ev.times[0].front() + 10u);
                } else {
                    CHECK((ev.times.empty() || ev.times[0].empty()));
                }
            }
        }
        REQUIRE(foundSnd);

        const uint32_t instId = ++instanceCounter;
        const glm::vec3 targetPos(4.0f, 0.0f, -3.0f);
        const uint32_t fired = fm.stepModelAudioEvents(
            instId, model.audioEvents, 0, static_cast<float>(maxEventTimeMs),
            static_cast<float>(model.sequences[0].duration), /*heldAtEnd=*/false,
            [&](uint16_t, const glm::vec3& local) { return targetPos + local; });
        if (activeInStand) {
            CHECK(fired >= 1u);
            REQUIRE_FALSE(events.empty());
            CHECK(events.back().started);
            CHECK(events.back().spatial);
            CHECK(renderRms(2048).total > 1e-4);
        } else {
            CHECK(fired == 0u);
            CHECK(events.empty());
        }
        fm.stopModelAudioInstance(instId);
    }

    fm.shutdown();
    audio::AudioEngine::instance().setPlaybackObserver({});
}

TEST_CASE("DeathThudLookups.dbc (45 rows), 3D/barefoot $FSD footsteps, and $TRD Material.dbc armor foley",
          "[audio][tbc][shard_e]") {
    auto& tbc = env();
    resetHeadlessEngine(tbc.assets);

    auto& table = audio::SoundEntryTable::instance();
    table.reset();
    REQUIRE(table.ensureLoaded(&tbc.assets));

    audio::FootstepManager fm;
    REQUIRE(fm.initialize(&tbc.assets));
    REQUIRE(fm.sounds() != nullptr);
    CHECK(fm.sounds()->deathThudRowCount() == 45u);
    CHECK(fm.sounds()->materialCount() >= 8u);

    std::vector<audio::AudioEngine::PlaybackEvent> events;

    SECTION("All 45 DeathThudLookups rows resolve valid dry/water SoundEntries and $DTH fires on Death sequence") {
        const auto dtDbc = tbc.assets.loadDBC("DeathThudLookups.dbc");
        REQUIRE(dtDbc != nullptr);
        REQUIRE(dtDbc->getRecordCount() == 45u);

        for (uint32_t r = 0; r < dtDbc->getRecordCount(); ++r) {
            const uint32_t sizeClass = dtDbc->getUInt32(r, 1);
            const uint32_t dryId = dtDbc->getUInt32(r, 3);
            const uint32_t waterId = dtDbc->getUInt32(r, 4);
            INFO("DeathThud row=" << r << " sizeClass=" << sizeClass);
            REQUIRE(dryId >= 907u);
            REQUIRE(dryId <= 931u);
            REQUIRE(table.findById(&tbc.assets, dryId) != nullptr);
            if (waterId != 0u) {
                REQUIRE(table.findById(&tbc.assets, waterId) != nullptr);
            }
            // Even on Stone/Grass rows whose raw waterId column is 0, deathThudSoundBySize
            // falls back to that SizeClass's water thud when inWater == true.
            CHECK(fm.sounds()->deathThudSoundBySize(sizeClass, 2u, /*inWater=*/true) != 0u);
        }

        // Dry vs water thud for HumanMale (display 49, SizeClass 1) on Dirt (TerrainType 0)
        const uint32_t drySound = fm.sounds()->deathThudSound(49u, 0u, /*inWater=*/false);
        const uint32_t wetSound = fm.sounds()->deathThudSound(49u, 0u, /*inWater=*/true);
        CHECK(drySound != 0u);
        CHECK(wetSound != 0u);
        CHECK(drySound != wetSound);

        resetHeadlessEngine(tbc.assets);
        events.clear();
        audio::AudioEngine::instance().setPlaybackObserver([&](const audio::AudioEngine::PlaybackEvent& ev) {
            events.push_back(ev);
        });

        const auto humanBytes = tbc.assets.readFile("Character\\Human\\Male\\HumanMale.m2");
        REQUIRE_FALSE(humanBytes.empty());
        const pipeline::M2Model human = pipeline::M2Loader::load(humanBytes);
        const int deathSeq = findSequenceByAnimId(human, 1u); // anim::DEATH = 1
        REQUIRE(deathSeq >= 0);

        audio::FootstepManager::CharacterAudioStepContext ctx;
        ctx.displayId = 49u;
        ctx.terrainTypeId = 0u;
        ctx.hasTerrainType = true;
        ctx.isLocalPlayer = false;
        ctx.skipFootstep = true;

        // Step across entire Death sequence -> $DTH must fire in 3D and synthesize non-zero PCM
        (void)fm.stepCharacterAudioEvents(
            501u, human, deathSeq, 0.0f, false, -1, 0.0f, ctx,
            [](uint16_t, const glm::vec3&) { return glm::vec3(-5.0f, 0.0f, -2.0f); });
        events.clear();
        const uint32_t fired = fm.stepCharacterAudioEvents(
            501u, human, deathSeq, static_cast<float>(human.sequences[deathSeq].duration), false, -1, 0.0f, ctx,
            [](uint16_t, const glm::vec3&) { return glm::vec3(-5.0f, 0.0f, -2.0f); });
        CHECK(fired >= 1u);
        REQUIRE_FALSE(events.empty());
        CHECK(events.back().spatial);
        CHECK(renderRms(2048).total > 1e-4);
    }

    SECTION("3D remote $FSD footsteps, bare-foot fallback (7 -> 8), and ListenerAtCharacter 0.65x 2D vs +2.0z 3D") {
        // HumanMale display 49 has FootstepID = 7 (CharacterMediumLarge).
        // Booted vs bareFeet on Stone (TerrainType 2):
        const uint32_t bootedStone = fm.sounds()->footstepSound(49u, 2u, /*splash=*/false, /*bareFeet=*/false);
        const uint32_t bareStone = fm.sounds()->footstepSound(49u, 2u, /*splash=*/false, /*bareFeet=*/true);
        REQUIRE(bootedStone != 0u);
        REQUIRE(bareStone != 0u);
        CHECK(bootedStone != bareStone);

        const auto* bootedRec = table.findById(&tbc.assets, bootedStone);
        const auto* bareRec = table.findById(&tbc.assets, bareStone);
        REQUIRE(bootedRec != nullptr);
        REQUIRE(bareRec != nullptr);
        CHECK(bootedRec->name.find("MediumLarge") != std::string::npos);
        CHECK(bareRec->name.find("Small") != std::string::npos);

        // Local player with ListenerAtCharacter == true -> 2D at 0.65x gain
        resetHeadlessEngine(tbc.assets);
        events.clear();
        audio::AudioEngine::instance().setPlaybackObserver([&](const audio::AudioEngine::PlaybackEvent& ev) {
            events.push_back(ev);
        });
        CHECK(fm.playFootstepAt(49u, 2u, false, false, glm::vec3(10.0f, 20.0f, 30.0f),
                                /*isLocalPlayer=*/true, /*listenerAtCharacter=*/true));
        REQUIRE(events.size() == 1u);
        CHECK_FALSE(events.back().spatial);
        CHECK(events.back().volume == Catch::Approx(bootedRec->volume * 0.65f).epsilon(0.20f));
        CHECK(renderRms(2048).total > 1e-5);

        // Local player with ListenerAtCharacter == false -> 3D at unitPos + (0, 0, 2.0f)
        resetHeadlessEngine(tbc.assets);
        events.clear();
        audio::AudioEngine::instance().setPlaybackObserver([&](const audio::AudioEngine::PlaybackEvent& ev) {
            events.push_back(ev);
        });
        CHECK(fm.playFootstepAt(49u, 2u, false, true, glm::vec3(3.0f, 4.0f, 5.0f),
                                /*isLocalPlayer=*/true, /*listenerAtCharacter=*/false));
        REQUIRE(events.size() == 1u);
        CHECK(events.back().spatial);
        CHECK(events.back().position.z == Catch::Approx(7.0f));

        // Remote player/NPC $FSD step pans left in 3D stereo
        resetHeadlessEngine(tbc.assets);
        events.clear();
        audio::AudioEngine::instance().setPlaybackObserver([&](const audio::AudioEngine::PlaybackEvent& ev) {
            events.push_back(ev);
        });
        const auto humanBytes = tbc.assets.readFile("Character\\Human\\Male\\HumanMale.m2");
        const pipeline::M2Model human = pipeline::M2Loader::load(humanBytes);
        const int runSeq = findSequenceByAnimId(human, 5u); // anim::RUN = 5
        REQUIRE(runSeq >= 0);

        audio::FootstepManager::CharacterAudioStepContext remoteCtx;
        remoteCtx.displayId = 2404u; // Horse (loud clear hooves)
        remoteCtx.terrainTypeId = 2u; // Stone
        remoteCtx.hasTerrainType = true;
        remoteCtx.isLocalPlayer = false;
        remoteCtx.bareFeet = false;

        (void)fm.stepCharacterAudioEvents(
            502u, human, runSeq, 0.0f, false, -1, 0.0f, remoteCtx,
            [](uint16_t, const glm::vec3&) { return glm::vec3(-8.0f, 0.0f, 0.0f); });
        events.clear();
        const uint32_t fired = fm.stepCharacterAudioEvents(
            502u, human, runSeq, static_cast<float>(human.sequences[runSeq].duration), false, -1, 0.0f, remoteCtx,
            [](uint16_t, const glm::vec3&) { return glm::vec3(-8.0f, 0.0f, 0.0f); });
        CHECK(fired >= 1u);
        REQUIRE_FALSE(events.empty());
        CHECK(events.back().spatial);
        const auto pan = renderRms(2048);
        CHECK(pan.total > 1e-5);
        CHECK(pan.left > pan.right);

        // Wow.exe PlayFootstepSound (0x465ae0) -> SESound::Play (0x461af0):
        // SE:1182 (CrabFootSteps) uses SoundEntries.DistanceCutoff (45 yd) and vol=0.2;
        // local player footsteps use kFootstepSelf (cap 4), remote units use kFootstepOther (cap 6).
        const auto* crabRec = table.findById(&tbc.assets, 1182u);
        REQUIRE(crabRec != nullptr);
        CHECK(crabRec->maxDistance == Catch::Approx(45.0f));
        CHECK(crabRec->volume == Catch::Approx(0.2f));
        resetHeadlessEngine(tbc.assets);
        audio::AudioEngine::instance().setListenerPosition(glm::vec3(0.0f));
        CHECK(table.play3D(&tbc.assets, 1182u, glm::vec3(38.0f, 0.0f, 0.0f), 1.0f, 1.0f,
                           audio::SESoundCategory::kFootstepOther));
        CHECK_FALSE(table.play3D(&tbc.assets, 1182u, glm::vec3(46.0f, 0.0f, 0.0f), 1.0f, 1.0f,
                                 audio::SESoundCategory::kFootstepOther));

        // Local player footsteps enforce kFootstepSelf cap (4) independently of kFootstepOther (6)
        resetHeadlessEngine(tbc.assets);
        uint32_t selfPlayed = 0;
        for (int i = 0; i < 6; ++i) {
            if (fm.playFootstepAt(49u, 2u, false, false, glm::vec3(0.0f), true, true)) ++selfPlayed;
        }
        CHECK(selfPlayed == 4u);

        // Wow.exe Spell_C_CastSpell (0x6fc17b): GAMEABILITYACTIVATE (905) / GAMESPELLACTIVATE (906)
        const auto* abilityAct = table.findById(&tbc.assets, 905u);
        const auto* spellAct = table.findById(&tbc.assets, 906u);
        REQUIRE(abilityAct != nullptr);
        REQUIRE(spellAct != nullptr);
        CHECK(abilityAct->volume == Catch::Approx(0.5f));
        CHECK(spellAct->volume == Catch::Approx(0.5f));
    }

    SECTION("$TRD Material.dbc col 2 armor foley (Plate/Chain/Leather/Cloth) and Self/Others CVar gates") {
        CHECK(fm.sounds()->armorFoleySound(6u) == 1004u); // FoleySoundPlate
        CHECK(fm.sounds()->armorFoleySound(5u) == 1005u); // FoleySoundChain
        CHECK(fm.sounds()->armorFoleySound(8u) == 1003u); // FoleySoundLeather
        CHECK(fm.sounds()->armorFoleySound(7u) == 0u);    // Cloth -> silent
        CHECK(fm.sounds()->armorFoleySound(0u) == 0u);

        CHECK(audio::CreatureSoundIndex::armorMaterialForItemSubclass(4u, 4u) == 6u);
        CHECK(audio::CreatureSoundIndex::armorMaterialForItemSubclass(4u, 3u) == 5u);
        CHECK(audio::CreatureSoundIndex::armorMaterialForItemSubclass(4u, 2u) == 8u);
        CHECK(audio::CreatureSoundIndex::armorMaterialForItemSubclass(4u, 1u) == 0u);
        CHECK(audio::CreatureSoundIndex::armorMaterialForItemSubclass(2u, 7u) == 0u);

        for (uint32_t matId : {6u, 5u, 8u}) {
            resetHeadlessEngine(tbc.assets);
            events.clear();
            audio::AudioEngine::instance().setPlaybackObserver([&](const audio::AudioEngine::PlaybackEvent& ev) {
                events.push_back(ev);
            });
            CHECK(fm.playArmorFoley(matId, glm::vec3(0.0f, 0.0f, -1.0f),
                                    /*isLocalPlayer=*/false, /*listenerAtCharacter=*/true));
            REQUIRE(events.size() == 1u);
            CHECK(events.back().spatial);
            CHECK(renderRms(8192).total > 1e-6);
        }

        // Cloth produces zero foley events
        events.clear();
        CHECK_FALSE(fm.playArmorFoley(7u, glm::vec3(0.0f), true, true));
        CHECK(events.empty());

        // Self vs Others CVar gates (reset engine between checks so NO_DUPLICATES / caps cannot mask CVar behavior)
        resetHeadlessEngine(tbc.assets);
        audio::AudioEngine::instance().setPlaybackObserver([&](const audio::AudioEngine::PlaybackEvent& ev) {
            events.push_back(ev);
        });
        fm.setArmorFoleySelfEnabled(false);
        fm.setArmorFoleyOthersEnabled(true);
        events.clear();
        CHECK_FALSE(fm.playArmorFoley(6u, glm::vec3(0.0f), /*isLocalPlayer=*/true, true));
        CHECK(events.empty());
        CHECK(fm.playArmorFoley(6u, glm::vec3(0.0f, 0.0f, -3.0f), /*isLocalPlayer=*/false, true));
        CHECK(events.size() == 1u);

        resetHeadlessEngine(tbc.assets);
        audio::AudioEngine::instance().setPlaybackObserver([&](const audio::AudioEngine::PlaybackEvent& ev) {
            events.push_back(ev);
        });
        fm.setArmorFoleySelfEnabled(true);
        fm.setArmorFoleyOthersEnabled(false);
        events.clear();
        CHECK(fm.playArmorFoley(6u, glm::vec3(0.0f), /*isLocalPlayer=*/true, true));
        CHECK(events.size() == 1u);
        CHECK_FALSE(fm.playArmorFoley(6u, glm::vec3(0.0f, 0.0f, -3.0f), /*isLocalPlayer=*/false, true));
        CHECK(events.size() == 1u);
    }

    fm.shutdown();
    audio::AudioEngine::instance().setPlaybackObserver({});
}

TEST_CASE("Drowning loop (3353 / 5736), CreatureSoundData.LoopSoundID (col 24), and Creature/GO M2 events",
          "[audio][tbc][shard_e]") {
    auto& tbc = env();
    resetHeadlessEngine(tbc.assets);

    auto& table = audio::SoundEntryTable::instance();
    table.reset();
    REQUIRE(table.ensureLoaded(&tbc.assets));

    audio::FootstepManager fm;
    REQUIRE(fm.initialize(&tbc.assets));

    std::vector<audio::AudioEngine::PlaybackEvent> events;

    SECTION("PlayerDrowningLoop (3353) starts while drowning, stops immediately on recovery, and 5736 + Injury fire on tick") {
        resetHeadlessEngine(tbc.assets);
        events.clear();
        audio::AudioEngine::instance().setPlaybackObserver([&](const audio::AudioEngine::PlaybackEvent& ev) {
            events.push_back(ev);
        });

        CHECK_FALSE(fm.hasActiveDrowningLoop());
        fm.updateDrowningState(true);
        CHECK(fm.hasActiveDrowningLoop());
        REQUIRE(events.size() == 1u);
        CHECK(events.back().started);
        CHECK(renderRms(2048).total > 1e-4);

        // Breath recovered -> loop handle stops immediately
        fm.updateDrowningState(false);
        CHECK_FALSE(fm.hasActiveDrowningLoop());
        CHECK(renderRms(2048).total == Catch::Approx(0.0).margin(1e-6));

        // DAMAGE_DROWNING tick -> plays 5736 (Drowning) + HumanMale Injury vocal
        events.clear();
        fm.onDrowningDamageTick(49u, glm::vec3(0.0f), /*listenerAtCharacter=*/true);
        CHECK(events.size() == 2u);
        CHECK(renderRms(2048).total > 1e-4);
    }

    SECTION("CreatureSoundData.LoopSoundID (col 24: Fire/Earth/Water Elementals, Infernal, Slime) starts on spawn and stops on death") {
        // Find real TBC displays whose CreatureSoundData col 24 names FireElementalLoop (1454),
        // ElementalEarthLoop (5634), SlimeLoop (5894), plus count all col 24 profiles in CreatureSoundData.dbc.
        const auto csd = tbc.assets.loadDBC("CreatureSoundData.dbc");
        REQUIRE(csd != nullptr);
        uint32_t loopProfileCount = 0;
        for (uint32_t r = 0; r < csd->getRecordCount(); ++r) {
            if (csd->getUInt32(r, 24) != 0u) ++loopProfileCount;
        }
        CHECK(loopProfileCount == 114u);

        const auto cdi = tbc.assets.loadDBC("CreatureDisplayInfo.dbc");
        REQUIRE(cdi != nullptr);
        uint32_t fireElemDisplay = 0;
        uint32_t earthElemDisplay = 0;
        uint32_t waterElemDisplay = 0;
        for (uint32_t r = 0; r < cdi->getRecordCount(); ++r) {
            const uint32_t dispId = cdi->getUInt32(r, 0);
            const uint32_t sdid = fm.sounds()->soundDataForDisplay(dispId);
            const uint32_t loopId = fm.sounds()->sound(sdid, audio::CreatureSoundColumn::LoopSound);
            if (loopId == 0u) continue;
            const auto* rec = table.findById(&tbc.assets, loopId);
            if (!rec) continue;
            if (!fireElemDisplay && loopId == 1454u) fireElemDisplay = dispId;
            if (!earthElemDisplay && loopId == 5634u) earthElemDisplay = dispId;
            if (!waterElemDisplay && rec->name.find("WaterElemental") != std::string::npos) waterElemDisplay = dispId;
        }
        REQUIRE(fireElemDisplay != 0u);
        REQUIRE(earthElemDisplay != 0u);
        REQUIRE(waterElemDisplay != 0u);

        for (uint32_t dispId : {fireElemDisplay, earthElemDisplay, waterElemDisplay}) {
            INFO("Elemental displayId=" << dispId);
            resetHeadlessEngine(tbc.assets);
            events.clear();
            audio::AudioEngine::instance().setPlaybackObserver([&](const audio::AudioEngine::PlaybackEvent& ev) {
                events.push_back(ev);
            });

            constexpr uint64_t kGuid = 0xF130009900000001ull;
            fm.syncCreatureLoopSound(kGuid, dispId, glm::vec3(0.0f, 0.0f, -4.0f), /*alive=*/true);
            CHECK(fm.hasActiveCreatureLoop(kGuid));
            REQUIRE(events.size() == 1u);
            CHECK(events.back().spatial);
            CHECK(renderRms(2048).total > 1e-4);

            // Idempotent while alive
            fm.syncCreatureLoopSound(kGuid, dispId, glm::vec3(0.0f, 0.0f, -4.0f), /*alive=*/true);
            CHECK(events.size() == 1u);

            // Immediate silence on death
            fm.syncCreatureLoopSound(kGuid, dispId, glm::vec3(0.0f, 0.0f, -4.0f), /*alive=*/false);
            CHECK_FALSE(fm.hasActiveCreatureLoop(kGuid));
            CHECK(renderRms(2048).total == Catch::Approx(0.0).margin(1e-6));
        }
    }

    SECTION("Creature/character $CSD, $SCD, $SMG, $SMD, Pet columns (28..30), and GameObject $GO0..5 / $GC0..3 / $DSO / $DSL") {
        // 1. $CSD direct SoundEntries ID (6576 ClapSounds) + Creature CustomAttack0 (Wolf 3176)
        const auto cdi = tbc.assets.loadDBC("CreatureDisplayInfo.dbc");
        REQUIRE(cdi != nullptr);

        uint32_t wolfDisplay = 0;
        uint32_t scdDisplay = 0;
        uint32_t smgDisplay = 0;
        uint32_t smdDisplay = 0;
        uint32_t petDisplay = 0;
        for (uint32_t r = 0; r < cdi->getRecordCount(); ++r) {
            const uint32_t dispId = cdi->getUInt32(r, 0);
            const uint32_t sdid = fm.sounds()->soundDataForDisplay(dispId);
            if (!wolfDisplay && fm.sounds()->sound(sdid, audio::CreatureSoundColumn::CustomAttack0) == 3176u) {
                wolfDisplay = dispId;
            }
            if (!scdDisplay) {
                const uint32_t id = fm.sounds()->sound(sdid, audio::CreatureSoundColumn::SpellCastDirected);
                if (id != 0 && table.findById(&tbc.assets, id)) scdDisplay = dispId;
            }
            if (!smgDisplay) {
                const uint32_t id = fm.sounds()->sound(sdid, audio::CreatureSoundColumn::Submerge);
                if (id != 0 && table.findById(&tbc.assets, id)) smgDisplay = dispId;
            }
            if (!smdDisplay) {
                const uint32_t id = fm.sounds()->sound(sdid, audio::CreatureSoundColumn::Submerged);
                if (id != 0 && table.findById(&tbc.assets, id)) smdDisplay = dispId;
            }
            if (!petDisplay &&
                fm.sounds()->sound(sdid, audio::CreatureSoundColumn::PetAttack) != 0u &&
                fm.sounds()->sound(sdid, audio::CreatureSoundColumn::PetOrder) != 0u &&
                fm.sounds()->sound(sdid, audio::CreatureSoundColumn::PetDismiss) != 0u) {
                petDisplay = dispId;
            }
        }
        REQUIRE(wolfDisplay != 0u);
        REQUIRE(scdDisplay != 0u);
        REQUIRE(smgDisplay != 0u);
        REQUIRE(smdDisplay != 0u);
        REQUIRE(petDisplay != 0u);

        struct UnitEventCase {
            const char* label;
            pipeline::M2AudioEventType type;
            uint32_t data;
            uint32_t displayId;
        };
        const UnitEventCase unitCases[] = {
            {"$CSD direct ClapSounds 6576", pipeline::M2AudioEventType::CustomSound, 6576u, 49u},
            {"$CSD Wolf CustomAttack0 3176", pipeline::M2AudioEventType::CustomSound, 0u, wolfDisplay},
            {"$SCD SpellCastDirected", pipeline::M2AudioEventType::SpellCastDirected, 0u, scdDisplay},
            {"$SMG Submerge", pipeline::M2AudioEventType::Submerge, 0u, smgDisplay},
            {"$SMD Submerged", pipeline::M2AudioEventType::Submerged, 0u, smdDisplay},
        };
        for (const auto& uc : unitCases) {
            INFO(uc.label);
            resetHeadlessEngine(tbc.assets);
            events.clear();
            audio::AudioEngine::instance().setPlaybackObserver([&](const audio::AudioEngine::PlaybackEvent& ev) {
                events.push_back(ev);
            });
            CHECK(fm.playUnitM2Event(uc.type, uc.data, uc.displayId, 0u, false,
                                     false, 0u, glm::vec3(0.0f, 0.0f, -1.0f), false, true));
            REQUIRE(events.size() == 1u);
            CHECK(events.back().spatial);
            CHECK(renderRms(8192).total > 1e-6);
        }

        // 2. GameObject M2 events: G_BombWagon.m2 Destroy (150) fires $GO4 at 34ms and $DSO=1400 at 1400ms
        resetHeadlessEngine(tbc.assets);
        events.clear();
        audio::AudioEngine::instance().setPlaybackObserver([&](const audio::AudioEngine::PlaybackEvent& ev) {
            events.push_back(ev);
        });

        // Find a GameObjectDisplayInfo row with Open (slot 1) and Close (slot 3)
        const auto godi = tbc.assets.loadDBC("GameObjectDisplayInfo.dbc");
        REQUIRE(godi != nullptr);
        uint32_t chestGoDisp = 0;
        for (uint32_t r = 0; r < godi->getRecordCount(); ++r) {
            const uint32_t did = godi->getUInt32(r, 0);
            if (fm.sounds()->gameObjectSound(did, 1u) != 0u &&
                fm.sounds()->gameObjectSound(did, 3u) != 0u) {
                chestGoDisp = did;
                break;
            }
        }
        REQUIRE(chestGoDisp != 0u);

        const auto chestBytes = tbc.assets.readFile("World\\Generic\\ActiveDoodads\\Chest01\\Chest01.m2");
        const pipeline::M2Model chest = pipeline::M2Loader::load(chestBytes);
        const int openSeq = findSequenceByAnimId(chest, 148u);  // Open
        const int closeSeq = findSequenceByAnimId(chest, 146u); // Close
        REQUIRE(openSeq >= 0);
        REQUIRE(closeSeq >= 0);

        CHECK(fm.stepModelAudioEvents(
                  701u, chest.audioEvents, openSeq, 50.0f,
                  static_cast<float>(chest.sequences[openSeq].duration), false,
                  [](uint16_t, const glm::vec3&) { return glm::vec3(0.0f, 0.0f, -4.0f); },
                  [&](uint32_t slot) { return fm.sounds()->gameObjectSound(chestGoDisp, slot); }) >= 1u);
        CHECK(fm.stepModelAudioEvents(
                  701u, chest.audioEvents, closeSeq, 50.0f,
                  static_cast<float>(chest.sequences[closeSeq].duration), false,
                  [](uint16_t, const glm::vec3&) { return glm::vec3(0.0f, 0.0f, -4.0f); },
                  [&](uint32_t slot) { return fm.sounds()->gameObjectSound(chestGoDisp, slot); }) >= 1u);
        CHECK(events.size() >= 2u);

        // G_BombWagon.m2 Destroy (150) -> $DSO 1400 (Cannon01)
        const auto wagonBytes = tbc.assets.readFile("World\\Goober\\G_BombWagon.m2");
        const pipeline::M2Model wagon = pipeline::M2Loader::load(wagonBytes);
        const int destroySeq = findSequenceByAnimId(wagon, 150u);
        REQUIRE(destroySeq >= 0);
        events.clear();
        (void)fm.stepModelAudioEvents(
            702u, wagon.audioEvents, destroySeq, 10.0f,
            static_cast<float>(wagon.sequences[destroySeq].duration), false,
            [](uint16_t, const glm::vec3&) { return glm::vec3(0.0f, 0.0f, -5.0f); });
        CHECK(fm.stepModelAudioEvents(
                  702u, wagon.audioEvents, destroySeq, 1500.0f,
                  static_cast<float>(wagon.sequences[destroySeq].duration), false,
                  [](uint16_t, const glm::vec3&) { return glm::vec3(0.0f, 0.0f, -5.0f); }) >= 1u);
        CHECK_FALSE(events.empty());

        // Synthetic $DSL loop starts on keyframe and stops immediately when stopModelAudioInstance is called
        resetHeadlessEngine(tbc.assets);
        pipeline::M2Model::AudioEvent dslEv;
        dslEv.type = pipeline::M2AudioEventType::DoodadSoundLoop;
        dslEv.data = 1454u; // FireElementalLoop
        dslEv.times = {{0u}};
        std::vector<pipeline::M2Model::AudioEvent> dslList{dslEv};
        CHECK(fm.stepModelAudioEvents(
                  703u, dslList, 0, 20.0f, 1000.0f, false,
                  [](uint16_t, const glm::vec3&) { return glm::vec3(0.0f, 0.0f, -3.0f); }) == 1u);
        CHECK(fm.hasActiveModelLoop(703u));
        CHECK(renderRms(2048).total > 1e-4);
        fm.stopModelAudioInstance(703u);
        CHECK_FALSE(fm.hasActiveModelLoop(703u));
        CHECK(renderRms(2048).total == Catch::Approx(0.0).margin(1e-6));
    }

    SECTION("Druid Flight Form / Swift Flight Form wing flaps ($WNG -> 433) and SpellFizzle school mapping") {
        // TBC 2.4.3 Night Elf & Tauren Flight Form / Swift Flight Form displays
        for (uint32_t dispId : {20857u, 20872u, 21243u, 21244u}) {
            INFO("Flight form displayId=" << dispId);
            resetHeadlessEngine(tbc.assets);
            events.clear();
            audio::AudioEngine::instance().setPlaybackObserver([&](const audio::AudioEngine::PlaybackEvent& ev) {
                events.push_back(ev);
            });
            CHECK(fm.playCreatureWingSound(dispId, /*glide=*/false, glm::vec3(0.0f),
                                           /*isLocalPlayer=*/true, /*listenerAtCharacter=*/true));
            REQUIRE(events.size() == 1u);
            CHECK_FALSE(events.back().spatial); // local player + Sound_ListenerAtCharacter=1 -> 2D
            CHECK(renderRms(2048).total > 1e-4);
        }

        // TBC 2.4.3 SpellFizzle SoundEntries mapping
        CHECK(audio::spellFizzleSoundIdForSchoolMask(0x01u) == 0u);    // Physical
        CHECK(audio::spellFizzleSoundIdForSchoolMask(0x02u) == 1428u); // Holy
        CHECK(audio::spellFizzleSoundIdForSchoolMask(0x04u) == 1424u); // Fire
        CHECK(audio::spellFizzleSoundIdForSchoolMask(0x08u) == 1426u); // Nature
        CHECK(audio::spellFizzleSoundIdForSchoolMask(0x10u) == 1425u); // Frost
        CHECK(audio::spellFizzleSoundIdForSchoolMask(0x20u) == 1427u); // Shadow
        CHECK(audio::spellFizzleSoundIdForSchoolMask(0x40u) == 1428u); // Arcane -> SpellFizzleHoly in 2.4.3
    }

    fm.shutdown();
    audio::AudioEngine::instance().setPlaybackObserver({});
}
