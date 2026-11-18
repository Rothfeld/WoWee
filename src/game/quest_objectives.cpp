#include "game/quest_objectives.hpp"

#include "game/quest_progress.hpp"

#include <cstdlib>

namespace wowee {
namespace game {

namespace {

/// What a kill objective is about, by name: a creature for a positive id, a
/// game object for a negative one. Empty while the query is out; this asks
/// again, because a log built from a saved character can reach an objective
/// whose query was never sent.
std::string targetName(GameHandler& gh, int32_t npcOrGoId) {
    const uint32_t entry = static_cast<uint32_t>(std::abs(npcOrGoId));
    if (npcOrGoId > 0) {
        std::string name = gh.getCachedCreatureName(entry);
        if (name.empty()) gh.queryCreatureInfo(entry, 0);
        return name;
    }
    const auto* info = gh.getCachedGameObjectInfo(entry);
    if (!info || info->name.empty()) {
        gh.queryGameObjectInfo(entry, 0);
        return {};
    }
    return info->name;
}

}  // namespace

std::vector<QuestObjective> questObjectives(GameHandler& gh,
                                            const GameHandler::QuestLogEntry& q) {
    std::vector<QuestObjective> out;
    // The EndText line ("event"), what the server reports done by completing
    // the quest. First, ahead of kills and items, as 2.4.3 lists it: "Return
    // to Knot Thimblejack" before the Gordok Ogre Suit's four items,
    // "Brokentoe Defeated" before the kill.
    if (!q.endText.empty()) {
        QuestObjective line;
        line.text = q.endText;
        line.type = "event";
        line.finished = q.complete;
        out.push_back(std::move(line));
    }
    for (size_t k = 0; k < q.killObjectives.size(); ++k) {
        const auto& kill = q.killObjectives[k];
        if (!isQuestObjective(kill.npcOrGoId, kill.required)) continue;
        const uint32_t key = static_cast<uint32_t>(std::abs(kill.npcOrGoId));
        uint32_t current = 0;
        if (auto it = q.killCounts.find(key); it != q.killCounts.end()) current = it->second.first;
        const bool isObject = kill.npcOrGoId < 0;
        QuestObjective line;
        // The quest's own wording where it has one, without "slain":
        // "Capture The Overlook: 0/1" (2.4.3, Hellfire Fortifications).
        if (k < q.objectiveTexts.size() && !q.objectiveTexts[k].empty()) {
            line.text = q.objectiveTexts[k] + ": " + std::to_string(current) + "/" +
                        std::to_string(kill.required);
        } else {
            line.text = questObjectiveLine(targetName(gh, kill.npcOrGoId), isObject, current,
                                           kill.required);
        }
        line.type = isObject ? "object" : "monster";
        line.finished = current >= kill.required;
        out.push_back(std::move(line));
    }
    for (const auto& item : q.itemObjectives) {
        if (!isQuestObjective(static_cast<int32_t>(item.itemId), item.required)) continue;
        uint32_t current = 0;
        if (auto it = q.itemCounts.find(item.itemId); it != q.itemCounts.end()) current = it->second;
        // By name; the number stands in while the item query is out, and the
        // answer lands with a QUEST_LOG_UPDATE behind it.
        std::string name;
        const auto* info = gh.getItemInfo(item.itemId);
        if (info && !info->name.empty()) {
            name = info->name;
        } else {
            gh.queryItemInfo(item.itemId, 0);
            name = "Item #" + std::to_string(item.itemId);
        }
        QuestObjective line;
        line.text = name + ": " + std::to_string(current) + "/" + std::to_string(item.required);
        line.type = "item";
        line.finished = current >= item.required;
        out.push_back(std::move(line));
    }
    return out;
}

}  // namespace game
}  // namespace wowee
