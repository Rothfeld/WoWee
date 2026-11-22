#include "rendering/visual_mesh_pick.hpp"
#include "rendering/m2_track_sampler.hpp"
// What a click acts on, ranked as build 8606 ranks it: the nearest surface
// the ray reaches (CM2Scene 0x70b2b0), with dead units that have nothing left
// to do considered only when nothing else is there (CGUnit_C 0x60e870).
// See docs/unit-size-rules.md.
#include <catch_amalgamated.hpp>

#include <cstdint>

#include "ui/scene_pick.hpp"
#include "game/corpse_harvesting.hpp"
#include "game/game_object_interaction.hpp"
#include "game/game_object_reach.hpp"
#include "game/faction_hostility.hpp"
#include "game/resource_tracking.hpp"

using wowee::ui::ScenePick;

namespace {
constexpr uint64_t kVendor = 0xF130000001u;
constexpr uint64_t kStall  = 0xF110000002u;
constexpr uint64_t kCorpse = 0xF130000004u;
}  // namespace

TEST_CASE("the nearest surface takes the click, whatever it belongs to") {
    ScenePick pick;
    pick.closestGuid = kVendor;
    pick.closestT = 8.0f;
    pick.closestInteractive = true;
    CHECK(pick.resolve() == kVendor);
}

TEST_CASE("scenery in front of a unit takes the click and uses nothing") {
    // A GENERIC object nearer than the vendor: the cursor is on the object,
    // which a click does not use, so nothing is selected.
    ScenePick pick;
    pick.closestGuid = kStall;
    pick.closestT = 6.0f;
    pick.closestInteractive = false;
    pick.namedObjectGuid = kStall;
    CHECK(pick.resolve() == 0u);
    CHECK(pick.namedObjectGuid == kStall);
}

TEST_CASE("a corpse with nothing to do is picked only when nothing else is there") {
    ScenePick pick;
    pick.deferredGuid = kCorpse;
    pick.deferredT = 5.0f;
    CHECK(pick.resolve() == kCorpse);

    // Anything else under the cursor takes it, nearer or not.
    pick.closestGuid = kVendor;
    pick.closestT = 9.0f;
    pick.closestInteractive = true;
    CHECK(pick.resolve() == kVendor);
}

TEST_CASE("an empty pick resolves to nothing") {
    ScenePick pick;
    CHECK(pick.resolve() == 0u);
}

TEST_CASE("local player 3D picking is enabled only during spell or item unit targeting", "[scene-pick][hoverself]") {
    using wowee::ui::isScenePickCandidate;
    using wowee::ui::shouldPickLocalPlayer;
    constexpr uint64_t kPlayer = 0x0000000000000042u;
    constexpr uint64_t kOther  = 0xF130000001u;

    // Normal mode: local player is excluded so clicks pass through to world.
    CHECK_FALSE(shouldPickLocalPlayer(false));
    CHECK_FALSE(isScenePickCandidate(kPlayer, kPlayer, shouldPickLocalPlayer(false)));
    CHECK(isScenePickCandidate(kOther, kPlayer, shouldPickLocalPlayer(false)));
    CHECK_FALSE(isScenePickCandidate(0u, kPlayer, shouldPickLocalPlayer(false)));

    // Unit targeting mode (SpellIsTargeting / item unit target): local player is pickable.
    CHECK(shouldPickLocalPlayer(true));
    CHECK(isScenePickCandidate(kPlayer, kPlayer, shouldPickLocalPlayer(true)));
    CHECK(isScenePickCandidate(kOther, kPlayer, shouldPickLocalPlayer(true)));
    CHECK_FALSE(isScenePickCandidate(0u, kPlayer, shouldPickLocalPlayer(true)));
}

#include "ui/world_cursor.hpp"
#include <string>

