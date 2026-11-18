#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

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

/// Height of the plain name line's bottom over the name attachment, as the
/// world name label uses it (game_screen_hud.cpp). Paired c5 capture: an
/// ordinary number is born with its bottom on that line (native 363-364 vs
/// name bottom 367 px); a critical hangs below it (top 368 px).
constexpr float kNameLineOffset = 0.25f;

/// Ordinary numbers climb from the unit's name attachment at this rate in
/// world yards per second (1.40 at 10.5 yd camera distance; 1.31 at 24 yd).
/// Critical numbers do not climb.
constexpr float kRiseYardsPerSecond = 1.4f;

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

constexpr float kCritFadeOutStart = 1.0f;

inline float alpha(float age, bool crit) {
    if (age < kFadeInEnd) return kFadeInPeak * std::max(age, 0.0f) / kFadeInEnd;
    const float fadeOut = crit ? kCritFadeOutStart : kFadeOutStart;
    if (age < fadeOut) return 1.0f;
    return std::clamp((kLifetime - age) / (kLifetime - fadeOut), 0.0f, 1.0f);
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
