// M2 struct layout and field tests (header-only, no loader source)
#include <catch_amalgamated.hpp>
#include "pipeline/m2_loader.hpp"
#include "rendering/m2_model_classifier.hpp"
#include "rendering/m2_renderer_internal.h"
#include <cstring>

using namespace wowee::pipeline;

TEST_CASE("an M2 batch without an authored material is not assumed one-sided", "[m2]") {
    const wowee::rendering::M2ModelGPU::BatchGPU batch;
    CHECK_FALSE(batch.hasAuthoredMaterial);
}

TEST_CASE("Animated glow centers follow their skinned bones", "[m2][lights]") {
    wowee::rendering::M2Instance instance;
    instance.modelMatrix = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f));
    instance.boneMatrices = {
        glm::mat4(1.0f),
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 2.0f))
    };

    wowee::rendering::M2ModelGPU::BatchGPU batch;
    // Average of two rigid vertices: (0,0,0) on bone 0 and (2,0,0)
    // on bone 1. Moving bone 1 upward by two moves their center upward by one.
    batch.lightBoneAnchors.push_back({0, glm::vec4(0.0f, 0.0f, 0.0f, 0.5f)});
    batch.lightBoneAnchors.push_back({1, glm::vec4(1.0f, 0.0f, 0.0f, 0.5f)});

    const glm::vec3 center = wowee::rendering::animatedBatchWorldCenter(instance, batch);
    CHECK(center.x == Catch::Approx(11.0f));
    CHECK(center.y == Catch::Approx(0.0f));
    CHECK(center.z == Catch::Approx(1.0f));
}

TEST_CASE("Hanging lantern light pools project from suspension to animated tip", "[m2][lights]") {
    wowee::rendering::M2Instance instance;
    instance.modelMatrix = glm::mat4(1.0f);
    instance.scale = 1.0f;
    instance.boneMatrices = {
        glm::mat4(1.0f),
        glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 1.0f))
    };

    wowee::rendering::M2ModelGPU::BatchGPU batch;
    batch.lightBoneAnchors.push_back({1, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)});
    batch.lightSuspensionBone = 0;
    batch.lightSuspensionPoint = glm::vec3(0.0f, 0.0f, 2.0f);

    CHECK(wowee::rendering::animatedBatchWorldCenter(instance, batch).x ==
          Catch::Approx(1.0f));
    const glm::vec3 pool = wowee::rendering::animatedBatchLightWorldCenter(instance, batch);
    CHECK(pool.x == Catch::Approx(4.0f));
    CHECK(pool.y == Catch::Approx(0.0f));
    CHECK(pool.z == Catch::Approx(1.0f));
}

TEST_CASE("Foliage asset paths never block player movement", "[m2][collision]") {
    const auto grass = wowee::rendering::classifyM2Model(
        "World\\NoDXT\\Detail\\ElwynnGrass01.m2",
        glm::vec3(-1.0f), glm::vec3(1.0f), 100, 0);
    REQUIRE(grass.collisionNoBlock);

    const auto herb = wowee::rendering::classifyM2Model(
        "World\\SkillActivated\\TradeskillNodes\\Bush_Magebloom01.m2",
        glm::vec3(-1.0f), glm::vec3(1.0f), 100, 0);
    REQUIRE(herb.collisionNoBlock);
}

TEST_CASE("Lighthouse beams are classified for distant bone updates", "[m2][animation]") {
    const auto beam = wowee::rendering::classifyM2Model(
        "World\\Generic\\Human\\Passive Doodads\\Stormwind\\Stormwind_LighthouseBeam_01.m2",
        glm::vec3(-1.0f), glm::vec3(1.0f), 100, 0);
    REQUIRE(beam.isLightBeam);
    REQUIRE_FALSE(beam.disableAnimation);
}

TEST_CASE("Ship machinery is classified for distant bone updates", "[m2][animation][transport]") {
    const auto sails = wowee::rendering::classifyM2Model(
        "World\\Generic\\PassiveDoodads\\Ships\\ShipAnimation\\TransportShip_Sails.m2",
        glm::vec3(-40.0f), glm::vec3(40.0f), 161, 0);
    const auto paddle = wowee::rendering::classifyM2Model(
        "World\\Generic\\PassiveDoodads\\Ships\\ShipAnimation\\PaddleWheel\\Icebreaker_Paddlewheel.m2",
        glm::vec3(-14.0f), glm::vec3(14.0f), 2676, 4);
    REQUIRE(sails.isTransportDoodad);
    REQUIRE(paddle.isTransportDoodad);
}

