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
};

inline Presentation presentationFor(const game::CombatTextEntry& e) {
    using T = game::CombatTextEntry;
    Presentation p;
    if (e.reduction) return p;
    p.spell = e.spellId != 0;
    const auto word = [&](const char* name) { p.kind = Presentation::Word; p.word = name; return p; };
    switch (e.type) {
        case T::MELEE_DAMAGE: case T::GLANCING: case T::CRUSHING:
        case T::SPELL_DAMAGE: case T::PERIODIC_DAMAGE: case T::CRIT_DAMAGE:
            if (e.amount == 0) return word("MISS");
            p.kind = Presentation::Number;
            p.number = e.amount;
            p.critical = e.type == T::CRIT_DAMAGE;
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

/// Lays texts out newest first; every frame starts again from the natural
/// positions, so a text drops back down once the one below it expires
/// (measured with two criticals).
///
/// The newest text keeps its natural place. An older text that collides is
/// lifted over the texts above it while the total lift stays within one row;
/// otherwise it is set right of the first text it collides with at its
/// natural height; if that place is taken it drops one row below its natural
/// place; only then does it keep stepping right. Measured cases: two
/// simultaneous numbers stack; of three, the oldest goes beside the newest on
/// the bottom row; of four (1, 22, 333, 4 sent together, st-original frames
/// 520-545), 4 stays, 333 rises a row, 22 sits right of 4 and 1 hangs a row
/// below the name line; a 0.3 s stream forms one column; in a 0.15 s stream
/// the second number sits right of the third, slightly lower.
inline const Box* firstBlocker(const Box& box, const std::vector<Box>& placed, size_t count) {
    for (size_t j = 0; j < count; ++j)
        if (overlaps(box, placed[j])) return &placed[j];
    return nullptr;
}

inline void layout(std::vector<Box>& newestFirst) {
    for (size_t i = 0; i < newestFirst.size(); ++i) {
        Box& box = newestFirst[i];
        box.x = box.naturalX;
        box.top = box.naturalTop;
        // Each step passes one placed text, so i steps always suffice.
        for (size_t step = 0; step < i; ++step) {
            const Box* blocker = firstBlocker(box, newestFirst, i);
            if (!blocker) break;
            box.top = blocker->top + 0.5f * blocker->fontPx
                - 0.5f * (rowHeight(*blocker) + rowHeight(box)) - 0.5f * box.fontPx;
        }
        if (!firstBlocker(box, newestFirst, i) &&
            box.naturalTop - box.top <= rowHeight(box) + 0.01f) continue;
        box.top = box.naturalTop;
        const auto beside = [&](const Box& blocker) {
            return blocker.x + 0.5f * (blocker.width + box.width) + 0.5f * (gap(blocker) + gap(box));
        };
        if (const Box* blocker = firstBlocker(box, newestFirst, i)) {
            box.x = beside(*blocker);
            if (!firstBlocker(box, newestFirst, i)) continue;
        }
        box.x = box.naturalX;
        box.top = box.naturalTop + rowHeight(box);
        if (!firstBlocker(box, newestFirst, i)) continue;
        box.top = box.naturalTop;
        for (size_t step = 0; step < i; ++step) {
            const Box* blocker = firstBlocker(box, newestFirst, i);
            if (!blocker) break;
            box.x = beside(*blocker);
        }
    }
}

/// A number whose anchor would be born above that top line starts this far
/// below the point of the unit's vertical axis that projects onto the line
/// instead (Outland Mtn. Giant at 3-8 yd, ten camera pitches: 1.63-1.73 yd,
/// first visible frame, same convention as the ordinary anchor; pair 7
/// fcttrack.py). Below the line the anchor is unchanged.
constexpr float kOffTopDrop = 1.67f;

/// Applied after the layout, in pixels: `uiScale` is pixels per UI unit.
inline void clampToTop(Box& box, float uiScale) {
    box.top = std::max(box.top, kTopBaseline * uiScale - box.fontPx);
}

} // namespace fct
} // namespace ui
} // namespace wowee
