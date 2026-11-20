#pragma once

/**
 * geoset_rules.hpp - how a character geoset id is chosen when a model does not
 * have the one that was asked for.
 *
 * A geoset id is a group and a variant: group * 100 + variant. The members of a
 * group are alternatives for one part of a character - five kinds of boot, six
 * kinds of cloak - so when a model does not carry the exact variant asked for,
 * another member of the same group is usually the right answer.
 *
 * Usually. Variant 1 means NONE - bare feet, no cloak, no beard - and so does
 * variant 0 where a DBC stores the variant directly and uses zero for absent.
 * Every other member of the group is *something*, so substituting for none does
 * not approximate it, it contradicts it. That single mistake produced three
 * separate faults: a clean-shaven NPC with a beard, a character with no cloak
 * wearing an untextured one, and a player with no feet.
 *
 * It produced them three times because this rule was written five times, as a
 * local lambda in each place that needed it, and only some of them learned it.
 * It lives here now, with a test, and the call sites ask rather than decide.
 */

#include <cstdint>
#include <unordered_set>

namespace wowee {
namespace core {

// Default (bare) geoset IDs per equipment group.
// Each group's base is groupNumber * 100; variant 01 is typically bare/default.
constexpr uint16_t kGeosetDefaultConnector = 101;   // Group  1: default hair connector
constexpr uint16_t kGeosetBareForearms     = 401;   // Group  4: no gloves
constexpr uint16_t kGeosetBareShins        = 501;   // Group  5: no boots
constexpr uint16_t kGeosetDefaultEars      = 702;   // Group  7: ears
constexpr uint16_t kGeosetBareSleeves      = 801;   // Group  8: no chest armor sleeves
constexpr uint16_t kGeosetDefaultKneepads  = 902;   // Group  9: kneepads
constexpr uint16_t kGeosetNoChestFlap      = 1001;  // Group 10: no flap below the waist
constexpr uint16_t kGeosetNoTabard         = 1201;  // Group 12: no tabard (TBC character models carry only 1202)
constexpr uint16_t kGeosetBarePants        = 1301;  // Group 13: no leggings
constexpr uint16_t kGeosetNoCape           = 1501;  // Group 15: no cape
constexpr uint16_t kGeosetWithCape         = 1502;  // Group 15: with cape
constexpr uint16_t kGeosetBareFeet         = 2002;  // Group 20: bare feet
/// The other half of group 20. Models that split the feet out of the body do
/// not agree on which number to use - an HD human male carries 2002 and an HD
/// human female carries 2001 - so both are asked for and the model draws the
/// one it has. Stock models carry neither and are unaffected.
constexpr uint16_t kGeosetBareFeetAlt      = 2001;

/// Group 17: the eye-glow overlay - the mesh that makes a night elf's eyes
/// shine rather than sit there pale.
///
/// Off for everyone else, which is why the renderer strips group 17 from any
/// model drawn without a geoset filter: an NPC given the whole model drew
/// glowing eyes whatever it was.
constexpr uint16_t kGeosetEyeGlow          = 1701;


/// The body part a geoset id belongs to: 501 and 505 are both group 5, boots.
constexpr uint16_t geosetGroup(uint16_t id) { return static_cast<uint16_t>(id / 100); }

/// Which alternative within the group: 505 is variant 5.
constexpr uint16_t geosetVariant(uint16_t id) { return static_cast<uint16_t>(id % 100); }

/// Whether this id is the group's way of saying the character has none of it.
///
/// Both spellings appear. The geoset tables use variant 1 - 501 is bare feet,
/// 1501 is no cloak, 101 is no beard. The DBCs that store a variant directly,
/// CharFacialHairStyles among them, use 0 for absent, and 0 arrives here as
/// group*100 + 0.
constexpr bool geosetMeansNone(uint16_t id) {
    const uint16_t variant = geosetVariant(id);
    return variant == 0 || variant == 1;
}

/// The geoset a model should actually draw for `preferred`.
///
/// Returns `preferred` when the model has it; the group's lowest member when it
/// does not and a substitute is meaningful; and 0 - draw nothing - when what was
/// asked for was none and the model has no way to say so. A caller that gets 0
/// adds nothing for that group.
///
/// `available` is what the model carries. An empty set means "unknown", and then
/// `preferred` is returned unchanged rather than guessed at.
inline uint16_t resolveGeoset(uint16_t preferred,
                              const std::unordered_set<uint16_t>& available) {
    if (available.empty()) return preferred;
    if (available.count(preferred) > 0) return preferred;
    if (geosetMeansNone(preferred)) return 0;

    const uint16_t group = geosetGroup(preferred);
    uint16_t lowest = 0;
    for (uint16_t id : available) {
        if (geosetGroup(id) != group) continue;
        if (lowest == 0 || id < lowest) lowest = id;
    }
    return lowest;
}

/// Add a character's facial-hair geosets - beard, moustache, sideburns - to a set.
///
/// CharFacialHairStyles stores a variant per group and uses 0 for "this
/// character has none of that". Zero must not be turned into an id: group*100+0
/// is a geoset no model carries, and a caller that then substitutes within the
/// group hands a beard to someone who asked for none. That happened, on NPCs.
///
/// A caller that inserts raw ids was only accidentally safe - the invalid id
/// matched nothing - so both spellings of the mistake are removed by asking here
/// instead.
inline void addFacialHairGeosets(std::unordered_set<uint16_t>& out,
                                 uint16_t variant100, uint16_t variant200,
                                 uint16_t variant300) {
    if (variant100 != 0) out.insert(static_cast<uint16_t>(100 + variant100));
    if (variant200 != 0) out.insert(static_cast<uint16_t>(200 + variant200));
    if (variant300 != 0) out.insert(static_cast<uint16_t>(300 + variant300));
}

/// Key for the (race, sex, variation) maps built from CharHairGeosets.dbc and
/// CharFacialHairStyles.dbc.
///
/// It was packed by hand in ten places across three files. Every one of them had
/// to agree bit for bit with every other, or a lookup would miss and say
/// nothing - and a hair or beard lookup that misses does not report a fault, it
/// quietly draws the default.
constexpr uint32_t appearanceKey(uint8_t race, uint8_t sex, uint8_t variation) {
    return (static_cast<uint32_t>(race) << 16) |
           (static_cast<uint32_t>(sex) << 8) |
           static_cast<uint32_t>(variation);
}

/// Whether a race's eyes glow.
///
/// Night elves', and only theirs among the playable races - it is not an option
/// they choose, both sexes have it and always have. The NPC path had this rule
/// and the player path did not, so a night elf player looked out of pale eyes
/// while every night elf standing next to them glowed.
///
/// Death knights glow too, whatever their race, and are not handled here: that
/// depends on the class rather than the model and the paths that build these
/// sets do not all know it.
inline bool raceHasGlowingEyes(uint8_t raceId) {
    return raceId == 4;  // NightElf
}

/// The geosets a character shows with nothing equipped, before any armour is
/// known: the body, the chosen hair scalp, the chosen facial features, and the
/// bare variant of every equipment group.
///
/// Written twice before this - once for the player and once for the portrait -
/// and the two had drifted apart in exactly the ways that cost something. The
/// portrait named one of the two feet variants, so an HD model spelling its feet
/// the other way lost them there while the player kept his. The player named the
/// cloak mesh and the portrait named the no-cloak panel.
///
/// `hairScalp` and the three facial variants come from the DBC maps; a zero
/// facial variant means the character has none of that feature and adds nothing.
inline std::unordered_set<uint16_t> bareCharacterGeosets(uint16_t hairScalp,
                                                         uint16_t facial100,
                                                         uint16_t facial200,
                                                         uint16_t facial300,
                                                         uint8_t raceId = 0) {
    std::unordered_set<uint16_t> geosets;
    geosets.insert(0);                      // the body
    if (hairScalp != 0) geosets.insert(hairScalp);
    addFacialHairGeosets(geosets, facial100, facial200, facial300);
    if (raceHasGlowingEyes(raceId)) geosets.insert(kGeosetEyeGlow);

    geosets.insert(kGeosetBareForearms);    // no gloves
    geosets.insert(kGeosetBareShins);       // no boots
    geosets.insert(kGeosetDefaultEars);
    geosets.insert(kGeosetBareSleeves);     // no chest sleeves
    // No kneepads. Group 9 has no "none" of its own here, so 902 is a kneepad
    // mesh and not a bare knee: on the human female it is a band from mid-shin
    // to above the knee, twice the radius of the shin under it, and drawn on a
    // character wearing no leg armour it reads as the cuff of a boot. The knee
    // itself is carried by 1301 and 501, which cover every vertex 902 does.
    geosets.insert(kGeosetBarePants);       // no leggings
    // The feet, both spellings. The models the game shipped have no group 20 at
    // all and are unaffected; the replacements split the feet out and do not
    // agree on the number, so naming one loses them on half of them.
    geosets.insert(kGeosetBareFeet);
    geosets.insert(kGeosetBareFeetAlt);
    // No cloak geoset at all. This set is built before equipment is known, so
    // naming the cloak mesh gives an untextured cape to a character wearing
    // none, and naming the no-cloak panel is wrong on the models that have no
    // such panel. The equipment pass adds the cape when there is one.
    return geosets;
}

/// The geoset a piece of equipment selects within its group.
///
/// ItemDisplayInfo's GeosetGroup columns hold a small number G meaning "the Gth
/// variant after the bare one" - so a chest with G=2 wants group 8 variant 3,
/// which is the bare sleeves id plus 2. The arithmetic is a single addition and
/// was written out at a dozen call sites, half of them against the named bare
/// constant and half against the literal number it holds, with the convention
/// itself recorded nowhere.
///
/// G of zero means the item does not touch that group, and the caller keeps
/// whatever it had.
constexpr uint16_t equippedGeoset(uint16_t bareId, uint32_t geosetGroupValue) {
    return static_cast<uint16_t>(bareId + geosetGroupValue);
}

/// The cape a cloak selects: its own variant, or the long cape when the item names none.
constexpr uint16_t cloakGeoset(uint32_t geosetGroupValue) {
    return geosetGroupValue > 0 ? equippedGeoset(kGeosetNoCape, geosetGroupValue) : kGeosetWithCape;
}

/// Which geoset group each worn item drives, and the bare variant it replaces.
///
/// The two paths that read equipment come at it from different directions - a
/// player's inventory is numbered one way and an NPC's CreatureDisplayInfoExtra
/// array another - so the slot numbers cannot be shared. What they were both
/// restating is this: which part of the body a given piece of armour changes.
///
/// Kept as named constants rather than a table, because each call site reads one
/// of them and a table would only be looked up by an enum that says the same
/// thing. What matters is that "a chest changes the sleeves" is written once.
namespace equipment {
constexpr uint16_t kChestBare     = kGeosetBareSleeves;   ///< group 8
constexpr uint16_t kLegsBare      = kGeosetBarePants;     ///< group 13
constexpr uint16_t kKneepadsBare  = 901;                  ///< group 9, from legs GeosetGroup2
constexpr uint16_t kPantsCuffBare = 1101;                 ///< group 11, from legs GeosetGroup1
constexpr uint16_t kBootsBare     = kGeosetBareShins;     ///< group 5
constexpr uint16_t kGlovesBare    = kGeosetBareForearms;  ///< group 4
constexpr uint16_t kWristBare     = kGeosetBareSleeves;   ///< group 8, same as chest
constexpr uint16_t kBeltBase      = 1801;                 ///< group 18, the buckle
/// Group 12. A tabard's GeosetGroup1 is 1 (1098 of 1286 CreatureDisplayInfoExtra
/// tabards) and selects 1202, the flap every character model carries; 0 selects
/// nothing. Scholomance Adept 11150 (gnome male) wears tabard 3454.
constexpr uint16_t kTabardBare = kGeosetNoTabard;
/// Group 10, from the chest's GeosetGroup2: 1002 is the flap a doublet hangs
/// below the waist, 1001 none.
constexpr uint16_t kChestFlapBare = kGeosetNoChestFlap;

/// A robe's chest piece or a leg-slot kilt names the skirt over the legs in
/// GeosetGroup3 (1301 + value -> 1302), with chest taking priority over legs.
constexpr uint16_t kRobeKiltBare = kGeosetBarePants;      ///< group 13
}  // namespace equipment

/// Authored ItemDisplayInfo GeosetGroup values for a player's worn equipment.
///
/// TBC ItemDisplayInfo.dbc maps the three GeosetGroup columns per slot:
/// - Chest/Robe/Shirt: GG1 = sleeves (group 8), GG2 = chest flap (group 10),
///   GG3 = robe skirt (group 13).
/// - Legs: GG1 = waist cuff (group 11), GG2 = kneepads (group 9),
///   GG3 = leg-slot kilt/skirt (group 13, when chest GG3 is 0).
/// - Boots: GG1 = shins/boots (group 5).
/// - Gloves: GG1 = forearms/gloves (group 4).
/// - Wrists: GG1 = sleeves fallback (group 8, when chest/shirt GG1 is 0).
/// - Belt: GG1 = buckle (group 18).
/// - Back/Cloak: GG1 = cape style (group 15).
/// - Tabard: GG1 = tabard flap (group 12, hidden when wearing a robe or kilt).
struct PlayerEquipmentGeosetInput {
    uint32_t chestGG1 = 0;
    uint32_t chestGG2 = 0;
    uint32_t chestGG3 = 0;
    uint32_t wristsGG1 = 0;
    uint32_t legsGG1 = 0;
    uint32_t legsGG2 = 0;
    uint32_t legsGG3 = 0;
    uint32_t bootsGG1 = 0;
    uint32_t glovesGG1 = 0;
    uint32_t beltGG1 = 0;
    bool hasCape = false;
    uint32_t capeGG1 = 0;
    bool hasTabard = false;
    uint32_t tabardGG1 = 0;
};

/// Apply equipment geosets onto a character's base geoset set, resolving each
/// group against `available` (the submesh IDs carried by the M2 model).
///
/// Shared by local world player (GameScreen::updateCharacterGeosets), remote
/// world players (EntitySpawner::setOnlinePlayerEquipment), and paperdoll /
/// portrait previews (CharacterPreview::applyEquipment).
inline void applyPlayerEquipmentGeosets(std::unordered_set<uint16_t>& geosets,
                                        const std::unordered_set<uint16_t>& available,
                                        const PlayerEquipmentGeosetInput& input) {
    for (auto it = geosets.begin(); it != geosets.end();) {
        const uint16_t g = geosetGroup(*it);
        if (g == 4 || g == 5 || g == 8 || g == 9 || g == 10 ||
            g == 11 || g == 12 || g == 13 || g == 15 || g == 18) {
            it = geosets.erase(it);
        } else {
            ++it;
        }
    }

    auto insertResolved = [&](uint16_t preferred, uint16_t fallback = 0) {
        uint16_t chosen = resolveGeoset(preferred, available);
        if (chosen == 0 && fallback != 0) {
            chosen = resolveGeoset(fallback, available);
        }
        if (chosen != 0) geosets.insert(chosen);
    };

    // Lowest authored geoset in a group (used for group 5 bare-shin fallback on
    // races like Gnome and Dwarf Female that carry 502..504 without 501).
    auto lowestInGroup = [&](uint16_t group) -> uint16_t {
        uint16_t lowest = 0;
        for (uint16_t id : available) {
            if (geosetGroup(id) != group) continue;
            if (lowest == 0 || id < lowest) lowest = id;
        }
        return lowest;
    };

    const uint32_t robeGG = input.chestGG3 > 0 ? input.chestGG3 : input.legsGG3;
    const bool wearsSkirt = (robeGG > 0);

    // Group 4: gloves / forearms
    insertResolved(
        input.glovesGG1 > 0 ? equippedGeoset(equipment::kGlovesBare, input.glovesGG1)
                            : kGeosetBareForearms,
        kGeosetBareForearms);

    // Group 5: boots / shins. A robe or leg-slot kilt (1302) covers the shins,
    // so keep bare shins underneath rather than bulky cuffed boot meshes.
    const uint16_t wantBoots = (!wearsSkirt && input.bootsGG1 > 0)
        ? equippedGeoset(equipment::kBootsBare, input.bootsGG1)
        : kGeosetBareShins;
    insertResolved(wantBoots, lowestInGroup(5));

    // Group 8: sleeves (chest/shirt takes priority over wrists)
    const uint32_t sleeveGG = input.chestGG1 > 0 ? input.chestGG1 : input.wristsGG1;
    insertResolved(
        sleeveGG > 0 ? equippedGeoset(equipment::kChestBare, sleeveGG)
                     : kGeosetBareSleeves,
        kGeosetBareSleeves);

    // Group 10: chest flap below the waist (from chest GeosetGroup2)
    if (input.chestGG2 > 0) {
        insertResolved(equippedGeoset(equipment::kChestFlapBare, input.chestGG2));
    }

    // Group 9 (kneepads, legs GG2) and Group 11 (waist cuff, legs GG1):
    // suppressed when a robe or leg-slot kilt covers the lower body.
    if (!wearsSkirt) {
        if (input.legsGG2 > 0) {
            insertResolved(equippedGeoset(equipment::kKneepadsBare, input.legsGG2));
        }
        if (input.legsGG1 > 0) {
            insertResolved(equippedGeoset(equipment::kPantsCuffBare, input.legsGG1));
        }
    }

    // Group 13: trousers (1301) or robe/kilt skirt (1302+)
    insertResolved(
        wearsSkirt ? equippedGeoset(equipment::kRobeKiltBare, robeGG)
                   : kGeosetBarePants,
        kGeosetBarePants);

    // Group 18: belt buckle (HD models carry 1801 waist base + 1802+ buckles;
    // stock TBC models carry no group 18 and resolve to 0).
    if (!available.empty() || input.beltGG1 > 0) {
        insertResolved(
            input.beltGG1 > 0 ? equippedGeoset(equipment::kBeltBase, input.beltGG1)
                              : equipment::kBeltBase,
            equipment::kBeltBase);
    }

    // Group 15: cloak mesh or no-cape back panel
    insertResolved(
        input.hasCape ? cloakGeoset(input.capeGG1) : kGeosetNoCape,
        kGeosetNoCape);

    // Group 12: tabard flap (suppressed by both chest robes and leg-slot kilts)
    if (input.hasTabard && !wearsSkirt && input.tabardGG1 > 0) {
        insertResolved(equippedGeoset(equipment::kTabardBare, input.tabardGG1));
    }
}

/// Texture compositing layer priority by WoW inventoryType.
///
/// Ordinary pants composite under boots so boot shins cover lower pant legs.
/// A leg-slot kilt/skirt (`legsHasSkirt == true`, legs GeosetGroup3 > 0) uses
/// the skirt mesh (1302) and composites over boots so the kilt's LegLower hem
/// is not overwritten by boot shins, while still yielding to a chest robe.
constexpr int equipmentTextureLayer(uint8_t inventoryType, bool legsHasSkirt) {
    switch (inventoryType) {
        case 4:  return 0;                       // shirt
        case 7:  return legsHasSkirt ? 2 : 1;    // leggings / leg-slot kilt
        case 8:  return legsHasSkirt ? 1 : 2;    // boots
        case 5:
        case 20: return 3;                       // chest / robe
        case 19: return 5;                       // tabard on top of chest/belt
        default: return 4;
    }
}

}  // namespace core
}  // namespace wowee