TEST_CASE("Steam Tonk vehicles are not steam VFX", "[m2][classification]") {
    // The TBC/Turtle SteamTonk overlay is a tiny proxy that slips under the
    // low-poly vertex gate; combined with a smoke emitter it was mis-classified
    // as an additive spell effect (glowing translucent). A "tonk"/"tank" name
    // must never be treated as steam VFX regardless of geometry.
    const auto tonkLow = wowee::rendering::classifyM2Model(
        "Creature\\SteamTonk\\SteamTonk.m2",
        glm::vec3(-1.0f), glm::vec3(1.0f), 23, 2);
    REQUIRE_FALSE(tonkLow.isSpellEffect);
    const auto tonkHi = wowee::rendering::classifyM2Model(
        "Creature\\SteamTonk\\SteamTonk.m2",
        glm::vec3(-1.0f), glm::vec3(1.0f), 606, 1);
    REQUIRE_FALSE(tonkHi.isSpellEffect);

    // A genuine low-poly steam emitter is still a spell effect.
    const auto steam = wowee::rendering::classifyM2Model(
        "Spells\\Steam.m2",
        glm::vec3(-1.0f), glm::vec3(1.0f), 40, 2);
    REQUIRE(steam.isSpellEffect);
}

TEST_CASE("M2Sequence fields are default-initialized", "[m2]") {
    M2Sequence seq{};
    REQUIRE(seq.id == 0);
    REQUIRE(seq.duration == 0);
    REQUIRE(seq.movingSpeed == 0.0f);
    REQUIRE(seq.flags == 0);
    REQUIRE(seq.blendTime == 0);
    REQUIRE(seq.boundRadius == 0.0f);
}

TEST_CASE("M2AnimationTrack hasData", "[m2]") {
    M2AnimationTrack track;
    REQUIRE_FALSE(track.hasData());

    track.sequences.push_back({});
    REQUIRE(track.hasData());
}

TEST_CASE("M2AnimationTrack default interpolation", "[m2]") {
    M2AnimationTrack track;
    REQUIRE(track.interpolationType == 0);
    REQUIRE(track.globalSequence == -1);
}

TEST_CASE("M2Bone parent defaults to root", "[m2]") {
    M2Bone bone{};
    bone.parentBone = -1;
    REQUIRE(bone.parentBone == -1);
    REQUIRE(bone.keyBoneId == 0);
}

TEST_CASE("M2Vertex layout", "[m2]") {
    M2Vertex vert{};
    vert.position = glm::vec3(1.0f, 2.0f, 3.0f);
    vert.boneWeights[0] = 255;
    vert.boneWeights[1] = 0;
    vert.boneWeights[2] = 0;
    vert.boneWeights[3] = 0;
    vert.boneIndices[0] = 5;
    vert.normal = glm::vec3(0.0f, 1.0f, 0.0f);
    vert.texCoords[0] = glm::vec2(0.5f, 0.5f);

    REQUIRE(vert.position.x == 1.0f);
    REQUIRE(vert.boneWeights[0] == 255);
    REQUIRE(vert.boneIndices[0] == 5);
    REQUIRE(vert.normal.y == 1.0f);
    REQUIRE(vert.texCoords[0].x == 0.5f);
}

TEST_CASE("M2Texture stores filename", "[m2]") {
    M2Texture tex{};
    tex.type = 1;
    tex.filename = "Creature\\Hogger\\Hogger.blp";
    REQUIRE(tex.filename == "Creature\\Hogger\\Hogger.blp");
}

TEST_CASE("M2Batch submesh fields", "[m2]") {
    M2Batch batch{};
    batch.skinSectionIndex = 3;
    batch.textureCount = 2;
    batch.indexStart = 100;
    batch.indexCount = 300;
    batch.vertexStart = 0;
    batch.vertexCount = 150;
    batch.submeshId = 0;
    batch.submeshLevel = 0;

    REQUIRE(batch.skinSectionIndex == 3);
    REQUIRE(batch.textureCount == 2);
    REQUIRE(batch.indexCount == 300);
    REQUIRE(batch.vertexCount == 150);
}

