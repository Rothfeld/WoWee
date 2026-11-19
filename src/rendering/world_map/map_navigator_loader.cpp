// map_navigator_loader.cpp - MapNavigatorData from the client's DBC and ZMP files.
#include "rendering/world_map/map_navigator.hpp"

#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"

#include <algorithm>
#include <cstring>

namespace wowee {
namespace rendering {
namespace world_map {

namespace {

/// The columns one table needs, or nothing (and one warning naming what is
/// missing) when this expansion's dbc_layouts.json does not declare them.
struct Columns {
    const pipeline::DBCFieldMap* map = nullptr;
    bool ok = false;
    uint32_t operator[](const char* name) const { return map->tryField(name); }
};

Columns columns(const char* dbc, std::initializer_list<const char*> names) {
    Columns c;
    const auto* layout = pipeline::getActiveDBCLayout();
    c.map = layout ? layout->getLayout(dbc) : nullptr;
    if (!c.map) {
        LOG_WARNING("MapNavigator: dbc_layouts.json has no ", dbc, " layout; world map ",
                    "data from it is unavailable");
        return c;
    }
    for (const char* n : names) {
        if (c.map->tryField(n) == 0xFFFFFFFFu) {
            LOG_WARNING("MapNavigator: dbc_layouts.json ", dbc, " lacks ", n,
                        "; world map data from it is unavailable");
            return c;
        }
    }
    c.ok = true;
    return c;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return s;
}

}  // namespace

MapNavigatorData loadMapNavigatorData(pipeline::AssetManager& assets) {
    MapNavigatorData d;

    if (auto c = columns("AreaTable", {"ID", "MapID", "ParentAreaNum", "ExploreFlag", "Name"}); c.ok) {
        if (auto dbc = assets.loadDBC("AreaTable.dbc"); dbc && dbc->isLoaded()) {
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                d.areas.push_back({dbc->getUInt32(i, c["ID"]), dbc->getUInt32(i, c["MapID"]),
                                   dbc->getUInt32(i, c["ParentAreaNum"]), dbc->getUInt32(i, c["ExploreFlag"]),
                                   dbc->getString(i, c["Name"])});
            }
        }
    }

    if (auto c = columns("WorldMapArea", {"ID", "MapID", "AreaID", "AreaName", "LocLeft", "LocRight",
                                          "LocTop", "LocBottom", "DisplayMapID"}); c.ok) {
        if (auto dbc = assets.loadDBC("WorldMapArea.dbc"); dbc && dbc->isLoaded()) {
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                d.mapAreas.push_back({dbc->getUInt32(i, c["ID"]), dbc->getUInt32(i, c["MapID"]),
                                      dbc->getUInt32(i, c["AreaID"]), dbc->getString(i, c["AreaName"]),
                                      dbc->getFloat(i, c["LocLeft"]), dbc->getFloat(i, c["LocRight"]),
                                      dbc->getFloat(i, c["LocTop"]), dbc->getFloat(i, c["LocBottom"]),
                                      dbc->getInt32(i, c["DisplayMapID"])});
            }
        }
    }

    if (auto c = columns("WorldMapOverlay", {"MapAreaID", "AreaID0", "AreaID1", "AreaID2", "AreaID3",
                                             "TextureName", "TextureWidth", "TextureHeight", "OffsetX",
                                             "OffsetY", "HitRectTop", "HitRectLeft", "HitRectBottom",
                                             "HitRectRight"}); c.ok) {
        if (auto dbc = assets.loadDBC("WorldMapOverlay.dbc"); dbc && dbc->isLoaded()) {
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                MapNavigatorData::Overlay o;
                o.mapAreaId = dbc->getUInt32(i, c["MapAreaID"]);
                o.areaIds = {dbc->getUInt32(i, c["AreaID0"]), dbc->getUInt32(i, c["AreaID1"]),
                             dbc->getUInt32(i, c["AreaID2"]), dbc->getUInt32(i, c["AreaID3"])};
                o.texture = dbc->getString(i, c["TextureName"]);
                o.width = static_cast<int>(dbc->getUInt32(i, c["TextureWidth"]));
                o.height = static_cast<int>(dbc->getUInt32(i, c["TextureHeight"]));
                o.offsetX = static_cast<int>(dbc->getUInt32(i, c["OffsetX"]));
                o.offsetY = static_cast<int>(dbc->getUInt32(i, c["OffsetY"]));
                o.hitTop = static_cast<int>(dbc->getUInt32(i, c["HitRectTop"]));
                o.hitLeft = static_cast<int>(dbc->getUInt32(i, c["HitRectLeft"]));
                o.hitBottom = static_cast<int>(dbc->getUInt32(i, c["HitRectBottom"]));
                o.hitRight = static_cast<int>(dbc->getUInt32(i, c["HitRectRight"]));
                if (o.texture.empty() || o.width == 0 || o.height == 0) continue;
                d.overlays.push_back(std::move(o));
            }
        }
    }

