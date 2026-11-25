// TBC 2.4.3 (build 8606) Audio Parity Shard D validation suite:
//   - GAP-06: VocalUISounds.dbc (641 rows), all 58 vocal GameError codes (46 VocalUIEnums),
//             and SESound::IsPlaying(&s_hErrorSpeechSound) active-handle gating.
//   - GAP-07: EmotesTextSound.dbc (522 rows), local-player SMSG_TEXT_EMOTE / slash self-playback
//             with echo deduplication, and $ESD Emotes.dbc col 6 (EmoteSoundID) helper.
//   - GAP-08: CreatureSoundData.dbc race, shapeshift, and 3D remote-player vocals across all
//             20 playable race-gender profiles + Druid/Shaman forms, with zero *MainJump.wav.
//
// Run with:
//   WOWEE_TBC_DATA_DIR=/workspace/azerothcore/client/wowee-data/expansions/tbc ./build/bin/test_audio_shard_d_vocals_emotes

#include <catch_amalgamated.hpp>

#include "game_replay.hpp"

#include "audio/activity_sound_manager.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/player_voice_manager.hpp"
#include "audio/sound_entries.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"
#include "rendering/animation_controller.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using namespace wowee;

namespace {

struct ShardDAudioEnv {
    pipeline::AssetManager assets;
    pipeline::DBCLayout dbcLayout;
    bool ready = false;

