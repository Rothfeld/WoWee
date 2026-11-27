// TBC 2.4.3 (build 8606) Unusable Spell / Ability / Action / Item Click & Keybind Audio Suite.
//
// Validates black-box empirical parity across the entire class of unusable spell and action
// activations (mouse click, keybind, /cast, stance bar, pet bar, and inventory items):
//   1. Client-side pre-send cast refusals play 3D school SpellFizzle* (SE:1424..1428) across
//      dead caster, cooldown/GCD, out-of-power, no-target, invalid-target, moving, missing
//      reagent/equipment, and out-of-range refusals, while Physical (0x01) abilities play no fizzle.
//   2. Action-bar UseAction and keybind dispatch do not silently drop cooldown slots; pressing a
//      spell or ability on cooldown triggers ERR_ABILITY_COOLDOWN (Attributes & 0x10) or
//      ERR_SPELL_COOLDOWN vocal speech + school SpellFizzle* + UNIT_SPELLCAST_FAILED.
//   3. Targeted activations (UnitAim != None and SPELL_ATTR_HELD_ITEM_ONLY empty-mainhand) play
//      SE:905 GAMEABILITYACTIVATE / SE:906 GAMESPELLACTIVATE (uChatScrollButton.wav).
//   4. Sound\Interface\Error.wav never plays on UIErrorsFrame cast/combat/pet/inventory refusals,
//      and GameHandler::errorGlobal dispatches VocalUISounds.dbc speech (ERR_NO_ATTACK_TARGET,
//      ERR_INVALID_ATTACK_TARGET, etc.).
//   5. Sound_EnableErrorSpeech=0 suppresses vocal error speech while preserving SpellFizzle* and
//      GAMEABILITYACTIVATE / GAMESPELLACTIVATE UI click feedback.

#include <catch_amalgamated.hpp>

