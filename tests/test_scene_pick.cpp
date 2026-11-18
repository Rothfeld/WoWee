#include "rendering/visual_mesh_pick.hpp"
// What a click acts on, ranked as build 8606 ranks it: the nearest surface
// the ray reaches (CM2Scene 0x70b2b0), with dead units that have nothing left
// to do considered only when nothing else is there (CGUnit_C 0x60e870).
// See docs/unit-size-rules.md.
#include <catch_amalgamated.hpp>

#include <cstdint>

#include "ui/scene_pick.hpp"
#include "game/corpse_harvesting.hpp"
#include "game/game_object_interaction.hpp"
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
    CHECK(far(true, 0, 5, NPC_FLAG_INNKEEPER | NPC_FLAG_QUESTGIVER, QuestGiverStatus::LOW_LEVEL_AVAILABLE) ==
          "Interface\\Cursor\\UnableQuest.blp");
    CHECK(far(true, 0, 5, NPC_FLAG_GOSSIP, QuestGiverStatus::NONE, "Directions", false) ==
          "Interface\\Cursor\\UnableDirections.blp");
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
