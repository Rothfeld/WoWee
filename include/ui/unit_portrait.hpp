#pragma once

// A live head-and-shoulders view of a unit, for the interface's portraits.
//
// WoW's portraits are not pictures on disk: they are the character itself,
// rendered small. The same offscreen pass the character-select screen uses
// already frames a face when zoomed all the way in, so this is that pass kept
// running while in the world, at a size a portrait needs.
//
// Four units: the player, the target, the pet and the focus. They differ only
// in whose appearance is loaded - a player from race, appearance bytes and
// what they are visibly wearing, anything else from the model its display id
// names. The party frames want the same thing and are left out on cost: each
// of these is a 640x800 offscreen target and a character pass every frame.

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "pipeline/guild_tabard.hpp"

namespace wowee {
namespace game { class GameHandler; struct EquipmentItem; }
namespace pipeline { class AssetManager; }
namespace rendering { class CharacterPreview; class Renderer; }

namespace ui {

/// TBC 2.4.3 fallback 2D portrait texture path
/// (`Interface\CharacterFrame\TemporaryPortrait-*`) for units without a live
/// 3D portrait model (e.g. a never-seen out-of-range party member).
/// `gender`: 0 = Male, 1 = Female, 0xFF = unknown.
inline std::string temporaryPortraitPath(uint8_t raceId, uint8_t gender, bool isPet = false) {
    if (isPet) {
        return "Interface\\CharacterFrame\\TemporaryPortrait-Pet";
    }
    const char* raceName = nullptr;
    switch (raceId) {
        case 1:  raceName = "Human";    break;
        case 2:  raceName = "Orc";      break;
        case 3:  raceName = "Dwarf";    break;
        case 4:  raceName = "NightElf"; break;
        case 5:  raceName = "Scourge";  break;
        case 6:  raceName = "Tauren";   break;
        case 7:  raceName = "Gnome";    break;
        case 8:  raceName = "Troll";    break;
        case 10: raceName = "BloodElf"; break;
        case 11: raceName = "Draenei";  break;
        default: break;
    }
    if (raceName && (gender == 0 || gender == 1)) {
        return std::string("Interface\\CharacterFrame\\TemporaryPortrait-") +
               (gender == 1 ? "Female-" : "Male-") + raceName;
    }
    return "Interface\\CharacterFrame\\TemporaryPortrait";
}

/// Resolve the 2D fallback portrait path for a unit slot when no 3D portrait is
/// drawn (`drawnTextureId == 0`, e.g. an out-of-visibility party member where
/// `!UnitIsVisible("party1")`). Matches Native TBC 2.4.3 `SetPortraitTexture`.
inline std::string resolveUnitPortraitFallback(uint64_t drawnTextureId,
                                               std::string_view customPortrait,
                                               uint64_t guid,
                                               uint8_t raceId,
                                               uint8_t gender,
                                               bool isPet = false) {
    if (!customPortrait.empty()) return std::string(customPortrait);
    if (drawnTextureId != 0 || guid == 0) return {};
    return temporaryPortraitPath(raceId, gender, isPet);
}

/// Resolve `CreatureDisplayInfo.dbc` col 9 (`PortraitTextureName`) to a UI
/// texture path (`Interface\Icons\<Name>`). Empty when the display uses a
/// normal 3D head portrait.
inline std::string creatureDisplayPortraitPath(std::string_view portraitTextureName) {
    if (portraitTextureName.empty()) return {};
    if (portraitTextureName.find('\\') != std::string_view::npos ||
        portraitTextureName.find('/') != std::string_view::npos) {
        return std::string(portraitTextureName);
    }
    std::string path = "Interface\\Icons\\";
    path.append(portraitTextureName);
    return path;
}

class UnitPortrait {
public:
    /// How much of the character to show. A portrait is the face in a circle;
    /// the paperdoll wants the whole figure in a tall rectangle. The offscreen
    /// pass is the same either way - only the framing differs, which is why
    /// this is one class and not two.
    enum class Framing { Face, FullBody };

    UnitPortrait();
    ~UnitPortrait();

    /// Builds the offscreen view on first use and keeps it in step with the
    /// player's appearance and equipment afterwards. Safe to call every frame;
    /// it reloads the model only when something about it actually changed.
    /// `hiddenSlots` (bit = EquipSlot) are left off the figure: the weapons
    /// the world model stopped drawing when they broke.
    void update(game::GameHandler& gameHandler, pipeline::AssetManager* assets,
                rendering::Renderer* renderer, float deltaTime,
                uint32_t hiddenSlots = 0);

    /// Answers whether there is actually a model to show. A load can fail -
    /// a race this client has no model path for, an M2 the install is missing
    /// - and the view then keeps whatever it had, which for a frame that has
    /// just changed unit means the previous one's face. The caller blanks the
    /// texture instead.
    ///
    /// Show another player, from the appearance the world already reads for
    /// them: race, gender, the packed appearance bytes and facial features.
    ///
    /// Dressed in what they are visibly wearing, which for a portrait framed
    /// on the head is the helm and the shoulders - the two pieces that change
    /// a face most. An empty list leaves the model as it is rather than
    /// stripping it, because "nothing known yet" and "wearing nothing" arrive
    /// looking the same and only one of them should undress anybody.
    bool updatePlayer(uint8_t race, uint8_t gender, uint32_t appearanceBytes,
                      uint8_t facialFeatures,
                      const std::vector<game::EquipmentItem>& equipment,
                      pipeline::AssetManager* assets,
                      rendering::Renderer* renderer, float deltaTime,
                      uint8_t textureRace = 0, uint8_t textureGender = 0xFF);

