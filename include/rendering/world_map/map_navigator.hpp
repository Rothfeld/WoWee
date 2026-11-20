// map_navigator.hpp - The world map as the original FrameXML map API sees it.
//
// WorldMapFrame (TBC 2.4.3) draws the whole map itself - detail tiles, explored
// overlays, landmarks, highlight, party markers - and only asks the client what
// is being shown and what lies under a point. This class answers those questions
// for every continent at once, from the client's DBC data, independently of
// which map the client's own ImGui map window has loaded.
//
// View levels and their API numbers, as GetCurrentMapContinent answers them:
//   -1  cosmic (Azeroth and Outland side by side; FrameXML's own buttons)
//    0  world  (the Azeroth continents on one sheet)
//    1+ a continent, in GetMapContinents order; zone 0 is the continent map
//       itself and zone N is row N of GetMapZones.
//
// Positions are in the DBC/server convention, X north and Y west, as AreaPOI
// and WorldMapArea store them. This client's "canonical" coordinates have the
// two exchanged; callers convert.
//
// Rules here were read off the original client through its own Lua API (see
// the world map section of the README): landmarks by AreaPOI flag and explored
// area, zone-level landmark texture 15, world-sheet placement from
// WorldMapContinent, entrance-based zone for instances.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {
namespace world_map {

/// The DBC rows the navigator works from. Plain data, so tests can build it.
struct MapNavigatorData {
    struct Area {            // AreaTable
        uint32_t id = 0, mapId = 0, parentId = 0, exploreBit = 0;
        std::string name;
    };
    struct MapArea {         // WorldMapArea
        uint32_t id = 0, mapId = 0, areaId = 0;
        std::string folder;  // texture folder, e.g. "Elwynn"
        // Canonical yards: left/right are Y (west), top/bottom are X (north).
        float left = 0, right = 0, top = 0, bottom = 0;
        int32_t displayMapId = -1;   // -1: shown on its own map
    };
    struct Overlay {         // WorldMapOverlay
        uint32_t mapAreaId = 0;
        std::array<uint32_t, 4> areaIds{};
        std::string texture;         // e.g. "NORTHSHIREVALLEY"
        int width = 0, height = 0, offsetX = 0, offsetY = 0;
        int hitTop = 0, hitLeft = 0, hitBottom = 0, hitRight = 0;
    };
    struct Poi {             // AreaPOI
        uint32_t id = 0, icon = 0, flags = 0, mapId = 0, worldState = 0;
        int32_t areaId = 0;
        float x = 0, y = 0;
        std::string name, description;
    };
    struct Continent {       // WorldMapContinent: placement on the world sheet
        uint32_t mapId = 0;
        int left = 0, right = 0, top = 0, bottom = 0;   // ADT tile range
        float offsetX = 0, offsetY = 0, scale = 1;
    };
    struct Transform {       // WorldMapTransforms
        uint32_t sourceMapId = 0, targetMapId = 0;
        float minX = 0, minY = 0, maxX = 0, maxY = 0;
        float offsetX = 0, offsetY = 0;
    };
    struct Entrance {        // Map.dbc ghost entrance of an instance
        uint32_t mapId = 0, entranceMapId = 0;
        float x = 0, y = 0;
    };
    struct ContinentName { uint32_t mapId = 0; std::string name; };
    struct Zmp { uint32_t mapId = 0; std::vector<uint32_t> cells; };  // 128x128 AreaTable ids

    std::vector<Area> areas;
    std::vector<MapArea> mapAreas;
    std::vector<Overlay> overlays;
    std::vector<Poi> pois;
    std::vector<Continent> worldSheet;      // continents drawn on the world (Azeroth) sheet
    std::vector<Transform> transforms;
    std::vector<Entrance> entrances;
    std::vector<ContinentName> continents;  // GetMapContinents order
    std::vector<Zmp> zmps;
};

/// Read MapNavigatorData from the client's data files.
MapNavigatorData loadMapNavigatorData(pipeline::AssetManager& assets);

class MapNavigator {
public:
    static constexpr int kCosmic = -1;
    static constexpr int kWorld = 0;
    /// The world map frame's detail area, in the units FrameXML lays it out in.
    static constexpr float kFrameWidth = 1002.0f;
    static constexpr float kFrameHeight = 668.0f;

