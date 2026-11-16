#include "rendering/visual_mesh_pick.hpp"
// Which of ScenePick's answers a caller wants.
//
// The struct carries several, and two of them are easy to confuse: closestGuid
// is the nearest *entry point* of anything the ray touched, and resolve() is
// what a click acts on. They disagree exactly where it matters. A game object's
// fallback sphere is 2.5 yards against a unit's 1.8, so a wide object beside an
// NPC is entered first even when the NPC's centre is nearer - which is why
// kMaxGameObjectPickRadius exists at all, and why the vendor cursor, asking
// closestGuid, lost every argument to the stall a merchant stands behind.
//
// Nothing raises when a caller picks the wrong one. It just quietly describes a
// different thing from the one the click will hit.
#include <catch_amalgamated.hpp>

#include <cstdint>

#include "ui/scene_pick.hpp"
#include "game/corpse_harvesting.hpp"
#include "game/game_object_interaction.hpp"

using wowee::ui::ScenePick;

namespace {
constexpr uint64_t kVendor = 0xF130000001u;
constexpr uint64_t kStall  = 0xF110000002u;
constexpr uint64_t kWolf   = 0xF130000003u;
constexpr uint64_t kCorpse = 0xF130000004u;
}  // namespace

TEST_CASE("a unit behind a wider object still wins the click") {
    // The vendor's centre is nearer, but the stall's bigger sphere is entered
    // first - so closestGuid names the stall and resolve() names the vendor.
    ScenePick pick;
    pick.closestGuid = kStall;      // entered at 8.0
    pick.closestT = 8.0f;
    pick.livingUnitGuid = kVendor;
    pick.livingUnitCenterT = 10.0f;
    pick.objectGuid = kStall;
    pick.objectCenterT = 10.5f;

    CHECK(pick.resolve() == kVendor);
    CHECK(pick.closestGuid != pick.resolve());
}

TEST_CASE("an object clearly in front of the unit takes it") {
    // Clearly is two yards. A mailbox between the player and an NPC behind it
    // is what the player means to click.
    ScenePick pick;
    pick.livingUnitGuid = kVendor;
    pick.livingUnitCenterT = 14.0f;
    pick.objectGuid = kStall;
    pick.objectCenterT = 11.0f;
    CHECK(pick.resolve() == kStall);

    // Just inside the bias, and the unit keeps it.
    pick.objectCenterT = 12.5f;
    CHECK(pick.resolve() == kVendor);
}

TEST_CASE("a hostile outranks whatever else is under the cursor") {
    ScenePick pick;
    pick.livingUnitGuid = kVendor;
    pick.livingUnitCenterT = 6.0f;
    pick.hostileUnitGuid = kWolf;
    pick.hostileUnitT = 9.0f;
    CHECK(pick.resolve() == kWolf);
}

TEST_CASE("a corpse is picked only when nothing living is there") {
    ScenePick pick;
    pick.deadUnitGuid = kCorpse;
    pick.deadUnitCenterT = 5.0f;
    CHECK(pick.resolve() == kCorpse);

    // Someone standing on the body takes it back, however the distances fall.
    pick.livingUnitGuid = kVendor;
    pick.livingUnitCenterT = 7.0f;
    CHECK(pick.resolve() == kVendor);
}

TEST_CASE("an empty pick resolves to nothing") {
    ScenePick pick;
    CHECK(pick.resolve() == 0u);
    CHECK(pick.unitGuid() == 0u);
}

#include "ui/world_cursor.hpp"
#include <string>

