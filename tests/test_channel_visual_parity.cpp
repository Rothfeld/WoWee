#include <catch_amalgamated.hpp>

#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/m2_loader.hpp"
#include "rendering/channel_visual_lifecycle.hpp"
#include "rendering/m2_track_sampler.hpp"
#include "rendering/spell_launch_origin.hpp"
#include "rendering/spell_visual_system.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

using wowee::pipeline::DBCFile;
using wowee::pipeline::DBCLayout;
using wowee::pipeline::M2Loader;
using wowee::pipeline::M2Model;
using namespace wowee::rendering;

namespace {

std::vector<uint8_t> readBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.good()) return {};
    return {std::istreambuf_iterator<char>(in), {}};
}

std::filesystem::path resolveTbcDataDir() {
    if (const char* env = std::getenv("WOWEE_TBC_DATA_DIR"); env && *env) {
        return std::filesystem::path(env);
    }
    const std::filesystem::path fallback("/workspace/azerothcore/client/wowee-data/expansions/tbc");
    if (std::filesystem::exists(fallback)) return fallback;
    return {};
}

std::filesystem::path resolveTbcDbcFile(const std::string& filename) {
    const std::filesystem::path serverPath =
        std::filesystem::path("/workspace/azerothcore/server/data/dbc") / filename;
    if (std::filesystem::exists(serverPath)) return serverPath;

    const auto dataDir = resolveTbcDataDir();
    if (!dataDir.empty()) {
        const auto dbcDir = dataDir / "dbfilesclient";
        if (std::filesystem::is_directory(dbcDir)) {
            std::string wantLower = filename;
            for (char& c : wantLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (const auto& entry : std::filesystem::directory_iterator(dbcDir)) {
                std::string nameLower = entry.path().filename().string();
                for (char& c : nameLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (nameLower == wantLower) return entry.path();
            }
        }
    }
    return {};
}

bool loadTbcDbc(const std::string& filename, DBCFile& out) {
    const auto path = resolveTbcDbcFile(filename);
    if (path.empty()) return false;
    const auto bytes = readBytes(path);
    return !bytes.empty() && out.load(bytes);
}

}  // namespace

TEST_CASE("M2 position deduplication excludes spell visual effects while preserving static doodads",
          "[channel][spell-visual][m2]") {
    // Static world/WMO doodads use position dedup by default.
    CHECK(m2ModelUsesPositionDedup(/*allowPositionDedup=*/true,
                                   /*isGroundDetail=*/false,
                                   /*isSpellEffect=*/false));
    CHECK(m2InstanceUsesPositionDedup(/*isGroundDetail=*/false,
                                      /*isSpellEffect=*/false,
                                      /*isSpellVisual=*/false,
                                      /*isGameObject=*/false,
                                      /*externalPose=*/false));

    // Spell-effect M2s (e.g. Spells\ManaFunnel_Impact_Chest.m2 shared by Mind Flay
    // CastKit 429 and ChannelKit 430 at SpellLeftHand 21) and explicit allowPositionDedup=false
    // must never deduplicate into an existing instance.
    CHECK_FALSE(m2ModelUsesPositionDedup(/*allowPositionDedup=*/true,
                                         /*isGroundDetail=*/false,
                                         /*isSpellEffect=*/true));
    CHECK_FALSE(m2ModelUsesPositionDedup(/*allowPositionDedup=*/false,
                                         /*isGroundDetail=*/false,
                                         /*isSpellEffect=*/false));
    CHECK_FALSE(m2ModelUsesPositionDedup(/*allowPositionDedup=*/true,
                                         /*isGroundDetail=*/true,
                                         /*isSpellEffect=*/false));

    // Instances marked isSpellVisual, isSpellEffect, isGameObject, isGroundDetail,
    // or externalPose must never register/move/erase static doodad dedup cells.
    CHECK_FALSE(m2InstanceUsesPositionDedup(false, true, false, false, false));
    CHECK_FALSE(m2InstanceUsesPositionDedup(false, false, true, false, false));
    CHECK_FALSE(m2InstanceUsesPositionDedup(false, false, false, true, false));
    CHECK_FALSE(m2InstanceUsesPositionDedup(true, false, false, false, false));
    CHECK_FALSE(m2InstanceUsesPositionDedup(false, false, false, false, true));
}

TEST_CASE("channelDurationSeconds converts finite durations and preserves infinity for AOI channels",
          "[channel][spell-visual]") {
    CHECK(channelDurationSeconds(4000) == Catch::Approx(4.0f));
    CHECK(channelDurationSeconds(3000) == Catch::Approx(3.0f));
    CHECK(std::isinf(channelDurationSeconds(std::numeric_limits<uint32_t>::max())));
}

TEST_CASE("reconcileFieldChannels handles MSG_CHANNEL_START, mid-channel AOI entry, clipping, and teardown",
          "[channel][spell-visual]") {
    constexpr uint64_t kDuskPriestGuid = 0xf13000631a596dadULL;
    constexpr uint32_t kDuskPriestInst = 154;
    constexpr uint32_t kMindFlayVisual = 277;
    constexpr uint32_t kArcaneMissilesVisual = 262;

    std::unordered_map<uint32_t, uint32_t> fieldChannels;
    std::unordered_set<uint32_t> heldChannelKits;
    std::unordered_set<uint64_t> endedChannels;

    SECTION("MSG_CHANNEL_START before SMSG_UPDATE_OBJECT keeps hand glow alive until channel end") {
        // Tick 1: MSG_CHANNEL_START arrived (activeVisualByInstance has 277),
        // but UNIT_CHANNEL_SPELL has not arrived yet (rawEntries empty).
        std::unordered_map<uint32_t, uint32_t> active{{kDuskPriestInst, kMindFlayVisual}};
        std::unordered_set<uint32_t> effects{kDuskPriestInst};
        auto plan1 = reconcileFieldChannels({}, active, effects, fieldChannels, heldChannelKits, endedChannels);
        CHECK(plan1.instancesToStop.empty());
        CHECK(plan1.channelsToStart.empty());

        // Tick 2: SMSG_UPDATE_OBJECT arrives with UNIT_CHANNEL_SPELL = 46562 (visual 277).
        // Binds fieldChannels without restarting or resetting the timed channel.
        std::vector<FieldChannelEntry> raw{{kDuskPriestGuid, kDuskPriestInst, kMindFlayVisual, 0}};
        auto plan2 = reconcileFieldChannels(raw, active, effects, fieldChannels, heldChannelKits, endedChannels);
        CHECK(plan2.instancesToStop.empty());
        CHECK(plan2.channelsToStart.empty());
        CHECK(fieldChannels[kDuskPriestInst] == kMindFlayVisual);
        CHECK_FALSE(heldChannelKits.contains(kDuskPriestInst));

        // Tick 3: MSG_CHANNEL_UPDATE 0 arrives ~300 ms before UNIT_CHANNEL_SPELL clears:
        // caller stops visual (clears active/effects/fieldChannels) and adds endedChannels.
        active.clear();
        effects.clear();
        fieldChannels.erase(kDuskPriestInst);
        endedChannels.insert(kDuskPriestGuid);
        auto plan3 = reconcileFieldChannels(raw, active, effects, fieldChannels, heldChannelKits, endedChannels);
        CHECK(plan3.liveChannelIndices.empty());
        CHECK(plan3.channelsToStart.empty());
        CHECK(endedChannels.contains(kDuskPriestGuid));

        // Tick 4: UNIT_CHANNEL_SPELL clears on the server -> endedChannels pruned cleanly.
        auto plan4 = reconcileFieldChannels({}, active, effects, fieldChannels, heldChannelKits, endedChannels);
        CHECK_FALSE(endedChannels.contains(kDuskPriestGuid));
    }

    SECTION("Mid-channel AOI entry waits for render instance, attaches ChannelKit, and clears on field=0") {
        // Frame 1: CREATE_OBJECT arrived mid-channel, creature model still streaming (inst == 0).
        std::vector<FieldChannelEntry> streaming{{kDuskPriestGuid, 0, kMindFlayVisual, 0}};
        auto p0 = reconcileFieldChannels(streaming, {}, {}, fieldChannels, heldChannelKits, endedChannels);
        CHECK(p0.channelsToStart.empty());
        CHECK(p0.liveChannelIndices.size() == 1);

        // Frame 2: CharacterRenderer instance 154 created -> ChannelKit spawned once.
        std::vector<FieldChannelEntry> spawned{{kDuskPriestGuid, kDuskPriestInst, kMindFlayVisual, 0}};
        auto p1 = reconcileFieldChannels(spawned, {}, {}, fieldChannels, heldChannelKits, endedChannels);
        REQUIRE(p1.channelsToStart == std::vector<size_t>{0});
        fieldChannels[kDuskPriestInst] = kMindFlayVisual;
        heldChannelKits.insert(kDuskPriestInst);
        std::unordered_map<uint32_t, uint32_t> active{{kDuskPriestInst, kMindFlayVisual}};
        std::unordered_set<uint32_t> effects{kDuskPriestInst};

        // Frame 3: Steady channel -> no duplicate start, no premature stop.
        auto p2 = reconcileFieldChannels(spawned, active, effects, fieldChannels, heldChannelKits, endedChannels);
        CHECK(p2.channelsToStart.empty());
        CHECK(p2.instancesToStop.empty());

        // Frame 4: UNIT_CHANNEL_SPELL clears to 0 -> held ChannelKit stops immediately.
        auto p3 = reconcileFieldChannels({}, active, effects, fieldChannels, heldChannelKits, endedChannels);
        CHECK(p3.instancesToStop == std::vector<uint32_t>{kDuskPriestInst});
        CHECK(fieldChannels.empty());
        CHECK(heldChannelKits.empty());
    }

    SECTION("Switching channels via MSG_CHANNEL_START is not killed by stale previous UNIT_CHANNEL_SPELL") {
        // Unit was channeling Mind Flay (277), then MSG_CHANNEL_START switched to
        // Arcane Missiles (262) before SMSG_UPDATE_OBJECT updated UNIT_CHANNEL_SPELL.
        // startChannelVisual cleared fieldChannels[inst] and set active[inst] = 262.
        std::unordered_map<uint32_t, uint32_t> active{{kDuskPriestInst, kArcaneMissilesVisual}};
        std::unordered_set<uint32_t> effects{kDuskPriestInst};
        std::vector<FieldChannelEntry> staleField{{kDuskPriestGuid, kDuskPriestInst, kMindFlayVisual, 0}};

        auto p1 = reconcileFieldChannels(staleField, active, effects, fieldChannels, heldChannelKits, endedChannels);
        CHECK(p1.instancesToStop.empty());
        CHECK(p1.channelsToStart.empty());
        CHECK_FALSE(fieldChannels.contains(kDuskPriestInst));

        // Next tick: SMSG_UPDATE_OBJECT catches up to Arcane Missiles (262).
        std::vector<FieldChannelEntry> updatedField{{kDuskPriestGuid, kDuskPriestInst, kArcaneMissilesVisual, 0}};
        auto p2 = reconcileFieldChannels(updatedField, active, effects, fieldChannels, heldChannelKits, endedChannels);
        CHECK(p2.instancesToStop.empty());
        CHECK(p2.channelsToStart.empty());
        CHECK(fieldChannels[kDuskPriestInst] == kArcaneMissilesVisual);
    }

    SECTION("AOI-only unit switching UNIT_CHANNEL_SPELL directly stops old kit and starts new kit") {
        fieldChannels[kDuskPriestInst] = kMindFlayVisual;
        heldChannelKits.insert(kDuskPriestInst);
        std::unordered_map<uint32_t, uint32_t> active{{kDuskPriestInst, kMindFlayVisual}};
        std::unordered_set<uint32_t> effects{kDuskPriestInst};

        std::vector<FieldChannelEntry> switched{{kDuskPriestGuid, kDuskPriestInst, kArcaneMissilesVisual, 0}};
        auto p = reconcileFieldChannels(switched, active, effects, fieldChannels, heldChannelKits, endedChannels);
        CHECK(p.instancesToStop == std::vector<uint32_t>{kDuskPriestInst});
        CHECK(p.channelsToStart == std::vector<size_t>{0});
    }
}

TEST_CASE("TBC 2.4.3 dbc_layouts.json SpellVisual and SpellVisualKit match shipped channeled spell DBC rows",
          "[channel][spell-visual][dbc]") {
    DBCLayout layout;
    const std::string tbcLayoutPath =
        (std::filesystem::path(WOWEE_SOURCE_DIR) / "Data/expansions/tbc/dbc_layouts.json").string();
    REQUIRE(layout.loadFromJson(tbcLayoutPath));

    const auto* spLayout = layout.getLayout("Spell");
    const auto* svLayout = layout.getLayout("SpellVisual");
    const auto* kitLayout = layout.getLayout("SpellVisualKit");
    const auto* fxLayout = layout.getLayout("SpellVisualEffectName");
    REQUIRE(spLayout != nullptr);
    REQUIRE(svLayout != nullptr);
    REQUIRE(kitLayout != nullptr);
    REQUIRE(fxLayout != nullptr);

    // Verify TBC 2.4.3 (build 8606) column indices.
    CHECK(svLayout->field("PrecastKit") == 1u);
    CHECK(svLayout->field("CastKit") == 2u);
    CHECK(svLayout->field("ImpactKit") == 3u);
    CHECK(svLayout->field("StateKit") == 4u);
    CHECK(svLayout->field("ChannelKit") == 5u);
    CHECK(svLayout->field("MissileModel") == 7u);
    CHECK(svLayout->field("MissileDestinationAttachment") == 9u);
    CHECK(svLayout->field("CasterImpactKit") == 13u);
    CHECK(svLayout->field("TargetImpactKit") == 14u);
    CHECK(svLayout->field("MissileAttachment") == 15u);

    CHECK(kitLayout->field("HeadEffect") == 3u);
    CHECK(kitLayout->field("ChestEffect") == 4u);
    CHECK(kitLayout->field("BaseEffect") == 5u);
    CHECK(kitLayout->field("LeftHandEffect") == 6u);
    CHECK(kitLayout->field("RightHandEffect") == 7u);
    CHECK(kitLayout->field("BreathEffect") == 8u);
    CHECK(kitLayout->field("SpecialEffect0") == 11u);
    CHECK(kitLayout->field("WorldEffect") == 14u);

    DBCFile spellDbc, svDbc, kitDbc, fxDbc;
    if (!loadTbcDbc("Spell.dbc", spellDbc) ||
        !loadTbcDbc("SpellVisual.dbc", svDbc) ||
        !loadTbcDbc("SpellVisualKit.dbc", kitDbc) ||
        !loadTbcDbc("SpellVisualEffectName.dbc", fxDbc)) {
        SKIP("TBC 2.4.3 DBC files not available locally");
    }

    REQUIRE(svDbc.getFieldCount() == 25u);
    REQUIRE(kitDbc.getFieldCount() == 38u);
    REQUIRE(fxDbc.getFieldCount() == 5u);

    // Sunblade Dusk Priest Mind Flay (46562) and Priest Mind Flay Ranks 1 & 7 (15407, 25387):
    // SpellVisual 277 -> CastKit 429, StateKit 814, ChannelKit 430 -> FX 271 ManaFunnel_Impact_Chest.
    for (const uint32_t spellId : {46562u, 15407u, 25387u}) {
        INFO("Mind Flay spellId=" << spellId);
        const int32_t spRec = spellDbc.findRecordById(spellId);
        REQUIRE(spRec >= 0);
        const uint32_t visualId =
            spellDbc.getUInt32(static_cast<uint32_t>(spRec), spLayout->field("SpellVisualID"));
        CHECK(visualId == 277u);
    }

    const int32_t sv277 = svDbc.findRecordById(277);
    REQUIRE(sv277 >= 0);
    CHECK(svDbc.getUInt32(static_cast<uint32_t>(sv277), svLayout->field("PrecastKit")) == 0u);
    CHECK(svDbc.getUInt32(static_cast<uint32_t>(sv277), svLayout->field("CastKit")) == 429u);
    CHECK(svDbc.getUInt32(static_cast<uint32_t>(sv277), svLayout->field("ImpactKit")) == 0u);
    CHECK(svDbc.getUInt32(static_cast<uint32_t>(sv277), svLayout->field("StateKit")) == 814u);
    CHECK(svDbc.getUInt32(static_cast<uint32_t>(sv277), svLayout->field("ChannelKit")) == 430u);

    const int32_t kit429 = kitDbc.findRecordById(429);
    const int32_t kit430 = kitDbc.findRecordById(430);
    REQUIRE(kit429 >= 0);
    REQUIRE(kit430 >= 0);
    CHECK(kitDbc.getUInt32(static_cast<uint32_t>(kit429), 2) == 124u);  // ChannelCastDirected
    CHECK(kitDbc.getUInt32(static_cast<uint32_t>(kit430), 2) == 124u);  // ChannelCastDirected
    CHECK(kitDbc.getUInt32(static_cast<uint32_t>(kit429), kitLayout->field("LeftHandEffect")) == 271u);
    CHECK(kitDbc.getUInt32(static_cast<uint32_t>(kit430), kitLayout->field("LeftHandEffect")) == 271u);
    CHECK(kitDbc.getUInt32(static_cast<uint32_t>(kit430), kitLayout->field("RightHandEffect")) == 0u);
    // CharProc0 == 0 (SpellChainEffects), CharParamZero0 == 4.0f (ManaBeam.blp)
    CHECK(kitDbc.getInt32(static_cast<uint32_t>(kit430), kitLayout->field("CharProc0")) == 0);
    CHECK(kitDbc.getFloat(static_cast<uint32_t>(kit430), kitLayout->field("CharParamZero0")) ==
          Catch::Approx(4.0f));

    const int32_t fx271 = fxDbc.findRecordById(271);
    REQUIRE(fx271 >= 0);
    CHECK(wowee::pipeline::modelPathToM2(
              fxDbc.getString(static_cast<uint32_t>(fx271), fxLayout->field("FilePath"))) ==
          "Spells\\ManaFunnel_Impact_Chest.m2");
    CHECK(fxDbc.getFloat(static_cast<uint32_t>(fx271), fxLayout->field("Scale")) == Catch::Approx(1.0f));

    // Arcane Missiles (5143, 38699): Visual 262 -> ChannelKit 729 -> Magic_PreCast_Hand on BOTH hands (21 & 22).
    const int32_t sv262 = svDbc.findRecordById(262);
    REQUIRE(sv262 >= 0);
    CHECK(svDbc.getUInt32(static_cast<uint32_t>(sv262), svLayout->field("ChannelKit")) == 729u);
    const int32_t kit729 = kitDbc.findRecordById(729);
    REQUIRE(kit729 >= 0);
    CHECK(kitDbc.getUInt32(static_cast<uint32_t>(kit729), kitLayout->field("LeftHandEffect")) == 272u);
    CHECK(kitDbc.getUInt32(static_cast<uint32_t>(kit729), kitLayout->field("RightHandEffect")) == 272u);

    // Drain Life (689, 27220): Visual 177 -> CastKit 341 (Chest 298), ChannelKit 402 (LeftHand 298, chain 8).
    const int32_t sv177 = svDbc.findRecordById(177);
    REQUIRE(sv177 >= 0);
    CHECK(svDbc.getUInt32(static_cast<uint32_t>(sv177), svLayout->field("CastKit")) == 341u);
    CHECK(svDbc.getUInt32(static_cast<uint32_t>(sv177), svLayout->field("ChannelKit")) == 402u);
    const int32_t kit402 = kitDbc.findRecordById(402);
    REQUIRE(kit402 >= 0);
    CHECK(kitDbc.getUInt32(static_cast<uint32_t>(kit402), kitLayout->field("LeftHandEffect")) == 298u);
}

TEST_CASE("Sunblade Dusk Priest BloodElf M2s and ManaFunnel_Impact_Chest.m2 author valid channel hand glow data",
          "[channel][spell-visual][assets]") {
    const auto dataDir = resolveTbcDataDir();
    if (dataDir.empty()) {
        SKIP("TBC extracted asset directory not available");
    }

    // 1. BloodElfMale and BloodElfFemale (Sunblade Dusk Priest displays 23158/23545/23546/23547)
    // carry SpellLeftHand (21) and SpellRightHand (22), and extend $CSL on ChannelCastDirected (124).
    for (const char* rel : {"character/bloodelf/male/bloodelfmale.m2",
                            "character/bloodelf/female/bloodelffemale.m2"}) {
        INFO(rel);
        const auto bytes = readBytes(dataDir / rel);
        REQUIRE_FALSE(bytes.empty());
        const M2Model model = M2Loader::load(bytes);
        REQUIRE(model.attachmentLookup.size() > SpellVisualSystem::kAttachSpellRightHand);

        const uint16_t leftIdx = model.attachmentLookup[SpellVisualSystem::kAttachSpellLeftHand];
        const uint16_t rightIdx = model.attachmentLookup[SpellVisualSystem::kAttachSpellRightHand];
        REQUIRE(leftIdx < model.attachments.size());
        REQUIRE(rightIdx < model.attachments.size());
        CHECK(model.attachments[leftIdx].id == SpellVisualSystem::kAttachSpellLeftHand);
        CHECK(model.attachments[rightIdx].id == SpellVisualSystem::kAttachSpellRightHand);
        CHECK(model.attachments[leftIdx].bone < model.bones.size());
        CHECK(model.attachments[rightIdx].bone < model.bones.size());
        CHECK(model.attachments[leftIdx].position.y > 0.15f);   // Left palm (+Y)
        CHECK(model.attachments[rightIdx].position.y < -0.15f); // Right palm (-Y)

        const auto beamOrigin = resolveBeamStartOrigin(&model, 124, -1);
        CHECK(beamOrigin.source == SpellLaunchOrigin::Source::EventBone);
        CHECK(beamOrigin.localOffset.y > 0.15f);  // $CSL left hand matches LeftHandEffect (21)
    }

    // 2. Spells\ManaFunnel_Impact_Chest.m2 authors Stand (0), Hold (158), and Decay (159),
    // keeping both particle emitters enabled and both StarFlash_Grey flare batches opaque (alpha=1)
    // throughout Stand and the looping Hold sequence.
    {
        const auto bytes = readBytes(dataDir / "spells/manafunnel_impact_chest.m2");
        REQUIRE_FALSE(bytes.empty());
        const M2Model fx = M2Loader::load(bytes);
        REQUIRE(fx.sequences.size() >= 3);
        CHECK(fx.sequences[0].id == 0u);    // Stand
        CHECK(fx.sequences[1].id == 158u);  // Hold
        CHECK(fx.sequences[2].id == 159u);  // Decay
        REQUIRE(fx.batches.size() == 2);
        REQUIRE(fx.particleEmitters.size() == 2);

        for (int seqIdx : {0, 1}) {
            const float midMs = static_cast<float>(fx.sequences[seqIdx].duration) * 0.5f;
            for (const auto& em : fx.particleEmitters) {
                const float enabled = m2_track::sampleFloat(
                    em.enabledIn, seqIdx, midMs, 100.0f, fx.globalSequenceDurations, 1.0f);
                const float rate = m2_track::sampleFloat(
                    em.emissionRate, seqIdx, midMs, 100.0f, fx.globalSequenceDurations, 0.0f);
                CHECK(enabled > 0.5f);
                CHECK(rate >= 20.0f);
                CHECK_FALSE(m2_track::pastLastStepKey(
                    em.emissionRate, seqIdx, midMs, 100.0f, fx.globalSequenceDurations));
            }
            for (const auto& alphaTrack : fx.colorAlphaTracks) {
                const float alpha = m2_track::sampleFloat(
                    alphaTrack, seqIdx, midMs, 100.0f, fx.globalSequenceDurations, 1.0f);
                CHECK(alpha == Catch::Approx(1.0f));
            }
        }
    }
}
