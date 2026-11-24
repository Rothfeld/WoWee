#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "game/spell_defines.hpp"

namespace wowee {
namespace ui {

/// Build 8606 floating damage text over a unit, as measured frame by frame
/// against the original client (_experiments/20261117T211838Z.fct_layout).
///
/// Sizes are interface units on the 768-unit-high UI, so the 1280x720
/// captures read them as px * 768 / 720.
namespace fct {

/// FRIZQT__.TTF height of an ordinary number. "200" measured 56 px wide and
/// 19 px tall at 720 lines, at 9 and 24 yards alike: the size is fixed on
/// screen, not shrunk with distance.
constexpr float kFontHeight = 30.0f;
/// A critical number settles at one and a half times that (28 -> 42 px).
constexpr float kCritScale = 1.5f;
/// Text shadow: black at half opacity, 3 px right and 1 px up at 720 lines.
constexpr float kShadowX = 3.0f * 768.0f / 720.0f;
constexpr float kShadowY = -1.0f * 768.0f / 720.0f;
constexpr float kShadowAlpha = 0.5f;

/// Where a text is born, in yards below the unit's name attachment (M2
/// attachment 18, the point the world name label stands on): the baseline of
/// every text over a unit - number, critical, Miss/Dodge/... - starts there.
/// Measured on the original at two camera distances (11 and 17 yards) on
/// HumanMale at display scale 1 and 1.4 and on DragonSpawn: the baseline of
/// the first fully opaque frame (age 0.15 s) sits on the attachment, 0.21 yd
/// of climb above its birth; the birth itself, from the faint fade-in frames,
/// 0.21-0.24 yd below the attachment. The same in yards at both distances, so
/// it is a world offset, not a pixel one, and it is not scaled with the model
/// (_experiments/20261119T122954Z.combat_text, fctz-*.json, faint.py).
/// An ordinary number therefore overlaps the name as it appears and a
/// critical, which does not climb, hangs below the name.
constexpr float kBirthBelowNameAttachment = 0.23f;

/// Ordinary numbers and the outcome words climb at this rate in world yards
/// per second; critical numbers do not climb. Measured on the original: 1.40
/// at 10.5 yd and 1.31 at 24 yd (fct_layout), 1.34, 1.37 and 1.29 at 11 yd and
/// 1.31 at 17 yd (combat_text melee1, crit_original, anchor2, fz*); the mean.
constexpr float kRiseYardsPerSecond = 1.33f;

constexpr float kLifetime = 1.5f;
constexpr float kFadeInEnd = 0.15f;
/// Opacity reached at the end of the fade-in before the text turns fully
/// opaque: the measured ramp reads 0.015..0.1 and then jumps to 1.
constexpr float kFadeInPeak = 0.1f;
constexpr float kFadeOutStart = 0.76f;

/// A number never climbs above this line: the baseline of a "1" dealt to an
/// Outland Mtn. Giant whose name was at the top of the screen rose until its
/// ink spanned 58..79 px at 720 lines and then stayed there until it faded
/// (pair 7, _experiments/20261118T074350Z.plates_fct gf1). 79 px is 84.3
/// units of the 768-unit UI.
constexpr float kTopBaseline = 79.0f * 768.0f / 720.0f;

/// A number never drops below the top of the bottom action/XP bar: at 720
/// lines, bottom-clamped digits span 643..662 px, resting at baseline 662 px
/// (58 px above the bottom of the 720p viewport; hostile_A_spam_20yd in
/// _experiments/20261123T213000Z.damage_spam_layout_parity).
constexpr float kBottomBaselineFromBottom = 58.0f * 768.0f / 720.0f;

/// Criticals and the outcome words (Miss, Dodge, Parry, Immune, Absorb...)
/// stay fully opaque until 1.0 s: a "Miss" and a "Dodge" on the original read
/// full white from 0.15 s to 1.0 s and faded out after, where a number of the
/// same recording starts fading at 0.76 s (combat_text melee1, melee3).
constexpr float kCritFadeOutStart = 1.0f;

inline float alpha(float age, bool lateFade) {
    if (age < kFadeInEnd) return kFadeInPeak * std::max(age, 0.0f) / kFadeInEnd;
    const float fadeOut = lateFade ? kCritFadeOutStart : kFadeOutStart;
    if (age < fadeOut) return 1.0f;
    return std::clamp((kLifetime - age) / (kLifetime - fadeOut), 0.0f, 1.0f);
}

/// What the original writes over the unit for one line the player caused,
/// for the kinds measured against it (_experiments/20261119T122954Z.combat_text).
/// A hit shows its damage and nothing else: the parts absorbed, resisted or
/// blocked of a hit that still did damage are not written (`reduction`
/// lines). A hit that did no damage names why - Absorb, Resist, Block - and
/// one with no reason at all reads Miss, as CombatFeedback.lua reads a WOUND
/// of 0. The words are GlobalStrings MISS, DODGE, ... at the size of a number,
/// white for a swing and yellow for a spell, like the numbers.
struct Presentation {
    enum Kind { NotWorldText, Number, Word };
    Kind kind = NotWorldText;
    /// GlobalStrings name of the word.
    const char* word = nullptr;
    int32_t number = 0;
    bool critical = false;
    /// Ability yellow rather than swing white.
    bool spell = false;
    /// Outgoing heal on another unit (+N in TBC heal green).
    bool heal = false;
};

struct ColorRgb {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
};

/// Measured RGB colors in TBC build 8606 (WOWEE_PAIR=8 & WOWEE_PAIR=3 live captures):
/// - Player melee swing / miss / dodge: white (255, 255, 255)
/// - Player or pet spell (Imp Firebolt): yellow (255, 222, 0)
/// - Pet melee swing (normal & crit): orange (255, 132, 0)
/// - Outgoing heal on another unit: green (26, 255, 26) = (0.1, 1.0, 0.1)
constexpr ColorRgb kPlayerMeleeColor{1.0f, 1.0f, 1.0f};
constexpr ColorRgb kSpellColor{1.0f, 222.0f / 255.0f, 0.0f};
constexpr ColorRgb kPetMeleeColor{1.0f, 132.0f / 255.0f, 0.0f};
constexpr ColorRgb kHealColor{0.1f, 1.0f, 0.1f};

inline ColorRgb colorFor(const Presentation& p, bool isPetSource) {
    if (p.heal) return kHealColor;
    if (p.spell) return kSpellColor;
    if (isPetSource) return kPetMeleeColor;
    return kPlayerMeleeColor;
}

inline Presentation presentationFor(const game::CombatTextEntry& e) {
    using T = game::CombatTextEntry;
    Presentation p;
    if (e.reduction) return p;
    p.spell = e.spellId != 0 && !e.normalRangedAttack;
    const auto word = [&](const char* name) { p.kind = Presentation::Word; p.word = name; return p; };
    switch (e.type) {
        case T::MELEE_DAMAGE: case T::GLANCING: case T::CRUSHING:
        case T::SPELL_DAMAGE: case T::PERIODIC_DAMAGE: case T::CRIT_DAMAGE:
            if (e.amount == 0) return word("MISS");
            p.kind = Presentation::Number;
            p.number = e.amount;
            p.critical = e.type == T::CRIT_DAMAGE;
            return p;
        case T::HEAL: case T::CRIT_HEAL: case T::PERIODIC_HEAL:
            if (e.amount <= 0) return p;
            p.kind = Presentation::Number;
            p.number = e.amount;
            p.critical = e.type == T::CRIT_HEAL;
            p.heal = true;
            return p;
        case T::MISS: return word("MISS");
        case T::DODGE: return word("DODGE");
        case T::PARRY: return word("PARRY");
        case T::BLOCK: return word("BLOCK");
        case T::EVADE: return word("EVADE");
        case T::IMMUNE: return word("IMMUNE");
        case T::DEFLECT: return word("DEFLECT");
        case T::REFLECT: return word("REFLECT");
        case T::ABSORB: return word("ABSORB");
        case T::RESIST: return word("RESIST");
        default: return p;
    }
}

/// Whether build 8606 draws this entry type as world-space floating text over
/// a unit. Power gains and drains (Furor, Enrage, Bloodrage, Innervate, Drain
/// Mana, ...), honor gains, and proc triggers are never drawn as world text;
/// lines on the player are reported via COMBAT_TEXT_UPDATE to Blizzard_CombatText.
inline bool showsInWorld(game::CombatTextEntry::Type type) {
    using T = game::CombatTextEntry;
    return type != T::ENERGIZE && type != T::POWER_DRAIN &&
           type != T::PROC_TRIGGER && type != T::HONOR_GAIN;
}

/// Whether build 8606 draws this specific entry in 3D world space.
/// Lines landing on the local player (self-heals, self-HoTs, self-absorbs,
/// self-damage) belong exclusively to 2D Blizzard_CombatText and unit-frame
/// CombatFeedback; only XP_GAIN is drawn in world space for the player.
inline bool showsForUnit(const game::CombatTextEntry& e, uint64_t playerGuid, uint64_t petGuid) {
    using T = game::CombatTextEntry;
    if (!showsInWorld(e.type) || e.reduction) return false;
    if (e.type == T::XP_GAIN) return true;
    if (playerGuid != 0 && e.dstGuid == playerGuid) return false;
    const bool isPetSource = !e.isPlayerSource && e.srcGuid != 0 && e.srcGuid == petGuid;
    return e.isPlayerSource || isPetSource;
}

/// Critical size multiplier on top of kCritScale: grows from 0.1 to 2 over
/// the first 150 ms and shrinks back to 1 by 300 ms (measured widths 0.87,
/// 1.30, 1.96, 1.60, 1.21, 1.00 of the settled size at ~50 ms steps).
inline float critPulse(float age) {
    const float p = age / kLifetime;
    return p < 0.1f ? 0.1f + 19.0f * p : p < 0.2f ? 3.0f - 10.0f * p : 1.0f;
}

/// One text being laid out, in screen pixels. `x` is the horizontal centre
/// and `top` the top edge; `naturalX`/`naturalTop` are where it would be
/// drawn alone (projected anchor, top-centre aligned).
struct Box {
    float naturalX = 0.0f;
    float naturalTop = 0.0f;
    float width = 0.0f;
    /// Font height in pixels at this frame.
    float fontPx = 0.0f;
    float x = 0.0f;
    float top = 0.0f;
};

/// Texts over one unit never overlap. Each occupies a row twice its font
/// height (two simultaneous numbers measured 54-55 px apart with 28 px text,
/// two criticals 84-85 px with 42 px text) and its width plus half a font
/// height of horizontal gap (10-16 px measured).
inline float rowHeight(const Box& b) { return 2.0f * b.fontPx; }
inline float gap(const Box& b) { return 0.5f * b.fontPx; }

inline bool overlaps(const Box& a, const Box& b) {
    const float ay = a.top + 0.5f * a.fontPx;
    const float by = b.top + 0.5f * b.fontPx;
    const float dy = std::abs(ay - by);
    const float dx = std::abs(a.x - b.x);
    return dy < 0.5f * (rowHeight(a) + rowHeight(b)) - 0.01f &&
           dx < 0.5f * (a.width + b.width) + 0.5f * (gap(a) + gap(b)) - 0.01f;
}

/// Optional screen bounds enforced during layout so top/bottom/edge-clamped
/// candidates participate in collision checks instead of piling on top of each
/// other after layout. Leave zero-initialized for unbounded layout.
struct Viewport {
    float width = 0.0f;
    float height = 0.0f;
    float uiScale = 0.0f;
};

inline float clampVertical(float top, const Box& box, const Viewport& vp) {
    if (vp.uiScale <= 0.0f || vp.height <= 0.0f) return top;
    const float minTop = kTopBaseline * vp.uiScale - box.fontPx;
    const float maxTop = std::max(minTop, vp.height - kBottomBaselineFromBottom * vp.uiScale - box.fontPx);
    return std::clamp(top, minTop, maxTop);
}

inline float clampHorizontal(float x, const Box& box, const Viewport& vp) {
    if (vp.width <= 0.0f) return x;
    const float halfW = 0.5f * box.width + 2.0f;
    if (2.0f * halfW >= vp.width) return 0.5f * vp.width;
    return std::clamp(x, halfW, vp.width - halfW);
}

inline bool fitsHorizontally(const Box& box, const Viewport& vp) {
    if (vp.width <= 0.0f) return true;
    const float halfW = 0.5f * box.width + 2.0f;
    return box.x >= halfW - 0.01f && box.x <= vp.width - halfW + 0.01f;
}

/// Lays texts out newest first; every frame starts again from the natural
/// positions, so a text drops back down once the one below it expires
/// (measured with two criticals).
///
/// Measured against TBC 2.4.3 build 8606 (_experiments/20261117T211838Z.fct_layout
/// and _experiments/20261123T213000Z.damage_spam_layout_parity):
/// 1. The newest text keeps its clamped natural place.
/// 2. An older colliding text lifts above center blockers while total lift from
///    its clamped natural top stays within one row (and within top clamp).
/// 3. Otherwise it tries the right flank at its clamped natural height.
/// 4. If right is taken, it pushes down below center blockers up to one row
///    below the base anchor (and within bottom clamp).
/// 5. If below is taken, it tries the left flank at its clamped natural height.
/// 6. Any further simultaneous burst expands symmetrically right and left
///    around the unit rather than marching off-screen in a rightward train.
inline const Box* firstBlocker(const Box& box, const std::vector<Box>& placed, size_t count) {
    for (size_t j = 0; j < count; ++j)
        if (overlaps(box, placed[j])) return &placed[j];
    return nullptr;
}

inline void layout(std::vector<Box>& newestFirst, const Viewport& viewport = {}) {
    if (newestFirst.empty()) return;
    const float newestNatTop = clampVertical(newestFirst[0].naturalTop, newestFirst[0], viewport);

    for (size_t i = 0; i < newestFirst.size(); ++i) {
        Box& box = newestFirst[i];
        const float natX = clampHorizontal(box.naturalX, box, viewport);
        const float natTop = clampVertical(box.naturalTop, box, viewport);
        box.x = natX;
        box.top = natTop;
        if (i == 0) continue;

        // 1. Lift above center blockers while within one row of clamped natural top.
        for (size_t step = 0; step < i; ++step) {
            const Box* blocker = firstBlocker(box, newestFirst, i);
            if (!blocker) break;
            const float lifted = blocker->top + 0.5f * blocker->fontPx
                - 0.5f * (rowHeight(*blocker) + rowHeight(box)) - 0.5f * box.fontPx;
            const float clamped = clampVertical(lifted, box, viewport);
            if (std::abs(clamped - box.top) < 1e-4f) break;
            box.top = clamped;
        }
        if (!firstBlocker(box, newestFirst, i) &&
            natTop - box.top <= rowHeight(box) + 0.01f) continue;

        const auto besideRight = [&](const Box& blocker) {
            return blocker.x + 0.5f * (blocker.width + box.width) + 0.5f * (gap(blocker) + gap(box));
        };
        const auto besideLeft = [&](const Box& blocker) {
            return blocker.x - 0.5f * (blocker.width + box.width) - 0.5f * (gap(blocker) + gap(box));
        };

        // 2. Right flank at clamped natural height.
        box.x = natX;
        box.top = natTop;
        if (const Box* blocker = firstBlocker(box, newestFirst, i)) {
            box.x = besideRight(*blocker);
            if (!firstBlocker(box, newestFirst, i) && fitsHorizontally(box, viewport)) continue;
        }

        // 3. Push down below center blockers up to one row below the base anchor.
        const float maxDropTop = clampVertical(std::max(natTop, newestNatTop) + rowHeight(box), box, viewport);
        box.x = natX;
        box.top = natTop;
        for (size_t step = 0; step < i; ++step) {
            const Box* blocker = firstBlocker(box, newestFirst, i);
            if (!blocker) break;
            const float dropped = blocker->top + 0.5f * blocker->fontPx
                + 0.5f * (rowHeight(*blocker) + rowHeight(box)) - 0.5f * box.fontPx;
            const float clamped = clampVertical(dropped, box, viewport);
            if (std::abs(clamped - box.top) < 1e-4f) break;
            box.top = clamped;
        }
        if (!firstBlocker(box, newestFirst, i) && box.top <= maxDropTop + 0.01f) continue;

        // 4. Left flank at clamped natural height.
        box.x = natX;
        box.top = natTop;
        if (const Box* blocker = firstBlocker(box, newestFirst, i)) {
            box.x = besideLeft(*blocker);
            if (!firstBlocker(box, newestFirst, i) && fitsHorizontally(box, viewport)) continue;
        }

        // 5. Symmetric outward horizontal expansion (with upper/lower row fallback if viewport edges hit).
        const auto trySymmetricRow = [&](float rowTop) -> bool {
            Box rightCand = box;
            rightCand.x = natX;
            rightCand.top = rowTop;
            for (size_t step = 0; step <= i; ++step) {
                const Box* blocker = firstBlocker(rightCand, newestFirst, i);
                if (!blocker) break;
                rightCand.x = besideRight(*blocker);
            }
            const bool rightOk = !firstBlocker(rightCand, newestFirst, i) && fitsHorizontally(rightCand, viewport);

            Box leftCand = box;
            leftCand.x = natX;
            leftCand.top = rowTop;
            for (size_t step = 0; step <= i; ++step) {
                const Box* blocker = firstBlocker(leftCand, newestFirst, i);
                if (!blocker) break;
                leftCand.x = besideLeft(*blocker);
            }
            const bool leftOk = !firstBlocker(leftCand, newestFirst, i) && fitsHorizontally(leftCand, viewport);

            if (rightOk && leftOk) {
                box = (std::abs(leftCand.x - natX) + 0.01f < std::abs(rightCand.x - natX)) ? leftCand : rightCand;
                return true;
            }
            if (rightOk) { box = rightCand; return true; }
            if (leftOk) { box = leftCand; return true; }
            return false;
        };

        if (trySymmetricRow(natTop)) continue;
        bool placed = false;
        for (int r = 1; r <= 6 && !placed; ++r) {
            const float upRow = clampVertical(natTop - static_cast<float>(r) * rowHeight(box), box, viewport);
            if (trySymmetricRow(upRow)) { placed = true; break; }
            const float downRow = clampVertical(std::max(natTop, newestNatTop) + static_cast<float>(r) * rowHeight(box), box, viewport);
            if (trySymmetricRow(downRow)) { placed = true; break; }
        }
        if (!placed) {
            box.x = natX;
            box.top = natTop;
        }
    }
}

/// A number whose anchor would be born above that top line starts this far
/// below the point of the unit's vertical axis that projects onto the line
/// instead (Outland Mtn. Giant at 3-8 yd, ten camera pitches: 1.63-1.73 yd,
/// first visible frame, same convention as the ordinary anchor; pair 7
/// fcttrack.py). Below the line the anchor is unchanged.
constexpr float kOffTopDrop = 1.67f;

} // namespace fct
} // namespace ui
} // namespace wowee
