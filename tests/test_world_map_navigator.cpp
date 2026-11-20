// MapNavigator against answers read off the original 2.4.3 client's own Lua
// API (GetMapLandmarkInfo, UpdateMapHighlight, GetMapInfo...) for the same
// DBC rows, which are copied here from the 8606 data.
#include <catch_amalgamated.hpp>
#include "rendering/world_map/map_navigator.hpp"
#include "core/coordinates.hpp"

#include <set>

using namespace wowee::rendering::world_map;
using Catch::Approx;

namespace {

constexpr float kTile = wowee::core::coords::TILE_SIZE;

/// Mark the ZMP cell holding a canonical position.
void paint(MapNavigatorData::Zmp& zmp, float x, float y, uint32_t area) {
    const int col = static_cast<int>((32.0f - y / kTile) / 64.0f * 128.0f);
    const int row = static_cast<int>((32.0f - x / kTile) / 64.0f * 128.0f);
    zmp.cells[static_cast<size_t>(row) * 128 + static_cast<size_t>(col)] = area;
}

MapNavigatorData data() {
    MapNavigatorData d;
    // AreaTable: the explore bit is the id here.
    d.areas = {{12, 0, 0, 12, "Elwynn Forest"},   {9, 0, 12, 9, "Northshire Valley"},
               {87, 0, 12, 87, "Goldshire"},       {18, 0, 12, 18, "Crystal Lake"},
               {57, 0, 12, 57, "Fargodeep Mine"},  {63, 0, 12, 63, "The Stonefield Farm"},
               {46, 0, 0, 46, "Burning Steppes"},  {10, 0, 0, 10, "Duskwood"},
               {35, 0, 33, 35, "Booty Bay"},        {33, 0, 0, 33, "Stranglethorn Vale"},
               {41, 0, 0, 41, "Deadwind Pass"},
               {3557, 530, 3524, 3557, "The Exodar"}, {3524, 530, 0, 3524, "Azuremyst Isle"},
               {3483, 530, 0, 3483, "Hellfire Peninsula"}};
    d.mapAreas = {{13, 1, 0, "Kalimdor", 17066.6f, -19733.211f, 12799.9f, -11733.3f, -1},
                  {14, 0, 0, "Azeroth", 18171.971f, -22569.211f, 11176.344f, -15973.344f, -1},
                  {30, 0, 12, "Elwynn", 1535.417f, -1935.417f, -7939.583f, -10254.166f, -1},
                  {29, 0, 46, "BurningSteppes", -266.667f, -3195.833f, -7031.25f, -8983.333f, -1},
                  {34, 0, 10, "Duskwood", 833.333f, -1866.667f, -9716.666f, -11516.666f, -1},
                  {32, 0, 41, "DeadwindPass", -833.333f, -3333.333f, -9866.666f, -11533.333f, -1},
                  {466, 530, 0, "Expansion01", 12996.039f, -4468.039f, 5821.359f, -5821.359f, -1},
                  {465, 530, 3483, "Hellfire", 5539.583f, 375.0f, 1481.25f, -1962.5f, -1},
                  {464, 530, 3524, "AzuremystIsle", -10500.0f, -14570.833f, -2793.75f, -5508.333f, 1}};
    d.overlays = {
        {30, {9, 0, 0, 0}, "NORTHSHIREVALLEY", 256, 256, 381, 147, 190, 425, 375, 600},
        {30, {18, 0, 0, 0}, "CRYSTALLAKE", 225, 220, 422, 332, 380, 450, 475, 605},
        {30, {57, 63, 0, 0}, "FARGODEEPMINE", 256, 240, 238, 428, 510, 300, 615, 470},
    };
    d.pois = {{16, 5, 29, 0, 0, 12, -9153.77f, 364.057f, "Stormwind City", ""},
              {7, 6, 5, 0, 0, 87, -9480.09f, 63.522f, "Goldshire", ""},
              {40, 4, 13, 0, 0, 35, -14383.3f, 487.139f, "Booty Bay", ""},
              {1785, 5, 29, 530, 0, 3557, -4007.31f, -11873.6f, "The Exodar", ""}};
    d.worldSheet = {{0, 23, 47, 9, 61, 14.5f, -4.75f, 0.75f}, {1, 14, 48, 9, 52, -19.0f, -0.322498f, 0.75f}};
    d.transforms = {{530, 1, -6933.333f, -16000.0f, 533.333f, -8000.0f, 10133.333f, 17600.0f}};
    d.entrances = {{532, 0, -11110.41f, -2004.07f}};
    d.continents = {{1, "Kalimdor"}, {0, "Eastern Kingdoms"}, {530, "Outland"}};
    MapNavigatorData::Zmp ek{0, std::vector<uint32_t>(128 * 128)};
    paint(ek, -8000.0f, -1700.0f, 46);   // Burning Steppes
    paint(ek, -11110.41f, -2004.07f, 41);  // Karazhan's entrance, Deadwind Pass
    paint(ek, -9500.0f, 0.0f, 87);        // Goldshire, in Elwynn
    paint(ek, -9900.0f, 1500.0f, 10);     // Duskwood's edge as it shows on Elwynn's map
    d.zmps.push_back(std::move(ek));
    return d;
}

MapNavigator navigator() {
    static const std::set<uint32_t> explored = {12, 9, 87, 57, 3557, 3483};
    return MapNavigator(data(), [](uint32_t bit) { return explored.contains(bit); },
                        [](uint32_t) { return 0u; },
                        [](const std::string&) { return glm::ivec2(256, 256); });
}

int zoneRow(const MapNavigator& nav, int continent, const std::string& name) {
    const auto names = nav.zoneNames(continent);
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == name) return static_cast<int>(i) + 1;
    FAIL("no zone " << name);
    return 0;
}

}  // namespace