#include "game_replay.hpp"
#include "addons/addon_manager.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/player_voice_manager.hpp"
#include "audio/sound_entries.hpp"
#include "audio/ui_sound_manager.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unordered_map>
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
        if (std::filesystem::exists(p / "dbfilesclient" / "spell.dbc") ||
            std::filesystem::exists(p / "DBFilesClient" / "Spell.dbc") ||
            std::filesystem::exists(p / "dbc.MPQ") ||
            std::filesystem::exists(p / "common.MPQ")) {
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

struct UnusableAudioFixture {
    pipeline::AssetManager assets;
    pipeline::DBCLayout layout;
    bool ready = false;

    UnusableAudioFixture() {
        const std::string dataDir = findTbcDataPath();
        const std::string layoutPath = findTbcLayoutJsonPath();
        if (dataDir.empty() || layoutPath.empty()) return;
        if (!assets.initialize(dataDir)) return;
        assets.setExpansionDataPath(dataDir);
        if (!layout.loadFromJson(layoutPath)) return;
        pipeline::setActiveDBCLayout(&layout);

        auto& eng = audio::AudioEngine::instance();
        eng.shutdown();
        if (!eng.initializeHeadless(48000, 2)) return;
        eng.setMasterVolume(1.0f);
        eng.setAssetManager(&assets);
        audio::SoundEntryTable::instance().reset();
        if (!audio::SoundEntryTable::instance().ensureLoaded(&assets)) return;
        ready = true;
    }

    ~UnusableAudioFixture() {
        auto& eng = audio::AudioEngine::instance();
        eng.setPlaybackObserver({});
        eng.setAssetManager(nullptr);
        eng.shutdown();
        audio::SoundEntryTable::instance().reset();
        pipeline::setActiveDBCLayout(nullptr);
        assets.shutdown();
    }
};

UnusableAudioFixture& sharedFixture() {
    static UnusableAudioFixture fix;
    return fix;
}

std::string toLowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool hasEventMatching(const std::vector<audio::AudioEngine::PlaybackEvent>& events,
                      std::string_view needleLower) {
    for (const auto& ev : events) {
        if (!ev.started) continue;
        if (toLowerCopy(ev.path).find(needleLower) != std::string::npos) return true;
    }
    return false;
}

network::Packet buildTbcInitialSpells(const std::vector<uint32_t>& spells) {
    network::Packet pkt(game::wireOpcode(game::Opcode::SMSG_INITIAL_SPELLS));
    pkt.writeUInt8(0); // unk
    pkt.writeUInt16(static_cast<uint16_t>(spells.size()));
    for (uint32_t sp : spells) {
        pkt.writeUInt16(static_cast<uint16_t>(sp));
        pkt.writeUInt16(0);
    }
    pkt.writeUInt16(0); // cooldown count
    return pkt;
}

network::Packet buildTbcSpellCooldown(uint64_t guid, uint32_t spellId, uint32_t cooldownMs) {
    network::Packet pkt(game::wireOpcode(game::Opcode::SMSG_SPELL_COOLDOWN));
    pkt.writeUInt64(guid);
    pkt.writeUInt8(0); // flags
    pkt.writeUInt32(spellId);
    pkt.writeUInt32(cooldownMs);
    return pkt;
}

struct TestHarness {
    replay::Session session{"tbc"};
    audio::AudioCoordinator coordinator;
    addons::AddonManager luaMgr;
    std::vector<audio::AudioEngine::PlaybackEvent> events;
    std::vector<std::string> uiErrors;
    std::vector<std::string> failedSpells;
    bool bogusUiErrorBeep = false;

    explicit TestHarness(UnusableAudioFixture& fix,
                         uint32_t hp = 1000,
                         uint32_t mana = 1000,
                         uint32_t energy = 100,
                         uint8_t powerType = 0) {
        pipeline::setActiveDBCLayout(&fix.layout);
        resetMixer(fix);

        REQUIRE(coordinator.initialize());
        coordinator.initializeEarlyAssets(&fix.assets);
        if (auto* ui = coordinator.getUiSoundManager()) ui->initialize(&fix.assets);
        if (auto* pvm = coordinator.getPlayerVoiceManager()) {
            pvm->initialize(&fix.assets);
            pvm->setEnabled(true);
            pvm->setVolumeScale(1.0f);
        }

        session.services().assetManager = &fix.assets;
        session.services().audioCoordinator = &coordinator;
        REQUIRE(luaMgr.initialize(&session.handler()));

        auto* am = &fix.assets;
        session.handler().setSpellDataResolver([am](uint32_t spellId) -> game::GameHandler::SpellDataInfo {
            game::GameHandler::SpellDataInfo info{};
            auto spellDbc = am->loadDBC("Spell.dbc");
            const auto* layout = pipeline::getActiveDBCLayout()
                ? pipeline::getActiveDBCLayout()->getLayout("Spell")
                : nullptr;
            if (!spellDbc || !spellDbc->isLoaded() || !layout) return info;
            const int32_t row = spellDbc->findRecordById(spellId);
            if (row < 0) return info;
            const auto r = static_cast<uint32_t>(row);
            info.powerType = static_cast<uint8_t>(spellDbc->getUInt32(r, layout->field("PowerType")));
            info.manaCost = spellDbc->getUInt32(r, layout->field("ManaCost"));
            const uint32_t ctIdx = spellDbc->getUInt32(r, layout->field("CastingTimeIndex"));
            if (auto ctDbc = am->loadDBC("SpellCastTimes.dbc"); ctDbc && ctDbc->isLoaded()) {
                const int32_t ctRow = ctDbc->findRecordById(ctIdx);
                if (ctRow >= 0) {
                    const int32_t ms = ctDbc->getInt32(static_cast<uint32_t>(ctRow), 1);
                    if (ms > 0) info.castTimeMs = static_cast<uint32_t>(ms);
                }
            }
            const uint32_t rngIdx = spellDbc->getUInt32(r, layout->field("RangeIndex"));
            if (auto rngDbc = am->loadDBC("SpellRange.dbc"); rngDbc && rngDbc->isLoaded()) {
                const int32_t rngRow = rngDbc->findRecordById(rngIdx);
                if (rngRow >= 0) {
                    info.minRange = rngDbc->getFloat(static_cast<uint32_t>(rngRow), 1);
                    info.maxRange = rngDbc->getFloat(static_cast<uint32_t>(rngRow), 2);
                    info.rangeFlags = rngDbc->getUInt32(static_cast<uint32_t>(rngRow), 3);
                }
            }
            return info;
        });

        static const std::unordered_map<std::string, std::string> kStrings = {
            {"ERR_GENERIC_NO_TARGET", "You have no target."},
            {"ERR_NO_ATTACK_TARGET", "There is nothing to attack."},
            {"ERR_INVALID_ATTACK_TARGET", "You cannot attack that target."},
            {"ERR_ABILITY_COOLDOWN", "Ability is not ready yet."},
            {"ERR_SPELL_COOLDOWN", "Spell is not ready yet."},
            {"ERR_OUT_OF_MANA", "Not enough mana"},
            {"ERR_OUT_OF_ENERGY", "Not enough energy"},
            {"ERR_OUT_OF_RAGE", "Not enough rage"},
            {"ERR_SPELL_OUT_OF_RANGE", "Out of range."},
            {"ERR_ITEM_COOLDOWN", "Item is not ready yet."},
            {"ERR_POTION_COOLDOWN", "You cannot drink any more yet."},
            {"ERR_PROFICIENCY_NEEDED", "You do not have the required proficiency for that item."},
            {"ERR_ITEM_LOCKED", "Item is locked."},
            {"ERR_PLAYER_DEAD", "You can't do that when you're dead."},
            {"SPELL_FAILED_BAD_TARGETS", "Invalid target"},
            {"SPELL_FAILED_CASTER_DEAD", "You are dead"},
            {"SPELL_FAILED_MOVING", "Can't do that while moving"},
            {"SPELL_FAILED_MAINHAND_EMPTY", "Your weapon hand is empty"},
            {"SPELL_FAILED_NO_COMBO_POINTS", "That ability requires combo points"},
            {"SPELL_FAILED_REAGENTS", "Missing reagent: %s"},
            {"SPELL_FAILED_EQUIPPED_ITEM_CLASS", "Must have a %s equipped"},
            {"SPELL_FAILED_EQUIPPED_ITEM_CLASS_MAINHAND", "Must have a %s equipped in the main hand"},
            {"SPELL_FAILED_NOT_READY", "Not yet recovered"},
        };
        session.handler().setGlobalStringLookup([](const char* name) -> std::string {
            if (!name) return {};
            const auto it = kStrings.find(name);
            return it != kStrings.end() ? it->second : std::string{};
        });

        session.handler().setUIErrorCallback([this](const std::string& msg) {
            uiErrors.push_back(msg);
            if (!session.handler().isUIErrorSoundSuppressed()) {
                bogusUiErrorBeep = true;
                if (auto* ui = coordinator.getUiSoundManager()) ui->playError();
            }
        });

        session.handler().setAddonEventCallback(
            [this](const std::string& ev, const std::vector<std::string>& args) {
                if (ev == "UNIT_SPELLCAST_FAILED" && args.size() >= 2) {
                    failedSpells.push_back(args[1]);
                }
            });

        // Human Male (race=1, class=8, gender=0) packed in UNIT_FIELD_BYTES_0:
        // byte0=race(1), byte1=class(8), byte2=gender(0), byte3=powerType
        const uint32_t bytes0 = 1u | (8u << 8u) | (0u << 16u) | (static_cast<uint32_t>(powerType) << 24u);
        session.spawnPlayer({
            {game::UF::UNIT_FIELD_BYTES_0, bytes0},
            {game::UF::UNIT_FIELD_HEALTH, hp},
            {game::UF::UNIT_FIELD_MAXHEALTH, 1000},
            {game::UF::UNIT_FIELD_POWER1, mana},
            {game::UF::UNIT_FIELD_MAXPOWER1, 1000},
            {game::UF::UNIT_FIELD_LEVEL, 70},
        });
        const uint16_t energyIdx = static_cast<uint16_t>(game::fieldIndex(game::UF::UNIT_FIELD_POWER1) + 3u);
        const uint16_t maxEnergyIdx = static_cast<uint16_t>(game::fieldIndex(game::UF::UNIT_FIELD_MAXPOWER1) + 3u);
        const uint16_t modCastSpeedIdx = game::fieldIndex(game::UF::UNIT_MOD_CAST_SPEED);
        const uint32_t oneFloatBits = 0x3F800000u; // 1.0f
        session.receive(replay::UpdateObject().tbc().valuesAt(
            replay::Session::kPlayerGuid,
            {{energyIdx, energy}, {maxEnergyIdx, 100u}, {modCastSpeedIdx, oneFloatBits}}));
    }

    ~TestHarness() {
        luaMgr.shutdown();
    }

    void resetMixer(UnusableAudioFixture& fix) {
        auto& eng = audio::AudioEngine::instance();
        if (auto* pvm = coordinator.getPlayerVoiceManager()) pvm->stopErrorSpeech();
        eng.shutdown();
        REQUIRE(eng.initializeHeadless(48000, 2));
        eng.setMasterVolume(1.0f);
        eng.setAssetManager(&fix.assets);
        eng.setListenerPosition(glm::vec3(0.0f, 0.0f, 2.0f));
        events.clear();
        uiErrors.clear();
        failedSpells.clear();
        bogusUiErrorBeep = false;
        eng.setPlaybackObserver([this](const audio::AudioEngine::PlaybackEvent& ev) {
            events.push_back(ev);
        });
    }
};

} // namespace

