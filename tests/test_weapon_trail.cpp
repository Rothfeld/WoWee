#include <catch_amalgamated.hpp>

#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"
#include "rendering/weapon_trail.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <vector>

using Catch::Approx;
using namespace wowee::rendering;

namespace {

std::vector<uint8_t> readFileBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

}  // namespace

TEST_CASE("parseWeaponTrailCharProc unpacks TBC SpellVisualKit CharProc 8 parameters and whole-second truncation",
          "[weapon_trail]") {
    // Whirlwind (kit 369): 0xF71717, fadeOutRate=20, duration=2000ms -> 2s, alpha=100
    const auto ww = parseWeaponTrailCharProc(static_cast<float>(0xF71717u), 20.0f, 2000.0f, 100.0f);
    REQUIRE(ww.has_value());
    CHECK(ww->color.r == Approx(0xF7 / 255.0f).margin(1e-4f));
    CHECK(ww->color.g == Approx(0x17 / 255.0f).margin(1e-4f));
    CHECK(ww->color.b == Approx(0x17 / 255.0f).margin(1e-4f));
    CHECK(ww->fadeOutRate == Approx(20.0f));
    CHECK(ww->durationSec == Approx(2.0f));
    CHECK(ww->alpha == Approx(100.0f / 255.0f).margin(1e-4f));

    // Charge / Surge (kit 44) & Tackle Hit (kit 8456): 0xDB0B10, 1000ms -> 1s
    const auto chg = parseWeaponTrailCharProc(static_cast<float>(0xDB0B10u), 20.0f, 1000.0f, 100.0f);
    REQUIRE(chg.has_value());
    CHECK(chg->color.r == Approx(0xDB / 255.0f).margin(1e-4f));
    CHECK(chg->color.g == Approx(0x0B / 255.0f).margin(1e-4f));
    CHECK(chg->color.b == Approx(0x10 / 255.0f).margin(1e-4f));
    CHECK(chg->durationSec == Approx(1.0f));
    CHECK(chg->alpha == Approx(100.0f / 255.0f).margin(1e-4f));

    // Pure black trail with >= 1000ms duration and nonzero alpha is valid;
    // fractional ms truncates down to whole seconds (1999ms -> 1s).
    const auto blk = parseWeaponTrailCharProc(0.0f, 20.0f, 1999.0f, 100.0f);
    REQUIRE(blk.has_value());
    CHECK(blk->color.r == Approx(0.0f));
    CHECK(blk->color.g == Approx(0.0f));
    CHECK(blk->color.b == Approx(0.0f));
    CHECK(blk->durationSec == Approx(1.0f));
    CHECK(blk->alpha == Approx(100.0f / 255.0f).margin(1e-4f));

    // Sub-second melee kits (< 1000ms) truncate to 0s on native TBC 2.4.3 and draw no trail:
    // Mortal Strike / Heroic Strike / Overpower / Raptor Strike / Bloodthirst (kit 324, 600ms)
    CHECK_FALSE(parseWeaponTrailCharProc(static_cast<float>(0xF82929u), 20.0f, 600.0f, 100.0f).has_value());
    // Cleave (kit 366, 600ms)
    CHECK_FALSE(parseWeaponTrailCharProc(static_cast<float>(0xC1FC38u), 20.0f, 600.0f, 100.0f).has_value());
    // Sinister Strike (kit 399, 400ms)
    CHECK_FALSE(parseWeaponTrailCharProc(static_cast<float>(0xBD55C6u), 15.0f, 400.0f, 96.0f).has_value());
    // Feint (kit 823, 700ms)
    CHECK_FALSE(parseWeaponTrailCharProc(static_cast<float>(0x409FEEu), 25.0f, 700.0f, 65.0f).has_value());
    // Boundary: 999ms truncates to 0s
    CHECK_FALSE(parseWeaponTrailCharProc(static_cast<float>(0xF82929u), 20.0f, 999.0f, 100.0f).has_value());

    // Zero alpha or zero duration produces no trail (e.g. kits 6349, 8554)
    CHECK_FALSE(parseWeaponTrailCharProc(static_cast<float>(0xFF0D13u), 0.0f, 1000.0f, 0.0f).has_value());
    CHECK_FALSE(parseWeaponTrailCharProc(static_cast<float>(0x39FE16u), 500.0f, 3000.0f, 0.0f).has_value());
    CHECK_FALSE(parseWeaponTrailCharProc(static_cast<float>(0xF82929u), 20.0f, 0.0f, 100.0f).has_value());
}

