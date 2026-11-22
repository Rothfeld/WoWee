#include <catch_amalgamated.hpp>

#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"
#include "rendering/spell_launch_origin.hpp"
#include "rendering/spell_missile_motion.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using wowee::pipeline::DBCFile;
using wowee::pipeline::DBCLayout;
using namespace wowee::rendering;

namespace {

std::vector<uint8_t> readBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.good()) return {};
    return {std::istreambuf_iterator<char>(in), {}};
}

std::filesystem::path resolveTbcDbcFile(const std::string& filename) {
    std::vector<std::filesystem::path> dirs;
    if (const char* env = std::getenv("WOWEE_TBC_DATA_DIR"); env && *env) {
        dirs.push_back(std::filesystem::path(env) / "dbfilesclient");
    }
    dirs.emplace_back("/workspace/azerothcore/client/wowee-data/expansions/tbc/dbfilesclient");
    dirs.emplace_back("/workspace/azerothcore/server/data/dbc");

    std::string wantLower = filename;
    for (char& c : wantLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    for (const auto& dir : dirs) {
        if (!std::filesystem::is_directory(dir)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            std::string nameLower = entry.path().filename().string();
            for (char& c : nameLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (nameLower == wantLower) return entry.path();
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

TEST_CASE("MissileBasis and applyMissileMotion map polar and Cartesian offsets in Z-up world space",
          "[missile][motion]") {
    const glm::vec3 start(0.0f, 0.0f, 0.0f);
    const glm::vec3 end(10.0f, 0.0f, 0.0f);

    const MissileBasis basis = computeMissileBasis(end - start);
    CHECK(basis.forward.x == Catch::Approx(1.0f));
    CHECK(basis.right.y == Catch::Approx(-1.0f));
    CHECK(basis.up.z == Catch::Approx(1.0f));

    // transAngle = 0 degrees points straight up (+Z).
    MissileMotionOutput upArc;
    upArc.transAngle = 0.0f;
    upArc.transMag = 7.5f;
    const glm::vec3 midUp = applyMissileMotion(start, end, 0.5f, upArc);
    CHECK(midUp.x == Catch::Approx(5.0f));
    CHECK(midUp.y == Catch::Approx(0.0f));
    CHECK(midUp.z == Catch::Approx(7.5f));

    // transAngle = 90 degrees points right (-Y for +X chord).
    MissileMotionOutput rightArc;
    rightArc.transAngle = 90.0f;
    rightArc.transMag = 2.0f;
    const glm::vec3 midRight = applyMissileMotion(start, end, 0.5f, rightArc);
    CHECK(midRight.x == Catch::Approx(5.0f));
    CHECK(midRight.y == Catch::Approx(-2.0f));
    CHECK(midRight.z == Catch::Approx(0.0f).margin(1e-5f));

    // Default parabolic fallback for MissilePathType == 2 (peak = 0.15 * startDistance).
    const MissileMotionOutput para0 = defaultParabolicMotion(20.0f, 0.0f);
    const MissileMotionOutput paraMid = defaultParabolicMotion(20.0f, 0.5f);
    const MissileMotionOutput para1 = defaultParabolicMotion(20.0f, 1.0f);
    CHECK(para0.transMag == Catch::Approx(0.0f));
    CHECK(paraMid.transMag == Catch::Approx(3.0f));
    CHECK(para1.transMag == Catch::Approx(0.0f));
}

TEST_CASE("TBC 2.4.3 dbc_layouts.json SpellVisual declares MissilePathType and MissileMotion for Supremus Volcano",
          "[missile][motion][dbc]") {
    DBCLayout layout;
    const std::string tbcLayoutPath =
        (std::filesystem::path(WOWEE_SOURCE_DIR) / "Data/expansions/tbc/dbc_layouts.json").string();
    REQUIRE(layout.loadFromJson(tbcLayoutPath));

    const auto* svLayout = layout.getLayout("SpellVisual");
    REQUIRE(svLayout != nullptr);
    CHECK(svLayout->field("MissileModel") == 7u);
    CHECK(svLayout->field("MissilePathType") == 8u);
    CHECK(svLayout->field("MissileDestinationAttachment") == 9u);
    CHECK(svLayout->field("MissileAttachment") == 15u);
    CHECK(svLayout->field("MissileMotion") == 20u);

    DBCFile svDbc;
    if (!loadTbcDbc("SpellVisual.dbc", svDbc)) {
        SKIP("TBC 2.4.3 SpellVisual.dbc not available locally");
    }
    REQUIRE(svDbc.getFieldCount() == 25u);

    // Supremus Volcano Volcanic Geyser (spell 40118 -> SpellVisual 9466):
    // MissileModel = 3532 (Fireball_Blue_Missile_High.m2), MissileMotion = 40 (Fountain).
    const int32_t rec9466 = svDbc.findRecordById(9466);
    REQUIRE(rec9466 >= 0);
    CHECK(svDbc.getUInt32(static_cast<uint32_t>(rec9466), svLayout->field("MissileModel")) == 3532u);
    CHECK(svDbc.getUInt32(static_cast<uint32_t>(rec9466), svLayout->field("MissilePathType")) == 0u);
    CHECK(svDbc.getUInt32(static_cast<uint32_t>(rec9466), svLayout->field("MissileMotion")) == 40u);
    CHECK(svDbc.getUInt32(static_cast<uint32_t>(rec9466), svLayout->field("ImpactAreaKit")) == 8045u);
}

TEST_CASE("All 83 shipped TBC 2.4.3 SpellMissileMotion.dbc scripts compile and evaluate cleanly",
          "[missile][motion][dbc]") {
    DBCFile smmDbc;
    if (!loadTbcDbc("SpellMissileMotion.dbc", smmDbc)) {
        SKIP("TBC 2.4.3 SpellMissileMotion.dbc not available locally");
    }

    SpellMissileMotionTable table;
    REQUIRE(table.loadFromDbc(smmDbc));
    REQUIRE(table.count() == smmDbc.getRecordCount());
    REQUIRE(table.count() == 83u);

    // Every shipped script must execute cleanly across the entire flight without NaN/Inf.
    for (uint32_t i = 0; i < smmDbc.getRecordCount(); ++i) {
        const uint32_t id = smmDbc.getUInt32(i, 0);
        INFO("SpellMissileMotion id=" << id << " name=" << smmDbc.getString(i, 1));
        REQUIRE(table.hasMotion(id));
        const uint32_t count = table.missileCount(id);
        CHECK(count >= 1u);

        for (float p : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
            MissileMotionInput in;
            in.progress = p;
            in.time = p * 1.5f;
            in.startDistance = 12.0f;
            in.totalDistance = 12.0f;
            in.missileIndex = 0;
            in.missileCount = static_cast<int>(count);
            MissileMotionOutput out;
            REQUIRE(table.evaluate(id, in, out));
            CHECK(std::isfinite(out.transFront));
            CHECK(std::isfinite(out.transRight));
            CHECK(std::isfinite(out.transUp));
            CHECK(std::isfinite(out.transAngle));
            CHECK(std::isfinite(out.transMag));
            CHECK(std::isfinite(out.scale));
            CHECK(out.scale > 0.0f);
            CHECK(out.speedScalar > 0.0f);
        }
    }

    // Supremus Volcano Fountain (id=40): launches steeply upward from crater and lands at end.
    {
        const glm::vec3 start(720.0f, 740.0f, 67.0f);
        const glm::vec3 end(730.0f, 740.0f, 67.0f);  // 10 yd east
        MissileMotionInput in;
        in.startDistance = 10.0f;
        in.totalDistance = 10.0f;

        MissileMotionOutput out0, out25, out50, out100;
        in.progress = 0.0f;
        REQUIRE(table.evaluate(40, in, out0));
        const glm::vec3 p0 = applyMissileMotion(start, end, 0.0f, out0);
        CHECK(p0.x == Catch::Approx(start.x));
        CHECK(p0.z == Catch::Approx(start.z));

        in.progress = 0.25f;
        REQUIRE(table.evaluate(40, in, out25));
        const glm::vec3 p25 = applyMissileMotion(start, end, 0.25f, out25);
        CHECK(p25.x - start.x == Catch::Approx(0.625f));
        CHECK(p25.z - start.z == Catch::Approx(5.625f));

        in.progress = 0.5f;
        REQUIRE(table.evaluate(40, in, out50));
        const glm::vec3 p50 = applyMissileMotion(start, end, 0.5f, out50);
        CHECK(p50.x - start.x == Catch::Approx(2.5f));
        CHECK(p50.z - start.z == Catch::Approx(7.5f));

        in.progress = 1.0f;
        REQUIRE(table.evaluate(40, in, out100));
        const glm::vec3 p100 = applyMissileMotion(start, end, 1.0f, out100);
        CHECK(p100.x == Catch::Approx(end.x));
        CHECK(p100.z == Catch::Approx(end.z));
    }

    // Orbital Strike (id=37): descends vertically onto destination from 2 * totalDistance above.
    {
        const glm::vec3 start(0.0f, 0.0f, 0.0f);
        const glm::vec3 end(10.0f, 0.0f, 0.0f);
        MissileMotionInput in;
        in.progress = 0.0f;
        in.startDistance = 10.0f;
        in.totalDistance = 10.0f;
        MissileMotionOutput out;
        REQUIRE(table.evaluate(37, in, out));
        const glm::vec3 p0 = applyMissileMotion(start, end, 0.0f, out);
        CHECK(p0.x == Catch::Approx(10.0f));
        CHECK(p0.z == Catch::Approx(20.0f));
    }

    // Spiral Vortex (id=19): 7 missiles evenly spaced around the flight axis.
    CHECK(table.missileCount(19) == 7u);
    {
        MissileMotionInput in;
        in.progress = 0.5f;
        in.time = 0.0f;
        in.startDistance = 20.0f;
        in.totalDistance = 20.0f;
        in.missileCount = 7;
        MissileMotionOutput a, b;
        in.missileIndex = 0;
        REQUIRE(table.evaluate(19, in, a));
        in.missileIndex = 1;
        REQUIRE(table.evaluate(19, in, b));
        CHECK(b.transAngle - a.transAngle == Catch::Approx(360.0f / 7.0f));
    }
}

TEST_CASE("TBC 2.4.3 MissileDestinationAttachment and ground-lob parabolic arc parity",
          "[missile][motion][dbc]") {
    // M2 attachment mapping for SpellVisual.MissileDestinationAttachment (col 9).
    CHECK(missileDestinationAttachmentToM2Id(1u) == kAttachChestBloodFrontId);
    CHECK(missileDestinationAttachmentToM2Id(2u) == kAttachBaseId);
    CHECK(missileDestinationAttachmentToM2Id(0u) == kAttachHeadId);
    CHECK(missileDestinationAttachmentToM2Id(3u) == kAttachSpellLeftHandId);
    CHECK(missileDestinationAttachmentToM2Id(4u) == kAttachSpellRightHandId);
    CHECK(missileDestinationAttachmentToM2Id(5u) == kAttachBreathId);

    // Native 2.4.3 (build 8606) verified truth table for MissileMotion == 0:
    // - Telonicus Bomb 37036 (visual 148) & Rough Copper Bomb 4064 (visual 3819):
    //   pathType=0, destAtt=2, ground destination -> parabolic arc to feet.
    CHECK(missileUsesDefaultParabolicArc(/*pathType=*/0u, /*dbcDestinationAttachment=*/2u,
                                         /*isGroundDestination=*/true));
    // - Fiery Burst 13900 (516), Cannon Fire 17501 (612), Throw Syndicate Bomb 3720 (67):
    //   pathType=0, destAtt=1, ground destination -> straight line.
    CHECK_FALSE(missileUsesDefaultParabolicArc(0u, 1u, true));
    // - Incendiary Shot 30481 (7756): pathType=0, destAtt=2, unit-targeted -> straight to feet.
    CHECK_FALSE(missileUsesDefaultParabolicArc(0u, 2u, false));
    // - Fireball 133 (67): pathType=0, destAtt=1, unit-targeted -> straight to chest.
    CHECK_FALSE(missileUsesDefaultParabolicArc(0u, 1u, false));
    // - Incinerate 29722 (7675): pathType=1, destAtt=2, unit-targeted -> ground-hugging, not default parabola.
    CHECK_FALSE(missileUsesDefaultParabolicArc(1u, 2u, false));
    // - Throw Liquid Fire 23970 (7139), Mortar (7339), Boulder (7340): pathType=2 -> always parabolic arc.
    CHECK(missileUsesDefaultParabolicArc(2u, 2u, false));
    CHECK(missileUsesDefaultParabolicArc(2u, 1u, false));
    CHECK(missileUsesDefaultParabolicArc(2u, 2u, true));

    DBCFile svDbc;
    if (!loadTbcDbc("SpellVisual.dbc", svDbc)) {
        SKIP("TBC 2.4.3 SpellVisual.dbc not available locally");
    }
    REQUIRE(svDbc.getFieldCount() == 25u);

    // Telonicus Bomb (spell 37036 -> SpellVisual 148) and Rough Copper Bomb (spell 4064 -> SpellVisual 3819)
    // both carry pathType=0, destAtt=2, motion=0 with ImpactAreaKit != 0.
    for (const uint32_t vid : {148u, 3819u}) {
        const int32_t rec = svDbc.findRecordById(vid);
        REQUIRE(rec >= 0);
        const uint32_t r = static_cast<uint32_t>(rec);
        CHECK(svDbc.getUInt32(r, 8u) == 0u);   // MissilePathType
        CHECK(svDbc.getUInt32(r, 9u) == 2u);   // MissileDestinationAttachment (base/feet)
        CHECK(svDbc.getUInt32(r, 20u) == 0u);  // MissileMotion
        CHECK(svDbc.getUInt32(r, 23u) != 0u);  // ImpactAreaKit
    }
}