TEST_CASE("Unusable spell click audio: dead player plays school SpellFizzle* on magic spells and silence on physical/items",
          "[audio][tbc][unusable_spell]") {
    auto& fix = sharedFixture();
    if (!fix.ready) SKIP("TBC data not present");

    TestHarness h(fix, /*hp=*/0, /*mana=*/1000, /*energy=*/100, /*powerType=*/0);
    h.session.receive(buildTbcInitialSpells({133, 3044, 1978, 136, 1752, 2973}));

    // 1. Fireball (133, Fire 0x04) while dead -> SpellFizzleFire (SE:1424), zero vocal, zero button click, zero Error.wav
    h.resetMixer(fix);
    h.session.handler().castSpell(133, 0);
    CHECK_FALSE(h.bogusUiErrorBeep);
    CHECK_FALSE(hasEventMatching(h.events, "error.wav"));
    CHECK_FALSE(hasEventMatching(h.events, "uchatscrollbutton.wav"));
    CHECK_FALSE(hasEventMatching(h.events, "humanmale_err_"));
    CHECK(hasEventMatching(h.events, "fizzlefirea.wav"));
    CHECK(h.session.sent().of(game::Opcode::CMSG_CAST_SPELL).empty());

    // 2. Hunter Arcane Shot (3044, Arcane 0x40) while dead -> SpellFizzleHoly (SE:1428), exact user dead-Hunter case!
    h.resetMixer(fix);
    h.session.handler().castSpell(3044, 0);
    CHECK_FALSE(h.bogusUiErrorBeep);
    CHECK(hasEventMatching(h.events, "fizzleholya.wav"));
    CHECK_FALSE(hasEventMatching(h.events, "error.wav"));

    // 3. Hunter Serpent Sting (1978, Nature 0x08) & Mend Pet (136, Nature 0x08) while dead -> SpellFizzleNature (SE:1426)
    for (uint32_t natureSpell : {1978u, 136u}) {
        h.resetMixer(fix);
        h.session.handler().castSpell(natureSpell, 0);
        CHECK_FALSE(h.bogusUiErrorBeep);
        CHECK(hasEventMatching(h.events, "fizzlenaturea.wav"));
    }

    // 4. Physical abilities Sinister Strike (1752, 0x01) & Raptor Strike (2973, 0x01) while dead -> complete audio silence
    for (uint32_t physSpell : {1752u, 2973u}) {
        h.resetMixer(fix);
        h.session.handler().castSpell(physSpell, 0);
        CHECK_FALSE(h.bogusUiErrorBeep);
        CHECK(h.events.empty());
    }

    // 5. SMSG_INVENTORY_CHANGE_FAILURE ERR_PLAYER_DEAD (wire 38) while dead -> silent, zero double Error.wav
    h.resetMixer(fix);
    {
        network::Packet pkt(game::wireOpcode(game::Opcode::SMSG_INVENTORY_CHANGE_FAILURE));
        pkt.writeUInt8(38); // EQUIP_ERR_YOU_ARE_DEAD -> ERR_PLAYER_DEAD
        pkt.writeUInt64(0);
        pkt.writeUInt64(0);
        pkt.writeUInt8(0);
        h.session.receive(pkt);
    }
    CHECK_FALSE(h.bogusUiErrorBeep);
    CHECK_FALSE(hasEventMatching(h.events, "error.wav"));
    CHECK(h.events.empty());
}

