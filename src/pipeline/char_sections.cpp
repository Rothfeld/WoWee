#include "pipeline/char_sections.hpp"

#include "pipeline/dbc_loader.hpp"
#include "core/logger.hpp"
#include "pipeline/m2_loader.hpp"

namespace wowee {
namespace pipeline {

namespace {
// CharSections' BaseSection column: which part of a character a row describes.
constexpr uint32_t kSectionSkin = 0;
constexpr uint32_t kSectionFace = 1;
constexpr uint32_t kSectionFacialHair = 2;
constexpr uint32_t kSectionHair = 3;
constexpr uint32_t kSectionUnderwear = 4;
}  // namespace

CharacterSectionTextures resolveCharacterSections(
    const DBCFile* charSections,
    const CharSectionsFields& f,
    const CharacterAppearance& who,
    bool (*keepUnderwear)(const std::string&, void*),
    void* keepUnderwearContext) {

    CharacterSectionTextures out;
    if (!charSections) return out;

    auto keepOptionalTexture = [&](const std::string& tex) -> std::string {
        if (tex.empty()) return {};
        if (keepUnderwear && !keepUnderwear(tex, keepUnderwearContext)) return {};
        return tex;
    };

    // The nearest usable face, kept in case the exact pair is absent.
    //
    // Character creation falls back to a synthetic 0..9 range whenever its own
    // scan of this table comes up empty, so a character can be created carrying
    // a face number that has no row at all - and would then have no face for
    // good, because the lookup simply did not match and said nothing.
    std::string altFaceLower, altFaceUpper;
    bool haveAltFace = false;

    // Some races (e.g. Tauren female) author Section 2 facial overlays only for
    // colorIndex == 0 regardless of hairColorId.
    std::string altFacialLower, altFacialUpper;
    bool haveAltFacial = false;

    bool foundSkin = false, foundUnderwear = false, foundHairRow = false, foundFacial = false;

    for (uint32_t r = 0; r < charSections->getRecordCount(); r++) {
        if (charSections->getUInt32(r, f.raceId) != who.raceId) continue;
        if (charSections->getUInt32(r, f.sexId) != who.sexId) continue;

        const uint32_t section = charSections->getUInt32(r, f.baseSection);
        const uint32_t variation = charSections->getUInt32(r, f.variationIndex);
        const uint32_t colour = charSections->getUInt32(r, f.colorIndex);

        if (section == kSectionSkin && !foundSkin && colour == who.skinId) {
            std::string tex1 = charSections->getString(r, f.texture1);
            if (!tex1.empty()) {
                out.bodySkin = tex1;
                foundSkin = true;
            }
            out.skinExtra = charSections->getString(r, f.texture2);
        } else if (section == kSectionHair && !foundHairRow &&
                   variation == who.hairStyleId && colour == who.hairColorId) {
            out.hair = charSections->getString(r, f.texture1);
            out.haveHair = !out.hair.empty();
            out.scalpLower = keepOptionalTexture(charSections->getString(r, f.texture2));
            out.scalpUpper = keepOptionalTexture(charSections->getString(r, f.texture3));
            foundHairRow = true;
        } else if (section == kSectionFacialHair && !foundFacial &&
                   variation == who.facialHairId && colour == who.hairColorId) {
            out.facialLower = keepOptionalTexture(charSections->getString(r, f.texture1));
            out.facialUpper = keepOptionalTexture(charSections->getString(r, f.texture2));
            foundFacial = true;
        } else if (section == kSectionFacialHair && !foundFacial && !haveAltFacial &&
                   variation == who.facialHairId && colour == 0) {
            altFacialLower = keepOptionalTexture(charSections->getString(r, f.texture1));
            altFacialUpper = keepOptionalTexture(charSections->getString(r, f.texture2));
            haveAltFacial = !altFacialLower.empty() || !altFacialUpper.empty();
        } else if (section == kSectionFace && !out.exactFace &&
                   variation == who.faceId && colour == who.skinId) {
            out.faceLower = charSections->getString(r, f.texture1);
            out.faceUpper = charSections->getString(r, f.texture2);
            out.exactFace = true;
            out.haveFace = !out.faceLower.empty();
        } else if (section == kSectionFace && !out.exactFace && !haveAltFace &&
                   (variation == who.faceId || colour == who.skinId)) {
            // The same face in another skin colour, or failing that any face in
            // the right colour. Either beats a blank head.
            std::string tex1 = charSections->getString(r, f.texture1);
            if (!tex1.empty()) {
                altFaceLower = tex1;
                altFaceUpper = charSections->getString(r, f.texture2);
                haveAltFace = true;
            }
        } else if (section == kSectionUnderwear && !foundUnderwear &&
                   colour == who.skinId) {
            for (uint32_t column = 0; column < 3; column++) {
                std::string tex = charSections->getString(r, f.texture1 + column);
                if (tex.empty()) continue;
                if (keepUnderwear && !keepUnderwear(tex, keepUnderwearContext)) continue;
                const auto region = charSectionRegion(kSectionUnderwear, column);
                if (!region) {
                    LOG_ERROR("CharSections underwear row names texture column ", column + 1,
                              " (", tex, "), which has no body region");
                    continue;
                }
                out.underwear.push_back({tex, *region});
            }
            foundUnderwear = !out.underwear.empty();
        }

        if (foundSkin && foundHairRow && out.exactFace && foundFacial && foundUnderwear) break;
    }

    if (!out.exactFace && haveAltFace) {
        out.faceLower = altFaceLower;
        out.faceUpper = altFaceUpper;
        out.haveFace = true;
    }
    if (!foundFacial && haveAltFacial) {
        out.facialLower = altFacialLower;
        out.facialUpper = altFacialUpper;
    }
    return out;
}

void appendFaceOverlayLayers(std::vector<SkinLayer>& out,
                             const CharacterSectionTextures& sections) {
    if (!sections.faceLower.empty()) out.push_back({sections.faceLower, SkinRegion::FaceLower});
    if (!sections.faceUpper.empty()) out.push_back({sections.faceUpper, SkinRegion::FaceUpper});
    if (!sections.scalpLower.empty()) out.push_back({sections.scalpLower, SkinRegion::FaceLower});
    if (!sections.scalpUpper.empty()) out.push_back({sections.scalpUpper, SkinRegion::FaceUpper});
    if (!sections.facialLower.empty()) out.push_back({sections.facialLower, SkinRegion::FaceLower});
    if (!sections.facialUpper.empty()) out.push_back({sections.facialUpper, SkinRegion::FaceUpper});
}

std::optional<SkinRegion> charSectionRegion(uint32_t section, uint32_t column) {
    if (section == kSectionFace && column == 0) return SkinRegion::FaceLower;
    if (section == kSectionFace && column == 1) return SkinRegion::FaceUpper;
    if (section == kSectionFacialHair && column == 0) return SkinRegion::FaceLower;
    if (section == kSectionFacialHair && column == 1) return SkinRegion::FaceUpper;
    if (section == kSectionHair && column == 1) return SkinRegion::FaceLower;
    if (section == kSectionHair && column == 2) return SkinRegion::FaceUpper;
    if (section == kSectionUnderwear && column == 0) return SkinRegion::Pelvis;
    if (section == kSectionUnderwear && column == 1) return SkinRegion::Torso;
    return std::nullopt;
}

std::string resolveSkinExtraTexture(const CharacterSectionTextures& textures) {
    if (!textures.skinExtra.empty()) return textures.skinExtra;
    if (!textures.underwear.empty()) return textures.underwear.front().path;
    if (!textures.bodySkin.empty()) return textures.bodySkin;
    return {};
}

void applyCharacterTextures(M2Model& model,
                            const CharacterSectionTextures& textures,
                            const std::string& raceFolderName) {
    const std::string skinExtraPath = resolveSkinExtraTexture(textures);
    for (auto& tex : model.textures) {
        switch (tex.type) {
            case 1:  // the body, composited from skin, face and underwear
                if (!textures.bodySkin.empty()) tex.filename = textures.bodySkin;
                break;
            case 6:  // hair and scalp
                if (!textures.hair.empty()) {
                    tex.filename = textures.hair;
                } else if (tex.filename.empty()) {
                    tex.filename = "Character\\" + raceFolderName + "\\Hair00_00.blp";
                }
                break;
            case 8:
                // Skin Extra: Tauren mane/tail/fur, Naga fins/scales, or HD
                // head detail. Races that name none fall back to underwear or
                // body skin rather than leaving the slot unbound.
                if (!skinExtraPath.empty()) {
                    tex.filename = skinExtraPath;
                }
                break;
            default:
                break;
        }
    }
}

}  // namespace pipeline
}  // namespace wowee
