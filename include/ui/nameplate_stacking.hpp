#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
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

/// PLAYER_FLAGS bit 0x08 (PLAYER_FLAG_GM): GM tag visible on 3D overhead names.
constexpr uint32_t kPlayerFlagGm = 0x08u;

/// Format a player's 3D world overhead name, prepending "<GM>" when
/// PLAYER_FLAGS bit 0x08 (PLAYER_FLAG_GM) is set (matching Native TBC 2.4.3).
inline std::string formatPlayerWorldName(const std::string& name, uint32_t playerFlags, bool isPlate) {
    if (name.empty()) return name;
    if (!isPlate && (playerFlags & kPlayerFlagGm) != 0) {
        return "<GM>" + name;
    }
    return name;
}

/// Perspective-projected 3D world unit overhead name font size in pixels.
/// Uses the camera's vertical projection scale and clip-space depth (avoiding
/// near-plane W blowups from projecting an offset point) and caps close-range
/// growth proportionally to viewport height so melee-range units do not explode
/// into oversized billboards.
inline float worldNameFontSize(float projY, float clipW, float screenH, float nameScale) {
    const float safeW = std::max(clipW, 0.01f);
    const float scale = std::max(1.0f, nameScale);
    const float rawPx = std::abs(projY) * (0.20f * scale) * screenH * 0.5f / safeW;
    const float maxPx = 26.0f * (screenH / 720.0f) * scale;
    return std::clamp(rawPx, 1.0f, std::max(1.0f, maxPx));
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

/// Nameplate toggle keybinding commands (DefaultBindings.wtf: V / Shift+V / Ctrl+V).
enum class NameplateToggleCommand { Enemies, Friends, All };

struct NameplateToggleState {
    bool showEnemies = false;
    bool showFriends = false;
};

/// Exact TBC 2.4.3 FrameXML Bindings.xml toggle state machine for V (NAMEPLATES),
/// Shift+V (FRIENDNAMEPLATES), and Ctrl+V (ALLNAMEPLATES).
/// In TBC 2.4.3, V and Shift+V are mutually exclusive (pressing Shift+V turns off
/// enemy plates; pressing V turns off friendly plates), while Ctrl+V toggles both.
/// In WotLK 3.3.5+, V and Shift+V toggle independently.
inline NameplateToggleState toggleNameplates(NameplateToggleState state,
                                             NameplateToggleCommand cmd,
                                             bool preWotlk) {
    if (preWotlk) {
        switch (cmd) {
            case NameplateToggleCommand::Enemies:
                if (state.showEnemies && !state.showFriends) return {false, false};
                return {true, false};
            case NameplateToggleCommand::Friends:
                if (state.showFriends && !state.showEnemies) return {false, false};
                return {false, true};
            case NameplateToggleCommand::All:
                if (!(state.showEnemies && state.showFriends)) return {true, true};
                return {false, false};
        }
    }
    switch (cmd) {
        case NameplateToggleCommand::Enemies:
            return {!state.showEnemies, state.showFriends};
        case NameplateToggleCommand::Friends:
            return {state.showEnemies, !state.showFriends};
        case NameplateToggleCommand::All:
            if (!(state.showEnemies && state.showFriends)) return {true, true};
            return {false, false};
    }
    return state;
}

/// TBC 2.4.3 (build 8606) nameplate maximum distance from player: 20 yards
/// (measured on pair 8: visible at 19.8 yd, hidden at 20.2 yd).
constexpr float kLegacyPlateMaxRangeSq = 20.0f * 20.0f;

/// Whether a unit belongs to the enemy nameplate toggle (V) rather than the
/// friendly nameplate toggle (Shift+V).
/// In TBC 2.4.3 (build 8606), V shows attackable units (CGUnit_C::CanAttack)
/// and Shift+V shows non-attackable units — including friendly players,
/// friendly pets, neutral-standing home-faction NPCs (e.g. Blood Elves in
/// Orgrimmar or Humans in Exodar, reaction == 4 with canAttack == false), and
/// units unattackable while .gm on is active.
inline bool isEnemyNameplateUnit(bool isPlayer, bool isHostile,
                                 bool isFriendlyPlayerControlled,
                                 int reactionToPlayer,
                                 bool canAttack = true) {
    if (!canAttack) return false;
    return isHostile || (!isPlayer && !isFriendlyPlayerControlled && reactionToPlayer <= 4);
}

struct LegacyPlateEligibilityInput {
    bool isPlayer = false;
    bool isOwnCharacter = false;
    bool isHostile = false;
    bool canAttack = true;
    bool isOwnPet = false;
    bool isPlayerControlled = false;
    bool isCompanion = false;
    bool isTotem = false;
    bool isPet = false;
    bool isGuardian = false;
    bool isAlive = true;
    int reactionToPlayer = 4;
    float distSqFromPlayer = 0.0f;
    bool anchorInViewport = true;
    bool showEnemyPlates = false;
    bool showFriendlyPlates = false;
    bool showEnemyTotems = true;
    bool showFriendlyTotems = false;
    bool showEnemyPets = true;
    bool showFriendlyPets = true;
    bool showEnemyGuardians = true;
    bool showFriendlyGuardians = true;
};

/// TBC 2.4.3 (build 8606) 2D nameplate eligibility.
/// Both the player's own pet and other friendly players' pets receive green 2D
/// health bars when Shift+V (FRIENDNAMEPLATES) is active, within 20 yd and alive.
inline bool legacyPlateEligible(const LegacyPlateEligibilityInput& in) {
    if (in.isOwnCharacter || in.isCompanion || !in.isAlive) return false;
    const bool friendlyPlayerControlled =
        !in.isPlayer && !in.isHostile && (in.isOwnPet || in.isPlayerControlled);
    const bool enemyPlate = isEnemyNameplateUnit(
        in.isPlayer, in.isHostile, friendlyPlayerControlled, in.reactionToPlayer, in.canAttack);
    bool plate = enemyPlate ? in.showEnemyPlates : in.showFriendlyPlates;
    if (in.isTotem) {
        plate = plate && (in.isHostile ? in.showEnemyTotems : in.showFriendlyTotems);
    }
    if (in.isPet) {
        plate = plate && (in.isHostile ? in.showEnemyPets : in.showFriendlyPets);
    } else if (in.isGuardian) {
        plate = plate && (in.isHostile ? in.showEnemyGuardians : in.showFriendlyGuardians);
    }
    if (!plate) return false;
    return in.distSqFromPlayer <= kLegacyPlateMaxRangeSq && in.anchorInViewport;
}

struct LegacyPlateRgb {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

/// TBC 2.4.3 (build 8606) 3D world overhead unit name RGB (before 2D plate
/// override or ghost desaturation):
/// - PvP-flagged friendly players are green (0, 255, 0); unflagged friendly
///   players are blue (128, 128, 255).
/// - Friendly non-player player-controlled units (pets, minions, totems,
///   guardians) remain blue (128, 128, 255) even when CMaNGOS Unit::SetPvP
///   propagates UNIT_FLAG_PVP to them alongside their PvP-flagged owner.
/// - Hostile player-controlled units are (220, 80, 80); NPCs follow reaction.
inline LegacyPlateRgb worldUnitNameRgb(bool isCorpsePlate,
                                       bool isPlayer,
                                       bool isPlayerControlled,
                                       bool isPvpFlagged,
                                       bool isHostile,
                                       int reactionToPlayer) {
    if (isCorpsePlate) {
        return {128, 128, 128};
    }
    if (isPlayer || isPlayerControlled) {
        if (isHostile) {
            return {220, 80, 80};
        }
        if (isPlayer && isPvpFlagged) {
            return {0, 255, 0};
        }
        return {128, 128, 255};
    }
    if (isHostile || reactionToPlayer <= 2) {
        return {255, 0, 0};
    }
    if (reactionToPlayer == 3) {
        return {255, 128, 0};
    }
    if (reactionToPlayer == 4) {
        return {255, 255, 0};
    }
    return {0, 255, 0};
}

/// TBC 2.4.3 (build 8606) nameplate health bar fill RGB:
/// friendly player = blue (0,0,255), friendly pet/NPC = green (0,255,0),
/// neutral = yellow (255,255,0), hostile = red (255,0,0), tapped = grey (128,128,128).
inline LegacyPlateRgb legacyPlateFillColor(bool isPlayer, bool isHostile,
                                           bool isFriendlyPlayerControlled,
                                           int reactionToPlayer,
                                           bool tappedByOther) {
    if (tappedByOther) return {128, 128, 128};
    if (isPlayer && !isHostile) return {0, 0, 255};
    if (isHostile || (!isFriendlyPlayerControlled && reactionToPlayer <= 2)) return {255, 0, 0};
    if (!isFriendlyPlayerControlled && reactionToPlayer <= 4) return {255, 255, 0};
    return {0, 255, 0};
}

/// TBC 2.4.3 death-effect compositing (matching fxaa.frag.glsl compositeDeath):
/// Native 2.4.3 renders 3D overhead unit names into the 3D scene before the
/// screen-space ghost desaturation pass, while 2D V nameplates and unit frames
/// draw after it. Apply the same luminance + cyan midtone lift to 3D overhead
/// world labels while `isPlayerGhost() && ffxDeath != "0"`.
inline LegacyPlateRgb applyDeathEffectToWorldLabel(LegacyPlateRgb c) {
    const float r = static_cast<float>(c.r) / 255.0f;
    const float g = static_cast<float>(c.g) / 255.0f;
    const float b = static_cast<float>(c.b) / 255.0f;
    const float gray = std::clamp(0.299f * r + 0.587f * g + 0.144f * b, 0.0f, 1.0f);
    const float midtone = 4.0f * gray * (1.0f - gray);
    const auto toByte = [](float v) -> uint8_t {
        return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return {
        toByte(gray + (83.0f / 255.0f) * midtone),
        toByte(gray + (147.0f / 255.0f) * midtone),
        toByte(gray + (168.0f / 255.0f) * midtone),
    };
}

/// TBC 2.4.3 (build 8606) nameplate opacity: 1.0 when no target is selected or
/// when this unit is the current target; 0.5 for non-target plates while another
/// unit is targeted (measured on pair 8 via WorldFrame child GetAlpha()).
inline float legacyPlateAlpha(bool hasTarget, bool isTarget) {
    return (!hasTarget || isTarget) ? 1.0f : 0.5f;
}

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
