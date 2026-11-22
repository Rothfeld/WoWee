#pragma once

#include "game/world_packets.hpp"
#include "ui/native_text_metrics.hpp"

#include <algorithm>
#include <cmath>

namespace wowee::ui {

constexpr float kBubbleFontHeightUi = 16.0f;
constexpr float kBubbleWrapWidthUi = 316.0f;
constexpr float kBubblePaddingUi = 16.0f;
constexpr float kBubbleEdgeUi = 16.0f;

struct ChatBubbleColor {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
};

struct ChatBubbleMetrics {
    float scale = 1.0f;
    int pixelHeight = 16;
    float wrapWidthUi = kBubbleWrapWidthUi;
    float wrapWidthPx = kBubbleWrapWidthUi;
    float padX = kBubblePaddingUi;
    float padY = kBubblePaddingUi;
    float edge = kBubbleEdgeUi;
    float tailSize = kBubbleEdgeUi;
    float tailTopOffset = -0.25f * kBubbleEdgeUi;
};

struct ChatBubbleBox {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float bgX0 = 0.0f;
    float bgY0 = 0.0f;
    float bgX1 = 0.0f;
    float bgY1 = 0.0f;
    float tailX0 = 0.0f;
    float tailY0 = 0.0f;
    float tailX1 = 0.0f;
    float tailY1 = 0.0f;
};

/// TBC 2.4.3 ChatTypeInfo text color for speech bubbles.
inline ChatBubbleColor chatBubbleColor(game::ChatType type) {
    switch (type) {
        case game::ChatType::MONSTER_SAY:
            return {1.0f, 1.0f, 159.0f / 255.0f}; // #FFFF9F
        case game::ChatType::YELL:
        case game::ChatType::MONSTER_YELL:
            return {1.0f, 64.0f / 255.0f, 64.0f / 255.0f}; // #FF4040
        case game::ChatType::PARTY:
        case game::ChatType::MONSTER_PARTY:
            return {170.0f / 255.0f, 170.0f / 255.0f, 1.0f}; // #AAAAFF
        case game::ChatType::RAID:
            return {1.0f, 127.0f / 255.0f, 0.0f}; // #FF7F00
        case game::ChatType::SAY:
        default:
            return {1.0f, 1.0f, 1.0f}; // #FFFFFF
    }
}

/// True for party/raid speech bubbles gated by chatBubblesParty.
inline bool isPartyChatBubbleType(game::ChatType type) {
    return type == game::ChatType::PARTY || type == game::ChatType::RAID;
}

/// Scaled pixel metrics for speech bubbles at a given viewport height.
inline ChatBubbleMetrics chatBubbleMetrics(float screenH) {
    ChatBubbleMetrics m;
    m.scale = (screenH > 0.0f ? screenH : 720.0f) / 768.0f;
    m.pixelHeight = std::max(1, nativePixelHeight(kBubbleFontHeightUi, m.scale));
    m.wrapWidthUi = kBubbleWrapWidthUi;
    m.wrapWidthPx = kBubbleWrapWidthUi * m.scale;
    m.padX = kBubblePaddingUi * m.scale;
    m.padY = kBubblePaddingUi * m.scale;
    m.edge = kBubbleEdgeUi * m.scale;
    m.tailSize = m.edge;
    // Align tail row y=1 (grey shoulder) with bottom backdrop border row y=25/32.
    m.tailTopOffset = -0.25f * m.edge;
    return m;
}

/// Compute outer 9-slice frame, non-overlapping center background rect, and
/// tail quad above the projected speaker anchor (screenX, screenY).
inline ChatBubbleBox computeChatBubbleBox(float screenX, float screenY,
                                          float maxLineWidthPx, int lineCount,
                                          const ChatBubbleMetrics& m) {
    const int clampedLines = std::max(1, lineCount);
    const float textH = static_cast<float>(clampedLines * m.pixelHeight);
    ChatBubbleBox box;
    box.width = std::max(maxLineWidthPx + m.padX * 2.0f, m.edge * 2.0f);
    box.height = std::max(textH + m.padY * 2.0f, m.edge * 2.0f);
    // The anchor point sits at the speaker's name attachment; the bubble sits
    // directly above it, horizontally centered, with the tail hanging below.
    box.x0 = std::floor(screenX - box.width * 0.5f);
    box.y0 = std::floor(screenY - box.height);
    box.x1 = box.x0 + box.width;
    box.y1 = box.y0 + box.height;

    // ChatBubble-Backdrop.blp edge/corner slices already contain the alpha-187
    // interior fill, so ChatBubble-Background.blp fills only the center rect
    // inside the 9-slice border to avoid double-blending around the perimeter.
    box.bgX0 = box.x0 + m.edge;
    box.bgY0 = box.y0 + m.edge;
    box.bgX1 = box.x1 - m.edge;
    box.bgY1 = box.y1 - m.edge;

    box.tailX0 = std::floor(screenX - m.tailSize * 0.5f);
    box.tailY0 = std::floor(box.y1 + m.tailTopOffset);
    box.tailX1 = box.tailX0 + m.tailSize;
    box.tailY1 = box.tailY0 + m.tailSize;
    return box;
}

} // namespace wowee::ui
