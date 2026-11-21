// Which name tooltip a hovered game object shows, from its server template
// (CMaNGOS GameObject.h GAMEOBJECT_TYPE_GENERIC: data0 floatingTooltip,
// data1 highlight). Values are tbcmangos gameobject_template rows.
#include <catch_amalgamated.hpp>

#include "game/game_object_interaction.hpp"

#include <iterator>
#include <string>

using wowee::game::ObjectNameTooltip;
using wowee::game::objectNameTooltip;

TEST_CASE("generic objects label themselves from floatingTooltip and highlight", "[gameobject][tooltip]") {
    // 183865 Guild Master & Tabards: highlight only - ledger #04, the
    // original shows its name in the bottom-right tooltip.
    CHECK(objectNameTooltip(5, 0, 1) == ObjectNameTooltip::Default);
    // 92524 Guild: floating label at the cursor.
    CHECK(objectNameTooltip(5, 1, 1) == ObjectNameTooltip::Floating);
    // Unlabelled decorative scenery (953 rows with neither).
    CHECK(objectNameTooltip(5, 0, 0) == ObjectNameTooltip::None);
    // Chairs show their name in the bottom-right tooltip (Bench 133469 on
    // native 2.4.3), as do chests and other usable types.
    CHECK(objectNameTooltip(7, 1, 1) == ObjectNameTooltip::Default);
    CHECK(objectNameTooltip(3, 0, 0) == ObjectNameTooltip::Default);
}

using wowee::game::GameObjectRuleSource;
using wowee::game::gameObjectAffordance;
using wowee::game::gameObjectTypeRule;
using wowee::game::kGameObjectTypeRules;

TEST_CASE("the type table covers every type 0..34 in order", "[gameobject][type-rule]") {
    REQUIRE(std::size(kGameObjectTypeRules) == 35);
    for (uint32_t t = 0; t < 35; ++t) {
        const auto& r = gameObjectTypeRule(t);
        CHECK(r.type == t);
        // A usable row names its cursor; an unusable one has none.
        CHECK((r.cursor != nullptr) == r.use);
    }
    CHECK_FALSE(gameObjectTypeRule(35).use);
    CHECK(gameObjectTypeRule(35).tooltip == ObjectNameTooltip::None);
}

TEST_CASE("types the server never uses offer neither cursor nor name", "[gameobject][type-rule]") {
    // GameObject::Use has no case and no other handler accepts them.
    for (uint32_t t : {4u, 11u, 12u, 14u, 15u, 20u, 21u, 28u, 29u, 30u, 31u, 32u, 33u}) {
        INFO("type " << t);
        const auto a = gameObjectAffordance(t, 0, 0, true);
        CHECK_FALSE(a.use);
        CHECK_FALSE(a.hoverable());
        CHECK(gameObjectTypeRule(t).source == GameObjectRuleSource::Server);
    }
    // Vator, entry 80023, type 11 flags 0x28 faction 0: nothing natively.
    CHECK_FALSE(gameObjectAffordance(11, 80023 /*unused*/, 0, true).hoverable());
}

TEST_CASE("types the server uses offer a cursor and a click", "[gameobject][type-rule]") {
    struct Row { uint32_t type; const char* cursor; };
    for (const Row r : {Row{0, "Interact"}, Row{1, "Interact"}, Row{2, "Interact"}, Row{3, "Interact"},
                        Row{9, "Inspect"}, Row{10, "Interact"}, Row{13, "Interact"},
                        Row{17, "Interact"}, Row{18, "Interact"}, Row{19, "Mail"}, Row{22, "Interact"},
                        Row{23, "Interact"}, Row{24, "Interact"}, Row{26, "Interact"}}) {
        INFO("type " << r.type);
        const auto a = gameObjectAffordance(r.type, 0, 0, true);
        CHECK(a.use);
        CHECK(std::string(a.cursor) == r.cursor);
        CHECK(gameObjectTypeRule(r.type).source == GameObjectRuleSource::Server);
    }
}

