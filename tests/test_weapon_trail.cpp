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

TEST_CASE("parseWeaponTrailCharProc unpacks TBC SpellVisualKit CharProc 8 parameters", "[weapon_trail]") {
    // Heroic Strike / Overpower (kit 324): 0xF82929, fadeOutRate=20, duration=600ms, alpha=100
    const auto hs = parseWeaponTrailCharProc(static_cast<float>(0xF82929u), 20.0f, 600.0f, 100.0f);
    REQUIRE(hs.has_value());
    CHECK(hs->color.r == Approx(0xF8 / 255.0f).margin(1e-4f));
    CHECK(hs->color.g == Approx(0x29 / 255.0f).margin(1e-4f));
    CHECK(hs->color.b == Approx(0x29 / 255.0f).margin(1e-4f));
    CHECK(hs->fadeOutRate == Approx(20.0f));
    CHECK(hs->durationSec == Approx(0.6f));
    CHECK(hs->alpha == Approx(100.0f / 255.0f).margin(1e-4f));

    // Cleave (kit 366): 0xC1FC38 lime green
    const auto cl = parseWeaponTrailCharProc(static_cast<float>(0xC1FC38u), 20.0f, 600.0f, 100.0f);
    REQUIRE(cl.has_value());
    CHECK(cl->color.r == Approx(0xC1 / 255.0f).margin(1e-4f));
    CHECK(cl->color.g == Approx(0xFC / 255.0f).margin(1e-4f));
    CHECK(cl->color.b == Approx(0x38 / 255.0f).margin(1e-4f));

    // Pure black trail (kit 4909 Cleave): 0x000000 with alpha 100 is valid (alpha-blended)
    const auto blk = parseWeaponTrailCharProc(0.0f, 20.0f, 600.0f, 100.0f);
    REQUIRE(blk.has_value());
    CHECK(blk->color.r == Approx(0.0f));
    CHECK(blk->color.g == Approx(0.0f));
    CHECK(blk->color.b == Approx(0.0f));
    CHECK(blk->alpha == Approx(100.0f / 255.0f).margin(1e-4f));

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
    const auto specOpt = parseWeaponTrailCharProc(static_cast<float>(0xF71717u), 20.0f, 0.6f, 100.0f);
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
    const auto spec = *parseWeaponTrailCharProc(static_cast<float>(0xC1FC38u), 20.0f, 0.6f, 100.0f);
    WeaponTrailChannel ch;
    const std::pair<glm::vec3, glm::vec3> blade{{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}};

    for (int i = 0; i < 5; ++i) {
        stepWeaponTrailChannel(ch, spec, 0.020f, true, blade);
    }
    CHECK(ch.edges.size() == 1);
    CHECK(isWeaponTrailChannelDead(ch));
}

TEST_CASE("TBC 2.4.3 SpellVisualKit.dbc CharProc == 8 parses all shipped melee weapon trail kits", "[weapon_trail][tbc_data]") {
    const char* envDir = std::getenv("WOWEE_TBC_DATA_DIR");
    const std::filesystem::path tbcDir = envDir ? std::filesystem::path(envDir)
                                                : std::filesystem::path("/workspace/azerothcore/client/wowee-data/expansions/tbc");
    const auto dbcPath = tbcDir / "dbfilesclient/spellvisualkit.dbc";
    const auto layoutPath = tbcDir / "dbc_layouts.json";
    if (!std::filesystem::exists(dbcPath) || !std::filesystem::exists(layoutPath)) {
        SKIP("TBC data directory unavailable");
    }

    wowee::pipeline::DBCLayout layout;
    REQUIRE(layout.loadFromJson(layoutPath.string()));
    const auto* kitLayout = layout.getLayout("SpellVisualKit");
    REQUIRE(kitLayout != nullptr);

    wowee::pipeline::DBCFile dbc;
    REQUIRE(dbc.load(readFileBytes(dbcPath)));

    const uint32_t fc = dbc.getFieldCount();
    std::unordered_map<uint32_t, WeaponTrailSpec> parsed;
    uint32_t totalProc8 = 0;

    for (uint32_t i = 0; i < dbc.getRecordCount(); ++i) {
        const uint32_t kitId = dbc.getUInt32(i, 0);
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

            if (dbc.getUInt32(i, pf) == 8u) {
                ++totalProc8;
                const auto spec = parseWeaponTrailCharProc(
                    dbc.getFloat(i, p0f), dbc.getFloat(i, p1f),
                    dbc.getFloat(i, p2f), dbc.getFloat(i, p3f));
                if (spec) parsed[kitId] = *spec;
            }
        }
    }

    // TBC 2.4.3 ships 66 CharProc == 8 entries (64 visible + 2 zero-alpha kits 6349, 8554).
    CHECK(totalProc8 == 66);
    CHECK(parsed.size() == 64);

    // Spot-check iconic melee kits:
    // Heroic Strike / Overpower (kit 324): red 0xF82929, rate 20, 600ms, alpha 100
    REQUIRE(parsed.count(324) == 1);
    CHECK(parsed.at(324).color.r == Approx(0xF8 / 255.0f).margin(1e-4f));
    CHECK(parsed.at(324).durationSec == Approx(0.6f));
    CHECK(parsed.at(324).alpha == Approx(100.0f / 255.0f).margin(1e-4f));

    // Cleave (kit 366): lime green 0xC1FC38
    REQUIRE(parsed.count(366) == 1);
    CHECK(parsed.at(366).color.g == Approx(0xFC / 255.0f).margin(1e-4f));

    // Whirlwind (kit 369): red 0xF71717, 2000ms
    REQUIRE(parsed.count(369) == 1);
    CHECK(parsed.at(369).durationSec == Approx(2.0f));

    // Sinister Strike (kit 399): purple 0xBD55C6, rate 15, 400ms, alpha 96
    REQUIRE(parsed.count(399) == 1);
    CHECK(parsed.at(399).fadeOutRate == Approx(15.0f));
    CHECK(parsed.at(399).durationSec == Approx(0.4f));
    CHECK(parsed.at(399).alpha == Approx(96.0f / 255.0f).margin(1e-4f));

    // Hamstring / Wing Clip (kit 646): violet-blue 0x6D2CF5
    REQUIRE(parsed.count(646) == 1);
    CHECK(parsed.at(646).color.b == Approx(0xF5 / 255.0f).margin(1e-4f));

    // Charge / Intercept (kit 44): crimson 0xDB0B10, 1000ms
    REQUIRE(parsed.count(44) == 1);
    CHECK(parsed.at(44).durationSec == Approx(1.0f));
}
