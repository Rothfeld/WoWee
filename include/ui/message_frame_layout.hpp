#pragma once

// How build 8606 places lines inside a MessageFrame and a ScrollingMessageFrame.
//
// Measured on pair 22 at 1280x720 (ws = 0.9375) and frame scale 0.8 (ws = 0.75)
// across font heights 8..18, SetSpacing, wrapped multi-row messages, and both
// insert modes (_experiments/20261119T1700Z.uierrors_position/):
//
//  - Messages stay in chronological order (0 oldest, count-1 newest).
//  - insertMode="TOP" (UIErrorsFrame, GuildEventMessageFrame,
//    GuildBankMessageFrame) walks from newest (count - 1 - scroll) downward
//    starting at y0, stopping before a message whose bottom crosses y1.
//  - insertMode="BOTTOM" walks from newest upward:
//      * ScrollingMessageFrame (ChatFrame1..7) starts at y1.
//      * MessageFrame divides its height into floor((y1 - y0) / slotAdvance)
//        single-line slots from y0 and starts at y0 + slots * slotAdvance, so a
//        two-line BOTTOM MessageFrame of height 60 at font 12 occupies slots 2
//        and 3 of 0..3 rather than hugging y1.
//  - Within a wrapped message, rows step by pixelHeight + spacingPixels +
//    paddingPx. Between separate messages, ScrollingMessageFrame steps by that
//    same rowHeight, while MessageFrame steps its first row by
//    (ceil(scaledFontSize) + spacingPixels) / effScale + paddingPx (its per-slot
//    FontString height). SetJustifyV does not move message slots on either type.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace wowee::ui {

struct PlacedMessage {
    size_t index = 0;       ///< index in chronological messages container
    float top = 0.0f;       ///< unrounded top pixel coordinate for drawMarkupText
    float rowHeight = 0.0f; ///< pixel step between wrapped rows inside this message
};

template <typename IsLit, typename RowCount>
std::vector<PlacedMessage> placeMessageFrameLines(
        float y0, float y1, float scaledFontSize, int pixelHeight, int spacePx,
        float paddingPx, float effScale, bool plainMessageFrame, bool insertTop,
        int scroll, size_t count, IsLit isLit, RowCount rowCount) {
    std::vector<PlacedMessage> out;
    if (count == 0 || pixelHeight <= 0 || y1 <= y0) return out;

    const float scale = effScale > 0.0f ? effScale : 1.0f;
    const float rowHeight = static_cast<float>(pixelHeight + spacePx) + paddingPx;
    const float firstRowAdvance = plainMessageFrame
        ? (std::ceil(scaledFontSize - 1e-4f) + static_cast<float>(spacePx)) / scale + paddingPx
        : rowHeight;
    if (rowHeight <= 0.0f || firstRowAdvance <= 0.0f) return out;

    const int effScroll = plainMessageFrame ? 0 : std::max(0, scroll);
    const int start = static_cast<int>(count) - 1 - effScroll;
    if (start < 0) return out;

    constexpr float kSlack = 1e-3f;
    float y = y0;
    if (!insertTop) {
        if (plainMessageFrame) {
            const float slots = std::floor((y1 - y0 + kSlack) / firstRowAdvance);
            if (slots < 1.0f) return out;
            y = y0 + slots * firstRowAdvance;
        } else {
            y = y1;
        }
    }

    for (int i = start; i >= 0; --i) {
        const auto idx = static_cast<size_t>(i);
        if (!isLit(idx)) continue;
        const size_t rows = std::max<size_t>(1, rowCount(idx));
        const float msgH = firstRowAdvance + static_cast<float>(rows - 1) * rowHeight;
        if (insertTop) {
            if (y + msgH > y1 + kSlack) break;
            out.push_back({idx, y, rowHeight});
            y += msgH;
        } else {
            const float top = y - msgH;
            if (top < y0 - kSlack) break;
            out.push_back({idx, top, rowHeight});
            y = top;
        }
    }
    return out;
}

} // namespace wowee::ui
