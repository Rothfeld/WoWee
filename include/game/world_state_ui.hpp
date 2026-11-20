#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wowee {
namespace game {

/**
 * WorldStateAlwaysUpFrame's lines: WorldStateUI.dbc rows for where the player
 * is, filled in from the world states the server sent.
 *
 * Observed on build 8606 (GetWorldStateUIInfo dumped in Hellfire Peninsula,
 * Zangarmarsh and Eastern Plaguelands; _experiments/20261117T223500Z.worldstate_ui):
 *
 * - Every row whose MapID is the current map and whose AreaID is 0 or the
 *   current area/zone is a line, in file order, whatever its state. Hellfire
 *   answers three lines (rows 139, 140, 141) with the capture bar's at state 0.
 * - state is the value of the row's StateVariable world state; a row without
 *   one (StateVariable 0, the Plaguelands tower counts) answers 1.
 * - "%<id>w" in the text is the decimal value of world state <id>, 0 when the
 *   server never sent it: row 139 reads "Progress: %2427w" (a Plaguelands state)
 *   and native shows "Progress: 0" in Hellfire.
 * - icon, dynamicIcon, tooltip and dynamicTooltip are the row's strings as
 *   they are, whatever the state; extendedUIState1..3 are the values of the
 *   ExtendedUIStateVariable world states (0 for a variable of 0).
 */
struct WorldStateUIRow {
    uint32_t id = 0;
    uint32_t mapId = 0;
    uint32_t areaId = 0;
    uint32_t stateVariable = 0;
    uint32_t type = 0;
    std::string icon;
    std::string text;
    std::string tooltip;
    std::string dynamicIcon;
    std::string dynamicTooltip;
    std::string extendedUI;
    std::array<uint32_t, 3> extendedUIStateVariables{};
};

/// GetWorldStateUIInfo's eleven returns for one line.
struct WorldStateUILine {
    uint32_t uiType = 0;
    uint32_t state = 0;
    std::string text;
    std::string icon;
    std::string dynamicIcon;
    std::string tooltip;
    std::string dynamicTooltip;
    std::string extendedUI;
    std::array<uint32_t, 3> extendedUIStates{};
};

/// GetBattlefieldStatInfo's three returns for one scoreboard column (Type == 2).
struct WorldStateStatColumn {
    std::string text;
    std::string icon;
    std::string tooltip;
};

/// A world state's value, or nothing when the server has not sent it.
using WorldStateLookup = std::function<std::optional<uint32_t>(uint32_t)>;

/// The row text with every "%<digits>w" replaced by that world state's value.
/// Anything else, a lone "%" included ("Shield Remaining: %3054w%"), is copied.
inline std::string expandWorldStateText(std::string_view text, const WorldStateLookup& value) {
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '%') {
            size_t j = i + 1;
            while (j < text.size() && text[j] >= '0' && text[j] <= '9') ++j;
            if (j > i + 1 && j < text.size() && text[j] == 'w') {
                const auto id = static_cast<uint32_t>(
                    std::stoul(std::string(text.substr(i + 1, j - i - 1))));
                out += std::to_string(value(id).value_or(0));
                i = j + 1;
                continue;
            }
        }
        out += text[i];
        ++i;
    }
    return out;
}

/// The lines for a player on mapId inside every area of areaChain (the area
/// under the player and each of its AreaTable parents).
/// Type == 2 rows are battleground scoreboard stat columns (GetBattlefieldStatInfo)
/// rather than AlwaysUp/ExtendedUI HUD lines (GetWorldStateUIInfo).
inline std::vector<WorldStateUILine> worldStateUILines(
        const std::vector<WorldStateUIRow>& rows, uint32_t mapId,
        const std::vector<uint32_t>& areaChain, const WorldStateLookup& value) {
    std::vector<WorldStateUILine> out;
    for (const auto& row : rows) {
        if (row.mapId != mapId) continue;
        if (row.type == 2) continue;
        if (row.areaId != 0) {
            bool inArea = false;
            for (const uint32_t a : areaChain) inArea = inArea || a == row.areaId;
            if (!inArea) continue;
        }
        WorldStateUILine line;
        line.uiType = row.type;
        line.state = row.stateVariable == 0 ? 1u : value(row.stateVariable).value_or(0);
        line.text = expandWorldStateText(row.text, value);
        line.icon = row.icon;
        line.dynamicIcon = row.dynamicIcon;
        line.tooltip = row.tooltip;
        line.dynamicTooltip = row.dynamicTooltip;
        line.extendedUI = row.extendedUI;
        for (size_t k = 0; k < 3; ++k) {
            const uint32_t var = row.extendedUIStateVariables[k];
            line.extendedUIStates[k] = var == 0 ? 0u : value(var).value_or(0);
        }
        out.push_back(std::move(line));
    }
    return out;
}

/// Battleground scoreboard custom stat columns for mapId (WorldStateUI.dbc Type == 2),
/// in DBC file order.
inline std::vector<WorldStateStatColumn> worldStateBattlefieldStats(
        const std::vector<WorldStateUIRow>& rows, uint32_t mapId) {
    std::vector<WorldStateStatColumn> out;
    for (const auto& row : rows) {
        if (row.mapId != mapId || row.type != 2) continue;
        out.push_back({row.text, row.icon, row.tooltip});
    }
    return out;
}

}  // namespace game
}  // namespace wowee