    struct Highlight {
        std::optional<std::string> name;
        std::optional<std::string> file;
        float texU = 0, texV = 0, width = 0, height = 0, x = 0, y = 0;
    };
    struct Landmark {
        std::string name, description;
        int texture = 0;
        float x = 0, y = 0;
    };
    struct OverlayInfo {
        std::string texture;   // full path prefix, tiles are texture1..N
        int width = 0, height = 0, offsetX = 0, offsetY = 0;
    };

    /// explored(bit) answers PLAYER_EXPLORED_ZONES; worldState(id) the server's
    /// world state value (0 when unset); highlightSize(folder) the pixel size of
    /// Interface\WorldMap\<folder>\<folder>Highlight.
    MapNavigator(MapNavigatorData data,
                 std::function<bool(uint32_t)> explored,
                 std::function<uint32_t(uint32_t)> worldState,
                 std::function<glm::ivec2(const std::string&)> highlightSize);

    [[nodiscard]] int continent() const { return continent_; }
    [[nodiscard]] int zone() const { return zone_; }

    /// SetMapZoom(continent [, zone]). A zone row past the end shows the continent.
    void setZoom(int continent, int zone = 0);
    /// SetMapToCurrentZone, from where the player is and the zone (AreaTable id)
    /// the terrain under them belongs to (0 when unknown).
    void setToPosition(uint32_t mapId, float x, float y, uint32_t zoneAreaId);
    /// One level out, as the zoom-out button and right click do.
    void zoomOut();
    /// ProcessMapClick: world -> continent, continent -> zone. Zones do nothing.
    bool click(float u, float v);
    /// SetMapByID / the quest log: show a WorldMapArea row.
    bool showMapArea(uint32_t mapAreaId);

    /// GetMapInfo's file name; nullopt at world and cosmic level.
    [[nodiscard]] std::optional<std::string> mapFile() const;
    /// The WorldMapArea row shown, 0 at world and cosmic level.
    [[nodiscard]] uint32_t mapAreaId() const;

    [[nodiscard]] std::vector<std::string> continentNames() const;
    [[nodiscard]] std::vector<std::string> zoneNames(int continent) const;

    [[nodiscard]] Highlight highlight(float u, float v) const;
    [[nodiscard]] std::vector<Landmark> landmarks() const;
    [[nodiscard]] std::vector<OverlayInfo> overlays() const;
    /// Where a canonical position on mapId falls on the shown map, [0,1]^2.
    [[nodiscard]] std::optional<glm::vec2> project(uint32_t mapId, float x, float y) const;

private:
    struct ContinentEntry { uint32_t mapId; std::string name; int mapArea; std::vector<int> zones; };

    [[nodiscard]] const MapNavigatorData::MapArea* shownArea() const;
    [[nodiscard]] const MapNavigatorData::Area* area(uint32_t id) const;
    [[nodiscard]] bool areaExplored(uint32_t id) const;
    [[nodiscard]] uint32_t zoneOfArea(uint32_t areaId) const;
    [[nodiscard]] uint32_t zmpArea(uint32_t mapId, float x, float y) const;
    /// A position moved onto the map it is displayed on (Eversong onto Azeroth).
    [[nodiscard]] glm::vec3 toDisplayMap(uint32_t mapId, float x, float y) const;
    /// Bounds of a WorldMapArea on its display map.
    [[nodiscard]] glm::vec4 displayBounds(const MapNavigatorData::MapArea& a) const;
    [[nodiscard]] uint32_t displayMapOf(const MapNavigatorData::MapArea& a) const;
    [[nodiscard]] std::optional<glm::vec2> worldSheetPoint(uint32_t mapId, float x, float y) const;
    [[nodiscard]] glm::vec4 worldSheetRect(const MapNavigatorData::Continent& c) const;
    [[nodiscard]] int continentIndexForMap(uint32_t mapId) const;

    MapNavigatorData data_;
    std::function<bool(uint32_t)> explored_;
    std::function<uint32_t(uint32_t)> worldState_;
    std::function<glm::ivec2(const std::string&)> highlightSize_;
    std::vector<ContinentEntry> continents_;
    int continent_ = 0;
    int zone_ = 0;
    /// Index into data_.mapAreas when showing a standalone map with no continent
    /// (e.g. battlegrounds: AV, WSG, AB, EotS). Native reports continent=-1, zone=0.
    int standaloneMapArea_ = -1;
};

} // namespace world_map
} // namespace rendering
} // namespace wowee