TEST_CASE("World cursors distinguish unit actions and corpse priority") {
    using wowee::ui::unitCursorPath;
    using wowee::game::QuestGiverStatus;
    using namespace wowee::game;
    CHECK(std::string(unitCursorPath(true, 0, 2, NPC_FLAG_ANY_VENDOR, QuestGiverStatus::AVAILABLE)) == "Interface\\Cursor\\Attack.blp");
    CHECK(std::string(unitCursorPath(true, 0, 4, 0, QuestGiverStatus::NONE)) == "Interface\\Cursor\\Attack.blp");
    CHECK(std::string(unitCursorPath(false, UNIT_DYNFLAG_LOOTABLE, 2, NPC_FLAG_ANY_VENDOR, QuestGiverStatus::AVAILABLE)) == "Interface\\Cursor\\Pickup.blp");
    CHECK(std::string(unitCursorPath(false, 0, 2, 0, QuestGiverStatus::NONE)) == "Interface\\Cursor\\Point.blp");
    CHECK(std::string(unitCursorPath(true, 0, 5, NPC_FLAG_AUCTIONEER | NPC_FLAG_GOSSIP, QuestGiverStatus::NONE)) == "Interface\\Cursor\\Buy.blp");
    CHECK(std::string(unitCursorPath(true, 0, 5, NPC_FLAG_BANKER | NPC_FLAG_GOSSIP, QuestGiverStatus::NONE)) == "Interface\\Cursor\\Buy.blp");
    CHECK(std::string(unitCursorPath(true, 0, 5, NPC_FLAG_GOSSIP, QuestGiverStatus::NONE)) == "Interface\\Cursor\\Speak.blp");
    CHECK(std::string(unitCursorPath(true, 0, 5, NPC_FLAG_ANY_VENDOR, QuestGiverStatus::NONE)) == "Interface\\Cursor\\Pickup.blp");
    CHECK(std::string(unitCursorPath(true, 0, 5, NPC_FLAG_GOSSIP | NPC_FLAG_TRAINER | NPC_FLAG_ANY_VENDOR, QuestGiverStatus::NONE)) == "Interface\\Cursor\\Trainer.blp");
    CHECK(std::string(unitCursorPath(true, 0, 5, NPC_FLAG_VENDOR | NPC_FLAG_REPAIR, QuestGiverStatus::NONE)) == "Interface\\Cursor\\RepairNPC.blp");
    CHECK(std::string(unitCursorPath(true, 0, 5, NPC_FLAG_ANY_VENDOR, QuestGiverStatus::AVAILABLE)) == "Interface\\Cursor\\Quest.blp");
    CHECK(std::string(unitCursorPath(true, 0, 5, 0, QuestGiverStatus::REWARD)) == "Interface\\Cursor\\QuestTurnIn.blp");
    CHECK(std::string(unitCursorPath(true, 0, 5, NPC_FLAG_QUESTGIVER, QuestGiverStatus::UNAVAILABLE)) == "Interface\\Cursor\\Point.blp");
    CHECK(std::string(unitCursorPath(true, 0, 5, NPC_FLAG_AUCTIONEER | NPC_FLAG_GOSSIP, QuestGiverStatus::NONE)) == "Interface\\Cursor\\Buy.blp");
    CHECK(std::string(unitCursorPath(true, 0, 5, NPC_FLAG_BANKER | NPC_FLAG_GOSSIP, QuestGiverStatus::NONE)) == "Interface\\Cursor\\Buy.blp");
    CHECK(std::string(unitCursorPath(true, 0, 5, NPC_FLAG_GOSSIP, QuestGiverStatus::UNAVAILABLE)) == "Interface\\Cursor\\Speak.blp");
    CHECK(std::string(unitCursorPath(true, 0, 5, 0, QuestGiverStatus::NONE)) == "Interface\\Cursor\\Point.blp");
    // Build 8606, pair 16: Haughty Modiste (quest giver + vendor) and
    // Yeh'kinya (quest giver + gossip) to a level 70 character whose quests
    // they offer are out-levelled (CHAT), or taken and unfinished.
    CHECK(std::string(unitCursorPath(true, 0, 5, NPC_FLAG_QUESTGIVER | NPC_FLAG_VENDOR, QuestGiverStatus::CHAT)) == "Interface\\Cursor\\Quest.blp");
    CHECK(std::string(unitCursorPath(true, 0, 5, NPC_FLAG_QUESTGIVER | NPC_FLAG_GOSSIP, QuestGiverStatus::CHAT)) == "Interface\\Cursor\\Quest.blp");
    CHECK(std::string(unitCursorPath(true, 0, 5, NPC_FLAG_QUESTGIVER | NPC_FLAG_VENDOR, QuestGiverStatus::INCOMPLETE)) == "Interface\\Cursor\\Quest.blp");
    CHECK(std::string(unitCursorPath(true, 0, 5, NPC_FLAG_QUESTGIVER | NPC_FLAG_VENDOR, QuestGiverStatus::UNAVAILABLE)) == "Interface\\Cursor\\Pickup.blp");
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

TEST_CASE("an invisible WMO collision hull does not hide an object from the mouse", "[go-pick]") {
    // SSC (CoilfangRaid group 10): a collision-only quad (material 0xFF)
    // 2.5 yards in front of the Lady Vashj Bridge Console. Original 2.4.3
    // shows the console's gear and name through it; a drawn wall still hides.
    using wowee::rendering::pickDrawnWmoTriangles;
    using wowee::rendering::kWmoCollisionOnlyMaterial;
    const std::vector<glm::vec3> vertices{
        {-1,-1,2}, {1,-1,2}, {1,1,2}, {-1,1,2},   // hull at z=2
        {-1,-1,0}, {1,-1,0}, {1,1,0}, {-1,1,0}};  // wall at z=0
    const std::vector<uint16_t> indices{0,1,2, 0,2,3, 4,5,6, 4,6,7};
    const std::vector<uint32_t> all{0, 3, 6, 9};
    const glm::vec3 eye{0.2f, 0.1f, 5}, down{0, 0, -1};

    const std::vector<uint8_t> hullInvisible{kWmoCollisionOnlyMaterial, kWmoCollisionOnlyMaterial, 4, 4};
    CHECK(pickDrawnWmoTriangles(vertices, indices, hullInvisible, all, eye, down, 10) == 5);
    // The object at 4 yards stands in front of the wall: nothing hides it.
    CHECK(pickDrawnWmoTriangles(vertices, indices, hullInvisible, all, eye, down, 4) == 4);

    const std::vector<uint8_t> hullDrawn{4, 4, 4, 4};
    CHECK(pickDrawnWmoTriangles(vertices, indices, hullDrawn, all, eye, down, 10) == 3);
    // Only the triangles the spatial grid offers are tested.
    CHECK(pickDrawnWmoTriangles(vertices, indices, hullDrawn, std::vector<uint32_t>{6, 9},
                                eye, down, 10) == 5);
}

TEST_CASE("a door reaches as far as its Stand bounds grown by 5 yards", "[go-reach]") {
    // Shield Generator 185051: display 7265 (coilfang_floor_light.m2, Stand
    // bounds below), size 2.09919. Original 2.4.3 (pair 17) cursor: active to
    // 8.10 yd along local +Y and 8.0 along -X, Unable from 8.15 and 8.25.
    using wowee::game::withinGrownBox;
    const glm::vec3 standMin{-1.451f, -1.464f, -0.709f}, standMax{1.490f, 1.477f, 2.232f};
    constexpr float kScale = 2.09919f, kReach = 5.0f;
    CHECK(withinGrownBox({0, 8.10f, -1.34f}, standMin, standMax, kScale, kReach));
    CHECK_FALSE(withinGrownBox({0, 8.15f, -1.34f}, standMin, standMax, kScale, kReach));
    CHECK(withinGrownBox({-8.0f, 0, -1.34f}, standMin, standMax, kScale, kReach));
    CHECK_FALSE(withinGrownBox({-8.25f, 0, -1.34f}, standMin, standMax, kScale, kReach));
    // The display box (GameObjectDisplayInfo 7265) would refuse 7.5 yd, where
    // the original shows the active gear.
    const glm::vec3 displayMin{-0.805f, -1.093f, -1.354f}, displayMax{1.088f, 1.093f, 1.398f};
    CHECK_FALSE(withinGrownBox({0, 7.5f, -1.34f}, displayMin, displayMax, kScale, kReach));
    CHECK(withinGrownBox({0, 7.5f, -1.34f}, standMin, standMax, kScale, kReach));
}

TEST_CASE("a game object's frame has its left side on its left", "[go-reach]") {
    // Server frame (x, y) = canonical (y, x); server yaw s = canonical yaw + pi/2.
    // A point to the object's server-left, (-sin s, cos s), is local +Y.
    using wowee::game::gameObjectLocalOffset;
    for (const float s : {0.75f, -1.57952f, 3.14159f}) {
        const float c = s - 1.5707963f;
        const glm::vec3 serverLeft{-std::sin(s) * 5, std::cos(s) * 5, 0};
        const glm::vec3 serverFront{std::cos(s) * 3, std::sin(s) * 3, 0};
        const auto left = gameObjectLocalOffset({serverLeft.y, serverLeft.x, 0}, c);
        const auto front = gameObjectLocalOffset({serverFront.y, serverFront.x, 0}, c);
        CHECK(left.x == Catch::Approx(0).margin(1e-4));
        CHECK(left.y == Catch::Approx(5).margin(1e-4));
        CHECK(front.x == Catch::Approx(3).margin(1e-4));
        CHECK(front.y == Catch::Approx(0).margin(1e-4));
    }
    // Zul'Aman's Wooden Door 186306 (server yaw -1.57952, size 0.935587, Stand
    // bounds y -11.996..0.496) runs along its -Y side: a player before its
    // middle is inside its reach.
    const glm::vec3 offsetServer{117.6f - 123.256f, 911.0f - 914.421f, 0};
    const auto local = gameObjectLocalOffset({offsetServer.y, offsetServer.x, 0}, -1.57952f - 1.5707963f);
    CHECK(wowee::game::withinGrownBox(local, {-0.821f, -11.996f, 0.013f}, {0.345f, 0.496f, 9.001f},
                                      0.935587f, 5.0f));
}

TEST_CASE("a game object custom animation index names Custom0-Custom3", "[go-anim]") {
    // Strange Gong 187359 (G_GongTroll01: Stand 0, Custom0 153) gets index 0
    // from Zul'Aman Object Visual 45222 (ACTIVATE_OBJECT misc 1 = ANIMATE_CUSTOM_0).
    using wowee::game::gameObjectCustomAnimation;
    CHECK(gameObjectCustomAnimation(0) == std::optional<uint32_t>{153});
    CHECK(gameObjectCustomAnimation(3) == std::optional<uint32_t>{156});
    CHECK_FALSE(gameObjectCustomAnimation(4).has_value());
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
        QuestGiverStatus::NONE, {}, true, UNIT_FLAG_SKINNABLE)) == "Interface\\Cursor\\Pickup.blp");
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