TEST_CASE("Unusable spell click audio: UseAction & castSpell on cooldown play ABILITY_COOLDOWN vs SPELL_COOLDOWN + school fizzle",
          "[audio][tbc][unusable_spell]") {
    auto& fix = sharedFixture();
    if (!fix.ready) SKIP("TBC data not present");

    TestHarness h(fix, /*hp=*/1000, /*mana=*/1000, /*energy=*/100, /*powerType=*/0);
    h.session.receive(buildTbcInitialSpells({2983, 122, 1784}));

    // Put Sprint (2983, Physical ability, attr & 0x10) on slot 1, Frost Nova (122, Frost spell) on slot 2
    h.session.handler().actionBarRef()[0] = {
        .type = game::ActionBarSlot::SPELL, .id = 2983, .cooldownRemaining = 60.0f, .cooldownTotal = 300.0f};
    h.session.handler().actionBarRef()[1] = {
        .type = game::ActionBarSlot::SPELL, .id = 122, .cooldownRemaining = 15.0f, .cooldownTotal = 25.0f};
    h.session.receive(buildTbcSpellCooldown(replay::Session::kPlayerGuid, 2983, 60000));
    h.session.receive(buildTbcSpellCooldown(replay::Session::kPlayerGuid, 122, 15000));
    h.session.receive(buildTbcSpellCooldown(replay::Session::kPlayerGuid, 1784, 8000));

    // 1. Clicking/pressing Sprint on cooldown via Lua UseAction(1) must NOT be silently dropped by action.isReady()!
    h.resetMixer(fix);
    REQUIRE(h.luaMgr.runScript("UseAction(1)"));
    CHECK_FALSE(h.bogusUiErrorBeep);
    CHECK_FALSE(hasEventMatching(h.events, "error.wav"));
    CHECK(hasEventMatching(h.events, "humanmale_err_abilitycooldown"));
    CHECK_FALSE(hasEventMatching(h.events, "fizzle"));
    REQUIRE_FALSE(h.uiErrors.empty());
    CHECK(h.uiErrors.back() == "Ability is not ready yet.");
    CHECK_FALSE(h.failedSpells.empty());
    CHECK(h.session.sent().of(game::Opcode::CMSG_CAST_SPELL).empty());

    // 2. Clicking/pressing Frost Nova on cooldown via Lua UseAction(2) -> SpellFizzleFrost + SPELL_COOLDOWN vocal
    h.resetMixer(fix);
    REQUIRE(h.luaMgr.runScript("UseAction(2)"));
    CHECK_FALSE(h.bogusUiErrorBeep);
    CHECK_FALSE(hasEventMatching(h.events, "error.wav"));
    CHECK(hasEventMatching(h.events, "fizzlefrosta.wav"));
    CHECK(hasEventMatching(h.events, "humanmale_err_spellcooldown"));
    REQUIRE_FALSE(h.uiErrors.empty());
    CHECK(h.uiErrors.back() == "Spell is not ready yet.");
    CHECK(h.session.sent().of(game::Opcode::CMSG_CAST_SPELL).empty());

    // 3. Stance bar CastShapeshiftForm(1) (Stealth 1784, cost=0, attr & 0x10) on cooldown -> ABILITY_COOLDOWN vocal
    h.resetMixer(fix);
    REQUIRE(h.luaMgr.runScript("CastShapeshiftForm(1)"));
    CHECK_FALSE(h.bogusUiErrorBeep);
    CHECK(hasEventMatching(h.events, "humanmale_err_abilitycooldown"));
    REQUIRE_FALSE(h.uiErrors.empty());
    CHECK(h.uiErrors.back() == "Ability is not ready yet.");
}

