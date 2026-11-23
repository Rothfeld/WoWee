#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <glm/glm.hpp>

namespace wowee::ui {

/// The minimap draws the world rotated around the player and scaled from yards
/// to pixels. Both directions of that transform live here because the client
/// needs both: markers project outward, clicks invert back.
///
/// Offsets are minimap pixels measured from its centre with **+y down**, which
/// is ImGui's screen space and the space the marker code already worked in.
/// Deltas are render space, relative to the player.
struct MinimapView {
    /// Yards the minimap covers, centre to edge.
    float viewRadius = 100.0f;
    /// Half the minimap's on-screen width.
    float mapRadius = 70.0f;
    /// Clockwise camera bearing from north, pre-resolved. Both are 1/0 when the
    /// map does not rotate with the camera, which is this client's default.
    float cosBearing = 1.0f;
    float sinBearing = 0.0f;
};

/// Render-space delta from the player to an offset in minimap pixels.
///
/// Exact inverse of the minimap display shader:
///   mapUV = playerUV + vec2(rotated.y, -rotated.x) * zoom * 2
/// where rotated = R(bearing) * vec2(-center.x, center.y). Render +X is west
/// and +Y is north, while composite UV grows east/south - hence the negations.
inline glm::vec2 renderDeltaToMinimapOffset(float dx, float dy, const MinimapView& v) {
    const float rx = -(dy * v.cosBearing - dx * v.sinBearing);
    const float ry = -(dy * v.sinBearing + dx * v.cosBearing);
    if (v.viewRadius <= 0.0f) return {0.0f, 0.0f};
    return {rx / v.viewRadius * v.mapRadius, ry / v.viewRadius * v.mapRadius};
}

/// An offset in minimap pixels back to a render-space delta from the player.
inline glm::vec2 minimapOffsetToRenderDelta(float px, float py, const MinimapView& v) {
    if (v.mapRadius <= 0.0f) return {0.0f, 0.0f};
    const float rx = px / v.mapRadius * v.viewRadius;
    const float ry = py / v.mapRadius * v.viewRadius;
    const float oldRx = -rx;
    const float rotX = oldRx * v.cosBearing - ry * v.sinBearing;
    const float rotY = oldRx * v.sinBearing + ry * v.cosBearing;
    return {-rotY, rotX};
}

/// Distance inside the minimap rim where rotating direction arrows sit
/// (fitted to native 2.4.3 town/guide arrows at 1280x720).
inline constexpr float kMinimapRimArrowInset = 13.5f;

/// Result of placing a world POI onto the minimap disc or its rim.
struct MinimapPoiPlacement {
    /// True when the POI falls inside the minimap disc (draw icon at `pos`);
    /// false when outside (draw directional rim arrow at `pos` along `direction`).
    bool onMap = false;
    /// Screen-space centre of the icon or rim arrow.
    glm::vec2 pos{0.0f, 0.0f};
    /// Unit screen-space direction from minimap centre toward the POI
    /// (populated when `onMap` is false).
    glm::vec2 direction{0.0f, 0.0f};
};

/// Project a POI's render-space delta from the player onto the minimap disc,
/// or clamp it to a directional rim arrow position when outside the zoom radius.
inline MinimapPoiPlacement placeMinimapPoi(const glm::vec2& renderDelta,
                                           float centerX, float centerY,
                                           const MinimapView& view,
                                           float uiScale = 1.0f) {
    const glm::vec2 off = renderDeltaToMinimapOffset(renderDelta.x, renderDelta.y, view);
    const float dist = std::hypot(off.x, off.y);
    if (dist <= view.mapRadius - 3.0f) {
        return {true, {centerX + off.x, centerY + off.y}, {0.0f, 0.0f}};
    }
    if (dist <= 0.0f) {
        return {true, {centerX, centerY}, {0.0f, 0.0f}};
    }
    const glm::vec2 dir = off / dist;
    const float rimRadius = view.mapRadius - kMinimapRimArrowInset * uiScale;
    return {false, {centerX + dir.x * rimRadius, centerY + dir.y * rimRadius}, dir};
}

/// Whether an AreaPOI town rim arrow should be drawn for the player's current
/// area/zone. Suppresses the current city/area's own AreaPOI arrow (matching
/// Native TBC 2.4.3): capital cities in AreaPOI.dbc carry their surrounding
/// outdoor zone's AreaID (e.g. Stormwind City -> Elwynn Forest 12) so they
/// show in the outdoor zone and suppress once inside the city (zone 1519),
/// while same-zone city POIs (e.g. Shattrath City 3703) suppress when the
/// player is already in that zone.
inline bool shouldShowAreaPoiRimArrow(uint32_t poiAreaId,
                                      uint32_t poiZoneId,
                                      uint32_t currentAreaId,
                                      uint32_t currentZoneId,
                                      const std::string& poiName,
                                      const std::string& currentZoneName) {
    if (currentAreaId != 0 && poiAreaId == currentAreaId) {
        if (poiAreaId != currentZoneId || poiName == currentZoneName) {
            return false;
        }
    }
    if (currentZoneId != 0 && poiZoneId != 0 && poiZoneId != currentZoneId) {
        return false;
    }
    if (currentZoneId != 0 && poiAreaId == currentZoneId && !poiName.empty() && poiName == currentZoneName) {
        return false;
    }
    return true;
}

/// Top-down Light.dbc terrain colour multiplier for outdoor minimap tiles in
/// TBC 2.4.3 (normal = +Z against canonical top-down light z = -0.9, clamped
/// to [0, 1] so baked daytime BLP tiles darken and cool at night without
/// over-blowing daytime highlights).
inline glm::vec3 computeMinimapTerrainTint(const glm::vec3& ambientColor,
                                           const glm::vec3& diffuseColor) {
    constexpr float kTopDownNdotL = 0.9f;
    return glm::clamp(ambientColor + kTopDownNdotL * diffuseColor,
                      glm::vec3(0.0f), glm::vec3(1.0f));
}

}  // namespace wowee::ui