TEST_CASE("MapNavigator lists continents and their zones by name", "[world_map][navigator]") {
    auto nav = navigator();
    REQUIRE(nav.continentNames() == std::vector<std::string>{"Kalimdor", "Eastern Kingdoms", "Outland"});
    REQUIRE(nav.zoneNames(2) == std::vector<std::string>{"Burning Steppes", "Deadwind Pass", "Duskwood",
                                                         "Elwynn Forest"});
    // Azuremyst is on map 530 and shown on Kalimdor.
    REQUIRE(nav.zoneNames(1) == std::vector<std::string>{"Azuremyst Isle"});
    REQUIRE(nav.zoneNames(3) == std::vector<std::string>{"Hellfire Peninsula"});
}

TEST_CASE("MapNavigator levels and GetMapInfo", "[world_map][navigator]") {
    auto nav = navigator();
    nav.setZoom(2, zoneRow(nav, 2, "Elwynn Forest"));
    REQUIRE(*nav.mapFile() == "Elwynn");
    nav.zoomOut();
    REQUIRE(nav.continent() == 2);
    REQUIRE(nav.zone() == 0);
    REQUIRE(*nav.mapFile() == "Azeroth");
    nav.zoomOut();
    REQUIRE(nav.continent() == MapNavigator::kWorld);
    REQUIRE_FALSE(nav.mapFile());
    nav.zoomOut();
    REQUIRE(nav.continent() == MapNavigator::kCosmic);
    REQUIRE_FALSE(nav.mapFile());
    // Outland is not on the Azeroth sheet: it zooms out to cosmic.
    nav.setZoom(3);
    nav.zoomOut();
    REQUIRE(nav.continent() == MapNavigator::kCosmic);
    // A zone row past the end of the list shows the continent.
    nav.setZoom(3, 40);
    REQUIRE(nav.continent() == 3);
    REQUIRE(nav.zone() == 0);
    REQUIRE(*nav.mapFile() == "Expansion01");
}