TEST_CASE("Unusable spell click audio: client-side target, power, moving, reagent, weapon, pet, and CVar gates match TBC 2.4.3",
          "[audio][tbc][unusable_spell]") {
    auto& fix = sharedFixture();
    if (!fix.ready) SKIP("TBC data not present");

    TestHarness h(fix, /*hp=*/1000, /*mana=*/1000, /*energy=*/100, /*powerType=*/3);
    h.session.receive(buildTbcInitialSpells({81, 133, 130, 1459, 5504, 8232, 13877, 14251, 53, 2098}));

    // 1. Passive spell (Dodge 81): completely silent
    h.resetMixer(fix);
    h.session.handler().castSpell(81, 0);
    CHECK(h.events.empty());
    CHECK_FALSE(h.bogusUiErrorBeep);

    // 2. Hostile magic spell with NO target (Fireball 133):
    //    SE:906 GAMESPELLACTIVATE (uChatScrollButton.wav) + SpellFizzleFire (SE:1424) + ERR_GENERIC_NO_TARGET vocal
    h.resetMixer(fix);
    h.session.handler().castSpell(133, 0);
    CHECK_FALSE(h.bogusUiErrorBeep);
    CHECK(hasEventMatching(h.events, "uchatscrollbutton.wav"));
    CHECK(hasEventMatching(h.events, "fizzlefirea.wav"));
    CHECK(hasEventMatching(h.events, "humanmale_err_genericnotarget"));

    // 3. Hostile magic spell with friendly/self target (Fireball 133 on self -> SPELL_FAILED_BAD_TARGETS):
    //    SE:906 GAMESPELLACTIVATE + SpellFizzleFire, ZERO vocal speech!
    h.resetMixer(fix);
    h.session.handler().setTarget(replay::Session::kPlayerGuid);
    h.session.handler().castSpell(133, 0);
    CHECK_FALSE(h.bogusUiErrorBeep);
    CHECK(hasEventMatching(h.events, "uchatscrollbutton.wav"));
    CHECK(hasEventMatching(h.events, "fizzlefirea.wav"));
    CHECK_FALSE(hasEventMatching(h.events, "humanmale_err_"));
    h.session.handler().clearTarget();

    // 4. Sound_EnableErrorSpeech=0 suppresses vocal speech while keeping SpellFizzleFire + uChatScrollButton.wav
    h.resetMixer(fix);
    h.coordinator.getPlayerVoiceManager()->setEnabled(false);
    h.session.handler().castSpell(133, 0);
    CHECK(hasEventMatching(h.events, "uchatscrollbutton.wav"));
    CHECK(hasEventMatching(h.events, "fizzlefirea.wav"));
    CHECK_FALSE(hasEventMatching(h.events, "humanmale_err_"));
    h.coordinator.getPlayerVoiceManager()->setEnabled(true);

    // 5. Auto-acquire melee ability (Riposte 14251, no weapon req) with no enemy -> ERR_NO_ATTACK_TARGET vocal
    //    (SE:1906 HumanMale_CantAttackNoTarget -> humanmale_err_genericnotarget01/02/04.wav), zero Error.wav;
    //    with self selected -> ERR_INVALID_ATTACK_TARGET vocal, zero Error.wav
    h.resetMixer(fix);
    h.session.handler().castSpell(14251, 0);
    CHECK_FALSE(h.bogusUiErrorBeep);
    CHECK_FALSE(hasEventMatching(h.events, "error.wav"));
    CHECK(hasEventMatching(h.events, "humanmale_err_genericnotarget"));

    h.resetMixer(fix);
    h.session.handler().setTarget(replay::Session::kPlayerGuid);
    h.session.handler().castSpell(14251, 0);
    CHECK_FALSE(h.bogusUiErrorBeep);
    CHECK_FALSE(hasEventMatching(h.events, "error.wav"));
    CHECK(hasEventMatching(h.events, "humanmale_err_invalidattacktarget"));
    h.session.handler().clearTarget();

    // 6. Missing weapon on Physical ability (Backstab 53) & No Combo Points (Eviscerate 2098):
    //    zero Error.wav, zero SpellFizzle, zero vocal speech
    for (uint32_t silentPhys : {53u, 2098u}) {
        h.resetMixer(fix);
        h.session.handler().castSpell(silentPhys, 0);
        CHECK_FALSE(h.bogusUiErrorBeep);
        CHECK_FALSE(hasEventMatching(h.events, "error.wav"));
        CHECK(h.events.empty());
    }

    // 7. SPELL_ATTR_HELD_ITEM_ONLY with empty mainhand (Windfury Weapon 8232):
    //    plays SE:906 GAMESPELLACTIVATE (uChatScrollButton.wav), zero Error.wav, zero SpellFizzleNature
    h.resetMixer(fix);
    h.session.handler().castSpell(8232, 0);
    CHECK_FALSE(h.bogusUiErrorBeep);
    CHECK(hasEventMatching(h.events, "uchatscrollbutton.wav"));
    CHECK_FALSE(hasEventMatching(h.events, "fizzlenaturea.wav"));
    CHECK_FALSE(hasEventMatching(h.events, "error.wav"));

    // 8. Moving during cast-time spell (Conjure Water 5504, Arcane 0x40):
    //    plays SpellFizzleHoly (SE:1428), zero Error.wav
    h.resetMixer(fix);
    h.session.handler().movementInfoRef().flags |= static_cast<uint32_t>(game::MovementFlags::FORWARD);
    h.session.handler().castSpell(5504, 0);
    h.session.handler().movementInfoRef().flags = 0;
    CHECK_FALSE(h.bogusUiErrorBeep);
    CHECK(hasEventMatching(h.events, "fizzleholya.wav"));
    CHECK_FALSE(hasEventMatching(h.events, "error.wav"));

    // 9. Out of Mana (Arcane Intellect 1459, Arcane 0x40) & Out of Energy (Blade Flurry 13877, Physical 0x01)
    {
        const uint16_t manaIdx = game::fieldIndex(game::UF::UNIT_FIELD_POWER1);
        const uint16_t energyIdx = static_cast<uint16_t>(manaIdx + 3u);
        h.session.receive(replay::UpdateObject().tbc().valuesAt(
            replay::Session::kPlayerGuid,
            {{manaIdx, 0u}, {energyIdx, 0u}}));
    }

    h.resetMixer(fix);
    h.session.handler().castSpell(1459, 0);
    CHECK_FALSE(h.bogusUiErrorBeep);
    CHECK(hasEventMatching(h.events, "fizzleholya.wav"));
    CHECK(hasEventMatching(h.events, "humanmale_err_nomana"));

    h.resetMixer(fix);
    h.session.handler().castSpell(13877, 0);
    CHECK_FALSE(h.bogusUiErrorBeep);
    CHECK_FALSE(hasEventMatching(h.events, "fizzle"));
    CHECK(hasEventMatching(h.events, "humanmale_err_noenergy"));

    // 10. Pet cast failure (SMSG_PET_CAST_FAILED) & Vocal InventoryFailure (SMSG_INVENTORY_CHANGE_FAILURE 36 -> ERR_ITEM_LOCKED):
    //     zero bogus Error.wav
    h.resetMixer(fix);
    {
        network::Packet petFail(game::wireOpcode(game::Opcode::SMSG_PET_CAST_FAILED));
        petFail.writeUInt32(31707); // Waterbolt
        petFail.writeUInt8(0x0B);   // TBC SPELL_FAILED_BAD_TARGETS
        h.session.receive(petFail);
    }
    CHECK_FALSE(h.bogusUiErrorBeep);
    CHECK_FALSE(hasEventMatching(h.events, "error.wav"));
    CHECK(h.events.empty());

    h.resetMixer(fix);
    {
        network::Packet invFail(game::wireOpcode(game::Opcode::SMSG_INVENTORY_CHANGE_FAILURE));
        invFail.writeUInt8(36); // EQUIP_ERR_ITEM_LOCKED -> ERR_ITEM_LOCKED
        invFail.writeUInt64(0);
        invFail.writeUInt64(0);
        invFail.writeUInt8(0);
        h.session.receive(invFail);
    }
    CHECK_FALSE(h.bogusUiErrorBeep);
    CHECK_FALSE(hasEventMatching(h.events, "error.wav"));
    CHECK(hasEventMatching(h.events, "humanmale_err_itemlocked"));
}