TEST_CASE("weaponBladeLocalSpan selects blade axis for swords and fist weapons", "[weapon_trail]") {
    // Standard 1H/2H sword extending along +X
    const auto [sBase, sTip] = weaponBladeLocalSpan({-0.36f, -0.06f, -0.09f}, {1.24f, 0.06f, 0.09f});
    CHECK(sBase.x == Approx(1.24f * 0.20f));
    CHECK(sBase.y == Approx(0.0f));
    CHECK(sBase.z == Approx(0.0f));
    CHECK(sTip.x == Approx(1.24f));
    CHECK(sTip.y == Approx(0.0f));
    CHECK(sTip.z == Approx(0.0f));

    // Fist/claw weapon extending along -Z (e.g. hand_1h_blackwing_a_01.m2)
    const auto [fBase, fTip] = weaponBladeLocalSpan({-0.16f, -0.39f, -0.60f}, {0.21f, 0.03f, 0.43f});
    CHECK(fBase.z == Approx(0.0f));
    CHECK(fTip.z == Approx(-0.60f));
}

TEST_CASE("stepWeaponTrailChannel samples ribbon edges during swing and decays smoothly", "[weapon_trail]") {
    const auto specOpt = parseWeaponTrailCharProc(static_cast<float>(0xF71717u), 20.0f, 2000.0f, 100.0f);
    REQUIRE(specOpt.has_value());
    const WeaponTrailSpec& spec = *specOpt;

    WeaponTrailChannel ch;
    ch.weaponInstanceId = 42;

    // Step across a 120ms arc at 20ms intervals (6 samples)
    for (int i = 0; i < 6; ++i) {
        const float x = static_cast<float>(i) * 0.25f;
        const std::pair<glm::vec3, glm::vec3> blade{{x, 0.0f, 0.0f}, {x, 1.0f, 0.0f}};
        stepWeaponTrailChannel(ch, spec, 0.020f, true, blade);
    }

    REQUIRE(ch.edges.size() == 6);
    CHECK(ch.edges.back().alpha == Approx(spec.alpha));
    // Oldest edge aged by 5 * 0.020 = 0.100 s -> decayed by 0.100 * (20*20)/255 = 40/255
    CHECK(ch.edges.front().alpha == Approx((100.0f - 40.0f) / 255.0f).margin(1e-3f));
    CHECK_FALSE(isWeaponTrailChannelDead(ch));

    // Stop emitting and advance 0.300 s (> 0.250 s full fade lifetime): channel must die
    for (int i = 0; i < 15; ++i) {
        stepWeaponTrailChannel(ch, spec, 0.020f, false, std::nullopt);
    }
    CHECK(isWeaponTrailChannelDead(ch));
}

TEST_CASE("stepWeaponTrailChannel does not stack degenerate quads when weapon is stationary", "[weapon_trail]") {
    const auto spec = *parseWeaponTrailCharProc(static_cast<float>(0xDB0B10u), 20.0f, 1000.0f, 100.0f);
    WeaponTrailChannel ch;
    const std::pair<glm::vec3, glm::vec3> blade{{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}};

    for (int i = 0; i < 5; ++i) {
        stepWeaponTrailChannel(ch, spec, 0.020f, true, blade);
    }
    CHECK(ch.edges.size() == 1);
    CHECK(isWeaponTrailChannelDead(ch));
}