TEST_CASE("every usable GameObject type 0..34 maps to a valid TBC 2.4.3 cursor asset", "[gameobject][type-rule][cursor]") {
    for (uint32_t t = 0; t < 35; ++t) {
        const auto& r = gameObjectTypeRule(t);
        if (!r.use) {
            CHECK(r.cursor == nullptr);
            continue;
        }
        REQUIRE(r.cursor != nullptr);
        const std::string c(r.cursor);
        // Verified against TBC 2.4.3 Interface\Cursor\*.blp (Fishing.blp does not exist).
        CHECK((c == "Interact" || c == "Inspect" || c == "Mail" || c == "Buy"));
    }
    // Native 2.4.3 chair (Bench 133469, Chair 180046): gear cursor + default tooltip.
    const auto chair = gameObjectAffordance(7, 3, 2, true);
    CHECK(chair.use);
    CHECK(std::string(chair.cursor) == "Interact");
    CHECK(chair.tooltip == ObjectNameTooltip::Default);
    CHECK(gameObjectTypeRule(7).source == GameObjectRuleSource::Native);
}

TEST_CASE("generic objects are named from their data and never used", "[gameobject][type-rule]") {
    // HandleGameObjectUseOpcode refuses GENERIC outright.
    CHECK_FALSE(gameObjectAffordance(5, 1, 1, true).use);
    CHECK(gameObjectAffordance(5, 1, 0, true).hoverable());   // floating label
    CHECK(gameObjectAffordance(5, 0, 1, true).hoverable());   // highlight -> tooltip
    CHECK_FALSE(gameObjectAffordance(5, 0, 0, true).hoverable());
}

TEST_CASE("an unavailable object offers nothing whatever its type", "[gameobject][type-rule]") {
    // Flags IN_USE / NO_INTERACT, INTERACT_COND without ACTIVATE, a hostile
    // faction or a ghost player: canInteractWithGameObject is false.
    for (uint32_t t = 0; t < 35; ++t) {
        INFO("type " << t);
        CHECK_FALSE(gameObjectAffordance(t, 1, 1, false).hoverable());
    }
}

TEST_CASE("rows the server does not settle come from native hovers", "[gameobject][type-rule]") {
    for (uint32_t t : {6u, 8u, 16u, 25u, 27u, 34u}) {
        INFO("type " << t);
        CHECK(gameObjectTypeRule(t).source == GameObjectRuleSource::Native);
    }
    CHECK_FALSE(gameObjectAffordance(6, 0, 0, true).hoverable());
    // Teleport-Stone to Hyjal 211050 (MINI_GAME) shows the gear; the Guild
    // Vault 187299 shows Buy.
    CHECK(std::string(gameObjectAffordance(27, 0, 0, true).cursor) == "Interact");
    CHECK(std::string(gameObjectAffordance(34, 0, 0, true).cursor) == "Buy");
    // Fishing hole: named, fished rather than clicked.
    CHECK_FALSE(gameObjectAffordance(25, 0, 0, true).use);
    CHECK(gameObjectAffordance(25, 0, 0, true).hoverable());
    // Spell focus (anvils, forges): kept off the mouse until captured.
    CHECK_FALSE(gameObjectAffordance(8, 0, 0, true).hoverable());
}

#include "game/game_object_cursor.hpp"

using wowee::game::GameObjectCursorFacts;
using wowee::game::gameObjectCursorPath;

// Original 2.4.3 hovers, pair 0: _experiments/20261117T195204Z.pair0_go_cursors.
TEST_CASE("a game object's cursor follows lock, quest marker and reach", "[gameobject][cursor]") {
    GameObjectCursorFacts pod;  // Warpwood Pod 179526: chest, no lock, no loot
    pod.typeCursor = "Interact";
    CHECK(gameObjectCursorPath(pod) == "Interface\\Cursor\\Interact.blp");

    GameObjectCursorFacts herb = pod;  // Peacebloom 1618 without Herbalism
    herb.lockCursor = "GatherHerbs";
    CHECK(gameObjectCursorPath(herb) == "Interface\\Cursor\\GatherHerbs.blp");
    herb.inRange = false;
    CHECK(gameObjectCursorPath(herb) == "Interface\\Cursor\\UnableGatherHerbs.blp");

    // Battered Footlocker 179486: GO_FLAG_LOCKED - the greyed gear, not
    // PickLock, also with Pick Lock known.
    GameObjectCursorFacts footlocker = pod;
    footlocker.lockCursor = "PickLock";
    footlocker.flaggedLocked = true;
    CHECK(gameObjectCursorPath(footlocker) == "Interface\\Cursor\\UnableInteract.blp");

    // Wanted Poster 186426 offers a quest; Skull Pile 185913 has none.
    GameObjectCursorFacts poster = pod;
    poster.questCursor = "Quest";
    CHECK(gameObjectCursorPath(poster) == "Interface\\Cursor\\Quest.blp");
    poster.inRange = false;
    CHECK(gameObjectCursorPath(poster) == "Interface\\Cursor\\UnableQuest.blp");

    // Shattrath city portal out of reach.
    GameObjectCursorFacts portal = pod;
    portal.inRange = false;
    CHECK(gameObjectCursorPath(portal) == "Interface\\Cursor\\UnableInteract.blp");
}

