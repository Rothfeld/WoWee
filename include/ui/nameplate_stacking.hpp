#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace wowee {
namespace ui {

// Eligibility uses the original projected anchor, before artwork clamping.
// Once it leaves the viewport, legacy clients draw the unit's world name.
inline bool legacyPlateAnchorVisible(float x, float y, float z, float w) {
    return w > 0.01f && z >= 0.0f && z <= w &&
           x >= -w && x <= w && y >= -w && y <= w;
}

inline float legacyPlateCenterX(float x, float viewportWidth, float artScale) {
    const float halfWidth = 64.0f * artScale;
    return std::clamp(x, halfWidth, std::max(halfWidth, viewportWidth - halfWidth));
}

inline float legacyPlateBarTop(float y, float artScale) {
    // Build 8606 WorldFrame measurement: a 39.162796-unit frame clamps its
    // top 58.744141 units below the viewport top (1.5 frame heights).
    return std::max(y, (1.5f * 32.0f + 19.0f) * artScale);
}

/// One nameplate's screen box: its bar plus the name line above it.
struct PlateBox {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
};

/// Where a plate's bar goes so it clears every plate already placed.
///
/// Allow Nameplate Overlap, turned off, means two units at the same distance
/// must not print one bar through another. A plate that would land on one
/// already placed is lifted above it and asked again, because the place it
/// moved to may be taken as well.
///
/// Returns the bar's top edge. Callers place nearest first, so the plate that
/// moves is the one further away.
///
/// The guard is not decoration. Each lift moves strictly upward, so the loop
/// ends on any sane input - but "sane" here means boxes produced by a camera
/// projection, and a degenerate frame that put forty plates on one pixel would
/// otherwise walk the whole list forty times before drawing anything.
inline float plateTopClearOf(const std::vector<PlateBox>& placed,
                             float x0, float x1, float y,
                             float barHeight, float topExtent,
                             int maxLifts = 24) {
    for (int guard = 0; guard < maxLifts; ++guard) {
        bool lifted = false;
        for (const auto& r : placed) {
            const bool xHit = (x0 - 2.0f) < r.x1 && (x1 + 2.0f) > r.x0;
            const bool yHit = (y - topExtent) < r.y1 && (y + barHeight) > r.y0;
            if (xHit && yHit) {
                y = r.y0 - barHeight - 2.0f;
                lifted = true;
                break;
            }
        }
        if (!lifted) break;
    }
    return y;
}

/// A drawn plate's click box (name line, bar and their margin) and its unit.
struct PlateHit {
    uint64_t guid = 0;
    PlateBox box;
};

/// The unit whose plate is under the pointer, or 0. Plates are listed in draw
/// order, so the last one containing the point is the one drawn on top.
///
/// The plate is screen UI in front of the world: a click on it means its unit,
/// never whatever the world ray behind it hits or the current target.
inline uint64_t plateUnitAt(const std::vector<PlateHit>& plates, float x, float y) {
    for (auto it = plates.rbegin(); it != plates.rend(); ++it) {
        const PlateBox& b = it->box;
        if (x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1) return it->guid;
    }
    return 0;
}

} // namespace ui
} // namespace wowee
