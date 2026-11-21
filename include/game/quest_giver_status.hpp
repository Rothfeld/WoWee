#pragma once

/// What the server says an NPC has for you, and the mark that goes over its
/// head.
///
/// The values are DIALOG_STATUS_* from the server's QuestDef.h and there are
/// eleven of them. This client named six, and two of those it named wrongly:
/// 7 was called "available, low level" when the server means available-with-
/// reputation, and low-level-available is 2 - a value nothing here had a name
/// for at all.
///
/// A status with no name matched no branch in any of the four places that draw
/// the mark, so nothing was drawn. Statuses 2, 3 and 4 are what a questgiver
/// reports once you out-level its quests, and 9 is a turn-in that keeps its
/// mark off the minimap - so the mark simply stopped appearing over exactly
/// the NPCs a player has most of by mid-level, and appeared over the rest.
///
/// Four places mapped the status to a symbol and a colour, and they are one
/// mapping: the nameplate, the target frame, the focus frame and the minimap.

#include <cstdint>

namespace wowee::game {

/// DIALOG_STATUS_*, as the server defines it.
enum class QuestGiverStatus : uint8_t {
    NONE                    = 0,
    UNAVAILABLE             = 1,
    LOW_LEVEL_AVAILABLE     = 2,   ///< ! you have out-levelled
    LOW_LEVEL_REWARD_REP    = 3,
    LOW_LEVEL_AVAILABLE_REP = 4,
    INCOMPLETE              = 5,   ///< ? started, not finished
    REWARD_REP              = 6,
    AVAILABLE_REP           = 7,
    AVAILABLE               = 8,   ///< ! the ordinary one
    REWARD2                 = 9,   ///< ? turn-in, with no minimap dot
    REWARD                  = 10,  ///< ? turn-in
    /// TBC DIALOG_STATUS_CHAT (wire 2): every quest it offers you are more
    /// than Quests.LowLevelHideDiff levels above (WorldSession::
    /// getDialogStatus). No mark over it in build 8606, yet the Quest
    /// cursor. Not a WotLK wire value; WotLK sends LOW_LEVEL_AVAILABLE.
    CHAT                    = 11,
};

/// The mark to draw over a questgiver, or an empty symbol for none.
struct QuestGiverMarker {
    const char* symbol = nullptr;  ///< "!", "?", or null to draw nothing
    bool dim = false;              ///< grey rather than gold
    const char* tooltip = nullptr;
    /// Whether this also earns a dot on the minimap. Status 9 is the one that
    /// does not, which is the only thing separating it from 10.
    bool onMinimap = false;
    /// Repeatable/daily (the *_REP statuses): drawn blue - TalkToMeBlue.m2 /
    /// TalkToMeQuestion_ltblue.m2 over the NPC, the blue ObjectIcons cells on
    /// the minimap.
    bool repeatable = false;
};

/// One mapping, so the nameplate and the minimap cannot disagree about what an
/// NPC is offering.
inline QuestGiverMarker questGiverMarker(QuestGiverStatus status) {
    switch (status) {
        case QuestGiverStatus::AVAILABLE:
            return {.symbol = "!", .dim = false, .tooltip = "Has a quest available", .onMinimap = true};
        case QuestGiverStatus::AVAILABLE_REP:
            return {.symbol = "!", .dim = false, .tooltip = "Has a quest available", .onMinimap = true,
                    .repeatable = true};
        case QuestGiverStatus::LOW_LEVEL_AVAILABLE:
        case QuestGiverStatus::LOW_LEVEL_AVAILABLE_REP:
            return {.symbol = "!", .dim = true, .tooltip = "Has a low-level quest available", .onMinimap = true};
        case QuestGiverStatus::REWARD:
            return {.symbol = "?", .dim = false, .tooltip = "Quest ready to turn in", .onMinimap = true};
        case QuestGiverStatus::REWARD_REP:
            // Native TBC 2.4.3 (build 8606): repeatable turn-in draws the 3D
            // light-blue ? in world (Broken Trap 179485, Arygos 15380) without
            // a minimap icon.
            return {.symbol = "?", .dim = false, .tooltip = "Quest ready to turn in", .onMinimap = false,
                    .repeatable = true};
        // The same mark, and deliberately not on the minimap.
        case QuestGiverStatus::REWARD2:
            return {.symbol = "?", .dim = false, .tooltip = "Quest ready to turn in", .onMinimap = false};
        case QuestGiverStatus::LOW_LEVEL_REWARD_REP:
            return {.symbol = "?", .dim = true, .tooltip = "Quest ready to turn in", .onMinimap = false};
        case QuestGiverStatus::INCOMPLETE:
            return {.symbol = "?", .dim = true, .tooltip = "Quest in progress", .onMinimap = false};
        case QuestGiverStatus::UNAVAILABLE:
            return {.symbol = "!", .dim = true, .tooltip = "Quest not yet available", .onMinimap = false};
        case QuestGiverStatus::NONE:
        case QuestGiverStatus::CHAT:
            break;
    }
    return {};
}

/// 3D world marker variant for QuestMarkerRenderer::setMarker().
/// markerType: -1 none, 0 ! (gold/grey via grayscale), 1 gold ?, 2 grey ?,
/// 4 blue repeatable !, 5 light-blue repeatable ?.
struct QuestMarkerVisual {
    int markerType = -1;
    float grayscale = 0.0f;
};

inline QuestMarkerVisual questMarkerVisual(QuestGiverStatus status) {
    const auto marker = questGiverMarker(status);
    const float grayscale = marker.dim ? 1.0f : 0.0f;
    if (!marker.symbol) return {.markerType = -1, .grayscale = grayscale};
    const bool bang = marker.symbol[0] == '!';
    const int markerType = bang ? (marker.repeatable ? 4 : 0)
                                : (marker.dim ? 2 : (marker.repeatable ? 5 : 1));
    return {.markerType = markerType, .grayscale = grayscale};
}

/// The Interface\Cursor name a quest giver's status puts over it, or null
/// when the status leaves the giver its other cursor (a vendor's Pickup, a
/// gossip NPC's Speak, an object's gear).
///
/// It is not the mark's symbol. Build 8606, pair 16, Haughty Modiste 15165
/// (questgiver + vendor), Yeh'kinya 8579 (questgiver + gossip) and Wanted
/// Poster 142122, in every state from the server's status
/// (_experiments/20261118T181000Z.pair16_quest_cursor):
///   - NONE and UNAVAILABLE (grey !): the service cursor;
///   - available, also out-levelled (CHAT: no mark at level 70 for level
///     45 quests): Quest;
///   - taken and not finished (grey ?): Quest as well;
///   - a daily on offer (AVAILABLE_REP, Vindicator Xayann 24965): Quest;
///   - ready to hand in: QuestTurnIn, also a daily (Sky Sergeant Vanderlip
///     23120);
///   - a repeatable ready to hand in again (REWARD_REP, Arygos 15380):
///     QuestRepeatable.
/// REWARD2, LOW_LEVEL_AVAILABLE_REP and LOW_LEVEL_REWARD_REP are not in the
/// measurement; they follow their siblings.
inline const char* questGiverCursor(QuestGiverStatus status) {
    switch (status) {
        case QuestGiverStatus::NONE:
        case QuestGiverStatus::UNAVAILABLE:
            return nullptr;
        case QuestGiverStatus::REWARD:
        case QuestGiverStatus::REWARD2:
            return "QuestTurnIn";
        case QuestGiverStatus::REWARD_REP:
        case QuestGiverStatus::LOW_LEVEL_REWARD_REP:
            return "QuestRepeatable";
        case QuestGiverStatus::CHAT:
        case QuestGiverStatus::LOW_LEVEL_AVAILABLE:
        case QuestGiverStatus::LOW_LEVEL_AVAILABLE_REP:
        case QuestGiverStatus::INCOMPLETE:
        case QuestGiverStatus::AVAILABLE_REP:
        case QuestGiverStatus::AVAILABLE:
            return "Quest";
    }
    return nullptr;
}

}  // namespace wowee::game
