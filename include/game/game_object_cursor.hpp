#pragma once

#include <string>

namespace wowee::game {

/// What decides the cursor over a usable game object, read from its template,
/// its flags, its lock and the player.
struct GameObjectCursorFacts {
    /// Interface\Cursor name from the type table (game_object_interaction.hpp).
    const char* typeCursor = nullptr;
    /// "Quest" or "QuestTurnIn" for a quest giver object whose status has a
    /// marker; nullptr otherwise.
    const char* questCursor = nullptr;
    /// LockType CursorName of lock slot 0 ("PickLock", "GatherHerbs",
    /// "Mine"); empty when there is no lock or that slot names none.
    std::string lockCursor;
    /// GO_FLAG_LOCKED.
    bool flaggedLocked = false;
    bool inRange = true;
};

/// Interface\Cursor\<name>.blp, the way build 8606 shows it (pair 0 hovers,
/// _experiments/20261117T195204Z.pair0_go_cursors):
///   - a GO_FLAG_LOCKED object shows the gear greyed, whatever the player
///     knows or carries: Battered Footlocker 179486 (Lockpicking) also with
///     Pick Lock at skill 200, Coilskar Chest 184716 also with its key in
///     the backpack, Dire Maul's Door 177198 (Crescent Key first);
///   - a quest giver object with a quest marker shows that marker's cursor:
///     Wanted Poster 186426 Quest, Skull Pile 185913 (no quest) the gear;
///   - a lock whose first slot names a cursor shows it whatever the
///     player's skill: Peacebloom 1618 GatherHerbs without Herbalism, Tin
///     Vein 1732 Mine at Mining 1 of 65. Only slot 0 counts: Alliance
///     Strongbox 4095 (Lock 23: slot 0 empty, Lockpicking in slot 1) is
///     the gear, also with Pick Lock known;
///   - otherwise the type's cursor, the gear for chests too: Warpwood Pod
///     179526 and Battered Chest 2843;
///   - out of reach, the Unable variant.
inline std::string gameObjectCursorPath(const GameObjectCursorFacts& facts) {
    std::string name;
    if (facts.flaggedLocked) {
        name = "Interact";
        return "Interface\\Cursor\\Unable" + name + ".blp";
    }
    if (facts.questCursor) name = facts.questCursor;
    else if (!facts.lockCursor.empty()) name = facts.lockCursor;
    else name = facts.typeCursor;
    return std::string("Interface\\Cursor\\") + (facts.inRange ? "" : "Unable") + name + ".blp";
}

} // namespace wowee::game
