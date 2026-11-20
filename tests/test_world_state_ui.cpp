#include <catch_amalgamated.hpp>

#include "game/world_state_ui.hpp"

#include <map>

using wowee::game::WorldStateUIRow;
using wowee::game::expandWorldStateText;
using wowee::game::worldStateUILines;

namespace {

// Rows 139/140/141 (Hellfire Peninsula) and 136/138 (Eastern Plaguelands)
// as they are in the 2.4.3 WorldStateUI.dbc.
std::vector<WorldStateUIRow> rows() {
    WorldStateUIRow epAlliance{136, 0, 139, 0, 1,
        "Interface\\WorldStateFrame\\AllianceTower", "Controlled: %2327w",
        "Alliance Towers Controlled", "", "", "", {0, 0, 0}};
    WorldStateUIRow epBar{138, 0, 139, 2426, 1, "", "Progress: %2427w", "", "", "",
        "CAPTUREPOINT", {2427, 2428, 0}};
    WorldStateUIRow hpBar{139, 530, 3483, 2473, 1, "", "Progress: %2427w", "", "", "",
        "CAPTUREPOINT", {2474, 2475, 0}};
    WorldStateUIRow hpAlliance{140, 530, 3483, 2490, 1,
        "Interface\\WorldStateFrame\\AllianceTower", "Controlled: %2476w",
        "Alliance Forts Controlled", "Interface\\TargetingFrame\\UI-PVP-Alliance",
        "Last Winner: Alliance", "", {0, 0, 0}};
    WorldStateUIRow hpHorde{141, 530, 3483, 2489, 1,
        "Interface\\WorldStateFrame\\HordeTower", "Controlled: %2478w",
        "Horde Forts Controlled", "Interface\\TargetingFrame\\UI-PVP-Horde",
        "Last Winner: Horde", "", {0, 0, 0}};
    return {epAlliance, epBar, hpBar, hpAlliance, hpHorde};
}

auto lookup(const std::map<uint32_t, uint32_t>& states) {
    return [states](uint32_t key) -> std::optional<uint32_t> {
        const auto it = states.find(key);
        if (it == states.end()) return std::nullopt;
        return it->second;
    };
}

}  // namespace

// Native GetWorldStateUIInfo in Hellfire Peninsula, away from a tower:
//   1#1#0#Progress: 0#####CAPTUREPOINT#0#0#0
//   2#1#1#Controlled: 0#...AllianceTower#...UI-PVP-Alliance#Alliance Forts Controlled#Last Winner: Alliance##0#0#0
TEST_CASE("Hellfire lines match build 8606", "[worldstate]") {
    // What CMaNGOS OutdoorPvPHP sends on entering the zone.
    const auto lines = worldStateUILines(rows(), 530, {3483},
        lookup({{2476, 0}, {2478, 0}, {2490, 1}, {2489, 1}}));
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].uiType == 1);
    CHECK(lines[0].state == 0);
    CHECK(lines[0].text == "Progress: 0");
    CHECK(lines[0].extendedUI == "CAPTUREPOINT");
    CHECK(lines[1].state == 1);
    CHECK(lines[1].text == "Controlled: 0");
    CHECK(lines[1].icon == "Interface\\WorldStateFrame\\AllianceTower");
    CHECK(lines[1].dynamicIcon == "Interface\\TargetingFrame\\UI-PVP-Alliance");
    CHECK(lines[1].tooltip == "Alliance Forts Controlled");
    CHECK(lines[1].dynamicTooltip == "Last Winner: Alliance");
    CHECK(lines[2].text == "Controlled: 0");
}

TEST_CASE("capture bar states come from the extended state variables", "[worldstate]") {
    const auto lines = worldStateUILines(rows(), 530, {3537, 3483},
        lookup({{2473, 1}, {2474, 63}, {2475, 20}}));
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].state == 1);
    CHECK(lines[0].extendedUIStates[0] == 63);
    CHECK(lines[0].extendedUIStates[1] == 20);
    CHECK(lines[0].extendedUIStates[2] == 0);
}

// Native in Eastern Plaguelands: the tower counts (StateVariable 0) answer state 1.
TEST_CASE("a row without a state variable is shown", "[worldstate]") {
    const auto lines = worldStateUILines(rows(), 0, {139}, lookup({{2327, 2}}));
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].state == 1);
    CHECK(lines[0].text == "Controlled: 2");
    CHECK(lines[1].state == 0);
}

TEST_CASE("other maps and areas have no lines", "[worldstate]") {
    CHECK(worldStateUILines(rows(), 530, {3521}, lookup({})).empty());
    CHECK(worldStateUILines(rows(), 1, {139}, lookup({})).empty());
}

TEST_CASE("world state text substitution", "[worldstate]") {
    const auto value = lookup({{2509, 1}, {2512, 2}, {2510, 5}, {3054, 80}});
    CHECK(expandWorldStateText("Time Remaining - %2509w:%2512w%2510w", value) ==
          "Time Remaining - 1:25");
    CHECK(expandWorldStateText("Shield Remaining: %3054w%", value) == "Shield Remaining: 80%");
    CHECK(expandWorldStateText("West Beacon", value) == "West Beacon");
    CHECK(expandWorldStateText("%w %12x", value) == "%w %12x");
}

TEST_CASE("Alterac Valley Type 2 scoreboard stat rows are excluded from AlwaysUp HUD", "[worldstate]") {
    std::vector<WorldStateUIRow> avRows = {
        {180, 30, 0, 3134, 0, "Interface\\TargetingFrame\\UI-PVP-Alliance",
            "Reinforcements: %3127w", "Alliance Reinforcements", "", "", "", {0, 0, 0}},
        {181, 30, 0, 3133, 0, "Interface\\TargetingFrame\\UI-PVP-Horde",
            "Reinforcements: %3128w", "Horde Reinforcements", "", "", "", {0, 0, 0}},
        {63, 30, 0, 0, 2, "Interface\\WorldStateFrame\\ColumnIcon-GraveyardCapture",
            "Graveyards Assaulted", "Number of times enemy graveyard banners have been removed", "", "", "", {0, 0, 0}},
        {65, 30, 0, 0, 2, "Interface\\WorldStateFrame\\ColumnIcon-GraveyardDefend",
            "Graveyards Defended", "Number of times a graveyard capture has been prevented", "", "", "", {0, 0, 0}},
        {61, 30, 0, 0, 2, "Interface\\WorldStateFrame\\ColumnIcon-TowerCapture",
            "Towers Assaulted", "Number of times enemy tower banners have been removed", "", "", "", {0, 0, 0}},
        {64, 30, 0, 0, 2, "Interface\\WorldStateFrame\\ColumnIcon-TowerDefend",
            "Towers Defended", "Number of times a tower capture has been prevented", "", "", "", {0, 0, 0}},
        {82, 30, 0, 0, 2, "",
            "Secondary Objectives", "Number of secondary units activated", "", "", "", {0, 0, 0}},
    };

    const auto lines = worldStateUILines(avRows, 30, {2597},
        lookup({{3134, 1}, {3133, 1}, {3127, 600}, {3128, 600}}));
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].text == "Reinforcements: 600");
    CHECK(lines[1].text == "Reinforcements: 600");

    const auto stats = wowee::game::worldStateBattlefieldStats(avRows, 30);
    REQUIRE(stats.size() == 5);
    CHECK(stats[0].text == "Graveyards Assaulted");
    CHECK(stats[0].icon == "Interface\\WorldStateFrame\\ColumnIcon-GraveyardCapture");
    CHECK(stats[4].text == "Secondary Objectives");
    CHECK(stats[4].icon.empty());
}