TEST_CASE("M2Material blend modes", "[m2]") {
    M2Material mat{};
    mat.flags = 0;
    mat.blendMode = 2;  // Alpha blend
    REQUIRE(mat.blendMode == 2);

    mat.blendMode = 0;  // Opaque
    REQUIRE(mat.blendMode == 0);
}

TEST_CASE("M2Model isValid", "[m2]") {
    M2Model model{};
    REQUIRE_FALSE(model.isValid()); // no vertices or indices

    model.vertices.push_back({});
    REQUIRE_FALSE(model.isValid()); // vertices but no indices

    model.indices.push_back(0);
    REQUIRE(model.isValid()); // both present
}

TEST_CASE("M2Model bounding box", "[m2]") {
    M2Model model{};
    model.boundMin = glm::vec3(-1.0f, -2.0f, -3.0f);
    model.boundMax = glm::vec3(1.0f, 2.0f, 3.0f);
    model.boundRadius = 5.0f;

    glm::vec3 center = (model.boundMin + model.boundMax) * 0.5f;
    REQUIRE(center.x == Catch::Approx(0.0f));
    REQUIRE(center.y == Catch::Approx(0.0f));
    REQUIRE(center.z == Catch::Approx(0.0f));
}

TEST_CASE("M2ParticleEmitter defaults", "[m2]") {
    M2ParticleEmitter emitter{};
    emitter.textureRows = 1;
    emitter.textureCols = 1;
    emitter.enabled = true;
    REQUIRE(emitter.textureRows == 1);
    REQUIRE(emitter.textureCols == 1);
    REQUIRE(emitter.enabled);
}

TEST_CASE("M2RibbonEmitter defaults", "[m2]") {
    M2RibbonEmitter ribbon{};
    REQUIRE(ribbon.edgesPerSecond == Catch::Approx(15.0f));
    REQUIRE(ribbon.edgeLifetime == Catch::Approx(0.5f));
    REQUIRE(ribbon.gravity == Catch::Approx(0.0f));
}

TEST_CASE("M2Attachment position", "[m2]") {
    M2Attachment att{};
    att.id = 1;  // Right hand
    att.bone = 42;
    att.position = glm::vec3(0.1f, 0.2f, 0.3f);

    REQUIRE(att.id == 1);
    REQUIRE(att.bone == 42);
    REQUIRE(att.position.z == Catch::Approx(0.3f));
}

TEST_CASE("M2Model collections", "[m2]") {
    M2Model model{};

    // Bones
    model.bones.push_back({});
    model.bones[0].parentBone = -1;
    model.bones[0].pivot = glm::vec3(0, 0, 0);

    // Sequences
    model.sequences.push_back({});
    model.sequences[0].id = 0; // Stand
    model.sequences[0].duration = 1000;

    // Textures
    model.textures.push_back({});
    model.textures[0].type = 0;
    model.textures[0].filename = "test.blp";

    REQUIRE(model.bones.size() == 1);
    REQUIRE(model.sequences.size() == 1);
    REQUIRE(model.textures.size() == 1);
    REQUIRE(model.sequences[0].duration == 1000);
}

// darkirondwarfrune_g.m2 (BRD Chamber of Enchantment, GO 170584): vertex box
// -1.75,-1.76,-1.30 .. 1.76,1.76,1.36, no collision triangles, 20 emitters.
// Build 8606 walks over it; WoWee used to stand the player on its box.
TEST_CASE("models without authored collision triangles never block", "[m2][collision]") {
    using wowee::rendering::m2_internal::m2BlocksMovement;
    wowee::rendering::M2ModelGPU rune;
    rune.name = "World\\Generic\\DarkIronDwarf\\Active Doodads\\Runes\\DarkIronDwarfRune_G.m2";
    rune.boundMin = {-1.753f, -1.762f, -1.303f};
    rune.boundMax = {1.758f, 1.762f, 1.363f};
    // Whatever a name or size classifier concludes about it.
    rune.collisionSmallSolidProp = true;
    rune.collisionSteppedLowPlatform = true;
    CHECK_FALSE(m2BlocksMovement(rune));
    wowee::rendering::M2ModelGPU chest;
    chest.collision.triCount = 52;  // treasurechest01.m2: 156 indices, 28 vertices
    CHECK(m2BlocksMovement(chest));
    chest.collisionNoBlock = true;  // a guess does not discard authored geometry
    CHECK(m2BlocksMovement(chest));
    // A name-derived effect class does not discard authored triangles:
    // GnomeStructuralSpotLight02 ("spotlight") authors 56 of them.
    chest.isSpellEffect = true;
    CHECK(m2BlocksMovement(chest));
    // A model spawned as a spell visual does not block.
    chest.isRuntimeSpellVisual = true;
    CHECK_FALSE(m2BlocksMovement(chest));
}

