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
    // Chairs keep their cursor only; other types always name themselves.
    CHECK(objectNameTooltip(7, 1, 1) == ObjectNameTooltip::None);
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
                        Row{7, "Interact"}, Row{9, "Inspect"}, Row{10, "Interact"}, Row{13, "Interact"},
                        Row{17, "Fishing"}, Row{18, "Interact"}, Row{19, "Mail"}, Row{22, "Interact"},
                        Row{23, "Interact"}, Row{24, "Interact"}, Row{26, "Interact"}}) {
        INFO("type " << r.type);
        const auto a = gameObjectAffordance(r.type, 0, 0, true);
        CHECK(a.use);
        CHECK(std::string(a.cursor) == r.cursor);
        CHECK(gameObjectTypeRule(r.type).source == GameObjectRuleSource::Server);
    }
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
    for (uint32_t t : {8u, 16u, 25u, 27u, 34u}) {
        INFO("type " << t);
        CHECK(gameObjectTypeRule(t).source == GameObjectRuleSource::Native);
    }
    CHECK(gameObjectTypeRule(6).source == GameObjectRuleSource::NeedsCapture);
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