TEST_CASE("MapNavigator landmarks match the original's", "[world_map][navigator]") {
    auto nav = navigator();
    nav.setZoom(2);
    auto marks = nav.landmarks();
    REQUIRE(marks.size() == 1);   // Booty Bay unexplored, Goldshire zone-only
    REQUIRE(marks[0].name == "Stormwind City");
    REQUIRE(marks[0].texture == 5);
    REQUIRE(marks[0].x == Approx(0.43709859).margin(1e-5));
    REQUIRE(marks[0].y == Approx(0.74881572).margin(1e-5));

    nav.setZoom(2, zoneRow(nav, 2, "Elwynn Forest"));
    marks = nav.landmarks();
    REQUIRE(marks.size() == 2);
    REQUIRE(marks[0].name == "Stormwind City");
    REQUIRE(marks[0].texture == 15);
    REQUIRE(marks[0].x == Approx(0.33748657).margin(1e-5));
    REQUIRE(marks[0].y == Approx(0.52458113).margin(1e-5));
    REQUIRE(marks[1].name == "Goldshire");
    REQUIRE(marks[1].x == Approx(0.42407533).margin(1e-5));
    REQUIRE(marks[1].y == Approx(0.66556561).margin(1e-5));

    nav.setZoom(MapNavigator::kWorld);
    marks = nav.landmarks();
    REQUIRE(marks.size() == 2);
    REQUIRE(marks[0].name == "Stormwind City");
    REQUIRE(marks[0].x == Approx(0.72336197).margin(1e-5));
    REQUIRE(marks[0].y == Approx(0.69455063).margin(1e-5));
    REQUIRE(marks[1].name == "The Exodar");
    REQUIRE(marks[1].x == Approx(0.06801995).margin(1e-5));
    REQUIRE(marks[1].y == Approx(0.28593490).margin(1e-5));

    nav.setZoom(MapNavigator::kCosmic);
    REQUIRE(nav.landmarks().empty());
}

TEST_CASE("MapNavigator highlight on the world sheet", "[world_map][navigator]") {
    auto nav = navigator();
    nav.setZoom(MapNavigator::kWorld);
    auto h = nav.highlight(0.25f, 0.5f);
    REQUIRE(*h.name == "Kalimdor");
    REQUIRE(*h.file == "Kalimdor");
    REQUIRE(h.texU == Approx(0.79545454).margin(1e-6));
    REQUIRE(h.texV == Approx(1.0));
    REQUIRE(h.width == Approx(0.41916167).margin(1e-6));
    REQUIRE(h.height == Approx(0.79041916).margin(1e-6));
    REQUIRE(h.x == Approx(-0.01896207).margin(1e-6));
    REQUIRE(h.y == Approx(0.07910184).margin(1e-6));
    // Open sea inside the rectangle still names the continent, as the original does.
    h = nav.highlight(0.9f, 0.5f);
    REQUIRE(*h.name == "Eastern Kingdoms");
    REQUIRE(*h.file == "Azeroth");
    REQUIRE(h.texU == Approx(0.47169811).margin(1e-6));
    REQUIRE(h.width == Approx(0.29940119).margin(1e-6));
    REQUIRE(h.height == Approx(0.95209580).margin(1e-6));
    REQUIRE(h.x == Approx(0.62375247).margin(1e-6));
    REQUIRE(h.y == Approx(-0.02694610).margin(1e-6));
    REQUIRE_FALSE(nav.highlight(0.5f, 0.5f).name);

    REQUIRE(nav.click(0.25f, 0.5f));
    REQUIRE(nav.continent() == 1);
}

TEST_CASE("MapNavigator highlight and click on a continent", "[world_map][navigator]") {
    auto nav = navigator();
    nav.setZoom(2);
    // The point of Burning Steppes' ZMP cell.
    const float u = (18171.971f + 1700.0f) / (18171.971f + 22569.211f);
    const float v = (11176.344f + 8000.0f) / (11176.344f + 15973.344f);
    auto h = nav.highlight(u, v);
    REQUIRE(*h.name == "Burning Steppes");
    REQUIRE(*h.file == "BurningSteppes");
    REQUIRE(h.texU == Approx(1.0));
    REQUIRE(h.texV == Approx(0.6640625));
    REQUIRE(h.width == Approx(0.0718969).margin(1e-6));
    REQUIRE(h.height == Approx(0.0719008).margin(1e-6));
    REQUIRE(h.x == Approx(0.4525798).margin(1e-6));
    REQUIRE(h.y == Approx(0.6706373).margin(1e-6));
    REQUIRE(nav.click(u, v));
    REQUIRE(nav.zone() == zoneRow(nav, 2, "Burning Steppes"));
    // A zone map does not zoom further.
    REQUIRE_FALSE(nav.click(0.5f, 0.5f));
}