TEST_CASE("authored collision bounds contain the trunk beyond guessed tree radius", "[m2][collision]") {
    wowee::rendering::M2ModelGPU model;
    model.boundMin = {-10.0f, -10.0f, -1.0f};
    model.boundMax = {10.0f, 10.0f, 35.0f};
    model.collisionTreeTrunk = true;
    model.collision.triCount = 16;
    model.collision.boundsMin = {-2.4f, -3.1f, -0.9f};
    model.collision.boundsMax = {3.9f, 3.2f, 34.7f};
    glm::vec3 lo, hi;
    wowee::rendering::m2_internal::getTightCollisionBounds(model, lo, hi);
    CHECK(lo == model.collision.boundsMin);
    CHECK(hi == model.collision.boundsMax);
    // Classifier guesses must not alter the authored collider's broad phase.
    model.collisionTreeTrunk = false;
    model.collisionSmallSolidProp = true;
    wowee::rendering::m2_internal::getTightCollisionBounds(model, lo, hi);
    CHECK(lo == model.collision.boundsMin);
    CHECK(hi == model.collision.boundsMax);
}

TEST_CASE("trunk overlap recovery requires an enclosed horizontal section", "[m2][collision]") {
    wowee::rendering::M2ModelGPU::CollisionMesh mesh;
    mesh.vertices = {{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
                     {-1,-1,3},{1,-1,3},{1,1,3},{-1,1,3}};
    mesh.indices = {0,1,5,0,5,4,1,2,6,1,6,5,2,3,7,2,7,6,3,0,4,3,4,7};
    using wowee::rendering::m2_internal::insideTrunkCollision;
    CHECK(insideTrunkCollision(mesh, {0,0,1}));
    CHECK_FALSE(insideTrunkCollision(mesh, {2,0,1}));
    CHECK_FALSE(insideTrunkCollision(mesh, {0,0,4}));
    mesh.indices.resize(6);
    CHECK_FALSE(insideTrunkCollision(mesh, {0,0,1}));
}

TEST_CASE("Z billboard faces the viewer while staying upright", "[m2][billboard]") {
    wowee::rendering::M2ModelGPU model;
    model.bones.resize(1);
    model.bones[0].flags = 0x240;
    model.bones[0].parentBone = -1;
    model.bones[0].pivot = glm::vec3(0);
    wowee::rendering::M2Instance instance;
    instance.invModelMatrix = glm::mat4(1);
    for (const auto& camera : {glm::vec3(5,0,3), glm::vec3(0,5,-3),
                               glm::vec3(-5,0,8), glm::vec3(0,-5,0)}) {
        wowee::rendering::m2_internal::computeBoneMatrices(model, instance, &camera);
        const auto& pose = instance.boneMatrices[0];
        const auto toward = glm::normalize(glm::vec3(camera.x, camera.y, 0));
        CHECK(glm::dot(glm::vec3(pose[0]), toward) == Catch::Approx(1));
        CHECK(glm::vec3(pose[2]).z == Catch::Approx(1));
    }
}

TEST_CASE("child of a billboard bone stays on the far side from the camera", "[m2][billboard-child]") {
    // Helm_Robe_RaidWarlock_D_01_BEF.m2: smoke emitter bone 4 sits 0.43 yd
    // along -X of spherical billboard bone 1. Under an attached model's
    // placement (yawed, scaled) the smoke stays behind the pivot as seen
    // from any camera position.
    const glm::vec3 pivot(0.005f, 0.0f, -0.582f);
    const glm::vec3 child(-0.428f, 0.0f, -0.563f);
    const glm::mat4 placement = glm::scale(glm::rotate(glm::translate(glm::mat4(1), glm::vec3(10, 20, 5)),
                                                       1.1f, glm::vec3(0, 0, 1)), glm::vec3(2.6f));
    const glm::mat4 inverse = glm::inverse(placement);
    for (const auto& cameraWorld : {glm::vec3(30, 20, 9), glm::vec3(10, 40, 7), glm::vec3(-5, 0, 2)}) {
        const glm::vec3 cameraModel(inverse * glm::vec4(cameraWorld, 1));
        const glm::mat4 bone = wowee::rendering::billboardBoneMatrix(
            wowee::rendering::kM2BoneSphericalBillboard, pivot, glm::vec3(0), glm::vec3(1), glm::mat4(1), cameraModel);
        const glm::vec3 pivotWorld(placement * glm::vec4(pivot, 1));
        const glm::vec3 childWorld(placement * bone * glm::vec4(child, 1));
        const glm::vec3 toCamera = glm::normalize(cameraWorld - pivotWorld);
        CHECK(glm::dot(childWorld - pivotWorld, toCamera) == Catch::Approx(-0.433f * 2.6f).epsilon(0.02));
    }
}

TEST_CASE("authored bone scale preserves hidden phases and mirrored axes", "[m2][bone-scale]") {
    wowee::rendering::M2ModelGPU model;
    model.bones.resize(1);
    auto& bone = model.bones[0];
    bone.parentBone = -1;
    bone.pivot = glm::vec3(0.207966f, 0, 0.720042f);
    bone.scale.interpolationType = 1;
    // detectstealth_state_head.m2 birth sequence: hidden for first 200ms,
    // then expands into its first visible key over 100ms.
    bone.scale.sequences.push_back({3333, {3333,3533,3633},
        {{0,0,0},{0,0,0},{1.109565f,0.522139f,1}}});
    wowee::rendering::M2Instance instance;
    instance.currentSequenceIndex = 0;
    instance.invModelMatrix = glm::mat4(1);
    const glm::vec3 camera(5,0,3);
    for (uint32_t flags : {0u, 0x208u, 0x240u}) {
        bone.flags = flags;
        instance.animTime = 100;
        wowee::rendering::m2_internal::computeBoneMatrices(model, instance, &camera);
        CHECK(glm::vec3(instance.boneMatrices[0] * glm::vec4(2,3,4,1)) == bone.pivot);
        for (int column=0; column<4; ++column)
            for (int row=0; row<4; ++row)
                CHECK(std::isfinite(instance.boneMatrices[0][column][row]));
        instance.animTime = 250;
        wowee::rendering::m2_internal::computeBoneMatrices(model, instance, &camera);
        CHECK(glm::length(glm::vec3(instance.boneMatrices[0][0])) == Catch::Approx(1.109565f * .5f));
        CHECK(glm::length(glm::vec3(instance.boneMatrices[0][1])) == Catch::Approx(.522139f * .5f));
    }
    bone.flags = 0;
    bone.scale.sequences[0].vec3Values.assign(3, glm::vec3(-1,0,0.0001f));
    wowee::rendering::m2_internal::computeBoneMatrices(model, instance, &camera);
    CHECK(instance.boneMatrices[0][0][0] == Catch::Approx(-1));
    CHECK(instance.boneMatrices[0][1][1] == 0);
    CHECK(instance.boneMatrices[0][2][2] == Catch::Approx(.0001f));
}

TEST_CASE("state effects play Stand intro once before authored Hold", "[m2][state-effect]") {
    wowee::rendering::M2ModelGPU model;
    model.sequences.resize(3);
    model.sequences[0].id = 0;
    model.sequences[0].duration = 700;
    model.sequences[1].id = 158;
    model.sequences[1].duration = 2933;
    model.sequences[2].id = 159;
    model.sequences[2].duration = 1100;
    wowee::rendering::M2Instance instance;
    instance.animTime = 430;
    wowee::rendering::m2_internal::startStateEffect(model, instance);
    CHECK(instance.animTime == 0);
    CHECK(instance.currentSequenceIndex == 0);
    CHECK(instance.animDuration == 700);
    CHECK(instance.idleSequenceIndex == 1);
    CHECK(instance.playingVariation);
    CHECK(instance.stateEffectAnimation);
    // Effects without a Hold sequence retain their authored Stand loop.
    model.sequences.resize(1);
    wowee::rendering::m2_internal::startStateEffect(model, instance);
    CHECK(instance.idleSequenceIndex == 0);
    CHECK_FALSE(instance.playingVariation);
    model.sequences[0].id = 158;
    wowee::rendering::m2_internal::startStateEffect(model, instance);
    CHECK(instance.currentSequenceIndex == 0);
    CHECK_FALSE(instance.playingVariation);
}

TEST_CASE("batch opacity follows instance clocks and animation sequence", "[m2][material-opacity]") {
    using namespace wowee::rendering;
    M2ModelGPU model;
    model.globalSequenceDurations = {1000};
    M2ModelGPU::BatchGPU batch;
    M2Instance instance;
    instance.currentSequenceIndex = 0;
    CHECK(m2_internal::animatedBatchOpacity(model, batch, instance) == 1.0f);
    auto& alpha = batch.animatedColorAlpha;
    alpha.interpolationType = 1;
    alpha.sequences.resize(2);
    alpha.sequences[0].timestamps = {0, 500, 1000};
    alpha.sequences[0].floatValues = {0, .4f, 0};
    alpha.sequences[1].timestamps = {0};
    alpha.sequences[1].floatValues = {.7f};
    CHECK(m2_internal::opacityVaries(alpha));
    CHECK(m2_internal::animatedBatchOpacity(model, batch, instance) == 0.0f);
    instance.animTime = 250;
    CHECK(m2_internal::animatedBatchOpacity(model, batch, instance) == Catch::Approx(.2f));
    instance.currentSequenceIndex = 1;
    CHECK(m2_internal::animatedBatchOpacity(model, batch, instance) == Catch::Approx(.7f));
    auto& weight = batch.animatedTextureWeight;
    weight.globalSequence = 0;
    weight.interpolationType = 1;
    weight.sequences.resize(1);
    weight.sequences[0].timestamps = {0, 500, 1000};
    weight.sequences[0].floatValues = {0, 1, 0};
    instance.globalSequenceTime = 1250;
    CHECK(m2_internal::animatedBatchOpacity(model, batch, instance) == Catch::Approx(.35f));
    auto second = instance;
    second.globalSequenceTime = 1500;
    CHECK(m2_internal::animatedBatchOpacity(model, batch, second) == Catch::Approx(.7f));
    CHECK(m2_internal::animatedBatchOpacity(model, batch, instance) == Catch::Approx(.35f));
}

TEST_CASE("inherited billboard rotation preserves orbit and faces camera", "[m2][billboard-parent]") {
    using namespace wowee::rendering;
    M2ModelGPU model;
    model.bones.resize(2);
    auto& parent = model.bones[0];
    parent.parentBone = -1;
    parent.translation.sequences.push_back({0, {0}, {{1,2,3}}});
    parent.scale.sequences.push_back({0, {0}, {{2,3,4}}});
    parent.rotation.sequences.push_back({});
    parent.rotation.sequences[0].timestamps = {0};
    parent.rotation.sequences[0].quatValues = {glm::angleAxis(1.2f, glm::normalize(glm::vec3(1,2,3)))};
    auto& child = model.bones[1];
    child.parentBone = 0;
    child.flags = 8;
    child.pivot = {.2f,.3f,.4f};
    M2Instance instance;
    instance.invModelMatrix = glm::mat4(1);
    instance.currentSequenceIndex = 0;
    for (const auto& scale : {glm::vec3(2,3,4), glm::vec3(-2,3,4), glm::vec3(2,-3,4), glm::vec3(0)}) {
        parent.scale.sequences[0].vec3Values[0] = scale;
        for (const auto& camera : {glm::vec3(5,1,7), glm::vec3(-4,8,2)}) {
            m2_internal::computeBoneMatrices(model, instance, &camera);
            const auto& inherited = instance.boneMatrices[0];
            const auto& result = instance.boneMatrices[1];
            const glm::vec3 pivot(inherited * glm::vec4(child.pivot,1));
            CHECK(glm::length(glm::vec3(result * glm::vec4(child.pivot,1)) - pivot) < .0001f);
            for (int column=0; column<3; ++column)
                CHECK(glm::length(glm::vec3(result[column])) == Catch::Approx(glm::length(glm::vec3(inherited[column]))).margin(.0001f));
            const auto normal = glm::cross(glm::vec3(result[1]), glm::vec3(result[2]));
            if (glm::length(normal) > 0)
                CHECK(glm::dot(glm::normalize(normal), glm::normalize(camera-pivot)) == Catch::Approx(1).margin(.0001f));
            for (int column=0; column<4; ++column)
                for (int row=0; row<4; ++row) CHECK(std::isfinite(result[column][row]));
        }
    }
}
