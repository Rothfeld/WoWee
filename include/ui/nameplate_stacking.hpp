#pragma once

#include <algorithm>
#include <cmath>
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

// Plain world name over a unit: own character, a visible plate, the current
// target, or the unit category's name setting (UnitNameNPC and siblings).
// Mouse hover is deliberately absent: build 8606 shows no name over a hovered
// NPC while UnitNameNPC is 0.
inline bool unitWorldNameShown(bool own, bool plate, bool target, bool categorySetting) {
    return own || plate || target || categorySetting;
}

/// Size of a plain world name relative to an ordinary one, from the height of
/// the unit's name attachment over its feet (model scale included).
///
/// Build 8606, one unit morphed through 16 models and scaled 1.7..2.0
/// (pair 7, _experiments/20261118T074350Z.plates_fct namesize.json): names
/// keep the ordinary size up to an attachment height of 3.98 yd (Battle
/// Guard 3.89, Flesh Golem 3.79, human x1.8) and from 4.09 yd on (Crypt
/// Fiend 4.17, human x1.85) grow in proportion to it, 0.3796 per yard
/// (Mountain Giant 8.59 yd: 3.27x). The line under the name grows with it.
inline float unitNameScale(float attachmentHeight) {
    return attachmentHeight > 4.0f ? 0.3796f * attachmentHeight : 1.0f;
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

/// One build 8606 nameplate frame being spread: `x` is its horizontal centre
/// and `top` its top edge in screen pixels (y down), `depth` the anchor's
/// distance along the view axis. `naturalX`/`naturalTop` are where it would
/// be drawn alone.
struct LegacyPlate {
    float naturalX = 0.0f;
    float naturalTop = 0.0f;
    float depth = 0.0f;
    float x = 0.0f;
    float top = 0.0f;
};

/// Build 8606 spreads overlapping plates instead of drawing one over another
/// (pair 7, _experiments/20261118T074350Z.plates_fct: GetPoint of every
/// WorldFrame plate read back against projected anchors to 0.1 UI unit).
///
/// Measured:
/// - two frames collide when their centres are less than a frame width
///   (128 art units) apart horizontally and a frame height (32) vertically;
/// - a moved plate always lands exactly one frame height above or below, or
///   exactly one frame width beside, the plate it collided with;
/// - plates nearly level (tops within the half bar, 10.39 UI units at 768
///   lines, `kLegacyPlateRowTolerance` art units) are set side by side, the
///   right one moving right;
/// - otherwise the nearer plate moves vertically to the nearest free slot
///   above or below the plates in its column; far plates keep their place
///   (two units: the nearer always moved; four units: 18 of 29 views match
///   exactly with far-first placement).
///
/// Not matched (see the experiment README): at close range build 8606 also
/// sets plates side by side when the left one is up to ~0.49*dx - 7 units
/// higher, and at 14 yd it stacked nearly level plates; the rule behind
/// those two was not found.
constexpr float kLegacyPlateRowTolerance = 10.39f * 128.0f / 156.651184f;

inline void legacyPlateLayout(std::vector<LegacyPlate*>& plates, float artScale) {
    const float w = 128.0f * artScale;
    const float h = 32.0f * artScale;
    const float row = kLegacyPlateRowTolerance * artScale;
    std::stable_sort(plates.begin(), plates.end(),
                     [](const LegacyPlate* a, const LegacyPlate* b) { return a->depth > b->depth; });
    // A plate set exactly a frame away does not collide; the margin keeps
    // float rounding of top +- h from reading as an overlap.
    const auto collides = [&](float x, float top, const LegacyPlate* q) {
        return std::abs(x - q->x) < w - 0.01f && std::abs(top - q->top) < h - 0.01f;
    };
    for (size_t i = 0; i < plates.size(); ++i) {
        LegacyPlate& p = *plates[i];
        p.x = p.naturalX;
        p.top = p.naturalTop;
        // Each pass either clears the plate or moves one plate a whole frame
        // width right; the bound only guards a degenerate projection.
        for (size_t pass = 0; pass <= plates.size(); ++pass) {
            LegacyPlate* hit = nullptr;
            for (size_t j = 0; j < i; ++j) {
                if (!collides(p.x, p.top, plates[j])) continue;
                if (!hit || std::abs(plates[j]->top - p.top) < std::abs(hit->top - p.top)) hit = plates[j];
            }
            if (!hit) break;
            if (std::abs(hit->top - p.top) < row) {
                if (p.x >= hit->x) p.x = hit->x + w;
                else hit->x = p.x + w;
                continue;
            }
            float best = p.top;
            float bestMove = -1.0f;
            for (size_t j = 0; j < i; ++j) {
                const LegacyPlate* q = plates[j];
                if (std::abs(p.x - q->x) >= w - 0.01f) continue;
                for (const float candidate : {q->top - h, q->top + h}) {
                    bool free = true;
                    for (size_t k = 0; k < i && free; ++k) free = !collides(p.x, candidate, plates[k]);
                    const float move = std::abs(candidate - p.naturalTop);
                    // Equal distances prefer the slot above (smaller top).
                    if (free && (bestMove < 0.0f || move < bestMove - 0.01f ||
                                 (std::abs(move - bestMove) <= 0.01f && candidate < best))) {
                        best = candidate;
                        bestMove = move;
                    }
                }
            }
            p.top = best;
            break;
        }
    }
    // Pushing a placed plate right can put it onto a third one; level
    // neighbours are pushed on until the row is clear.
    for (size_t pass = 0; pass < plates.size(); ++pass) {
        bool moved = false;
        for (size_t i = 0; i < plates.size(); ++i) {
            for (size_t j = i + 1; j < plates.size(); ++j) {
                LegacyPlate& a = *plates[i];
                LegacyPlate& b = *plates[j];
                if (!collides(a.x, a.top, &b) || std::abs(a.top - b.top) >= row) continue;
                if (a.x <= b.x) b.x = a.x + w;
                else a.x = b.x + w;
                moved = true;
            }
        }
        if (!moved) break;
    }
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