TEST_CASE("MapNavigator names explored subzones and neighbours on a zone map", "[world_map][navigator]") {
    auto nav = navigator();
    nav.setZoom(2, zoneRow(nav, 2, "Elwynn Forest"));
    auto h = nav.highlight(0.49f, 0.41f);
    REQUIRE(*h.name == "Northshire Valley");
    REQUIRE_FALSE(h.file);
    REQUIRE(h.width == 0);
    // Crystal Lake's art is there, but it is not explored.
    REQUIRE_FALSE(nav.highlight(0.6f, 0.6f).name);
    // An overlay is named after its first area, whichever of them was explored.
    REQUIRE(*nav.highlight(0.33f, 0.85f).name == "Fargodeep Mine");
    // Another zone under the point is named, explored or not.
    const float u = (1535.417f - 1500.0f) / (1535.417f + 1935.417f);
    const float v = (-7939.583f + 9900.0f) / (-7939.583f + 10254.166f);
    REQUIRE(*nav.highlight(u, v).name == "Duskwood");
    REQUIRE(nav.overlays().size() == 2);
    REQUIRE(nav.overlays()[0].texture == "Interface\\WorldMap\\Elwynn\\NORTHSHIREVALLEY");
}

TEST_CASE("MapNavigator opens on the player's zone, an instance's entrance zone", "[world_map][navigator]") {
    auto nav = navigator();
    nav.setToPosition(0, -9516.0f, 550.0f, 12);
    REQUIRE(nav.continent() == 2);
    REQUIRE(*nav.mapFile() == "Elwynn");
    const auto p = nav.project(0, -9516.0f, 550.0f);
    REQUIRE(p);
    REQUIRE_FALSE(nav.project(1, 0.0f, 0.0f));
    nav.setToPosition(532, -11000.0f, -1990.0f, 0);
    REQUIRE(*nav.mapFile() == "DeadwindPass");
    REQUIRE_FALSE(nav.project(532, -11000.0f, -1990.0f));
    // Azuremyst terrain is map 530; the map opens on Kalimdor.
    nav.setToPosition(530, -4007.31f, -11873.6f, 3524);
    REQUIRE(nav.continent() == 1);
    REQUIRE(*nav.mapFile() == "AzuremystIsle");
}

TEST_CASE("MapNavigator opens battleground standalone map and dynamic landmarks", "[world_map][navigator]") {
    auto d = data();
    d.areas.push_back({2597, 30, 0, 0, "Alterac Valley"});
    d.mapAreas.push_back({401, 30, 2597, "AlteracValley", 1781.2f, -2456.2f, 1085.4f, -1739.6f, -1});
    // Dun Baldar (flags=0x4, area=0, ws=0 -> textureIndex 15 on zone view)
    d.pois.push_back({1328, 6, 0x04, 30, 0, 0, 693.1f, -19.2f, "Dun Baldar", ""});
    // Snowfall Graveyard Alliance Contested (flags=0x84, ws=1341 -> textureIndex 3)
    d.pois.push_back({1341, 3, 0x84, 30, 1341, 0, -202.6f, -112.8f, "Snowfall Graveyard", "In Conflict"});
    // Inactive world-state POI (ws=1342 == 0 -> excluded)
    d.pois.push_back({1342, 4, 0x84, 30, 1342, 0, -202.6f, -112.8f, "Snowfall Graveyard", "Alliance Controlled"});

    MapNavigator nav(std::move(d),
                     [](uint32_t) { return false; },
                     [](uint32_t ws) { return ws == 1341 ? 1u : 0u; },
                     [](const std::string&) { return glm::ivec2(256, 256); });

    nav.setToPosition(30, -202.6f, -112.8f, 2597);
    REQUIRE(nav.continent() == MapNavigator::kCosmic);
    REQUIRE(nav.zone() == 0);
    REQUIRE(nav.mapFile().has_value());
    REQUIRE(*nav.mapFile() == "AlteracValley");
    REQUIRE(nav.mapAreaId() == 401u);

    const auto marks = nav.landmarks();
    REQUIRE(marks.size() == 2);
    REQUIRE(marks[0].name == "Dun Baldar");
    REQUIRE(marks[0].texture == 15);
    REQUIRE(marks[1].name == "Snowfall Graveyard");
    REQUIRE(marks[1].description == "In Conflict");
    REQUIRE(marks[1].texture == 3);
    REQUIRE(marks[1].x > 0.0f);
    REQUIRE(marks[1].x < 1.0f);
    REQUIRE(marks[1].y > 0.0f);
    REQUIRE(marks[1].y < 1.0f);
}