using wowee::game::gameObjectHighlights;

TEST_CASE("TBC 2.4.3 game object hover highlight follows type and template highlight fields", "[gameobject][highlight]") {
    std::uint32_t emptyData[24] = {};

    // Every usable type (rule.use == true) highlights under the pointer.
    for (uint32_t t = 0; t < 35; ++t) {
        const auto& r = gameObjectTypeRule(t);
        if (r.use) {
            INFO("usable type " << t << " (" << r.name << ")");
            CHECK(r.highlight);
            CHECK(gameObjectHighlights(t, emptyData));
        }
    }

    // Real interactable objects: doors, buttons/levers, quest notice boards,
    // chests/herbs/veins, chairs, plaques/books, goobers, summoning altars,
    // mailboxes, spellcasters/portals, meeting stones, PvP flags, mini-game
    // teleport stones, and guild vaults.
    for (uint32_t t : {0u, 1u, 2u, 3u, 7u, 9u, 10u, 13u, 17u, 18u, 19u, 22u, 23u, 24u, 26u, 27u, 34u}) {
        INFO("interactable type " << t);
        CHECK(gameObjectHighlights(t, emptyData));
    }

    // Non-click types measured brightening on Native 2.4.3 (pair 5 & pair 15):
    // SPELL_FOCUS (8: Campfire 1798, Forge 1685), DUEL_ARBITER (16: Duel Flag
    // 21680), and AURA_GENERATOR (30: Vanguard of the Alliance 183333).
    for (uint32_t t : {8u, 16u, 30u}) {
        INFO("non-click lit type " << t);
        CHECK_FALSE(gameObjectTypeRule(t).use);
        CHECK(gameObjectHighlights(t, emptyData));
    }

    // GAMEOBJECT_TYPE_TRAP (6) never highlights on hover in Native 2.4.3:
    // Alliance Silithyst pillar 181618, Alliance Silithyst Extractor 181603,
    // Drakonid Bones 179804, and Serpentbloom 19535 (even with data1 != 0).
    std::uint32_t trapWithData1[24] = {};
    trapWithData1[1] = 1;
    trapWithData1[10] = 1;
    CHECK_FALSE(gameObjectHighlights(6, emptyData));
    CHECK_FALSE(gameObjectHighlights(6, trapWithData1));

    // Non-interactable scenery/utility types stay dark on Native 2.4.3:
    // BINDER (4), TRANSPORT (11: Vator 80023), AREADAMAGE (12), MAP_OBJECT (14),
    // MO_TRANSPORT (15), AUCTIONHOUSE (20: Gadgetzan Auction 175904),
    // GUARDPOST (21), FISHINGHOLE (25: Oily Blackmouth School 180582),
    // LOTTERY_KIOSK (28), DUNGEON_DIFFICULTY (31: heroic icon 184128),
    // BARBER_CHAIR (32), DESTRUCTIBLE (33), and unknown types >= 35.
    for (uint32_t t : {4u, 11u, 12u, 14u, 15u, 20u, 21u, 25u, 28u, 31u, 32u, 33u, 35u, 99u}) {
        INFO("non-lit type " << t);
        CHECK_FALSE(gameObjectHighlights(t, trapWithData1));
    }

    // GAMEOBJECT_TYPE_GENERIC (5): highlights iff data[1] (_generic.highlight) != 0.
    // Floating tooltip alone (data[0] != 0, data[1] == 0: Ironforge Auction house 176924) stays dark.
    std::uint32_t genericData[24] = {};
    CHECK_FALSE(gameObjectHighlights(5, genericData));
    genericData[0] = 2;
    CHECK_FALSE(gameObjectHighlights(5, genericData));
    genericData[1] = 1;  // Guild Master & Tabards 183865 / Old Coast Road 80
    CHECK(gameObjectHighlights(5, genericData));

    // GAMEOBJECT_TYPE_CAPTURE_POINT (29): highlights iff data[19] (capturePoint.highlight) != 0.
    // Visual Banner 184382 (data[18]=1 large, data[19]=0) stays dark on Native 2.4.3.
    std::uint32_t captureData[24] = {};
    captureData[18] = 1;
    CHECK_FALSE(gameObjectHighlights(29, captureData));
    captureData[19] = 1;
    CHECK(gameObjectHighlights(29, captureData));
}
