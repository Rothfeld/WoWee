#pragma once

// The interface's typefaces, by the name FrameXML calls them.
//
// A font object names its face as a path - "Fonts\MORPHEUS.ttf" - and the file
// on disk is lower case, so lookups go through a normalised key rather than the
// spelling either side happens to use. Faces are registered once, before the
// first frame, because the glyph atlas is built then and cannot take another
// afterwards without being torn down.

#include <cstddef>
#include <string>

#include <vector>

#include "ui/native_text_metrics.hpp"
#include "ui/text_wrap.hpp"

struct ImFont;

namespace wowee::ui {

/// Records a face under a path or filename. Later registrations of the same
/// face replace the earlier one.
void registerInterfaceFace(const std::string& pathOrName, ImFont* font);

/// The metric tables of a registered face: the widget's own, then the
/// interface default. Null when neither was registered.
const NativeFace* interfaceNativeFace(const std::string& fontFace);

/// The original client's measure for a widget's face at a pixel height.
/// Codepoints the face lacks are measured by what the merged fallback face
/// draws them at. `face` is null when no interface face is registered.
NativeTextMeasure interfaceTextMeasure(const std::string& fontFace, int pixelHeight);

/// The original client's measure for an interface face, found by the ImFont it
/// was registered with. `face` is null for a font that is not an interface face.
NativeTextMeasure interfaceTextMeasure(const ImFont* font, int pixelHeight);

/// The whole pixel height a widget's text is rasterized and laid out at:
/// floor(fontHeight * pixelsPerUnit + 0.5), after the size limit.
int interfacePixelHeight(float fontHeight, float pixelScale);

/// How wide an inline |T picture is in a row of `pixelHeight`, in pixels: its
/// own width, else its height, else the row's height.
float nativeTextureWidth(const WrapRun& run, int pixelHeight, float pixelScale);

/// The pixels a string's drop shadow adds to its width: round(x * ppu) for
/// a shadow offset to the right, nothing for one to the left. Measured on
/// 2.4.3 with SetShadowOffset: GetStringWidth grows by it and a wrapped
/// string has that much less room; the y offset changes neither.
int shadowPixels(float shadowX, float pixelScale);

/// A widget's text broken into rows the way the original breaks it.
/// `wrapWidth` is in interface units, zero for no soft wrapping; `shadowX`
/// the string's shadow x offset in units (zero without a shadow). Row widths
/// exclude the shadow. Empty when no interface face is registered.
std::vector<NativeWrappedLine> interfaceWrap(const std::string& text, const std::string& fontFace,
                                             float fontHeight, float wrapWidth, bool nonSpaceWrap,
                                             float pixelScale, float shadowX = 0.0f);

/// The pixels SetSpacing(spacing) puts between two rows: ceil(spacing * ppu).
/// Measured on 2.4.3 at 0.5625 pixels per unit: spacing 1, 2, 3, 5 gave 1, 2,
/// 2, 3 pixels; at 0.9375, 2 and 5 gave 2 and 5.
int spacingPixels(float spacing, float pixelScale);

/// How tall `rows` rows of a widget's text are, in interface units: each row
/// is the font's pixel height, with spacingPixels between rows.
float interfaceTextBlockHeight(int rows, float fontHeight, float spacing, float pixelScale);

/// The face for a path a font object named, or null if it was never loaded -
/// in which case the caller should draw with whatever it was already using.
ImFont* interfaceFace(const std::string& pathOrName);

/// How wide a piece of text is, in interface units.
///
/// The one answer to that question. It was asked twice with two different
/// answers: the renderer drew a font string in the interface's own face at the
/// widget's height, and GetTextWidth measured it in whatever face this client
/// had current at a flat twelve points. Anything that sizes itself from its own
/// text - and MoneyFrame sizes all three coin buttons that way - came out
/// narrower than the digits drawn in it, so the numbers ran into each other.
///
/// `fontFace` is the widget's own, `fontHeight` its own point size; zero for
/// either means the default, which is what the renderer falls back to too.
/// GetStringWidth: the widest |n line plus the shadow (see shadowPixels).
float interfaceTextWidth(const std::string& text, const std::string& fontFace,
                         float fontHeight, float pixelScale = 1.0f, float shadowX = 0.0f);

/// How many lines a piece of text takes when it is drawn.
///
/// A soft wrap inside `wrapWidth` - zero means none, which is what a label
/// sized by its own text wants - and |n breaks a line at any width. This is
/// the count the renderer's own measure arrives at, asked on demand: FrameXML
/// sets a string's text and reads its height in the same breath, long before
/// the next frame is laid out, and the quest tracker sizes every row that way.
int interfaceTextLines(const std::string& text, const std::string& fontFace,
                       float fontHeight, float wrapWidth, bool nonSpaceWrap, float pixelScale = 1.0f,
                       float shadowX = 0.0f);

/// The face a widget draws in, resolved the way the renderer resolves it: its
/// own, then the interface default, then whatever is current.
ImFont* interfaceFaceOrDefault(const std::string& fontFace);

/// Logical font size, with the rasterization limit applied in physical pixels.
float interfaceFontSize(float fontHeight, float pixelScale = 1.0f);

/// Configure the source client's font rasterization limit; zero means uncapped.
void setInterfaceFontSizeLimit(float maximum);

/// What ImGui's rasterizer scale has to be multiplied by so a face asked for at
/// height N draws with its EM square N pixels tall.
///
/// FrameXML's <FontHeight> is an em size, the way every traditional font API
/// and CSS mean "font size": ten means the em square is ten pixels. ImGui asks
/// stb_truetype for stbtt_ScaleForPixelHeight instead, which fits the whole
/// ascender-to-descender span into those ten pixels - stb's own header calls
/// the other one "probably what traditional APIs compute". FRIZQT__ spans 1215
/// units of a 1000-unit em, so every label in the interface was drawn and
/// measured at 82.3% of the size WoW draws it.
///
/// Smaller text is the visible half. The other half is that anything positioned
/// against a caption's right edge lands short by 18% of that caption's width,
/// and the interface anchors a great deal that way: the auction house's rarity
/// dropdown hangs off "Level Range" and came to rest on top of the level boxes.
///
/// One (ascent - descent) / unitsPerEm, read off the file's own head and hhea
/// tables. Faces differ, so it is asked per face rather than written down.
/// Answers 1 for anything it cannot parse, which leaves ImGui's own scaling.
float fontEmSizeScale(const void* ttfData, size_t byteCount);

/// The same, for a face still on disk. Answers 1 if the file cannot be read.
float fontEmSizeScaleOfFile(const std::string& path);

} // namespace wowee::ui