TEST_CASE("MapNavigator opens Arathi Basin and updates 5 base landmarks across Neutral/Contested/Controlled",
          "[world_map][navigator][ab]") {
    auto d = data();
    d.areas.push_back({3358, 529, 0, 0, "Arathi Basin"});
    d.mapAreas.push_back({461, 529, 3358, "ArathiBasin",
                          1858.33325f, 102.08333f, 1508.33325f, 337.5f, -1});
    // 5 neutral AB POIs + Stables Alliance Contested (1769) and Alliance Controlled (1767)
    d.pois.push_back({1614, 26, 0x84, 529, 1846, -1, 988.9f, 1012.6f, "Blacksmith", "Uncontrolled"});
    d.pois.push_back({1629, 31, 0x84, 529, 1845, -1, 824.3f, 826.3f, "Farm", "Uncontrolled"});
    d.pois.push_back({1624, 16, 0x84, 529, 1843, -1, 1178.2f, 822.5f, "Gold Mine", "Uncontrolled"});
    d.pois.push_back({1619, 21, 0x84, 529, 1844, -1, 813.1f, 1182.1f, "Lumber Mill", "Uncontrolled"});
    d.pois.push_back({1609, 36, 0x84, 529, 1842, -1, 1180.9f, 1185.8f, "Stables", "Uncontrolled"});
    d.pois.push_back({1612, 37, 0x84, 529, 1769, -1, 1181.5f, 1185.7f, "Stables", "In Conflict"});
    d.pois.push_back({1610, 38, 0x84, 529, 1767, -1, 1181.6f, 1185.6f, "Stables", "Alliance Controlled"});

    std::unordered_map<uint32_t, uint32_t> ws = {
        {1846, 1}, {1845, 1}, {1843, 1}, {1844, 1}, {1842, 1}
    };
    MapNavigator nav(std::move(d),
                     [](uint32_t) { return false; },
                     [&](uint32_t id) { auto it = ws.find(id); return it != ws.end() ? it->second : 0u; },
                     [](const std::string&) { return glm::ivec2(256, 256); });

    nav.setToPosition(529, 1166.8f, 1200.1f, 3358);
    REQUIRE(nav.continent() == MapNavigator::kCosmic);
    REQUIRE(nav.zone() == 0);
    REQUIRE(nav.mapFile().has_value());
    REQUIRE(*nav.mapFile() == "ArathiBasin");
    REQUIRE(nav.mapAreaId() == 461u);

    auto marks = nav.landmarks();
    REQUIRE(marks.size() == 5);
    // Blacksmith matches Native 2.4.3 measured UV (0.482, 0.444)
    CHECK(marks[0].name == "Blacksmith");
    CHECK(marks[0].texture == 26);
    CHECK(marks[0].x == Catch::Approx(0.4816f).margin(0.002f));
    CHECK(marks[0].y == Catch::Approx(0.4436f).margin(0.002f));
    CHECK(marks[4].name == "Stables");
    CHECK(marks[4].description == "Uncontrolled");
    CHECK(marks[4].texture == 36);

    // Transition Stables: Neutral -> Alliance Contested
    ws[1842] = 0;
    ws[1769] = 1;
    marks = nav.landmarks();
    REQUIRE(marks.size() == 5);
    CHECK(marks[4].name == "Stables");
    CHECK(marks[4].description == "In Conflict");
    CHECK(marks[4].texture == 37);

    // Transition Stables: Alliance Contested -> Alliance Controlled
    ws[1769] = 0;
    ws[1767] = 1;
    marks = nav.landmarks();
    REQUIRE(marks.size() == 5);
    CHECK(marks[4].name == "Stables");
    CHECK(marks[4].description == "Alliance Controlled");
    CHECK(marks[4].texture == 38);
}