TEST_CASE("World cursors distinguish unit actions and corpse priority") {
    using wowee::ui::unitCursorPath;
    using wowee::game::QuestGiverStatus;
    using namespace wowee::game;
    CHECK(std::string(unitCursorPath(true, 0, 2, NPC_FLAG_ANY_VENDOR, QuestGiverStatus::AVAILABLE)) == "Interface\\Cursor\\Attack.blp");
    CHECK(std::string(unitCursorPath(true, 0, 4, 0, QuestGiverStatus::NONE)) == "Interface\\Cursor\\Attack.blp");
    CHECK(std::string(unitCursorPath(false, UNIT_DYNFLAG_LOOTABLE, 2, NPC_FLAG_ANY_VENDOR, QuestGiverStatus::AVAILABLE)) == "Interface\\Cursor\\LootAll.blp");
    CHECK(std::string(unitCursorPath(false, 0, 2, 0, QuestGiverStatus::NONE)) == "Interface\\Cursor\\Point.blp");
    CHECK(std::string(unitCursorPath(true, 0, 5, NPC_FLAG_AUCTIONEER | NPC_FLAG_GOSSIP, QuestGiverStatus::NONE)) == "Interface\\Cursor\\Buy.blp");
    CHECK(std::string(unitCursorPath(true, 0, 5, NPC_FLAG_BANKER | NPC_FLAG_GOSSIP, QuestGiverStatus::NONE)) == "Interface\\Cursor\\Buy.blp");
    CHECK(std::string(unitCursorPath(true, 0, 5, NPC_FLAG_GOSSIP, QuestGiverStatus::NONE)) == "Interface\\Cursor\\Speak.blp");
    CHECK(std::string(unitCursorPath(true, 0, 5, NPC_FLAG_ANY_VENDOR, QuestGiverStatus::NONE)) == "Interface\\Cursor\\Buy.blp");
    CHECK(std::string(unitCursorPath(true, 0, 5, NPC_FLAG_VENDOR | NPC_FLAG_REPAIR, QuestGiverStatus::NONE)) == "Interface\\Cursor\\RepairNPC.blp");
    CHECK(std::string(unitCursorPath(true, 0, 5, NPC_FLAG_ANY_VENDOR, QuestGiverStatus::AVAILABLE)) == "Interface\\Cursor\\Quest.blp");
    CHECK(std::string(unitCursorPath(true, 0, 5, 0, QuestGiverStatus::REWARD)) == "Interface\\Cursor\\QuestTurnIn.blp");
    CHECK(std::string(unitCursorPath(true, 0, 5, NPC_FLAG_QUESTGIVER, QuestGiverStatus::UNAVAILABLE)) == "Interface\\Cursor\\Point.blp");
    CHECK(std::string(unitCursorPath(true, 0, 5, NPC_FLAG_AUCTIONEER | NPC_FLAG_GOSSIP, QuestGiverStatus::NONE)) == "Interface\\Cursor\\Buy.blp");
    CHECK(std::string(unitCursorPath(true, 0, 5, NPC_FLAG_BANKER | NPC_FLAG_GOSSIP, QuestGiverStatus::NONE)) == "Interface\\Cursor\\Buy.blp");
    CHECK(std::string(unitCursorPath(true, 0, 5, NPC_FLAG_GOSSIP, QuestGiverStatus::UNAVAILABLE)) == "Interface\\Cursor\\Speak.blp");
    CHECK(std::string(unitCursorPath(true, 0, 5, 0, QuestGiverStatus::NONE)) == "Interface\\Cursor\\Point.blp");
}

TEST_CASE("Terrain holes do not create invisible vertical selection walls") {
    using wowee::ui::terrainOccludesSceneRay;
    const wowee::rendering::Ray ray{{0, 0, 2}, {1, 0, 0}};
    // Anvilmar camera starts in a terrain hole; the room continues beneath
    // outdoor ground. There is no terrain surface along the viewing ray.
    CHECK_FALSE(terrainOccludesSceneRay(ray, 10.0f, [](float x, float) -> std::optional<float> {
        if (x < 3.0f) return std::nullopt;
        return 5.0f;
    }));
    // Entering a hole from above and leaving below also crosses no surface.
    CHECK_FALSE(terrainOccludesSceneRay(ray, 10.0f, [](float x, float) -> std::optional<float> {
        if (x < 2.0f) return 0.0f;
        if (x < 4.0f) return std::nullopt;
        return 5.0f;
    }));
    CHECK(terrainOccludesSceneRay(ray, 10.0f, [](float x, float) -> std::optional<float> {
        return x; // A real hillside crosses the ray.
    }));
    CHECK_FALSE(terrainOccludesSceneRay(ray, 10.0f, [](float, float) -> std::optional<float> {
        return 5.0f; // Camera and target both beneath the surface.
    }));
    // A ray that emerges from underground can still hit a later real hill.
    CHECK(terrainOccludesSceneRay(ray, 10.0f, [](float x, float) -> std::optional<float> {
        return std::abs(x - 5.0f);
    }));
}

TEST_CASE("Visual prop occlusion leaves gaps between solid parts selectable") {
    // Two separate candle faces share one broad bounding box.
    const std::vector<glm::vec3> vertices{
        {-2,-1,0}, {-1,-1,0}, {-1,1,0}, {-2,1,0},
        {1,-1,0}, {2,-1,0}, {2,1,0}, {1,1,0}};
    const std::vector<uint16_t> indices{0,1,2, 0,2,3, 4,5,6, 4,6,7};
    using wowee::rendering::pickVisualTriangles;
    CHECK(pickVisualTriangles(vertices, indices, {0,0,5}, {0,0,-1}, 10) == 10);
    CHECK(pickVisualTriangles(vertices, indices, {-1.5f,0,5}, {0,0,-1}, 10) == 5);
    CHECK(pickVisualTriangles(vertices, indices, {1.5f,0,5}, {0,0,-1}, 10) == 5);
    CHECK(pickVisualTriangles(vertices, indices, {1.5f,0,5}, {0,0,-0.5f}, 20) == 10);
    CHECK(pickVisualTriangles(vertices, indices, {1.5f,0,-5}, {0,0,1}, 10) == 5);
    CHECK(pickVisualTriangles(vertices, indices, {1.5f,0,5}, {0,0,-1}, 3) == 3);
}

