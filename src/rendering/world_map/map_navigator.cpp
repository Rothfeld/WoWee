// map_navigator.cpp - FrameXML world map state and queries (see map_navigator.hpp).
#include "rendering/world_map/map_navigator.hpp"

#include "core/coordinates.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <stdexcept>

namespace wowee {
namespace rendering {
namespace world_map {

namespace {

using Data = MapNavigatorData;

// AreaPOI.dbc Flags, as the original client filters them. Read off its
// GetMapLandmarkInfo: Goldshire (flags 5) and Echo Ridge Mine (4) are listed on
// Elwynn but not on the continent; Booty Bay (13) is on the continent but not
// the world sheet; Stormwind City and The Exodar (29) are on all three.
constexpr uint32_t kPoiOnZone = 0x04;
constexpr uint32_t kPoiOnContinent = 0x08;
constexpr uint32_t kPoiOnWorld = 0x10;
// World-state POIs (towers, Halaa, "Under Attack"): one row per state, shown
// only while the server reports that row's world state.
constexpr uint32_t kPoiWorldState = 0x80;
// Every zone-level landmark the original lists answers texture index 15, an
// empty cell of POIIcons: the name is shown on hover, the town art is already
// printed on the zone map.
constexpr int kZoneLandmarkTexture = 15;

// ZMP files are 128x128 cells over the 64x64 ADT grid of a map.
constexpr int kZmpSize = 128;
constexpr int kWorldTiles = 64;
// World sheet placement, fitted to the original's UpdateMapHighlight and
// GetMapLandmarkInfo answers at continent 0 (Kalimdor and Eastern Kingdoms,
// highlight rectangles and Stormwind/Exodar positions match to 1e-6): a
// continent's ADT column c lands at x = W/2 + 16*scale*(c - 32) + 16*offsetX,
// rows likewise with offsetY. 16 units per ADT tile at scale 1.
constexpr float kSheetUnitsPerTile = 16.0f;

bool inside(float value, float a, float b) {
    return value >= std::min(a, b) && value <= std::max(a, b);
}

}  // namespace

MapNavigator::MapNavigator(MapNavigatorData data,
                           std::function<bool(uint32_t)> explored,
                           std::function<uint32_t(uint32_t)> worldState,
                           std::function<glm::ivec2(const std::string&)> highlightSize)
    : data_(std::move(data)), explored_(std::move(explored)), worldState_(std::move(worldState)),
      highlightSize_(std::move(highlightSize)) {
    assert(explored_ && worldState_ && highlightSize_);
    for (const auto& c : data_.continents) {
        ContinentEntry entry{c.mapId, c.name, -1, {}};
        for (size_t i = 0; i < data_.mapAreas.size(); ++i) {
            const auto& a = data_.mapAreas[i];
            if (a.areaId == 0 && a.mapId == c.mapId) entry.mapArea = static_cast<int>(i);
        }
        if (entry.mapArea < 0) {
            LOG_WARNING("MapNavigator: continent '", c.name, "' (map ", c.mapId,
                        ") has no WorldMapArea row; left out of the continent list");
            continue;
        }
        for (size_t i = 0; i < data_.mapAreas.size(); ++i) {
            const auto& a = data_.mapAreas[i];
            if (a.areaId != 0 && displayMapOf(a) == c.mapId) entry.zones.push_back(static_cast<int>(i));
        }
        auto name = [this](int i) {
            const auto* row = area(data_.mapAreas[static_cast<size_t>(i)].areaId);
            return row ? row->name : data_.mapAreas[static_cast<size_t>(i)].folder;
        };
        std::sort(entry.zones.begin(), entry.zones.end(),
                  [&](int a, int b) { return name(a) < name(b); });
        continents_.push_back(std::move(entry));
    }
}

const Data::Area* MapNavigator::area(uint32_t id) const {
    for (const auto& a : data_.areas)
        if (a.id == id) return &a;
    return nullptr;
}

bool MapNavigator::areaExplored(uint32_t id) const {
    const auto* a = area(id);
    return a && explored_(a->exploreBit);
}

uint32_t MapNavigator::zoneOfArea(uint32_t areaId) const {
    for (size_t depth = 0; areaId != 0; ++depth) {
        if (depth > data_.areas.size()) throw std::runtime_error("Cycle in AreaTable parent chain");
        const auto* a = area(areaId);
        if (!a) return 0;
        if (a->parentId == 0) return a->id;
        areaId = a->parentId;
    }
    return 0;
}

uint32_t MapNavigator::displayMapOf(const Data::MapArea& a) const {
    return a.displayMapId >= 0 ? static_cast<uint32_t>(a.displayMapId) : a.mapId;
}

glm::vec3 MapNavigator::toDisplayMap(uint32_t mapId, float x, float y) const {
    for (const auto& t : data_.transforms) {
        if (t.sourceMapId == mapId && inside(x, t.minX, t.maxX) && inside(y, t.minY, t.maxY))
            return {static_cast<float>(t.targetMapId), x + t.offsetX, y + t.offsetY};
    }
    return {static_cast<float>(mapId), x, y};
}

glm::vec4 MapNavigator::displayBounds(const Data::MapArea& a) const {
    glm::vec4 b(a.left, a.right, a.top, a.bottom);
    if (displayMapOf(a) == a.mapId) return b;
    const float cx = (a.top + a.bottom) * 0.5f, cy = (a.left + a.right) * 0.5f;
    for (const auto& t : data_.transforms) {
        if (t.sourceMapId != a.mapId || t.targetMapId != displayMapOf(a)) continue;
        if (!inside(cx, t.minX, t.maxX) || !inside(cy, t.minY, t.maxY)) continue;
        return {a.left + t.offsetY, a.right + t.offsetY, a.top + t.offsetX, a.bottom + t.offsetX};
    }
    LOG_WARNING("MapNavigator: no WorldMapTransforms row moves '", a.folder, "' from map ",
                a.mapId, " onto map ", displayMapOf(a));
    return b;
}

uint32_t MapNavigator::zmpArea(uint32_t mapId, float x, float y) const {
    for (const auto& z : data_.zmps) {
        if (z.mapId != mapId) continue;
        const float col = (32.0f - y / core::coords::TILE_SIZE) / kWorldTiles * kZmpSize;
        const float row = (32.0f - x / core::coords::TILE_SIZE) / kWorldTiles * kZmpSize;
        if (col < 0 || row < 0 || col >= kZmpSize || row >= kZmpSize) return 0;
        return z.cells[static_cast<size_t>(row) * kZmpSize + static_cast<size_t>(col)];
    }
    return 0;
}

int MapNavigator::continentIndexForMap(uint32_t mapId) const {
    for (size_t i = 0; i < continents_.size(); ++i)
        if (continents_[i].mapId == mapId) return static_cast<int>(i) + 1;
    return 0;
}

const Data::MapArea* MapNavigator::shownArea() const {
    if (standaloneMapArea_ >= 0) return &data_.mapAreas[static_cast<size_t>(standaloneMapArea_)];
    if (continent_ <= 0) return nullptr;
    const auto& c = continents_[static_cast<size_t>(continent_) - 1];
    const int index = zone_ == 0 ? c.mapArea : c.zones[static_cast<size_t>(zone_) - 1];
    return &data_.mapAreas[static_cast<size_t>(index)];
}

void MapNavigator::setZoom(int continent, int zone) {
    standaloneMapArea_ = -1;
    if (continent <= kCosmic) { continent_ = kCosmic; zone_ = 0; return; }
    if (continent == kWorld || continent > static_cast<int>(continents_.size())) {
        continent_ = kWorld; zone_ = 0; return;
    }
    continent_ = continent;
    const int count = static_cast<int>(continents_[static_cast<size_t>(continent) - 1].zones.size());
    zone_ = (zone >= 1 && zone <= count) ? zone : 0;
}

void MapNavigator::setToPosition(uint32_t mapId, float x, float y, uint32_t zoneAreaId) {
    // An instance is shown where its entrance is, as the original does: inside
    // Karazhan it opens Deadwind Pass.
    for (const auto& e : data_.entrances) {
        if (e.mapId != mapId || (e.x == 0 && e.y == 0)) continue;
        mapId = e.entranceMapId;
        x = e.x;
        y = e.y;
        zoneAreaId = zoneOfArea(zmpArea(mapId, x, y));
        break;
    }
    const glm::vec3 shown = toDisplayMap(mapId, x, y);
    const int c = continentIndexForMap(static_cast<uint32_t>(shown.x));
    if (c == 0) {
        // A map with its own WorldMapArea row and no continent (e.g. battlegrounds):
        // build 8606 reports GetCurrentMapContinent() == -1, GetCurrentMapZone() == 0,
        // and GetMapInfo() == that row's folder ("AlteracValley", "WarsongGulch", ...).
        int best = -1;
        for (size_t i = 0; i < data_.mapAreas.size(); ++i) {
            if (data_.mapAreas[i].mapId != mapId) continue;
            if (best < 0) best = static_cast<int>(i);
            if (zoneAreaId != 0 && data_.mapAreas[i].areaId == zoneAreaId) {
                best = static_cast<int>(i);
                break;
            }
        }
        if (best >= 0) {
            continent_ = kCosmic;
            zone_ = 0;
            standaloneMapArea_ = best;
            return;
        }
        setZoom(kWorld);
        return;
    }
    const auto& zones = continents_[static_cast<size_t>(c) - 1].zones;
    for (size_t i = 0; i < zones.size(); ++i) {
        if (zoneAreaId != 0 && data_.mapAreas[static_cast<size_t>(zones[i])].areaId == zoneAreaId) {
            setZoom(c, static_cast<int>(i) + 1);
            return;
        }
    }
    setZoom(c, 0);
}

void MapNavigator::zoomOut() {
    if (standaloneMapArea_ >= 0) return;
    if (continent_ > 0 && zone_ != 0) { zone_ = 0; return; }
    if (continent_ > 0) {
        const uint32_t mapId = continents_[static_cast<size_t>(continent_) - 1].mapId;
        const bool onSheet = std::any_of(data_.worldSheet.begin(), data_.worldSheet.end(),
                                         [&](const auto& s) { return s.mapId == mapId; });
        setZoom(onSheet ? kWorld : kCosmic);
        return;
    }
    setZoom(kCosmic);
}

bool MapNavigator::click(float u, float v) {
    if (continent_ == kWorld) {
        for (const auto& s : data_.worldSheet) {
            const glm::vec4 r = worldSheetRect(s);
            if (u >= r.x && u <= r.x + r.z && v >= r.y && v <= r.y + r.w) {
                setZoom(continentIndexForMap(s.mapId));
                return true;
            }
        }
        return false;
    }
    if (continent_ <= 0 || zone_ != 0) return false;
    const auto& c = continents_[static_cast<size_t>(continent_) - 1];
    const glm::vec4 b = displayBounds(data_.mapAreas[static_cast<size_t>(c.mapArea)]);
    const float y = b.x - u * (b.x - b.y), x = b.z - v * (b.z - b.w);
    const uint32_t zoneArea = zoneOfArea(zmpArea(c.mapId, x, y));
    for (size_t i = 0; zoneArea != 0 && i < c.zones.size(); ++i) {
        if (data_.mapAreas[static_cast<size_t>(c.zones[i])].areaId == zoneArea) {
            zone_ = static_cast<int>(i) + 1;
            return true;
        }
    }
    return false;
}

bool MapNavigator::showMapArea(uint32_t mapAreaId) {
    for (size_t c = 0; c < continents_.size(); ++c) {
        const auto& entry = continents_[c];
        if (data_.mapAreas[static_cast<size_t>(entry.mapArea)].id == mapAreaId) {
            setZoom(static_cast<int>(c) + 1);
            return true;
        }
        for (size_t z = 0; z < entry.zones.size(); ++z) {
            if (data_.mapAreas[static_cast<size_t>(entry.zones[z])].id == mapAreaId) {
                setZoom(static_cast<int>(c) + 1, static_cast<int>(z) + 1);
                return true;
            }
        }
    }
    for (size_t i = 0; i < data_.mapAreas.size(); ++i) {
        if (data_.mapAreas[i].id == mapAreaId) {
            continent_ = kCosmic;
            zone_ = 0;
            standaloneMapArea_ = static_cast<int>(i);
            return true;
        }
    }
    return false;
}

std::optional<std::string> MapNavigator::mapFile() const {
    const auto* a = shownArea();
    if (!a) return std::nullopt;
    return a->folder;
}

uint32_t MapNavigator::mapAreaId() const {
    const auto* a = shownArea();
    return a ? a->id : 0;
}

std::vector<std::string> MapNavigator::continentNames() const {
    std::vector<std::string> out;
    for (const auto& c : continents_) out.push_back(c.name);
    return out;
}

std::vector<std::string> MapNavigator::zoneNames(int continent) const {
    std::vector<std::string> out;
    if (continent < 1 || continent > static_cast<int>(continents_.size())) return out;
    for (int index : continents_[static_cast<size_t>(continent) - 1].zones) {
        const auto& a = data_.mapAreas[static_cast<size_t>(index)];
        const auto* row = area(a.areaId);
        out.push_back(row ? row->name : a.folder);
    }
    return out;
}

glm::vec4 MapNavigator::worldSheetRect(const Data::Continent& c) const {
    const float unit = kSheetUnitsPerTile * c.scale;
    const float x = kFrameWidth * 0.5f + unit * (c.left - kWorldTiles / 2) + kSheetUnitsPerTile * c.offsetX;
    const float y = kFrameHeight * 0.5f + unit * (c.top - kWorldTiles / 2) + kSheetUnitsPerTile * c.offsetY;
    return {x / kFrameWidth, y / kFrameHeight,
            unit * (c.right - c.left + 1) / kFrameWidth, unit * (c.bottom - c.top + 1) / kFrameHeight};
}

std::optional<glm::vec2> MapNavigator::worldSheetPoint(uint32_t mapId, float x, float y) const {
    const glm::vec3 shown = toDisplayMap(mapId, x, y);
    for (const auto& s : data_.worldSheet) {
        if (s.mapId != static_cast<uint32_t>(shown.x)) continue;
        const float unit = kSheetUnitsPerTile * s.scale;
        const float col = kWorldTiles / 2 - shown.z / core::coords::TILE_SIZE;
        const float row = kWorldTiles / 2 - shown.y / core::coords::TILE_SIZE;
        const float px = kFrameWidth * 0.5f + unit * (col - kWorldTiles / 2) + kSheetUnitsPerTile * s.offsetX;
        const float py = kFrameHeight * 0.5f + unit * (row - kWorldTiles / 2) + kSheetUnitsPerTile * s.offsetY;
        return glm::vec2(px / kFrameWidth, py / kFrameHeight);
    }
    return std::nullopt;
}

MapNavigator::Highlight MapNavigator::highlight(float u, float v) const {
    Highlight h;
    if (u < 0 || u > 1 || v < 0 || v > 1) return h;
    if (continent_ == kWorld) {
        for (const auto& s : data_.worldSheet) {
            const glm::vec4 r = worldSheetRect(s);
            if (u < r.x || u > r.x + r.z || v < r.y || v > r.y + r.w) continue;
            const int c = continentIndexForMap(s.mapId);
            if (c == 0) continue;
            const auto& entry = continents_[static_cast<size_t>(c) - 1];
            const float tilesW = static_cast<float>(s.right - s.left + 1);
            const float tilesH = static_cast<float>(s.bottom - s.top + 1);
            const float side = std::max(tilesW, tilesH);
            h.name = entry.name;
            h.file = data_.mapAreas[static_cast<size_t>(entry.mapArea)].folder;
            h.texU = tilesW / side;
            h.texV = tilesH / side;
            h.width = r.z; h.height = r.w; h.x = r.x; h.y = r.y;
            return h;
        }
        return h;
    }
    if (standaloneMapArea_ >= 0) return h;
    const auto* shown = shownArea();
    if (!shown) return h;
    const auto& c = continents_[static_cast<size_t>(continent_) - 1];
    const glm::vec4 b = displayBounds(*shown);
    const float y = b.x - u * (b.x - b.y), x = b.z - v * (b.z - b.w);
    const uint32_t zoneArea = zoneOfArea(zmpArea(c.mapId, x, y));

    if (zone_ != 0) {
        // Within the zone shown: the explored subzone art under the point, named
        // after its first area. Over a neighbouring zone: that zone's name.
        if (zoneArea != 0 && zoneArea != shown->areaId) {
            if (const auto* a = area(zoneArea)) h.name = a->name;
            return h;
        }
        const float px = u * kFrameWidth, py = v * kFrameHeight;
        const Data::Overlay* best = nullptr;
        for (const auto& o : data_.overlays) {
            if (o.mapAreaId != shown->id || o.hitRight <= o.hitLeft || o.hitBottom <= o.hitTop) continue;
            if (px < o.hitLeft || px > o.hitRight || py < o.hitTop || py > o.hitBottom) continue;
            if (!std::any_of(o.areaIds.begin(), o.areaIds.end(),
                             [&](uint32_t id) { return id != 0 && areaExplored(id); })) continue;
            const auto size = [](const Data::Overlay& r) { return (r.hitRight - r.hitLeft) * (r.hitBottom - r.hitTop); };
            if (!best || size(o) < size(*best)) best = &o;
        }
        if (best) {
            if (const auto* a = area(best->areaIds[0])) h.name = a->name;
        }
        return h;
    }

    for (int index : c.zones) {
        const auto& z = data_.mapAreas[static_cast<size_t>(index)];
        if (z.areaId != zoneArea || zoneArea == 0) continue;
        const auto* a = area(z.areaId);
        h.name = a ? a->name : z.folder;
        const glm::vec2 texture = highlightSize_(z.folder);
        if (texture.x <= 0 || texture.y <= 0) {
            LOG_ERROR("MapNavigator: highlight art for '", z.folder, "' unavailable");
            return h;
        }
        const glm::vec4 zb = displayBounds(z);
        const float spanX = b.x - b.y, spanY = b.z - b.w;
        const float width = zb.x - zb.y, height = zb.z - zb.w;
        const float scale = std::min(texture.x / width, texture.y / height);
        h.file = z.folder;
        h.texU = std::floor(width * scale + 0.001f) / texture.x;
        h.texV = std::floor(height * scale + 0.001f) / texture.y;
        h.width = width / spanX;
        h.height = height / spanY;
        h.x = (b.x - zb.x) / spanX;
        h.y = (b.z - zb.z) / spanY;
        return h;
    }
    return h;
}

std::vector<MapNavigator::Landmark> MapNavigator::landmarks() const {
    std::vector<Landmark> out;
    if (standaloneMapArea_ < 0 && continent_ == kCosmic) return out;
    const uint32_t flag = standaloneMapArea_ >= 0
        ? kPoiOnZone
        : (continent_ == kWorld ? kPoiOnWorld : (zone_ == 0 ? kPoiOnContinent : kPoiOnZone));
    const bool isZoneView = (zone_ != 0 || standaloneMapArea_ >= 0);
    for (const auto& p : data_.pois) {
        if (!(p.flags & flag)) continue;
        if (p.flags & kPoiWorldState) {
            if (p.worldState == 0 || worldState_(p.worldState) == 0) continue;
        } else if (standaloneMapArea_ >= 0) {
            // Battleground base landmarks (Dun Baldar, Frostwolf Keep) carry areaId == 0
            // and flags == 0x4; show them without an exploration-bit check.
            if (p.areaId > 0 && !areaExplored(static_cast<uint32_t>(p.areaId))) continue;
        } else if (p.areaId <= 0 || !areaExplored(static_cast<uint32_t>(p.areaId))) {
            continue;
        }
        std::optional<glm::vec2> uv;
        if (standaloneMapArea_ < 0 && continent_ == kWorld) {
            uv = worldSheetPoint(p.mapId, p.x, p.y);
        } else {
            uv = project(p.mapId, p.x, p.y);
        }
        if (!uv) continue;
        Landmark l;
        l.name = p.name;
        l.description = p.description;
        l.texture = (isZoneView && !(p.flags & kPoiWorldState)) ? kZoneLandmarkTexture
                                                                : static_cast<int>(p.icon);
        l.x = uv->x;
        l.y = uv->y;
        out.push_back(std::move(l));
    }
    return out;
}

std::vector<MapNavigator::OverlayInfo> MapNavigator::overlays() const {
    std::vector<OverlayInfo> out;
    const auto* shown = shownArea();
    if (!shown || (zone_ == 0 && standaloneMapArea_ < 0)) return out;
    for (const auto& o : data_.overlays) {
        if (o.mapAreaId != shown->id) continue;
        if (!std::any_of(o.areaIds.begin(), o.areaIds.end(),
                         [&](uint32_t id) { return id != 0 && areaExplored(id); })) continue;
        out.push_back({"Interface\\WorldMap\\" + shown->folder + "\\" + o.texture,
                       o.width, o.height, o.offsetX, o.offsetY});
    }
    return out;
}

std::optional<glm::vec2> MapNavigator::project(uint32_t mapId, float x, float y) const {
    if (standaloneMapArea_ >= 0) {
        const auto* shown = shownArea();
        if (!shown || shown->mapId != mapId) return std::nullopt;
        const glm::vec4 b(shown->left, shown->right, shown->top, shown->bottom);
        const glm::vec2 uv((b.x - y) / (b.x - b.y), (b.z - x) / (b.z - b.w));
        if (uv.x < 0 || uv.x > 1 || uv.y < 0 || uv.y > 1) return std::nullopt;
        return uv;
    }
    if (continent_ == kCosmic) return std::nullopt;
    if (continent_ == kWorld) return worldSheetPoint(mapId, x, y);
    const auto* shown = shownArea();
    const glm::vec3 p = toDisplayMap(mapId, x, y);
    if (static_cast<uint32_t>(p.x) != continents_[static_cast<size_t>(continent_) - 1].mapId) return std::nullopt;
    const glm::vec4 b = displayBounds(*shown);
    const glm::vec2 uv((b.x - p.z) / (b.x - b.y), (b.z - p.y) / (b.z - b.w));
    if (uv.x < 0 || uv.x > 1 || uv.y < 0 || uv.y > 1) return std::nullopt;
    return uv;
}

} // namespace world_map
} // namespace rendering
} // namespace wowee