    /// Show a creature instead, by the model its display id names.
    ///
    /// Kept apart from update() rather than folded into it: a player is built
    /// from race, appearance bytes and equipment, and a creature is a path and
    /// nothing else. The two share the offscreen view and the framing and
    /// agree on nothing else, so one function taking both would be two
    /// functions sharing a name.
    ///
    /// Reloads only when the path changes, so this is safe every frame.
    bool updateCreature(const std::string& m2Path,
                        const std::vector<std::pair<uint32_t, std::string>>& skins,
                        pipeline::AssetManager* assets,
                        rendering::Renderer* renderer, float deltaTime);

    /// A pre-composited skin to put on the character once it is built,
    /// replacing the one made from CharSections.
    ///
    /// CreateDisplayInfoExtra carries one for nearly every humanoid NPC, and
    /// it is the whole appearance already baked - skin, face, hair and the
    /// armour they wear. Set before the update that should use it; it is part
    /// of what decides a rebuild, so changing it is enough to apply it.
    /// Set before the update to composite a specific guild tabard design
    /// (e.g. TabardModel preview or another player's guild emblem).
    void setGuildTabard(const pipeline::GuildTabardEmblem& emblem, bool forceEquip = false) {
        pendingGuildTabard_ = emblem;
        hasPendingGuildTabard_ = true;
        forceGuildTabard_ = forceEquip;
    }
    void clearGuildTabard() {
        pendingGuildTabard_ = pipeline::GuildTabardEmblem{};
        hasPendingGuildTabard_ = false;
        forceGuildTabard_ = false;
    }

    void setBakedSkin(const std::string& path) { pendingBake_ = path; }

    /// Set before the first update, since framing is applied when the model
    /// loads and the model loads once.
    void setFraming(Framing framing) { framing_ = framing; }

    /// How big the offscreen image is, in pixels. Set before the first update,
    /// because the view is built once and keeps the size it was built at.
    ///
    /// Worth setting: this is a full character pass every frame, and the
    /// paperdoll's 640x800 is enormously oversized for a face drawn into a
    /// circle fifty pixels across.
    void setTargetSize(int width, int height) {
        targetWidth_ = width;
        targetHeight_ = height;
    }

    /// What a full-body player figure is shown with: the Model frame's
    /// facing (SetRotation, radians) and camera scale (model scale at the
    /// last SetUnit over the current one). Applied every update.
    void setModelFrameView(float facing, float cameraScale) {
        modelFacing_ = facing;
        modelCameraScale_ = cameraScale;
    }

    /// Weapon sheath state for full-body previews (e.g. PaperDoll CharacterModelFrame).
    void setSheathState(uint8_t sheathState);

    /// The rendered portrait, or zero until the first composite has run. The
    /// value is a VkDescriptorSet, carried as an integer so this header does
    /// not drag Vulkan into the widget tree.
    [[nodiscard]] uint64_t textureId() const;

    void shutdown(rendering::Renderer* renderer);

private:
    /// setModelFrameView on the preview; false (logged once) when the model
    /// has no character-sheet camera.
    bool applyModelFrameView();

    std::unique_ptr<rendering::CharacterPreview> preview_;
    bool initialized_ = false;
    Framing framing_ = Framing::Face;
    float modelFacing_ = 0.0f;
    float modelCameraScale_ = 1.0f;
    /// A full-body player figure whose model lacks the character-sheet
    /// camera is not drawn; this is the load that found so.
    bool modelFrameViewMissing_ = false;
    int targetWidth_ = 640;
    int targetHeight_ = 800;
    bool registered_ = false;

    // What the loaded model was built from, so a reload happens only on a real
    // change rather than every frame.
    /// The creature model currently loaded, empty while a player is loaded.
    /// Also the guard against reloading: a portrait rebuilt every frame looks
    /// like one that flickers, and the two are indistinguishable from outside.
    std::string loadedCreaturePath_;
    /// The bake asked for, and the one already on the model.
    std::string pendingBake_;
    std::string loadedBake_;

    pipeline::GuildTabardEmblem pendingGuildTabard_;
    pipeline::GuildTabardEmblem loadedGuildTabard_;
    bool hasPendingGuildTabard_ = false;
    bool forceGuildTabard_ = false;
    bool loadedForceGuildTabard_ = false;

    uint64_t loadedGuid_ = 0;
    /// Race and gender as well, because another player is identified by these
    /// rather than by a guid that this only ever sees one of at a time.
    uint8_t  loadedRace_ = 0xFF;
    uint8_t  loadedGender_ = 0xFF;
    uint8_t  loadedTextureRace_ = 0;
    uint8_t  loadedTextureGender_ = 0xFF;
    uint32_t loadedAppearance_ = 0;
    uint8_t  loadedFacialFeatures_ = 0;
    // uint64_t, not size_t: the hash is sixty-four bits and storing it in a
    // pointer-sized field would truncate on a 32-bit build, where two outfits
    // sharing the low half would stop the portrait redrawing.
    uint64_t loadedEquipHash_ = 0;
    uint8_t sheathState_ = 1;
};

} // namespace ui
} // namespace wowee