TEST_CASE("TBC 2.4.3 Spell/SpellVisual/SpellVisualKit CharProc == 8 matches native trail parity",
          "[weapon_trail][tbc_data]") {
    const char* envDir = std::getenv("WOWEE_TBC_DATA_DIR");
    const std::filesystem::path tbcDir = envDir ? std::filesystem::path(envDir)
                                                : std::filesystem::path("/workspace/azerothcore/client/wowee-data/expansions/tbc");
    const auto kitDbcPath = tbcDir / "dbfilesclient/spellvisualkit.dbc";
    const auto visDbcPath = tbcDir / "dbfilesclient/spellvisual.dbc";
    const auto spellDbcPath = tbcDir / "dbfilesclient/spell.dbc";
    const auto layoutPath = tbcDir / "dbc_layouts.json";
    if (!std::filesystem::exists(kitDbcPath) || !std::filesystem::exists(visDbcPath) ||
        !std::filesystem::exists(spellDbcPath) || !std::filesystem::exists(layoutPath)) {
        SKIP("TBC data directory unavailable");
    }

    wowee::pipeline::DBCLayout layout;
    REQUIRE(layout.loadFromJson(layoutPath.string()));
    const auto* kitLayout = layout.getLayout("SpellVisualKit");
    const auto* visLayout = layout.getLayout("SpellVisual");
    const auto* spellLayout = layout.getLayout("Spell");
    REQUIRE(kitLayout != nullptr);
    REQUIRE(visLayout != nullptr);
    REQUIRE(spellLayout != nullptr);

    wowee::pipeline::DBCFile kitDbc, visDbc, spellDbc;
    REQUIRE(kitDbc.load(readFileBytes(kitDbcPath)));
    REQUIRE(visDbc.load(readFileBytes(visDbcPath)));
    REQUIRE(spellDbc.load(readFileBytes(spellDbcPath)));

    const uint32_t fc = kitDbc.getFieldCount();
    std::unordered_map<uint32_t, WeaponTrailSpec> parsed;
    uint32_t totalProc8 = 0;

    for (uint32_t i = 0; i < kitDbc.getRecordCount(); ++i) {
        const uint32_t kitId = kitDbc.getUInt32(i, 0);
        if (!kitId) continue;
        for (int k = 0; k < 4; ++k) {
            const uint32_t pf = kitLayout->field("CharProc" + std::to_string(k));
            const uint32_t p0f = kitLayout->field("CharParamZero" + std::to_string(k));
            const uint32_t p1f = kitLayout->field("CharParamOne" + std::to_string(k));
            const uint32_t p2f = kitLayout->field("CharParamTwo" + std::to_string(k));
            const uint32_t p3f = kitLayout->field("CharParamThree" + std::to_string(k));
            REQUIRE(pf < fc);
            REQUIRE(p0f < fc);
            REQUIRE(p1f < fc);
            REQUIRE(p2f < fc);
            REQUIRE(p3f < fc);

            if (kitDbc.getUInt32(i, pf) == 8u) {
                ++totalProc8;
                const auto spec = parseWeaponTrailCharProc(
                    kitDbc.getFloat(i, p0f), kitDbc.getFloat(i, p1f),
                    kitDbc.getFloat(i, p2f), kitDbc.getFloat(i, p3f));
                if (spec) parsed[kitId] = *spec;
            }
        }
    }

    // TBC 2.4.3 ships 66 CharProc == 8 entries:
    // 16 active (>= 1000ms and alpha > 0), 48 sub-second (< 1000ms -> 0s), 2 zero-alpha (6349, 8554).
    CHECK(totalProc8 == 66);
    CHECK(parsed.size() == 16);

    // Map spellId -> CastKit via Spell.dbc -> SpellVisual.dbc
    const uint32_t svCastField = visLayout->field("CastKit");
    std::unordered_map<uint32_t, uint32_t> visToCastKit;
    for (uint32_t i = 0; i < visDbc.getRecordCount(); ++i) {
        visToCastKit[visDbc.getUInt32(i, 0)] = visDbc.getUInt32(i, svCastField);
    }
    const uint32_t spVisField = spellLayout->field("SpellVisualID");
    std::unordered_map<uint32_t, uint32_t> spellToCastKit;
    for (uint32_t i = 0; i < spellDbc.getRecordCount(); ++i) {
        const uint32_t sid = spellDbc.getUInt32(i, 0);
        const uint32_t vid = spellDbc.getUInt32(i, spVisField);
        if (const auto it = visToCastKit.find(vid); it != visToCastKit.end()) {
            spellToCastKit[sid] = it->second;
        }
    }

    auto hasCastTrail = [&](uint32_t spellId) -> bool {
        const auto it = spellToCastKit.find(spellId);
        return it != spellToCastKit.end() && parsed.count(it->second) == 1;
    };

    // Representative TBC melee spells where native 2.4.3 produces NO weapon trail:
    CHECK_FALSE(hasCastTrail(30330));  // Mortal Strike (Rank 6, kit 324, 600ms)
    CHECK_FALSE(hasCastTrail(12294));  // Mortal Strike (Rank 1, kit 324, 600ms)
    CHECK_FALSE(hasCastTrail(78));     // Heroic Strike (kit 324, 600ms)
    CHECK_FALSE(hasCastTrail(845));    // Cleave (kit 366, 600ms)
    CHECK_FALSE(hasCastTrail(1464));   // Slam (kit 1664, no CharProc 8)
    CHECK_FALSE(hasCastTrail(7384));   // Overpower (kit 324, 600ms)
    CHECK_FALSE(hasCastTrail(23881));  // Bloodthirst (kit 324, 600ms)
    CHECK_FALSE(hasCastTrail(1752));   // Sinister Strike (kit 399, 400ms)
    CHECK_FALSE(hasCastTrail(2973));   // Raptor Strike (kit 324, 600ms)
    CHECK_FALSE(hasCastTrail(1715));   // Hamstring (kit 646, 600ms)
    CHECK_FALSE(hasCastTrail(2098));   // Eviscerate (kit 733, 450ms)
    CHECK_FALSE(hasCastTrail(7386));   // Sunder Armor (kit 557, 600ms)
    CHECK_FALSE(hasCastTrail(1966));   // Feint (kit 823, 700ms)

    // Representative TBC spells where native 2.4.3 DOES render weapon trails:
    CHECK(hasCastTrail(1680));   // Whirlwind (kit 369, 2000ms -> 2s)
    CHECK(hasCastTrail(100));    // Charge (kit 44, 1000ms -> 1s)
    CHECK(hasCastTrail(19196));  // Surge (kit 44, 1000ms -> 1s)
    CHECK(hasCastTrail(44030));  // Tackle Hit (kit 8456, 1000ms -> 1s)

    REQUIRE(parsed.count(369) == 1);
    CHECK(parsed.at(369).durationSec == Approx(2.0f));
    CHECK(parsed.at(369).color.r == Approx(0xF7 / 255.0f).margin(1e-4f));

    REQUIRE(parsed.count(44) == 1);
    CHECK(parsed.at(44).durationSec == Approx(1.0f));

    REQUIRE(parsed.count(370) == 1);
    CHECK(parsed.at(370).durationSec == Approx(10.0f));
}

