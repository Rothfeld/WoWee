#pragma once

#include "ui/interface_fonts.hpp"
#include "ui/widget_tree.hpp"

namespace wowee {
namespace ui {

/// Measure and size a FontString widget from its own text and layout constraints.
///
/// A FontString with declared width (or opposing horizontal anchors) and zero
/// declared height is a wrapping paragraph (`wrapsToWidth`): its width stays
/// fixed and its height tracks the wrapped line count whenever text, font, or
/// wrap width changes.
inline void sizeFontStringLayout(Widget* w, float pixelScale) {
    if (w->text.empty()) {
        if (w->autoSized || w->wrapsToWidth) {
            if (w->autoSized) w->width = 0.0f;
            w->height = 0.0f;
            w->wrappedLines = 0;
            w->measuredText.clear();
        }
        return;
    }

    const bool spansX = anchorsSpanAxis(w->anchors, true);
    const bool spansY = anchorsSpanAxis(w->anchors, false);
    const bool fixedW = spansX || (!w->autoSized && w->width > 0.0f);
    const bool fixedH = spansY || (!w->autoSized && !w->wrapsToWidth && w->height > 0.0f);
    if (fixedW && fixedH) return;

    // A width (or horizontal anchor span) with no explicit height is a
    // wrapping paragraph (`<AbsDimension x="270" y="0"/>` in QuestFrame.xml /
    // GossipFrame.xml / StaticPopup.xml).
    const bool paragraph = w->wrapsToWidth || (fixedW && !fixedH);

    const float size = interfaceFontSize(w->fontHeight, pixelScale);
    const int outlinePx = outlinePixelsForFlag(w->fontOutline);
    const bool sameMeasurement = w->measuredText == w->text &&
                                 w->measuredSize == size &&
                                 w->measuredFace == w->fontFace &&
                                 w->measuredOutline == w->fontOutline;
    if (paragraph) {
        if (!spansX && sameMeasurement) return;
    } else {
        if (w->autoSized && sameMeasurement) return;
    }

    if (paragraph) {
        // The width stays as declared (or anchor-spanned) and the height follows
        // the wrap. autoSized stays false on purpose: it is what the draw reads
        // to decide whether there is a box to wrap inside.
        // Mark wrapsToWidth before any early return (including spansX before
        // rectW is solved) so subsequent text or width changes re-measure height
        // instead of treating the computed height as an explicit fixed height.
        w->wrapsToWidth = true;
        const float wrapWidth = spansX ? w->rectW : w->width;
        if (spansX && wrapWidth <= 0.0f) {
            w->wrappedLines = 1;
            w->height = interfaceTextBlockHeight(1, w->fontHeight, w->lineSpacing, pixelScale);
            return;
        }
        const int rows = interfaceTextLines(w->text, w->fontFace, w->fontHeight, wrapWidth,
                                            w->nonSpaceWrap, pixelScale,
                                            w->hasShadow ? w->shadowX : 0.0f, outlinePx);
        w->wrappedLines = rows > 0 ? rows : 1;
        w->height = interfaceTextBlockHeight(w->wrappedLines, w->fontHeight, w->lineSpacing, pixelScale);
        if (!spansX) {
            w->measuredText = w->text;
            w->measuredSize = size;
            w->measuredFace = w->fontFace;
            w->measuredOutline = w->fontOutline;
        }
        return;
    }

    w->width = interfaceTextWidth(w->text, w->fontFace, w->fontHeight, pixelScale,
                                  w->hasShadow ? w->shadowX : 0.0f, outlinePx);
    const int rows = interfaceTextLines(w->text, w->fontFace, w->fontHeight, 0.0f, false, pixelScale,
                                        0.0f, outlinePx);
    w->wrappedLines = rows > 0 ? rows : 1;
    if (w->height <= 0.0f || w->autoSized) {
        w->height = interfaceTextBlockHeight(w->wrappedLines, w->fontHeight, w->lineSpacing, pixelScale);
    }
    w->autoSized = true;
    w->measuredText = w->text;
    w->measuredSize = size;
    w->measuredFace = w->fontFace;
    w->measuredOutline = w->fontOutline;
}

} // namespace ui
} // namespace wowee
