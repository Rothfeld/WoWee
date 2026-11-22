#include <catch_amalgamated.hpp>

#include "rendering/m2_transparent_sort.hpp"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using wowee::rendering::M2TransparentBatchKey;
using wowee::rendering::m2CharacterFlushBeforeBatch;
using wowee::rendering::m2TransparentBatchBefore;

namespace {

struct TaggedBatch {
    std::string label;
    M2TransparentBatchKey key;
};

} // namespace

TEST_CASE("M2 transparent batch sort orders by priorityPlane before viewDepth", "[m2][transparent_sort]") {
    std::vector<TaggedBatch> batches = {
        {"beam_prio3_far",   {3, -50.0f}},
        {"pool_prio0_near",  {0, -18.0f}},
        {"under_prio_neg",   {-1, -1.0f}},
        {"beam_prio2_mid",   {2, -30.0f}},
        {"pool_prio0_far",   {0, -22.0f}},
    };

    std::stable_sort(batches.begin(), batches.end(),
                     [](const TaggedBatch& a, const TaggedBatch& b) {
                         return m2TransparentBatchBefore(a.key, b.key);
                     });

    REQUIRE(batches[0].label == "under_prio_neg");
    REQUIRE(batches[1].label == "pool_prio0_far");
    REQUIRE(batches[2].label == "pool_prio0_near");
    REQUIRE(batches[3].label == "beam_prio2_mid");
    REQUIRE(batches[4].label == "beam_prio3_far");
}

TEST_CASE("Sunwell pool sorts before Sunwell_BeamFX batches when looking down", "[m2][transparent_sort]") {
    // SPELLS\SUNWELL_GROUNDFX.m2 sits at z=28.26 with submesh sortCenter z=-0.28 -> world z=27.92.
    // spells\Sunwell_BeamFX.m2 sits at z=27.69 (scale 2) with submesh sortCenters z=+0.43..+136.93
    // -> world z=28.55..301.55, plus priorityPlane 2 & 3 on its vertical beam billboards.
    // Looking down at -20 deg from (1680, 628, 33): view.z = -dot(P - cam, forward).
    const float pitch = -20.0f * 3.14159265f / 180.0f;
    const float fx = std::cos(pitch);
    const float fz = std::sin(pitch);
    auto viewZ = [&](float x, float z) {
        return -((x - 1680.0f) * fx + (z - 33.0f) * fz);
    };

    std::vector<TaggedBatch> batches = {
        {"beam_ground_glow", {0, viewZ(1698.94f, 28.55f)}},
        {"beam_tower",       {0, viewZ(1698.94f, 301.55f)}},
        {"beam_up_p2",       {2, viewZ(1698.94f, 45.99f)}},
        {"beam_up_p3",       {3, viewZ(1698.94f, 199.51f)}},
        {"groundfx_alpha",   {0, viewZ(1698.90f, 27.92f)}},
    };

    std::stable_sort(batches.begin(), batches.end(),
                     [](const TaggedBatch& a, const TaggedBatch& b) {
                         return m2TransparentBatchBefore(a.key, b.key);
                     });

    // Alpha-blended Sunwell pool must draw first so additive Sunwell_BeamFX layers
    // saturate over it instead of the pool alpha-blending yellow over the white glow.
    REQUIRE(batches[0].label == "groundfx_alpha");
    REQUIRE(batches[3].label == "beam_up_p2");
    REQUIRE(batches[4].label == "beam_up_p3");
}

TEST_CASE("Blended characters interleave on priority plane 0 and flush before positive planes", "[m2][transparent_sort]") {
    float flushDepth = 0.0f;
    REQUIRE_FALSE(m2CharacterFlushBeforeBatch({-1, -5.0f}, flushDepth));

    REQUIRE(m2CharacterFlushBeforeBatch({0, -14.5f}, flushDepth));
    REQUIRE(flushDepth == Catch::Approx(-14.5f));

    REQUIRE(m2CharacterFlushBeforeBatch({2, -40.0f}, flushDepth));
    REQUIRE(std::isinf(flushDepth));
    REQUIRE(flushDepth > 0.0f);
}