TEST_CASE("Channel weapon trails cap duration to active channel and stop emitting on channel end",
          "[weapon_trail][channel]") {
    // Kit 370 (Whirlwind channel, SpellVisual 7591 / spell 33500) authors 10000ms (10s),
    // but Cabal Executioner channels 33500 for 2000ms (2s). Capping prevents the trail
    // from persisting onto subsequent normal melee swings for 8 extra seconds.
    CHECK(resolveChannelWeaponTrailDuration(10.0f, 2.0f) == Approx(2.0f));
    CHECK(resolveChannelWeaponTrailDuration(1.0f, 5.0f) == Approx(1.0f));
    CHECK(resolveChannelWeaponTrailDuration(10.0f, 0.0f) == Approx(10.0f));

    auto spec = parseWeaponTrailCharProc(0xFB8713, 20.0f, 10000.0f, 100.0f);
    REQUIRE(spec.has_value());

    ActiveUnitWeaponTrail trail;
    trail.ownerInstanceId = 2814;
    trail.spec = *spec;
    trail.spec.durationSec = resolveChannelWeaponTrailDuration(spec->durationSec, 2.0f);
    trail.fromChannel = true;
    trail.elapsed = 0.8f;

    WeaponTrailChannel ch;
    ch.weaponInstanceId = 99;
    stepWeaponTrailChannel(ch, trail.spec, 0.025f, true,
                           std::make_pair(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
    stepWeaponTrailChannel(ch, trail.spec, 0.025f, true,
                           std::make_pair(glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)));
    REQUIRE(ch.edges.size() == 2);
    CHECK_FALSE(isWeaponTrailChannelDead(ch));

    // Channel ends early at 0.8s (MSG_CHANNEL_UPDATE 0)
    stopWeaponTrailEmission(trail);
    CHECK(trail.spec.durationSec == Approx(0.8f));
    CHECK_FALSE(trail.elapsed < trail.spec.durationSec);

    // Subsequent swings after channel end emit no new edges and decay cleanly to dead
    for (int i = 0; i < 15; ++i) {
        stepWeaponTrailChannel(ch, trail.spec, 0.025f, /*emitting=*/false, std::nullopt);
    }
    CHECK(isWeaponTrailChannelDead(ch));
}