    ShardDAudioEnv() {
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

    ~ShardDAudioEnv() {
        audio::SoundEntryTable::instance().reset();
        audio::AudioEngine::instance().setPlaybackObserver({});
        audio::AudioEngine::instance().setAssetManager(nullptr);
        audio::AudioEngine::instance().shutdown();
        pipeline::setActiveDBCLayout(nullptr);
        assets.shutdown();
    }
};

ShardDAudioEnv& env() {
    static ShardDAudioEnv instance;
    if (!instance.ready) {
        SKIP("Set WOWEE_TBC_DATA_DIR to extracted TBC 2.4.3 data directory");
    }
    return instance;
}

struct MixRms {
    double left = 0.0;
    double right = 0.0;
    double total = 0.0;
};

MixRms mixFramesRms(uint32_t frames) {
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
    MixRms out;
    out.left = std::sqrt(sumL / std::max<uint32_t>(1u, frames));
    out.right = std::sqrt(sumR / std::max<uint32_t>(1u, frames));
    out.total = std::sqrt((sumL + sumR) / std::max<uint32_t>(1u, frames * 2u));
    return out;
}

void drainHeadlessMixer() {
    auto& engine = audio::AudioEngine::instance();
    std::vector<float> scratch(8192u * 2u, 0.0f);
    for (int i = 0; i < 64 && engine.getActiveSoundCount() > 0; ++i) {
        (void)engine.renderMixFrames(scratch.data(), 8192u);
    }
}

constexpr std::array<uint8_t, 10> kPlayableRaces = {1, 2, 3, 4, 5, 6, 7, 8, 10, 11};

} // namespace

TEST_CASE("GAP-06: VocalUISounds.dbc loads 641 rows, maps all 58 vocal GameErrors, and gates on active handle",
          "[audio][shard_d][gap06]") {
    auto& tbc = env();
    auto& engine = audio::AudioEngine::instance();
    engine.shutdown();
    REQUIRE(engine.initializeHeadless(48000, 2));
    engine.setAssetManager(&tbc.assets);
    engine.setMasterVolume(1.0f);
    engine.setSuspended(false);

    auto& table = audio::SoundEntryTable::instance();
    table.reset();
    REQUIRE(table.ensureLoaded(&tbc.assets));

    audio::PlayerVoiceManager pvm;
    REQUIRE(pvm.initialize(&tbc.assets));
    CHECK(pvm.vocalUIRowCount() == 641u);

    std::vector<audio::AudioEngine::PlaybackEvent> events;
    engine.setPlaybackObserver([&](const audio::AudioEngine::PlaybackEvent& ev) {
        events.push_back(ev);
    });

    SECTION("All 58 vocal GameErrors from Wow.exe 0xb98770 map to exact VocalUIEnum categories") {
        struct ExpectedGameError {
            const char* name;
            uint32_t vocalUiEnum;
        };
        constexpr std::array<ExpectedGameError, 58> kExpected = {{
            {"ERR_INV_FULL",                    0},
            {"ERR_NOAMMO_S",                    1},
            {"ERR_CANT_EQUIP_LEVEL_I",          2},
            {"ERR_CANT_EQUIP_EVER",             3},
            {"ERR_DROP_BOUND_ITEM",             4},
            {"ERR_ITEM_COOLDOWN",               5},
            {"ERR_FOOD_COOLDOWN",               7},
            {"ERR_GROUP_FULL",                  8},
            {"ERR_OUT_OF_RANGE",               10},
            {"ERR_INVALID_ATTACK_TARGET",      11},
            {"ERR_SPELL_COOLDOWN",             12},
            {"ERR_SPELL_ALREADY_KNOWN_S",      13},
            {"ERR_OUT_OF_MANA",                15},
            {"ERR_ALREADY_IN_GROUP_S",         20},
            {"ERR_BANKSLOT_INSUFFICIENT_FUNDS",22},
            {"ERR_NOT_A_BAG",                  25},
            {"ERR_BAG_IN_BAG",                 26},
            {"ERR_WRONG_SLOT",                 27},
            {"ERR_AMMO_ONLY",                  28},
            {"ERR_WRONG_BAG_TYPE_SUBCLASS",    28},
            {"ERR_BAG_FULL",                   29},
            {"ERR_ITEM_MAX_COUNT",             30},
            {"ERR_LOOT_DIDNT_KILL",            31},
            {"ERR_LOOT_BAD_FACING",            32},
            {"ERR_LOOT_LOCKED",                33},
            {"ERR_LOOT_TOO_FAR",               35},
            {"ERR_NO_ATTACK_TARGET",           38},
            {"ERR_NOT_ENOUGH_MONEY",           40},
            {"ERR_GUILDEMBLEM_NOTENOUGHMONEY", 40},
            {"ERR_CANT_EQUIP_SKILL",           41},
            {"ERR_2HANDED_EQUIPPED",           42},
            {"ERR_2HSKILLNOTFOUND",            43},
            {"ERR_NOT_EQUIPPABLE",             44},
            {"ERR_GENERIC_NO_TARGET",          45},
            {"ERR_SPELL_OUT_OF_RANGE",         46},
            {"ERR_POTION_COOLDOWN",            47},
            {"ERR_PROFICIENCY_NEEDED",         48},
            {"ERR_MUST_EQUIP_ITEM",            49},
            {"ERR_ABILITY_COOLDOWN",           50},
            {"ERR_CANT_USE_ITEM",              51},
            {"ERR_CANT_USE_ITEM_IN_ARENA",     51},
            {"ERR_INVALID_ITEM_TARGET",        51},
            {"ERR_CHEST_IN_USE",               52},
            {"ERR_TAXINOTENOUGHMONEY",         54},
            {"ERR_USE_TOO_FAR",                57},
            {"ERR_TRADE_BOUND_ITEM",           59},
            {"ERR_TRADE_QUEST_ITEM",           59},
            {"ERR_MAIL_QUEST_ITEM",            59},
            {"ERR_MAIL_BOUND_ITEM",            59},
            {"ERR_AUCTION_QUEST_ITEM",         59},
            {"ERR_AUCTION_BOUND_ITEM",         59},
            {"ERR_ITEM_LOCKED",                61},
            {"ERR_USE_LOCKED",                 61},
            {"ERR_DOOR_LOCKED",                61},
            {"ERR_BUTTON_LOCKED",              61},
            {"ERR_GUILD_PERMISSIONS",          62},
            {"ERR_OUT_OF_RAGE",                63},
            {"ERR_OUT_OF_ENERGY",              64},
        }};

        std::set<uint32_t> distinctEnums;
        for (const auto& exp : kExpected) {
            INFO("GameError: " << exp.name);
            const auto got = audio::PlayerVoiceManager::vocalUIEnumForGameError(exp.name);
            REQUIRE(got.has_value());
            CHECK(*got == exp.vocalUiEnum);
            distinctEnums.insert(*got);
        }
        CHECK(distinctEnums.size() == 46u);

        // Non-vocal GameErrors (VocalUIEnum == 68 in Wow.exe) must not map to speech.
        CHECK_FALSE(audio::PlayerVoiceManager::vocalUIEnumForGameError("ERR_BANK_FULL").has_value());
        CHECK_FALSE(audio::PlayerVoiceManager::vocalUIEnumForGameError("ERR_DESTROY_NONEMPTY_BAG").has_value());
        CHECK_FALSE(audio::PlayerVoiceManager::vocalUIEnumForGameError("ERR_CANT_EQUIP_RANK").has_value());
    }

    SECTION("Previously missing 43 (ERR_2HSKILLNOTFOUND), 48 (ERR_PROFICIENCY_NEEDED), and unwired categories play across all 20 race-genders") {
        // Verify exact Human Male SoundEntries IDs from VocalUISounds.dbc
        CHECK(pvm.getVocalUISoundId(0, 1, 0) == 1875u);   // ERR_INV_FULL
        CHECK(pvm.getVocalUISoundId(43, 1, 0) == 1910u);  // ERR_2HSKILLNOTFOUND
        CHECK(pvm.getVocalUISoundId(48, 1, 0) == 1915u);  // ERR_PROFICIENCY_NEEDED
        CHECK(pvm.getVocalUISoundId(0, 1, 0, /*pissed=*/true) == 626u);

        constexpr const char* kSampleErrors[] = {
            "ERR_2HSKILLNOTFOUND",
            "ERR_PROFICIENCY_NEEDED",
            "ERR_GROUP_FULL",
            "ERR_ALREADY_IN_GROUP_S",
            "ERR_BAG_IN_BAG",
            "ERR_WRONG_SLOT",
            "ERR_LOOT_DIDNT_KILL",
            "ERR_LOOT_LOCKED",
            "ERR_LOOT_TOO_FAR",
            "ERR_POTION_COOLDOWN",
            "ERR_MUST_EQUIP_ITEM",
            "ERR_TRADE_BOUND_ITEM",
            "ERR_GUILD_PERMISSIONS",
        };

        for (uint8_t raceId : kPlayableRaces) {
            for (uint8_t gender : {uint8_t(0), uint8_t(1)}) {
                for (const char* errName : kSampleErrors) {
                    INFO("Race=" << int(raceId) << " Gender=" << int(gender) << " Error=" << errName);
                    pvm.stopErrorSpeech();
                    drainHeadlessMixer();
                    events.clear();

                    const auto vocalEnum = audio::PlayerVoiceManager::vocalUIEnumForGameError(errName);
                    REQUIRE(vocalEnum.has_value());
                    const uint32_t sid = pvm.getVocalUISoundId(*vocalEnum, raceId, gender);
                    REQUIRE(sid != 0u);

                    // Blizzard shipped 3 dangling SoundEntries IDs in TBC 2.4.3 VocalUISounds.dbc
                    // (1623 Dwarf Male ERR_LOOT_LOCKED, 2144 NightElf Male ERR_LOOT_LOCKED,
                    //  2327 Orc Male ERR_MUST_EQUIP_ITEM) that have no row in SoundEntries.dbc.
                    if (table.findById(&tbc.assets, sid) == nullptr) {
                        CHECK((sid == 1623u || sid == 2144u || sid == 2327u));
                        CHECK_FALSE(pvm.playGameError(errName, raceId, gender));
                        continue;
                    }

                    REQUIRE(pvm.playGameError(errName, raceId, gender));
                    REQUIRE(events.size() == 1u);
                    CHECK(events.back().started);
                    CHECK(events.back().decodedFrames > 500u);
                    CHECK(mixFramesRms(8192).total > 1e-5);
                }
            }
        }
    }

    SECTION("SESound::IsPlaying active-handle gating blocks overlap and unblocks immediately on clip end") {
        pvm.stopErrorSpeech();
        drainHeadlessMixer();
        events.clear();

        // Start clip 1 (Human Male ERR_INV_FULL)
        REQUIRE(pvm.playGameError("ERR_INV_FULL", 1, 0));
        REQUIRE(events.size() == 1u);
        const uint64_t clipFrames = events.back().decodedFrames;
        const uint32_t srcRate = std::max<uint32_t>(1u, events.back().sampleRate);
        const uint32_t outFramesForClip =
            static_cast<uint32_t>((clipFrames * 48000ull + srcRate - 1ull) / srcRate);
        REQUIRE(outFramesForClip > 8192u);
        CHECK(pvm.isErrorSpeechPlaying());

        // Immediate second error while clip 1 is active -> blocked
        CHECK_FALSE(pvm.playGameError("ERR_OUT_OF_MANA", 1, 0));
        CHECK(events.size() == 1u);

        // Mix 4096 frames (~85ms, still mid-clip) -> still active and blocked
        CHECK(mixFramesRms(4096).total > 1e-4);
        CHECK(pvm.isErrorSpeechPlaying());
        CHECK_FALSE(pvm.playGameError("ERR_OUT_OF_MANA", 1, 0));
        CHECK(events.size() == 1u);

        // Mix remaining resampled output frames past clip end -> immediately unblocks with zero wall-clock sleep!
        const uint32_t remain = outFramesForClip + 4096u;
        (void)mixFramesRms(remain);
        CHECK_FALSE(pvm.isErrorSpeechPlaying());

        REQUIRE(pvm.playGameError("ERR_OUT_OF_MANA", 1, 0));
        CHECK(events.size() == 2u);
        CHECK(pvm.isErrorSpeechPlaying());

        // Sound_EnableErrorSpeech = false suppresses new playback
        pvm.stopErrorSpeech();
        CHECK_FALSE(pvm.isErrorSpeechPlaying());
        pvm.setEnabled(false);
        CHECK_FALSE(pvm.playGameError("ERR_OUT_OF_MANA", 1, 0));
        pvm.setEnabled(true);
    }

    pvm.shutdown();
    engine.setPlaybackObserver({});
}

TEST_CASE("GAP-07: EmotesTextSound.dbc 522 rows, local self-emote playback/dedup, and $ESD Emotes.dbc col 6",
          "[audio][shard_d][gap07]") {
    auto& tbc = env();
    auto& engine = audio::AudioEngine::instance();
    engine.shutdown();
    REQUIRE(engine.initializeHeadless(48000, 2));
    engine.setAssetManager(&tbc.assets);
    engine.setMasterVolume(1.0f);
    engine.setSuspended(false);

    audio::SoundEntryTable::instance().reset();
    REQUIRE(audio::SoundEntryTable::instance().ensureLoaded(&tbc.assets));

    std::vector<audio::AudioEngine::PlaybackEvent> events;
    engine.setPlaybackObserver([&](const audio::AudioEngine::PlaybackEvent& ev) {
        events.push_back(ev);
    });

    SECTION("EmotesTextSound.dbc loads all 522 rows and resolves vocal slash emotes (/silly, /flirt, /train, /oom, /flee)") {
        audio::PlayerVoiceManager pvm;
        REQUIRE(pvm.initialize(&tbc.assets));
        CHECK(pvm.emoteTextSoundCount() == 522u);

        auto etsDbc = tbc.assets.loadDBC("EmotesTextSound.dbc");
        REQUIRE(etsDbc != nullptr);
        REQUIRE(etsDbc->getRecordCount() == 522u);

        // Verify all 522 rows resolve valid SoundEntries records
        size_t playableRows = 0;
        for (uint32_t r = 0; r < etsDbc->getRecordCount(); ++r) {
            const uint32_t textId  = etsDbc->getUInt32(r, 1);
            const auto raceId      = static_cast<uint8_t>(etsDbc->getUInt32(r, 2));
            const auto sexId       = static_cast<uint8_t>(etsDbc->getUInt32(r, 3));
            const uint32_t soundId = etsDbc->getUInt32(r, 4);
            CHECK(pvm.getEmoteTextSoundId(textId, raceId, sexId) == soundId);
            const auto* rec = audio::SoundEntryTable::instance().findById(&tbc.assets, soundId);
            REQUIRE(rec != nullptr);
            CHECK_FALSE(audio::SoundEntryTable::instance().pickExistingPath(&tbc.assets, *rec).empty());
            ++playableRows;
        }
        CHECK(playableRows == 522u);

        // Verify slash emote DBC names (/silly, /flirt, /train, /oom, /flee, /charge) via EmotesText.dbc
        auto etDbc = tbc.assets.loadDBC("EmotesText.dbc");
        REQUIRE(etDbc != nullptr);
        std::unordered_map<std::string, uint32_t> slashToEmotesTextId;
        for (uint32_t r = 0; r < etDbc->getRecordCount(); ++r) {
            std::string token = etDbc->getString(r, 1);
            std::transform(token.begin(), token.end(), token.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (!token.empty()) slashToEmotesTextId[token] = etDbc->getUInt32(r, 0);
        }

        for (const char* slash : {"joke", "flirt", "train", "oom", "flee", "charge", "cheer",
                                  "hello", "bye", "rasp", "whistle", "openfire", "healme", "helpme", "incoming"}) {
            INFO("Slash token: " << slash);
            auto it = slashToEmotesTextId.find(slash);
            REQUIRE(it != slashToEmotesTextId.end());
            for (uint8_t raceId : {uint8_t(1), uint8_t(2), uint8_t(10), uint8_t(11)}) {
                drainHeadlessMixer();
                events.clear();
                CHECK(pvm.playEmote(it->second, raceId, 0));
                REQUIRE(events.size() == 1u);
                CHECK(events.back().started);
                CHECK_FALSE(events.back().spatial);
                // Local emote with Sound_ListenerAtCharacter=1 uses rec->volume * 0.65f self-2D gain (Wow.exe 0x6277f2)
                const uint32_t sid = pvm.getEmoteTextSoundId(it->second, raceId, 0);
                const auto* srec = audio::SoundEntryTable::instance().findById(&tbc.assets, sid);
                REQUIRE(srec != nullptr);
                CHECK(events.back().volume == Catch::Approx(srec->volume * 0.65f).margin(0.16f));
                CHECK(mixFramesRms(8192).total > 1e-5);
            }
        }

        // Local emote with Sound_ListenerAtCharacter=0 plays 3D at unitBasePos + (0, 0, 2.0f)
        {
            auto helloIt = slashToEmotesTextId.find("hello");
            REQUIRE(helloIt != slashToEmotesTextId.end());
            drainHeadlessMixer();
            engine.setListenerPosition(glm::vec3(0.0f));
            engine.setListenerOrientation(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
            events.clear();
            CHECK(pvm.playLocalEmote(helloIt->second, 1, 0, glm::vec3(0.0f, 5.0f, 10.0f), /*listenerAtCharacter=*/false));
            REQUIRE(events.size() == 1u);
            CHECK(events.back().spatial);
            CHECK(events.back().position.z == Catch::Approx(12.0f));

            // Remote 3D emote also applies +2.0f Z offset
            drainHeadlessMixer();
            events.clear();
            CHECK(pvm.playEmote3D(helloIt->second, 1, 0, glm::vec3(-8.0f, 0.0f, 3.0f)));
            REQUIRE(events.size() == 1u);
            CHECK(events.back().spatial);
            CHECK(events.back().position.z == Catch::Approx(5.0f));
        }

        // Tauren-exclusive /moo (EmotesText 226)
        {
            auto mooIt = slashToEmotesTextId.find("moo");
            REQUIRE(mooIt != slashToEmotesTextId.end());
            for (uint8_t g : {uint8_t(0), uint8_t(1)}) {
                drainHeadlessMixer();
                events.clear();
                CHECK(pvm.playEmote(mooIt->second, 6, g));
                REQUIRE(events.size() == 1u);
                CHECK(mixFramesRms(8192).total > 1e-5);
            }
            CHECK_FALSE(pvm.playEmote(mooIt->second, 1, 0));
        }
        pvm.shutdown();
    }

    SECTION("$ESD Emotes.dbc col 6 (EmoteSoundID) resolves and plays 2D and 3D positional audio") {
        audio::PlayerVoiceManager pvm;
        REQUIRE(pvm.initialize(&tbc.assets));

        // TBC 2.4.3 Emotes.dbc col 6 rows: 233=STATE_WORK_MINING(3782), 234=STATE_WORK_CHOPWOOD(3202), 380=ONESHOT_FISHING(3354)
        CHECK(pvm.getEmoteSoundId(233u) == 3782u);
        CHECK(pvm.getEmoteSoundId(234u) == 3202u);
        CHECK(pvm.getEmoteSoundId(380u) == 3354u);
        CHECK(pvm.getEmoteSoundId(1u) == 0u);

        for (uint32_t emoteId : {233u, 234u, 380u}) {
            INFO("Emotes.dbc ID: " << emoteId);
            drainHeadlessMixer();
            events.clear();
            CHECK(pvm.playEmoteAnimSound(emoteId));
            REQUIRE(events.size() == 1u);
            CHECK_FALSE(events.back().spatial);
            CHECK(mixFramesRms(1024).total > 1e-5);
        }

        // 3D positional $ESD pans left
        drainHeadlessMixer();
        engine.setListenerPosition(glm::vec3(0.0f));
        engine.setListenerOrientation(glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        events.clear();
        CHECK(pvm.playEmoteAnimSound3D(233u, glm::vec3(-8.0f, 0.0f, 0.0f)));
        REQUIRE(events.size() == 1u);
        CHECK(events.back().spatial);
        const auto pan = mixFramesRms(2048);
        CHECK(pan.left > pan.right * 1.2);

        pvm.shutdown();
    }

    engine.setPlaybackObserver({});
}

TEST_CASE("GAP-08: ActivitySoundManager resolves CreatureSoundData.dbc for all 20 profiles, shapeshifts, 3D remote players, and zero *MainJump.wav",
          "[audio][shard_d][gap08]") {
    auto& tbc = env();
    auto& engine = audio::AudioEngine::instance();
    engine.shutdown();
    REQUIRE(engine.initializeHeadless(48000, 2));
    engine.setAssetManager(&tbc.assets);
    engine.setMasterVolume(1.0f);
    engine.setSuspended(false);

    audio::SoundEntryTable::instance().reset();
    REQUIRE(audio::SoundEntryTable::instance().ensureLoaded(&tbc.assets));

    audio::ActivitySoundManager act;
    REQUIRE(act.initialize(&tbc.assets));

    std::vector<audio::AudioEngine::PlaybackEvent> events;
    engine.setPlaybackObserver([&](const audio::AudioEngine::PlaybackEvent& ev) {
        events.push_back(ev);
    });

    SECTION("All 20 playable race-gender profiles play CreatureSoundData vocals and ZERO *MainJump.wav on jump") {
        for (uint8_t raceId : kPlayableRaces) {
            for (uint8_t gender : {uint8_t(0), uint8_t(1)}) {
                INFO("Race=" << int(raceId) << " Gender=" << int(gender));
                act.setShapeshiftForm(0);
                act.setActiveDisplayId(0);
                act.setCharacterRaceGender(raceId, gender);

                const auto profile = act.getActiveVocalProfile();
                REQUIRE(profile.displayId != 0u);
                REQUIRE(profile.soundDataId != 0u);
                CHECK(profile.exertionId != 0u);
                CHECK(profile.injuryId != 0u);
                CHECK(profile.injuryCritId != 0u);
                CHECK(profile.deathId != 0u);
                // TBC 2.4.3: SoundJumpStartID (col 26) and SoundJumpEndID (col 27) are 0 on ALL 20 player profiles!
                CHECK(profile.jumpStartId == 0u);
                CHECK(profile.jumpEndId == 0u);

                // Jump MUST NOT emit bogus unused Vanilla alpha *MainJump.wav!
                events.clear();
                act.playJump();
                CHECK(events.empty());

                // Exertion, Wound, WoundCrit, and Death all play via SoundEntryTable
                drainHeadlessMixer();
                events.clear();
                CHECK(act.playExertion(/*isCrit=*/false));
                REQUIRE(events.size() == 1u);
                CHECK(mixFramesRms(8192).total > 1e-5);

                drainHeadlessMixer();
                events.clear();
                act.playWound(/*isCrit=*/false);
                REQUIRE(events.size() == 1u);

                drainHeadlessMixer();
                events.clear();
                act.playWound(/*isCrit=*/true);
                REQUIRE(events.size() == 1u);

                drainHeadlessMixer();
                events.clear();
                act.playDeath();
                REQUIRE(events.size() == 1u);
            }
        }
    }

    SECTION("Druid and Shaman shapeshift forms swap CreatureSoundData (Bear, Cat, Moonkin, Tree, Travel, Aquatic, Flight, Ghost Wolf)") {
        // Start as Night Elf Female Druid (race 4, gender 1 -> display 56, csd 56)
        act.setCharacterRaceGender(4, 1);
        act.setShapeshiftForm(0);
        act.setActiveDisplayId(0);
        const auto humanoid = act.getActiveVocalProfile();
        CHECK(humanoid.soundDataId == 56u);
        CHECK(humanoid.exertionId == 2933u);

        struct FormCheck {
            uint8_t formId;
            const char* label;
            uint32_t expectedExertion;
            uint32_t expectedInjury;
            uint32_t expectedDeath;
            uint32_t expectedJumpStart;
        };
        constexpr FormCheck kForms[] = {
            {1,  "Cat Form",          643u,  644u,  646u,     0u},
            {2,  "Tree of Life",     1494u, 1495u, 3375u,     0u},
            {3,  "Travel Form",       643u,  644u,  646u,     0u},
            {4,  "Aquatic Form",     3403u, 3404u, 3406u,     0u},
            {5,  "Bear Form",         473u,  476u,  475u,     0u},
            {8,  "Dire Bear Form",    473u,  476u,  475u,     0u},
            {16, "Ghost Wolf",        391u,  392u,  394u,     0u},
            {27, "Epic Flight Form", 4554u, 4555u, 4557u, 10905u},
            {29, "Flight Form",      4554u, 4555u, 4557u, 10905u},
            {31, "Moonkin Form",     5356u, 5357u, 5359u,     0u},
        };

        for (const auto& fc : kForms) {
            INFO("Shapeshift form: " << fc.label << " (" << int(fc.formId) << ")");
            act.setShapeshiftForm(fc.formId);
            const auto fp = act.getActiveVocalProfile();
            CHECK(fp.exertionId == fc.expectedExertion);
            CHECK(fp.injuryId == fc.expectedInjury);
            CHECK(fp.deathId == fc.expectedDeath);
            CHECK(fp.jumpStartId == fc.expectedJumpStart);
            CHECK(fp.exertionId != humanoid.exertionId);

            drainHeadlessMixer();
            events.clear();
            CHECK(act.playExertion(false));
            REQUIRE(events.size() == 1u);
            CHECK(mixFramesRms(8192).total > 1e-5);

            drainHeadlessMixer();
            events.clear();
            act.playWound(true);
            REQUIRE(events.size() == 1u);

            drainHeadlessMixer();
            events.clear();
            act.playDeath();
            REQUIRE(events.size() == 1u);
        }

        // Druid Flight Form (29) authors SoundJumpStartID = 10905 (FM_Takeoff.wav,
        // baseVol=0.70 * kLocalPlayer2DGain 0.65 = 0.455 matching Native 8606).
        act.setShapeshiftForm(29);
        drainHeadlessMixer();
        events.clear();
        act.playJump();
        REQUIRE(events.size() == 1u);
        CHECK(events.back().started);
        CHECK_FALSE(events.back().spatial);
        CHECK(events.back().volume == Catch::Approx(0.70f * 0.65f).epsilon(1e-3f));

        // Tauren Druid Cat Form resolves Horde display 8571 (csd 1089, Death 647)
        act.setCharacterRaceGender(6, 0);
        act.setShapeshiftForm(1);
        const auto taurenCat = act.getActiveVocalProfile();
        CHECK(taurenCat.displayId == 8571u);
        CHECK(taurenCat.deathId == 647u);

        // Unshifting restores base humanoid profile and silent jump
        act.setShapeshiftForm(0);
        act.setActiveDisplayId(0);
        const auto taurenBase = act.getActiveVocalProfile();
        CHECK(taurenBase.soundDataId == 59u);
        CHECK(taurenBase.jumpStartId == 0u);
        events.clear();
        act.playJump();
        CHECK(events.empty());
    }

    SECTION("3D remote player vocals pan spatially for both humanoid and shapeshifted display IDs") {
        engine.setListenerPosition(glm::vec3(0.0f));
        engine.setListenerOrientation(glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));

        // Remote Blood Elf Female at -X (left)
        drainHeadlessMixer();
        events.clear();
        REQUIRE(act.playPlayerWound3D(10, 1, /*activeDisplayId=*/0, /*isCrit=*/true, /*isCrushing=*/false,
                                      glm::vec3(-10.0f, 0.0f, 0.0f)));
        REQUIRE(events.size() == 1u);
        CHECK(events.back().spatial);
        const auto leftPan = mixFramesRms(2048);
        CHECK(leftPan.left > leftPan.right * 1.2);

        // Remote Bear-shifted Druid (display 2281) at +X (right)
        drainHeadlessMixer();
        events.clear();
        REQUIRE(act.playPlayerDeath3D(4, 0, /*activeDisplayId=*/2281u, glm::vec3(10.0f, 0.0f, 0.0f)));
        REQUIRE(events.size() == 1u);
        CHECK(events.back().spatial);
        const auto rightPan = mixFramesRms(2048);
        CHECK(rightPan.right > rightPan.left * 1.2);
    }

    act.shutdown();
    engine.setPlaybackObserver({});
}

TEST_CASE("Shard D wire-packet integration: GameHandler dispatches vocal GameErrors, self-emotes, and shapeshift sync",
          "[audio][shard_d][replay]") {
    auto& tbc = env();
    auto& engine = audio::AudioEngine::instance();
    engine.shutdown();
    REQUIRE(engine.initializeHeadless(48000, 2));
    engine.setAssetManager(&tbc.assets);
    engine.setMasterVolume(1.0f);
    engine.setSuspended(false);

    audio::SoundEntryTable::instance().reset();
    REQUIRE(audio::SoundEntryTable::instance().ensureLoaded(&tbc.assets));

    audio::AudioCoordinator ac;
    REQUIRE(ac.initialize());
    // Re-enter deterministic headless mixer after AudioCoordinator::initialize()
    engine.shutdown();
    REQUIRE(engine.initializeHeadless(48000, 2));
    engine.setAssetManager(&tbc.assets);
    engine.setMasterVolume(1.0f);
    engine.setSuspended(false);
    REQUIRE(ac.getPlayerVoiceManager() != nullptr);
    REQUIRE(ac.getActivitySoundManager() != nullptr);
    REQUIRE(ac.getPlayerVoiceManager()->initialize(&tbc.assets));
    REQUIRE(ac.getActivitySoundManager()->initialize(&tbc.assets));

    replay::Session s("tbc");
    s.services().assetManager = &tbc.assets;
    s.services().audioCoordinator = &ac;

    const std::unordered_map<std::string, std::string> kGlobals = {
        {"ERR_PROFICIENCY_NEEDED", "You do not have the required proficiency for that item."},
        {"ERR_2HSKILLNOTFOUND", "You cannot dual-wield"},
        {"ERR_WRONG_SLOT", "That item does not go in that slot."},
        {"ERR_BAG_IN_BAG", "Can't put non-empty bags in other bags."},
        {"ERR_TRADE_BOUND_ITEM", "You can't trade a soulbound item."},
        {"ERR_LOOT_TOO_FAR", "You are too far away to loot that corpse."},
        {"ERR_LOOT_LOCKED", "Someone is already looting that corpse."},
        {"ERR_BANKSLOT_INSUFFICIENT_FUNDS", "You can't afford that."},
        {"ERR_GROUP_FULL", "Your party is full."},
        {"ERR_ALREADY_IN_GROUP_S", "%s is already in a group."},
        {"ERR_GUILD_PERMISSIONS", "You don't have permission to do that."},
    };
    s.handler().setGlobalStringLookup([&](const char* name) -> std::string {
        const auto it = kGlobals.find(name ? name : "");
        return (it != kGlobals.end()) ? it->second : std::string{};
    });

    // Spawn Human Male Warrior (race=1, class=1, gender=0 -> UNIT_FIELD_BYTES_0 = 0x00000101, displayId=49)
    s.receive(replay::UpdateObject().tbc().create(
        replay::Session::kPlayerGuid,
        game::ObjectType::PLAYER,
        {
            {game::UF::UNIT_FIELD_BYTES_0, 0x00000101u},
            {game::UF::UNIT_FIELD_DISPLAYID, 49u},
            {game::UF::UNIT_FIELD_HEALTH, 500u},
            {game::UF::UNIT_FIELD_MAXHEALTH, 500u},
        },
        /*self=*/true));

    std::vector<audio::AudioEngine::PlaybackEvent> events;
    engine.setPlaybackObserver([&](const audio::AudioEngine::PlaybackEvent& ev) {
        events.push_back(ev);
    });

    auto resetSpeech = [&]() {
        ac.getPlayerVoiceManager()->stopErrorSpeech();
        drainHeadlessMixer();
        events.clear();
    };

    // 1) SMSG_INVENTORY_CHANGE_FAILURE: 8 (ERR_PROFICIENCY_NEEDED -> 48) and 14 (ERR_2HSKILLNOTFOUND -> 43)
    for (uint8_t invErr : {uint8_t(8), uint8_t(14), uint8_t(3), uint8_t(5), uint8_t(79)}) {
        INFO("InventoryResult code=" << int(invErr));
        resetSpeech();
        network::Packet p(game::wireOpcode(game::Opcode::SMSG_INVENTORY_CHANGE_FAILURE));
        p.writeUInt8(invErr);
        p.writeUInt64(0);
        p.writeUInt64(0);
        p.writeUInt8(0);
        s.receive(p);
        REQUIRE_FALSE(events.empty());
        CHECK(events.back().started);
    }

    // 2) SMSG_LOOT_RESPONSE failure: 4 (ERR_LOOT_TOO_FAR -> 35)
    {
        resetSpeech();
        network::Packet p(game::wireOpcode(game::Opcode::SMSG_LOOT_RESPONSE));
        p.writeUInt64(0xF130000000009999ull);
        p.writeUInt8(0); // LOOT_NONE
        p.writeUInt8(4); // LOOT_ERROR_TOO_FAR
        s.receive(p);
        REQUIRE_FALSE(events.empty());
        CHECK(events.back().started);
    }

    // 3) SMSG_PARTY_COMMAND_RESULT: GROUP_FULL (4 -> ERR_GROUP_FULL -> 8) & ALREADY_IN_GROUP (5 -> 20)
    {
        resetSpeech();
        network::Packet p(game::wireOpcode(game::Opcode::SMSG_PARTY_COMMAND_RESULT));
        p.writeUInt32(0); // PARTY_OP_INVITE
        p.writeString("Thrall");
        p.writeUInt32(static_cast<uint32_t>(game::PartyResult::GROUP_FULL));
        s.receive(p);
        REQUIRE_FALSE(events.empty());
        CHECK(events.back().started);

        resetSpeech();
        network::Packet p2(game::wireOpcode(game::Opcode::SMSG_PARTY_COMMAND_RESULT));
        p2.writeUInt32(0);
        p2.writeString("Jaina");
        p2.writeUInt32(static_cast<uint32_t>(game::PartyResult::ALREADY_IN_GROUP));
        s.receive(p2);
        REQUIRE_FALSE(events.empty());
        CHECK(events.back().started);
    }

    // 4) SMSG_GUILD_COMMAND_RESULT: 0x08 (ERR_GUILD_PERMISSIONS -> 62)
    {
        resetSpeech();
        network::Packet p(game::wireOpcode(game::Opcode::SMSG_GUILD_COMMAND_RESULT));
        p.writeUInt32(0); // GUILD_CREATE_S
        p.writeString("");
        p.writeUInt32(0x08); // ERR_GUILD_PERMISSIONS
        s.receive(p);
        REQUIRE_FALSE(events.empty());
        CHECK(events.back().started);
    }

    // 5) SMSG_TEXT_EMOTE for self: plays when not pre-played, deduplicates when pre-played locally
    {
        auto etsDbc = tbc.assets.loadDBC("EmotesTextSound.dbc");
        REQUIRE(etsDbc != nullptr);
        uint32_t humanMaleEmoteTextId = 0;
        for (uint32_t r = 0; r < etsDbc->getRecordCount(); ++r) {
            if (etsDbc->getUInt32(r, 2) == 1u && etsDbc->getUInt32(r, 3) == 0u &&
                etsDbc->getUInt32(r, 4) != 0u) {
                humanMaleEmoteTextId = etsDbc->getUInt32(r, 1);
                break;
            }
        }
        REQUIRE(humanMaleEmoteTextId != 0u);

        auto buildSelfTextEmote = [&](uint32_t textEmoteId) {
            network::Packet p(game::wireOpcode(game::Opcode::SMSG_TEXT_EMOTE));
            p.writeUInt64(replay::Session::kPlayerGuid);
            p.writeUInt32(textEmoteId);
            p.writeUInt32(0); // emoteNum
            p.writeUInt32(1); // namelen (empty null-terminated)
            p.writeUInt8(0);
            return p;
        };

        // Case A: Server SMSG_TEXT_EMOTE without prior local playEmote -> plays once!
        resetSpeech();
        s.receive(buildSelfTextEmote(humanMaleEmoteTextId));
        CHECK(events.size() == 1u);

        // Case B: Local slash / DoEmote plays immediately, then server SMSG_TEXT_EMOTE echo arrives -> NO double play!
        resetSpeech();
        REQUIRE(ac.getPlayerVoiceManager()->playEmote(humanMaleEmoteTextId, 1, 0));
        REQUIRE(events.size() == 1u);
        s.receive(buildSelfTextEmote(humanMaleEmoteTextId));
        CHECK(events.size() == 1u);
    }

    // 6) Live UNIT_FIELD_DISPLAYID update into Bear Form (2281) syncs ActivitySoundManager via updateCombatText
    {
        s.receive(replay::UpdateObject().tbc().values(
            replay::Session::kPlayerGuid,
            {{game::UF::UNIT_FIELD_DISPLAYID, 2281u}}));
        s.handler().updateCombatText(0.016f);
        const auto activeProfile = ac.getActivitySoundManager()->getActiveVocalProfile();
        CHECK(activeProfile.displayId == 2281u);
        CHECK(activeProfile.exertionId == 473u); // BearAttack
    }

    engine.setPlaybackObserver({});
    ac.shutdown();
}