TEST_CASE("NPC cursor honors server cursor and attack permission") {
    using namespace wowee::game;
    using wowee::ui::unitCursorPath;
    CHECK(std::string(unitCursorPath(true, 0, 5, NPC_FLAG_GOSSIP,
        QuestGiverStatus::NONE, "Directions", false)) == "Interface\\Cursor\\Directions.blp");
    CHECK(std::string(unitCursorPath(true, 0, 4, NPC_FLAG_GOSSIP,
        QuestGiverStatus::NONE, "", false)) == "Interface\\Cursor\\Speak.blp");
    CHECK(std::string(unitCursorPath(true, 0, 4, 0,
        QuestGiverStatus::NONE, "", true)) == "Interface\\Cursor\\Attack.blp");
}

TEST_CASE("Skinnable corpse retains loot priority", "[gather-cursor]") {
    using namespace wowee::game;
    using wowee::ui::unitCursorPath;
    CHECK(std::string(unitCursorPath(false, 0, 2, 0,
        QuestGiverStatus::NONE, {}, true, UNIT_FLAG_SKINNABLE, true, true)) == "Interface\\Cursor\\Skin.blp");
    CHECK(std::string(unitCursorPath(false, 0, 2, 0,
        QuestGiverStatus::NONE, {}, true, UNIT_FLAG_SKINNABLE, false, true)) == "Interface\\Cursor\\SkinError.blp");
    CHECK(std::string(unitCursorPath(false, 0, 2, 0,
        QuestGiverStatus::NONE, {}, true, UNIT_FLAG_SKINNABLE, false, false)) == "Interface\\Cursor\\Point.blp");
    CHECK(std::string(unitCursorPath(false, UNIT_DYNFLAG_LOOTABLE, 2, 0,
        QuestGiverStatus::NONE, {}, true, UNIT_FLAG_SKINNABLE)) == "Interface\\Cursor\\LootAll.blp");
}

TEST_CASE("Corpse gathering requires profession rank and compatible carried tool", "[gather-cursor]") {
    using namespace wowee::game;
    CHECK(corpseHarvestRequirements(0).skill == 393);
    CHECK(corpseHarvestRequirements(0x100).skill == 182);
    CHECK(corpseHarvestRequirements(0x200).skill == 186);
    CHECK(corpseHarvestRequirements(0x100).toolCategory == 0);
    CHECK(corpseHarvestRequiredRank(9) == 0);
    CHECK(corpseHarvestRequiredRank(12) == 20);
    CHECK(corpseHarvestRequiredRank(20) == 100);
    CHECK(corpseHarvestDifficulty(5, 0, true) == HarvestDifficulty::Unavailable);
    CHECK(corpseHarvestDifficulty(12, 19, true) == HarvestDifficulty::Unavailable);
    CHECK(corpseHarvestDifficulty(12, 20, false) == HarvestDifficulty::Unavailable);
    CHECK(corpseHarvestDifficulty(12, 20, true) == HarvestDifficulty::Orange);
    CHECK(corpseHarvestDifficulty(12, 45, true) == HarvestDifficulty::Yellow);
    CHECK(corpseHarvestDifficulty(12, 70, true) == HarvestDifficulty::Green);
    CHECK(corpseHarvestDifficulty(12, 120, true) == HarvestDifficulty::Gray);
    CHECK(compatibleGatheringTool(1, 1, 1, 1));
    CHECK(compatibleGatheringTool(1, 3, 1, 1));
    CHECK_FALSE(compatibleGatheringTool(21, 1, 1, 1));
    CHECK_FALSE(compatibleGatheringTool(1, 2, 1, 1));
}

TEST_CASE("Innkeeper vendor and battlemaster cursors retain service priority", "[npc-cursor]") {
    using namespace wowee::game;
    using wowee::ui::unitCursorPath;
    CHECK(std::string(unitCursorPath(true, 0, 5, NPC_FLAG_INNKEEPER | NPC_FLAG_VENDOR,
        QuestGiverStatus::NONE, "Buy")) == "Interface\\Cursor\\Innkeeper.blp");
    CHECK(std::string(unitCursorPath(true, 0, 5, NPC_FLAG_BATTLEMASTER,
        QuestGiverStatus::NONE)) == "Interface\\Cursor\\Speak.blp");
}

TEST_CASE("Conditional quest objects require server activation", "[quest-object]") {
    using wowee::game::gameObjectInteractionEnabled;
    CHECK_FALSE(gameObjectInteractionEnabled(0x04, 0)); // Beached Sea Creature, quest inactive
    CHECK(gameObjectInteractionEnabled(0x04, 0x01));    // eligible questgiver
    CHECK(gameObjectInteractionEnabled(0x04, 0x09));    // eligible quest chest
    CHECK_FALSE(gameObjectInteractionEnabled(0x04, 0x08)); // sparkle alone does not activate
    CHECK(gameObjectInteractionEnabled(0, 0));         // ordinary door/chair
    CHECK(gameObjectInteractionEnabled(0x02, 0));      // locked chest still has a key interaction
    CHECK_FALSE(gameObjectInteractionEnabled(0x10, 0x09));
    CHECK_FALSE(gameObjectInteractionEnabled(0x01, 0x01));
}