TEST_CASE("Service cursors turn Unable beyond interaction reach", "[npc-cursor]") {
    using namespace wowee::game;
    using wowee::ui::unitCursorPath;
    const auto far = [](bool alive, uint32_t dyn, int reaction, uint32_t flags, QuestGiverStatus status,
                        std::string_view icon = {}, bool attackable = true) {
        return unitCursorPath(alive, dyn, reaction, flags, status, icon, attackable, 0, false, false, false);
    };
    CHECK(far(true, 0, 5, NPC_FLAG_BANKER | NPC_FLAG_GOSSIP, QuestGiverStatus::NONE) == "Interface\\Cursor\\UnableBuy.blp");
    CHECK(far(true, 0, 5, NPC_FLAG_GOSSIP, QuestGiverStatus::NONE) == "Interface\\Cursor\\UnableSpeak.blp");
    CHECK(far(true, 0, 5, NPC_FLAG_INNKEEPER | NPC_FLAG_QUESTGIVER, QuestGiverStatus::CHAT) ==
          "Interface\\Cursor\\UnableQuest.blp");
    CHECK(far(true, 0, 5, NPC_FLAG_GOSSIP, QuestGiverStatus::NONE, "Directions", false) ==
          "Interface\\Cursor\\UnableDirections.blp");
    // Lootable corpses turn UnablePickup beyond loot reach (General Rajaxx, Gruul, Swamp Jaguar).
    CHECK(far(false, UNIT_DYNFLAG_LOOTABLE, 2, 0, QuestGiverStatus::NONE) ==
          "Interface\\Cursor\\UnablePickup.blp");
    // The sword and the plain pointer have no out-of-reach look here.
    CHECK(far(true, 0, 2, 0, QuestGiverStatus::NONE) == "Interface\\Cursor\\Attack.blp");
    CHECK(far(true, 0, 5, 0x400000, QuestGiverStatus::NONE) == "Interface\\Cursor\\Point.blp");
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

TEST_CASE("Resource tracking matches authored lock skill types", "[resource-tracking]") {
    using wowee::game::tracksResourceLock;
    CHECK(tracksResourceLock(4, 2, 3)); // Find Minerals / Copper Vein lock 38.
    CHECK(tracksResourceLock(2, 2, 2)); // Find Herbs.
    CHECK_FALSE(tracksResourceLock(2, 2, 3));
    CHECK_FALSE(tracksResourceLock(4, 2, 2));
    CHECK_FALSE(tracksResourceLock(0, 2, 3));
    CHECK_FALSE(tracksResourceLock(4, 1, 3)); // Item key ID is not a lock type.
    CHECK_FALSE(tracksResourceLock(4, 2, 0));
    CHECK_FALSE(tracksResourceLock(4, 2, 33));
    using wowee::game::resourceTrackingAvailable;
    CHECK(resourceTrackingAvailable(0x01, 0)); // In use remains tracked.
    CHECK(resourceTrackingAvailable(0x10, 0)); // Interaction disabled is not hidden.
    CHECK_FALSE(resourceTrackingAvailable(0x04, 0));
    CHECK(resourceTrackingAvailable(0x05, 1));
}

TEST_CASE("Large object mesh is nearer than its enclosing sphere exit", "[quest-object]") {
    const wowee::rendering::Ray ray{{0, 0, 5}, {0, 0, -1}};
    float sphereExit = 0;
    REQUIRE(wowee::ui::raySphereIntersect(ray, {0, 0, 0}, 20, sphereExit));
    REQUIRE(sphereExit == 25);
    const std::vector<glm::vec3> mesh{{-5,-5,3}, {5,-5,3}, {0,5,3}};
    const std::vector<uint16_t> indices{0,1,2};
    const float surface = wowee::rendering::pickVisualTriangles(
        mesh, indices, ray.origin, ray.direction, sphereExit + 40);
    CHECK(surface == 2);
    CHECK_FALSE(wowee::ui::terrainOccludesSceneRay(ray, surface,
        [](float, float) { return std::optional<float>{0}; }));
    CHECK(wowee::ui::terrainOccludesSceneRay(ray, sphereExit,
        [](float, float) { return std::optional<float>{0}; }));
}

// FactionTemplate.dbc 2.4.3 rows: id, faction, factionGroup, friendGroup,
// enemyGroup, enemies, friends. A game object is usable only while its
// template regards the player better than unfriendly (CGUnit_C::CanInteract).
TEST_CASE("Game object faction reaction follows the client's template rule", "[quest-object][faction]") {
    using wowee::game::FactionTemplateRow;
    using wowee::game::factionTemplateReaction;
    const FactionTemplateRow dwarf{.id = 3, .faction = 3, .factionGroup = 3, .friendGroup = 2, .enemyGroup = 12};
    const FactionTemplateRow orc{.id = 2, .faction = 2, .factionGroup = 5, .friendGroup = 4, .enemyGroup = 10};
    // Doodad_DarkIronDwarfRune_G01 (BRD, entry 170584): enemy group = players.
    const FactionTemplateRow rune{.id = 114, .faction = 77, .enemyGroup = 1};
    // Adamantite Bound Chest and other treasure: neutral to everyone.
    const FactionTemplateRow treasure{.id = 94, .faction = 77, .friendGroup = 8};
    // Stormwind mailbox.
    const FactionTemplateRow stormwind{.id = 12, .faction = 72, .factionGroup = 2, .friendGroup = 2,
                                       .enemyGroup = 4, .friends = {72, 0, 0, 0}};
    // Arathi Basin "Alliance Banner": Horde template, clicked by the Horde.
    const FactionTemplateRow hordeBanner{.id = 83, .faction = 66, .factionGroup = 4, .friendGroup = 4,
                                         .enemyGroup = 2};
    CHECK(factionTemplateReaction(rune, dwarf) == 2);
    CHECK(factionTemplateReaction(rune, orc) == 2);
    CHECK(factionTemplateReaction(treasure, dwarf) == 4);
    CHECK(factionTemplateReaction(stormwind, dwarf) == 5);
    CHECK(factionTemplateReaction(stormwind, orc) == 2);
    CHECK(factionTemplateReaction(hordeBanner, dwarf) == 2);
    CHECK(factionTemplateReaction(hordeBanner, orc) == 5);
}

// Units use the same rule, unit first, against the player's live template.
// Build 8606 colours Kobold Vermin and Young Wolf yellow for a human; a game
// master (template 35, Player::SetGameMaster) sees a Defias Thug yellow too
// (20261117T163000Z.unit_reaction).
TEST_CASE("Unit reaction follows the template rule against the player's live template", "[faction]") {
    using wowee::game::FactionTemplateRow;
    using wowee::game::factionTemplateReaction;
    const FactionTemplateRow human{.id = 1, .faction = 1, .factionGroup = 3, .friendGroup = 2, .enemyGroup = 12};
    const FactionTemplateRow dwarf{.id = 3, .faction = 3, .factionGroup = 3, .friendGroup = 2, .enemyGroup = 12};
    const FactionTemplateRow gameMaster{.id = 35, .faction = 31, .friendGroup = 1, .friends = {31, 0, 0, 0}};
    const FactionTemplateRow koboldVermin{.id = 25, .faction = 25, .factionGroup = 8, .friends = {25, 0, 0, 0}};
    const FactionTemplateRow youngWolf{.id = 32, .faction = 29, .enemies = {28, 0, 0, 0}};
    const FactionTemplateRow defiasThug{.id = 17, .faction = 15, .factionGroup = 8, .enemyGroup = 1,
                                        .friends = {15, 0, 0, 0}};
    const FactionTemplateRow tamedBattleboar{.id = 152, .faction = 109, .factionGroup = 8, .enemyGroup = 1,
                                             .friends = {109, 111, 0, 0}};
    CHECK(factionTemplateReaction(koboldVermin, human) == 4);
    CHECK(factionTemplateReaction(youngWolf, human) == 4);
    CHECK(factionTemplateReaction(defiasThug, human) == 2);
    CHECK(factionTemplateReaction(defiasThug, gameMaster) == 4);
    CHECK(factionTemplateReaction(tamedBattleboar, dwarf) == 2);
}

#include <cstdlib>
#include <filesystem>
#include <fstream>

TEST_CASE("M2 skin batch flag 0x08 excludes non-pickable light columns while keeping portals and solid bowls", "[go-pick][highlight]") {
    using wowee::rendering::kM2BatchFlagNonPickable;
    using wowee::rendering::m2BatchIsPickable;
    using wowee::rendering::pickSkinnedModel;

    // AuraBlueVeryTall.m2 / AuraPurpleShort.m2: batchFlags = 0x18 (0x10 | 0x08), blendMode = 4.
    CHECK_FALSE(m2BatchIsPickable(0x18, 0, 4, 1.0f, false, true));
    CHECK_FALSE(m2BatchIsPickable(0x18, 0, 4, 1.0f, false, false));

    // UtherShrineLightBeam.m2 / G_HolyLightWell.m2 batch 0 (vertical beam): batchFlags = 0x08.
    CHECK_FALSE(m2BatchIsPickable(kM2BatchFlagNonPickable, 0, 2, 1.0f, false, true));
    CHECK_FALSE(m2BatchIsPickable(kM2BatchFlagNonPickable, 0, 0, 1.0f, false, true));

    // MagePortal_Ironforge.m2: batchFlags = 0x10, blendMode = 4 (additive).
    // Pickable as ownSurface (portal hover/click), ignored as M2 occluder.
    CHECK(m2BatchIsPickable(0x10, 0, 4, 1.0f, false, true));
    CHECK_FALSE(m2BatchIsPickable(0x10, 0, 4, 1.0f, false, false));

    // ElwynnCampfire.m2 / TreasureChest01.m2 / G_HolyLightWell.m2 batch 1 (bowl):
    // batchFlags 0x00 or 0x10, blendMode <= 1: pickable for both ownSurface and occlusion.
    CHECK(m2BatchIsPickable(0x10, 0, 0, 1.0f, false, true));
    CHECK(m2BatchIsPickable(0x10, 0, 0, 1.0f, false, false));
    CHECK(m2BatchIsPickable(0x00, 0, 1, 1.0f, false, true));
    CHECK(m2BatchIsPickable(0x00, 0, 1, 1.0f, false, false));

    // LOD > 0, faded-out batches, and sky star layers are never pickable.
    CHECK_FALSE(m2BatchIsPickable(0x10, 1, 0, 1.0f, false, true));
    CHECK_FALSE(m2BatchIsPickable(0x10, 0, 0, 0.0f, false, true));
    CHECK_FALSE(m2BatchIsPickable(0x10, 0, 0, 1.0f, true, true));

    // Synthetic two-batch model (beam quad at z=4 with flag 0x08 in front of solid bowl quad at z=1 with flag 0x00):
    // ray from z=6 downward ignores the z=4 beam and hits the z=1 bowl at t=5.
    wowee::pipeline::M2Model twoBatch;
    for (const float z : {4.0f, 1.0f}) {
        for (const glm::vec2 xy : {glm::vec2{-1, -1}, glm::vec2{1, -1}, glm::vec2{1, 1}, glm::vec2{-1, 1}}) {
            wowee::pipeline::M2Vertex v{};
            v.position = {xy.x, xy.y, z};
            twoBatch.vertices.push_back(v);
        }
    }
    twoBatch.indices = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
    wowee::pipeline::M2Material addMat{};
    addMat.blendMode = 4;
    wowee::pipeline::M2Material solidMat{};
    solidMat.blendMode = 0;
    twoBatch.materials = {addMat, solidMat};

    wowee::pipeline::M2Batch beamBatch{};
    beamBatch.flags = 0x18;
    beamBatch.materialIndex = 0;
    beamBatch.indexStart = 0;
    beamBatch.indexCount = 6;
    wowee::pipeline::M2Batch bowlBatch{};
    bowlBatch.flags = 0x00;
    bowlBatch.materialIndex = 1;
    bowlBatch.indexStart = 6;
    bowlBatch.indexCount = 6;
    twoBatch.batches = {beamBatch, bowlBatch};

    const float hitT = pickSkinnedModel(
        twoBatch, {},
        [&](const wowee::pipeline::M2Batch& b) {
            return m2BatchIsPickable(b.flags, b.submeshLevel,
                                     twoBatch.materials[b.materialIndex].blendMode, 1.0f, false, true);
        },
        {0.0f, 0.0f, 6.0f}, {0.0f, 0.0f, -1.0f}, 20.0f);
    CHECK(hitT == Catch::Approx(5.0f));
}

TEST_CASE("Shipped TBC pillar-of-light, Lightwell, portal, campfire, and chest M2s match pickability rules", "[go-pick][highlight][assets]") {
    namespace fs = std::filesystem;
    fs::path tbcRoot;
    if (const char* env = std::getenv("WOWEE_TBC_DATA_DIR"); env && *env) {
        tbcRoot = env;
    } else {
        tbcRoot = "/workspace/azerothcore/client/wowee-data/expansions/tbc";
    }
    if (!fs::is_directory(tbcRoot)) return;

    const auto loadM2 = [&](const char* relPath) {
        const fs::path full = tbcRoot / relPath;
        INFO("loading " << full.string());
        REQUIRE(fs::is_regular_file(full));
        std::ifstream in(full, std::ios::binary | std::ios::ate);
        REQUIRE(in.is_open());
        const auto sz = in.tellg();
        std::vector<uint8_t> bytes(static_cast<size_t>(sz));
        in.seekg(0);
        in.read(reinterpret_cast<char*>(bytes.data()), sz);
        return wowee::pipeline::M2Loader::load(bytes);
    };

    const auto countPickable = [](const wowee::pipeline::M2Model& model, bool ownSurface) {
        size_t count = 0;
        for (const auto& b : model.batches) {
            const uint8_t blend = b.materialIndex < model.materials.size()
                                      ? static_cast<uint8_t>(model.materials[b.materialIndex].blendMode)
                                      : 0u;
            if (wowee::rendering::m2BatchIsPickable(b.flags, b.submeshLevel, blend, 1.0f, false, ownSurface))
                ++count;
        }
        return count;
    };

    // Pure light-pillar / aura models: zero pickable batches.
    for (const char* rel : {
             "world/generic/passivedoodads/particleemitters/aurablueverytall.m2",
             "world/generic/passivedoodads/particleemitters/aurapurpleshort.m2",
             "world/nodxt/generic/passivedoodads/volumetriclights/uthershrinelightbeam.m2"}) {
        const auto m = loadM2(rel);
        REQUIRE_FALSE(m.batches.empty());
        CHECK(countPickable(m, true) == 0);
        CHECK(countPickable(m, false) == 0);
    }

    // Lightwell (G_HolyLightWell.m2): batch 0 (vertical beam, flag 0x08) skipped, batch 1 (bowl, flag 0x00) pickable.
    {
        const auto lw = loadM2("world/goober/g_holylightwell.m2");
        REQUIRE(lw.batches.size() == 2);
        CHECK((lw.batches[0].flags & wowee::rendering::kM2BatchFlagNonPickable) != 0);
        CHECK((lw.batches[1].flags & wowee::rendering::kM2BatchFlagNonPickable) == 0);
        CHECK(countPickable(lw, true) == 1);
    }

    // Shattrath portal (MagePortal_Ironforge.m2): pickable for ownSurface, non-occluding.
    {
        const auto portal = loadM2("world/generic/activedoodads/spellportals/mageportal_ironforge.m2");
        REQUIRE(portal.batches.size() == 1);
        CHECK(countPickable(portal, true) == 1);
        CHECK(countPickable(portal, false) == 0);
    }

    // Campfire and chest: pickable for ownSurface and solid occlusion.
    {
        const auto camp = loadM2("world/azeroth/elwynn/passivedoodads/campfire/elwynncampfire.m2");
        CHECK(countPickable(camp, true) >= 1);
        const auto chest = loadM2("world/skillactivated/containers/treasurechest01.m2");
        CHECK(countPickable(chest, true) == 1);
        CHECK(countPickable(chest, false) == 1);
    }

    // Multi-sequence collision-only / hidden doodads whose transparency track
    // is constant 0.0 across every sequence (e.g. Sunwell_BossCollision01/02
    // entries 188523/188524, Coilfang raid bridge parts 184203/184204/184205):
    // opacityVaries must be false, isAlwaysZero must be true, and resolved
    // static batchOpacity must be 0.0 so they are never hoverable/pickable.
    const auto resolvedStaticOpacity = [](const wowee::pipeline::M2Model& model,
                                          const wowee::pipeline::M2Batch& b) {
        float opacity = 1.0f;
        if (b.transparencyIndex < model.textureWeightLookup.size()) {
            const uint16_t wi = model.textureWeightLookup[b.transparencyIndex];
            if (wi < model.textureWeightTracks.size() &&
                !wowee::rendering::m2_track::opacityVaries(model.textureWeightTracks[wi]) &&
                wowee::rendering::m2_track::isAlwaysZero(model.textureWeightTracks[wi])) {
                opacity = 0.0f;
            }
        }
        return opacity;
    };

    for (const char* rel : {
             "world/expansion01/doodads/sunwell/passivedoodads/sunwell/sunwell_bosscollision01.m2",
             "world/expansion01/doodads/sunwell/passivedoodads/sunwell/sunwell_bosscollision02.m2",
             "world/khazmodan/ironforge/passivedoodads/auctionhouse/auctioneercollision.m2"}) {
        const auto m = loadM2(rel);
        REQUIRE_FALSE(m.batches.empty());
        REQUIRE_FALSE(m.textureWeightTracks.empty());
        CHECK_FALSE(wowee::rendering::m2_track::opacityVaries(m.textureWeightTracks[0]));
        CHECK(wowee::rendering::m2_track::isAlwaysZero(m.textureWeightTracks[0]));
        for (const auto& b : m.batches) {
            const uint8_t blend = b.materialIndex < m.materials.size()
                                      ? static_cast<uint8_t>(m.materials[b.materialIndex].blendMode)
                                      : 0u;
            const float op = resolvedStaticOpacity(m, b);
            CHECK(op == 0.0f);
            CHECK_FALSE(wowee::rendering::m2BatchIsPickable(
                b.flags, b.submeshLevel, blend, op, false, true));
            CHECK_FALSE(wowee::rendering::m2BatchIsPickable(
                b.flags, b.submeshLevel, blend, op, false, false));
        }
    }

    // Sunwell_BossForceField (188421): genuinely varies between 1.0 (closed)
    // and 0.0 (open); pickable only when sampled opacity is visible.
    {
        const auto ff = loadM2("world/expansion01/doodads/sunwell/passivedoodads/sunwell/sunwell_bossforcefield.m2");
        REQUIRE_FALSE(ff.batches.empty());
        REQUIRE_FALSE(ff.textureWeightTracks.empty());
        CHECK(wowee::rendering::m2_track::opacityVaries(ff.textureWeightTracks[0]));
        CHECK_FALSE(wowee::rendering::m2_track::isAlwaysZero(ff.textureWeightTracks[0]));
        const auto& b = ff.batches[0];
        const uint8_t blend = static_cast<uint8_t>(ff.materials[b.materialIndex].blendMode);
        CHECK(wowee::rendering::m2BatchIsPickable(b.flags, b.submeshLevel, blend, 1.0f, false, true));
        CHECK_FALSE(wowee::rendering::m2BatchIsPickable(b.flags, b.submeshLevel, blend, 0.0f, false, true));
    }
}

TEST_CASE("M2 opacityVaries distinguishes constant multi-sequence tracks from animated fades", "[go-pick][m2]") {
    using wowee::rendering::m2_track::isAlwaysZero;
    using wowee::rendering::m2_track::opacityVaries;
    wowee::pipeline::M2AnimationTrack track;

    CHECK_FALSE(opacityVaries(track));
    CHECK_FALSE(isAlwaysZero(track));

    // Four sequences all holding 0.0 (Sunwell_BossCollision01 layout): constant, always zero.
    track.sequences.resize(4);
    for (auto& seq : track.sequences) seq.floatValues = {0.0f};
    CHECK_FALSE(opacityVaries(track));
    CHECK(isAlwaysZero(track));

    // Four sequences all holding 1.0: constant, not zero.
    for (auto& seq : track.sequences) seq.floatValues = {1.0f};
    CHECK_FALSE(opacityVaries(track));
    CHECK_FALSE(isAlwaysZero(track));

    // Sequence 0 holds 0.0 while sequence 1 is empty (defaults to 1.0): varies.
    track.sequences.resize(2);
    track.sequences[0].floatValues = {0.0f};
    track.sequences[1].floatValues.clear();
    CHECK(opacityVaries(track));
    CHECK_FALSE(isAlwaysZero(track));

    // Sequence 0 holds 1.0 and sequence 1 fades to 0.0: varies.
    track.sequences[0].floatValues = {1.0f};
    track.sequences[1].floatValues = {0.0f};
    CHECK(opacityVaries(track));
    CHECK_FALSE(isAlwaysZero(track));
}

TEST_CASE("TBC 2.4.3 GameObject selectability matrix covers GO_FLAG and GAMEOBJECT_TYPE combinations", "[go-pick][gameobject]") {
    using wowee::game::gameObjectAffordance;
    using wowee::game::gameObjectInteractionEnabled;
    using wowee::game::ObjectNameTooltip;

    constexpr uint32_t kInUse = 0x01;
    constexpr uint32_t kLocked = 0x02;
    constexpr uint32_t kInteractCond = 0x04;
    constexpr uint32_t kTransport = 0x08;
    constexpr uint32_t kNoInteract = 0x10;
    constexpr uint32_t kNoDespawn = 0x20;
    constexpr uint32_t kDynActivate = 0x01;

    // Visible DOOR (type 0) with GO_FLAG_NODESPAWN (0x20) is interaction-enabled
    // (e.g. Herod's Door 101854, King's Square Gate 175352); BossCollision01/02
    // are excluded at the mesh layer because their M2 opacity is permanently 0.
    CHECK(gameObjectInteractionEnabled(kNoDespawn, 0));
    CHECK(gameObjectInteractionEnabled(kNoDespawn | kLocked, 0));
    CHECK(gameObjectAffordance(0, 0, 86, gameObjectInteractionEnabled(kNoDespawn, 0)).hoverable());

    // GO_FLAG_NO_INTERACT (0x10) disables hover/interaction on any type
    // (e.g. Sunwell Orb of the Blue Flight 187869, type 10, flags 16).
    CHECK_FALSE(gameObjectInteractionEnabled(kNoInteract, 0));
    CHECK_FALSE(gameObjectInteractionEnabled(kNoInteract | kNoDespawn, kDynActivate));
    CHECK_FALSE(gameObjectAffordance(10, 0, 0, gameObjectInteractionEnabled(kNoInteract, 0)).hoverable());

    // GO_FLAG_IN_USE (0x01) disables interaction while animating.
    CHECK_FALSE(gameObjectInteractionEnabled(kInUse, kDynActivate));

    // GO_FLAG_INTERACT_COND (0x04) requires dynamic ACTIVATE (0x01)
    // (e.g. Broken Trap 179485, type 2, flags 4).
    CHECK_FALSE(gameObjectInteractionEnabled(kInteractCond, 0));
    CHECK(gameObjectInteractionEnabled(kInteractCond, kDynActivate));
    CHECK_FALSE(gameObjectAffordance(2, 43, 6392, gameObjectInteractionEnabled(kInteractCond, 0)).hoverable());
    CHECK(gameObjectAffordance(2, 43, 6392, gameObjectInteractionEnabled(kInteractCond, kDynActivate)).hoverable());

    // GENERIC (type 5): Collision PC Size (188215, data0=0, data1=0) and
    // White Smoke Emitter (1605, data0=0, data1=0) are never hoverable;
    // Monument to the Fallen (187116, data0=0, data1=1) shows default tooltip.
    CHECK_FALSE(gameObjectAffordance(5, 0, 0, true).hoverable());
    const auto monument = gameObjectAffordance(5, 0, 1, true);
    CHECK(monument.hoverable());
    CHECK_FALSE(monument.use);
    CHECK(monument.tooltip == ObjectNameTooltip::Default);

    // TRAP (type 6), SPELL_FOCUS (type 8), TRANSPORT (type 11 with 0x08|0x20),
    // DUNGEON_DIFFICULTY (type 31): never hoverable with tooltip/cursor.
    CHECK_FALSE(gameObjectAffordance(6, 0, 0, true).hoverable());
    CHECK_FALSE(gameObjectAffordance(8, 0, 0, true).hoverable());
    CHECK_FALSE(gameObjectAffordance(11, 0, 0, gameObjectInteractionEnabled(kTransport | kNoDespawn, 0)).hoverable());
    CHECK_FALSE(gameObjectAffordance(31, 0, 1, true).hoverable());

    // Standard interactables (CHEST 3, CHAIR 7, TEXT 9, MAILBOX 19, MEETINGSTONE 23) stay hoverable.
    for (const uint32_t t : {3u, 7u, 9u, 19u, 23u}) {
        const auto a = gameObjectAffordance(t, 0, 0, true);
        CHECK(a.hoverable());
        CHECK(a.use);
    }
}

TEST_CASE("Lootable corpse Pass-2 fallback sphere covers faded VoidGod/Entropius corpse extent", "[scene-pick][corpse]") {
    using wowee::ui::corpseFallbackPickSphere;
    using wowee::ui::raySphereIntersect;
    using wowee::rendering::Ray;

    // Small/normal corpse (e.g. HumanMale, selection radius 0.84, boundRadius 0.9) floors at 1.0 yd.
    const auto smallCorpse = corpseFallbackPickSphere(glm::vec3(0.0f), 0.84f, 0.9f);
    CHECK(smallCorpse.radius == Catch::Approx(1.0f));
    CHECK(smallCorpse.center.z == Catch::Approx(0.3f));

    // Entropius (entry 25840, display 23428, Creature\VoidGod\VoidGod.m2 at scale 6.0):
    // Body batches 0..8 fade to alpha 0 in Death, leaving only submesh 9 (0.9 yd orb at z=3.97).
    // Selection radius = 5.03 yd, scaled M2 header boundingRadius = 1.1037 * 6 = 6.6222 yd.
    const auto entropius = corpseFallbackPickSphere(glm::vec3(0.0f), 5.03f, 6.6222f);
    CHECK(entropius.radius == Catch::Approx(6.6222f));
    CHECK(entropius.center.z == Catch::Approx(6.6222f * 0.3f));

    // Rays from 22.8 yd away matching Native 2.4.3 hover points on Mirage Raceway:
    // center smoke (z=1.5), ground smoke (z=0.5), left smoke (y=4.5, z=0.6), above orb (z=6.5) hit;
    // outside left edge (y=7.6, z=0.9) misses.
    const glm::vec3 camOrigin(-22.8f, 0.0f, 4.5f);
    auto testRay = [&](const glm::vec3& target) {
        float t = 0.0f;
        const Ray ray{camOrigin, glm::normalize(target - camOrigin)};
        return raySphereIntersect(ray, entropius.center, entropius.radius, t);
    };
    CHECK(testRay(glm::vec3(0.0f, 0.0f, 1.5f)));   // mid-smoke below floating orb
    CHECK(testRay(glm::vec3(0.0f, 0.0f, 0.5f)));   // ground smoke
    CHECK(testRay(glm::vec3(0.0f, 4.5f, 0.6f)));   // left edge of smoke
    CHECK(testRay(glm::vec3(0.0f, 0.0f, 6.5f)));   // above floating orb
    CHECK_FALSE(testRay(glm::vec3(0.0f, 8.0f, 0.9f))); // outside corpse extent
}
