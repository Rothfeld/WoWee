// Unit size as build 8606 measures it: the selection circle radius
// (CGUnit_C 0x60e560 over CreatureModelData's GeoBox) and scene picking of a
// model (CM2Scene 0x70b2b0: animation-bounds sphere, then drawn triangles).
// Values are worked from client/wowee-data TBC CreatureModelData rows; see
// docs/unit-size-rules.md.
#include <catch_amalgamated.hpp>

#include "core/unit_size.hpp"
#include "rendering/visual_mesh_pick.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <vector>

using wowee::core::ModelGeoBox;
using wowee::core::mountedGeoBox;
using wowee::core::selectionCircleRadius;

TEST_CASE("Selection circle radius follows the 8606 GeoBox rule", "[unit-size]") {
    // CreatureModelData 49, HumanMale: GeoBox 0.91 x 1.08 yards across.
    const ModelGeoBox human{{-0.4543f, -0.5394f, 0.0f}, {0.4543f, 0.5394f, 2.01f}};
    const float halfDiagonal = std::hypot(0.9086f, 1.0788f) * 0.5f;
    CHECK(selectionCircleRadius(human, 1.0f) == Catch::Approx(std::sqrt(halfDiagonal)));
    CHECK(selectionCircleRadius(human, 1.0f) == Catch::Approx(0.841f).margin(0.002f));

    // CreatureModelData 69, ElementalEarth (the BRD Magma Elemental's model):
    // 3.69 x 4.48 yards, radius 1.70 at scale 1 - whatever its animations'
    // bounds say.
    const ModelGeoBox earth{{-1.845f, -2.24f, 0.0f}, {1.845f, 2.24f, 3.46f}};
    CHECK(selectionCircleRadius(earth, 1.0f) == Catch::Approx(1.703f).margin(0.002f));

    // Scale goes in before the square root.
    CHECK(selectionCircleRadius(human, 4.0f) == Catch::Approx(2.0f * std::sqrt(halfDiagonal)));

    // Past 5 yards of scaled half-diagonal it grows by 0.06 per square yard.
    const ModelGeoBox wide{{-6.0f, -8.0f, 0.0f}, {6.0f, 8.0f, 1.0f}};  // half diagonal 10
    CHECK(selectionCircleRadius(wide, 1.0f) == Catch::Approx(std::sqrt(10.0f) + 25.0f * 0.06f));
    // And never past 10.
    CHECK(selectionCircleRadius(wide, 4.0f) == 10.0f);

    // A box with no horizontal extent gets a flat 1.2, unscaled.
    const ModelGeoBox flat{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 2.0f}};
    CHECK(selectionCircleRadius(flat, 3.0f) == 1.2f);
}

TEST_CASE("A rider's box rides at the mount height joined with the mount's", "[unit-size]") {
    const ModelGeoBox rider{{-0.5f, -0.5f, 0.0f}, {0.5f, 0.5f, 2.0f}};
    const ModelGeoBox mount{{-1.5f, -0.6f, 0.0f}, {1.5f, 0.6f, 1.8f}};
    const ModelGeoBox joined = mountedGeoBox(rider, mount, 2.0f);
    // Lifted so its middle (1 yard up) sits at the mount height: +1.
    CHECK(joined.min == glm::vec3(-1.5f, -0.6f, 0.0f));
    CHECK(joined.max == glm::vec3(1.5f, 0.6f, 3.0f));
    // The mount widens the circle; the rider's height does not enter it.
    CHECK(selectionCircleRadius(joined, 1.0f) > selectionCircleRadius(rider, 1.0f));
}

namespace {

// One quad in each of two batches, the second one moved by bone 1.
wowee::pipeline::M2Model twoQuadModel() {
    wowee::pipeline::M2Model model{};
    auto vertex = [](float x, float y, uint8_t bone) {
        wowee::pipeline::M2Vertex v{};
        v.position = {x, y, 0.0f};
        v.boneWeights[0] = 255;
        v.boneIndices[0] = bone;
        return v;
    };
    model.vertices = {vertex(-1, -1, 0), vertex(1, -1, 0), vertex(1, 1, 0), vertex(-1, 1, 0),
                      vertex(-1, -1, 1), vertex(1, -1, 1), vertex(1, 1, 1), vertex(-1, 1, 1)};
    model.indices = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
    wowee::pipeline::M2Batch first{};
    first.indexStart = 0;
    first.indexCount = 6;
    wowee::pipeline::M2Batch second = first;
    second.indexStart = 6;
    second.submeshId = 1;
    model.batches = {first, second};
    return model;
}

}  // namespace

TEST_CASE("Units are picked by their drawn triangles at the current pose", "[unit-size]") {
    using wowee::rendering::pickSkinnedModel;
    const auto model = twoQuadModel();
    // Bone 1 carries the second quad 5 yards along x.
    const std::vector<glm::mat4> bones{glm::mat4(1.0f),
        glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, 0.0f, 0.0f))};
    auto all = [](const wowee::pipeline::M2Batch&) { return true; };
    const glm::vec3 down(0.0f, 0.0f, -1.0f);
    // The posed quad is hit where it is drawn, not where the bind pose has it.
    CHECK(pickSkinnedModel(model, bones, all, {5.0f, 0.0f, 3.0f}, down, 100.0f) == 3.0f);
    // Empty space between the two parts is not part of the unit.
    CHECK(pickSkinnedModel(model, bones, all, {2.5f, 0.0f, 3.0f}, down, 100.0f) == 100.0f);
    // A batch that is not drawn is not clicked.
    auto firstOnly = [](const wowee::pipeline::M2Batch& b) { return b.submeshId == 0; };
    CHECK(pickSkinnedModel(model, bones, firstOnly, {5.0f, 0.0f, 3.0f}, down, 100.0f) == 100.0f);
    CHECK(pickSkinnedModel(model, bones, firstOnly, {0.0f, 0.0f, 3.0f}, down, 100.0f) == 3.0f);
}

TEST_CASE("The pick sphere is the animation's, else the header vertex box", "[unit-size]") {
    using wowee::rendering::modelPickSphere;
    auto model = twoQuadModel();
    model.vertexBoxMin = {-1.0f, -1.0f, 0.0f};
    model.vertexBoxMax = {1.0f, 1.0f, 2.0f};
    model.vertexRadius = 1.5f;
    wowee::pipeline::M2Sequence stand{};
    stand.boundMin = {-4.0f, -4.0f, 0.0f};
    stand.boundMax = {4.0f, 4.0f, 6.0f};
    stand.boundRadius = 6.0f;
    wowee::pipeline::M2Sequence unbounded = stand;
    unbounded.boundRadius = 0.0f;
    model.sequences = {stand, unbounded};

    const auto animated = modelPickSphere(model, 0);
    CHECK(animated.center == glm::vec3(0.0f, 0.0f, 3.0f));
    CHECK(animated.radius == 6.0f);
    const auto fallback = modelPickSphere(model, 1);
    CHECK(fallback.center == glm::vec3(0.0f, 0.0f, 1.0f));
    CHECK(fallback.radius == 1.5f);
    CHECK(modelPickSphere(model, -1).radius == 1.5f);
}
