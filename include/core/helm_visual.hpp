#pragma once

// Where a helmet's model and texture live, given its ItemDisplayInfo id.
//
// Head gear is race and gender specific - Helm_Plate_B_01_HuM.m2 is the human
// male cut of the same helm - with a base model as the fallback for the pieces
// that do not vary. Both the local player and every other player need the same
// answer, and they resolve appearance through different classes, so the rule
// lives here rather than in either of them. The cape texture is the standing
// example of what happens otherwise: it was applied in one path and not the
// other, and showed up on the paperdoll while the character in the world wore
// nothing on their back.

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace wowee {
namespace pipeline { class AssetManager; }

namespace core {

/// The M2 attachment point a helmet hangs from. Attachment 0 is the shield
/// mount; 11 is the head.
///
/// Written out separately in appearance_composer, entity_spawner and
/// entity_spawner_player, which is how one of the three came to keep its copy
/// after it stopped attaching anything.
constexpr uint32_t kAttachHelm = 11;

/// The suffix a per-race, per-gender model carries - "_HuM" and the like.
///
/// Helm_Plate_B_01_HuM.m2 is the human male cut of a helm, and the shoulders
/// use the same suffix. Both the helmet resolver and the shoulder attachment
/// had their own ten-entry map of it, and a race corrected in one would leave
/// the other asking for a file that is not there - which reads as "this piece
/// has no per-race cut" rather than as an error, because that is exactly how a
/// missing file is treated.
///
/// Empty for a race with no per-race art, which is the answer for goblin and
/// worgen: they arrive in Cataclysm and a 3.3.5 install has nothing to point
/// a suffix at.
inline std::string raceGenderSuffix(uint8_t raceId, uint8_t genderId) {
    // Read off the file names in the archive rather than from a race enum,
    // which is why the undead are "Sc" - the art uses Scourge.
    switch (raceId) {
        case 1:  return genderId == 0 ? "_HuM" : "_HuF";
        case 2:  return genderId == 0 ? "_OrM" : "_OrF";
        case 3:  return genderId == 0 ? "_DwM" : "_DwF";
        case 4:  return genderId == 0 ? "_NiM" : "_NiF";
        case 5:  return genderId == 0 ? "_ScM" : "_ScF";
        case 6:  return genderId == 0 ? "_TaM" : "_TaF";
        case 7:  return genderId == 0 ? "_GnM" : "_GnF";
        case 8:  return genderId == 0 ? "_TrM" : "_TrF";
        case 10: return genderId == 0 ? "_BeM" : "_BeF";
        case 11: return genderId == 0 ? "_DrM" : "_DrF";
        default: return {};
    }
}

struct HelmVisual {
    /// Candidate model paths, most specific first. The caller loads them in
    /// order because only it knows how to load an M2, and stops at the first
    /// that parses.
    std::string racialModelPath;   ///< Empty when the race has no known suffix.
    std::string baseModelPath;
    std::string texturePath;       ///< Already resolved against what exists.

    [[nodiscard]] bool valid() const { return !baseModelPath.empty(); }
};

/// Resolve the head model and texture for an ItemDisplayInfo id. Returns an
/// empty result when the id is unknown or carries no model.
HelmVisual resolveHelmVisual(pipeline::AssetManager& assets,
                             uint32_t itemDisplayInfoId,
                             uint8_t raceId,
                             uint8_t genderId);

/// HelmetGeosetVisData.dbc hide-mask columns, in file order, and the
/// character geoset group each one hides.
///
/// Each column is a race bitmask, bit N set for race id N (1 human .. 11
/// draenei). Read off the 2.4.3 file, row 248 (full helms such as
/// Helm_Plate_B_01): its ears column clears exactly bits 4, 8 and 10 - night
/// elf, troll and blood elf, the races whose ears stand out of a helmet.
///
/// Group 0 is the hair scalp (0 itself is the body and stays), 1/2/3 the three
/// CharacterFacialHairStyles channels, 7 the ears. Hiding a group selects its
/// x01 member: 1 is the bald cap, 701 the ear stub, and 101/201/301 the flat
/// face pieces the human male carries in place of beard meshes. Models that
/// have no x01 in a group (dwarf male 1xx/3xx) draw nothing there.
///
/// Columns 6 and 7 have no geoset group in any 2.4.3 character model; only
/// the Helm_Circlet_C_01 displays 46034/46037/46038 set them. They are read
/// and reported, never applied.
struct HelmHideColumn { const char* layoutName; uint16_t geosetGroup; };
inline constexpr HelmHideColumn kHelmHideColumns[] = {
    {"HideHair", 0}, {"HideFacial1", 1}, {"HideFacial2", 2}, {"HideFacial3", 3}, {"HideEars", 7},
};
inline constexpr const char* kHelmHideUnmappedColumns[] = {"HideUnknown6", "HideUnknown7"};

/// Whether a HelmetGeosetVisData mask covers this race.
constexpr bool helmMaskCoversRace(uint32_t mask, uint8_t raceId) {
    return raceId < 32 && (mask & (1u << raceId)) != 0;
}

/// The geoset groups a head item hides on a wearer of this race and gender.
///
/// ItemDisplayInfo.HelmetGeosetVis[gender] -> HelmetGeosetVisData row -> the
/// groups of every column whose mask has the wearer's race bit. Id 0 hides
/// nothing (circlets, most crowns and every non-helm display).
std::vector<uint16_t> helmHiddenGeosetGroups(pipeline::AssetManager& assets,
                                             uint32_t itemDisplayInfoId,
                                             uint8_t raceId,
                                             uint8_t genderId);

/// Replace every listed group in `geosets` with its x01 member. Group 0 keeps
/// the body (geoset 0) and gets the bald cap 1 in place of the hair scalp.
inline void hideGeosetGroups(std::unordered_set<uint16_t>& geosets,
                             const std::vector<uint16_t>& groups) {
    for (const uint16_t group : groups) {
        for (auto it = geosets.begin(); it != geosets.end();) {
            if (*it / 100 == group && *it != 0) it = geosets.erase(it);
            else ++it;
        }
        geosets.insert(static_cast<uint16_t>(group * 100 + 1));
    }
}

} // namespace core
} // namespace wowee