    if (auto c = columns("AreaPOI", {"ID", "Icon", "Flags", "MapID", "AreaID", "X", "Y", "Name",
                                     "Description", "WorldState"}); c.ok) {
        if (auto dbc = assets.loadDBC("AreaPOI.dbc"); dbc && dbc->isLoaded()) {
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                MapNavigatorData::Poi p;
                p.id = dbc->getUInt32(i, c["ID"]);
                p.icon = dbc->getUInt32(i, c["Icon"]);
                p.flags = dbc->getUInt32(i, c["Flags"]);
                p.mapId = dbc->getUInt32(i, c["MapID"]);
                p.areaId = dbc->getInt32(i, c["AreaID"]);
                p.x = dbc->getFloat(i, c["X"]);
                p.y = dbc->getFloat(i, c["Y"]);
                p.name = dbc->getString(i, c["Name"]);
                p.description = dbc->getString(i, c["Description"]);
                p.worldState = dbc->getUInt32(i, c["WorldState"]);
                d.pois.push_back(std::move(p));
            }
        }
    }

    if (auto c = columns("WorldMapTransforms", {"MapID", "RegionMinX", "RegionMinY", "RegionMaxX",
                                                "RegionMaxY", "NewMapID", "RegionOffsetX",
                                                "RegionOffsetY"}); c.ok) {
        if (auto dbc = assets.loadDBC("WorldMapTransforms.dbc"); dbc && dbc->isLoaded()) {
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                d.transforms.push_back({dbc->getUInt32(i, c["MapID"]), dbc->getUInt32(i, c["NewMapID"]),
                                        dbc->getFloat(i, c["RegionMinX"]), dbc->getFloat(i, c["RegionMinY"]),
                                        dbc->getFloat(i, c["RegionMaxX"]), dbc->getFloat(i, c["RegionMaxY"]),
                                        dbc->getFloat(i, c["RegionOffsetX"]), dbc->getFloat(i, c["RegionOffsetY"])});
            }
        }
    }

    // The continents GetMapContinents lists, in its order. The order is the
    // interface's (SetMapZoom takes the position), and the world sheet is the
    // Azeroth planet: Outland sits beside it on the cosmic sheet instead.
    constexpr uint32_t kContinentOrder[] = {1, 0, 530, 571};
    constexpr uint32_t kAzerothPlanet[] = {1, 0, 571};
    if (auto c = columns("Map", {"ID", "InternalName", "Name", "EntranceMapID", "EntranceX", "EntranceY"}); c.ok) {
        if (auto dbc = assets.loadDBC("Map.dbc"); dbc && dbc->isLoaded()) {
            for (uint32_t mapId : kContinentOrder) {
                for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                    if (dbc->getUInt32(i, c["ID"]) != mapId) continue;
                    const bool hasMap = std::any_of(d.mapAreas.begin(), d.mapAreas.end(), [&](const auto& a) {
                        return a.mapId == mapId && a.areaId == 0;
                    });
                    if (!hasMap) break;
                    d.continents.push_back({mapId, dbc->getString(i, c["Name"])});
                    const std::string zmpPath = "Interface\\WorldMap\\" +
                                                lower(dbc->getString(i, c["InternalName"])) + ".zmp";
                    auto bytes = assets.readFileOptional(zmpPath);
                    if (bytes.size() != 128 * 128 * sizeof(uint32_t)) {
                        LOG_WARNING("MapNavigator: ", zmpPath, " missing or not 128x128; continent hover ",
                                    "and click on map ", mapId, " find no zones");
                        break;
                    }
                    MapNavigatorData::Zmp zmp{mapId, std::vector<uint32_t>(128 * 128)};
                    std::memcpy(zmp.cells.data(), bytes.data(), bytes.size());
                    d.zmps.push_back(std::move(zmp));
                    break;
                }
            }
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                const float x = dbc->getFloat(i, c["EntranceX"]);
                const float y = dbc->getFloat(i, c["EntranceY"]);
                if (x == 0.0f && y == 0.0f) continue;
                d.entrances.push_back({dbc->getUInt32(i, c["ID"]), dbc->getUInt32(i, c["EntranceMapID"]), x, y});
            }
        }
    }

    if (auto c = columns("WorldMapContinent", {"MapID", "LeftBoundary", "RightBoundary", "TopBoundary",
                                               "BottomBoundary", "ContinentOffsetX", "ContinentOffsetY",
                                               "Scale"}); c.ok) {
        if (auto dbc = assets.loadDBC("WorldMapContinent.dbc"); dbc && dbc->isLoaded()) {
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                const uint32_t mapId = dbc->getUInt32(i, c["MapID"]);
                if (std::find(std::begin(kAzerothPlanet), std::end(kAzerothPlanet), mapId) ==
                    std::end(kAzerothPlanet)) continue;
                d.worldSheet.push_back({mapId,
                                        static_cast<int>(dbc->getUInt32(i, c["LeftBoundary"])),
                                        static_cast<int>(dbc->getUInt32(i, c["RightBoundary"])),
                                        static_cast<int>(dbc->getUInt32(i, c["TopBoundary"])),
                                        static_cast<int>(dbc->getUInt32(i, c["BottomBoundary"])),
                                        dbc->getFloat(i, c["ContinentOffsetX"]),
                                        dbc->getFloat(i, c["ContinentOffsetY"]),
                                        dbc->getFloat(i, c["Scale"])});
            }
        }
    }

    LOG_INFO("MapNavigator: ", d.continents.size(), " continents, ", d.mapAreas.size(), " map areas, ",
             d.overlays.size(), " overlays, ", d.pois.size(), " POIs, ", d.worldSheet.size(),
             " world-sheet continents, ", d.entrances.size(), " instance entrances, ",
             d.zmps.size(), " ZMP grids");
    return d;
}

} // namespace world_map
} // namespace rendering
} // namespace wowee
