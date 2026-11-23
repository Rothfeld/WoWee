#include "ui/unit_portrait.hpp"
#include "game/equipment_hash.hpp"

#include "core/logger.hpp"
#include "game/game_handler.hpp"
#include "game/inventory.hpp"
#include "rendering/character_preview.hpp"
#include "rendering/renderer.hpp"

namespace wowee::ui {

namespace {

/// FNV-1a over what actually changes a character's look, so a reload happens
/// when the gear changes and not when a stat does.

} // namespace

UnitPortrait::UnitPortrait() = default;

UnitPortrait::~UnitPortrait() = default;

void UnitPortrait::update(game::GameHandler& gameHandler,
                          pipeline::AssetManager* assets,
                          rendering::Renderer* renderer, float deltaTime,
                          uint32_t hiddenSlots) {
    if (!assets || !renderer) return;

    // The character list is where a player's appearance is described; the unit
    // in the world carries a display id, not the pieces it was built from.
    const game::Character* self = nullptr;
    for (const auto& c : gameHandler.getCharacters()) {
        if (c.guid == gameHandler.getPlayerGuid()) { self = &c; break; }
    }
    if (!self) return;

    if (!preview_) {
        preview_ = std::make_unique<rendering::CharacterPreview>();
        // Small: this is drawn into a circle a few dozen pixels across, and the
        // cost of the pass scales with the target.
        initialized_ = preview_->initialize(assets, targetWidth_, targetHeight_);
        if (!initialized_) {
            LOG_WARNING("UnitPortrait: could not build the offscreen view");
            preview_.reset();
            return;
        }
        renderer->registerPreview(preview_.get());
        registered_ = true;
    }

    // What they are wearing now, not what they wore when they logged in.
    //
    // The character list carries the equipment SMSG_CHAR_ENUM described and
    // nothing updates it afterwards, so this portrait kept the gear the
    // character was wearing at the select screen for the whole session. The
    // inventory is the live answer, and it is the same one the target frame
    // gets when this character is the target - the two portraits are of the
    // same person and must not disagree. The login list stands in until the
    // item queries have come back, which is a moment at most.
    const bool hideHelm = !gameHandler.isHelmVisible();
    const bool hideCloak = !gameHandler.isCloakVisible();
    std::vector<game::EquipmentItem> worn;
    {
        std::array<uint32_t, 19> displayIds{};
        std::array<uint8_t, 19> invTypes{};
        if (gameHandler.getOtherPlayerEquipment(self->guid, displayIds, invTypes)) {
            for (size_t slot = 0; slot < displayIds.size(); ++slot) {
                if (displayIds[slot] == 0) continue;
                if (hiddenSlots & (1u << slot)) continue;
                if (hideHelm && slot == static_cast<size_t>(game::EquipSlot::HEAD)) continue;
                if (hideCloak && slot == static_cast<size_t>(game::EquipSlot::BACK)) continue;
                uint8_t invType = invTypes[slot];
                // A One-Hand (13) weapon in the off-hand slot attaches to the
                // left hand (CharacterPreview maps 13 to right hand, 22 to left).
                if (slot == static_cast<size_t>(game::EquipSlot::OFF_HAND) &&
                    invType == game::InvType::ONE_HAND) {
                    invType = game::InvType::OFF_HAND;
                }
                uint8_t sheathType = 0;
                const auto& eqSlot = gameHandler.getInventory().getEquipSlot(
                    static_cast<game::EquipSlot>(slot));
                if (!eqSlot.empty()) {
                    if (const auto* info = gameHandler.getItemInfo(eqSlot.item.itemId)) {
                        sheathType = static_cast<uint8_t>(info->sheath);
                    }
                }
                worn.push_back({.displayModel = displayIds[slot],
                                .inventoryType = invType,
                                .enchantment = 0u,
                                .sheath = sheathType});
            }
        }
        if (worn.empty()) {
            for (const auto& item : self->equipment) {
                if (hideHelm && item.inventoryType == game::InvType::HEAD) continue;
                if (hideCloak && item.inventoryType == game::InvType::BACK) continue;
                worn.push_back(item);
            }
        }
    }

    pipeline::GuildTabardEmblem activeTabard;
    if (hasPendingGuildTabard_) {
        activeTabard = pendingGuildTabard_;
    } else {
        const auto& qd = gameHandler.getGuildQueryData();
        if (qd.guildId != 0) {
            activeTabard.emblemStyle = qd.emblemStyle;
            activeTabard.emblemColor = qd.emblemColor;
            activeTabard.borderStyle = qd.borderStyle;
            activeTabard.borderColor = qd.borderColor;
            activeTabard.backgroundColor = qd.backgroundColor;
        }
    }
    if (forceGuildTabard_) {
        // TabardModel previews the custom Guild Tabard on the player even if
        // they do not currently own or equip item 5976.
        worn.erase(std::remove_if(worn.begin(), worn.end(), [](const game::EquipmentItem& e) {
            return e.inventoryType == game::InvType::TABARD;
        }), worn.end());
        worn.push_back({.displayModel = pipeline::GuildTabardEmblem::kGuildTabardDisplayId,
                        .inventoryType = game::InvType::TABARD,
                        .enchantment = 0u});
    }

    uint32_t appearanceBytes = self->appearanceBytes;
    uint8_t facialFeatures = self->facialFeatures;
    {
        uint8_t liveRace = 0, liveGender = 0, liveFacial = 0;
        uint32_t liveAppearance = 0;
        if (gameHandler.getPlayerAppearance(self->guid, liveRace, liveGender,
                                            liveAppearance, liveFacial)) {
            appearanceBytes = liveAppearance;
            facialFeatures = liveFacial;
        }
    }

    const uint64_t equipHash = game::hashEquipmentAppearance(worn);
    const bool changed = (loadedGuid_ != self->guid) ||
                         (loadedAppearance_ != appearanceBytes) ||
                         (loadedFacialFeatures_ != facialFeatures) ||
                         (loadedEquipHash_ != equipHash) ||
                         (loadedGuildTabard_ != activeTabard) ||
                         (loadedForceGuildTabard_ != forceGuildTabard_);
    if (changed) {
        const uint8_t skin      =  appearanceBytes        & 0xFF;
        const uint8_t face      = (appearanceBytes >> 8)  & 0xFF;
        const uint8_t hairStyle = (appearanceBytes >> 16) & 0xFF;
        const uint8_t hairColor = (appearanceBytes >> 24) & 0xFF;

        // Declared before the model loads, so the racial backdrop is never
        // built in the first place. Face portraits clear to an opaque black
        // disc (matching native 2.4.3); full-body frames stay transparent.
        preview_->setBackgroundMode(
            rendering::portraitBackgroundModeForFraming(framing_ == Framing::Face));
        if (preview_->loadCharacter(self->race, self->gender, skin, face,
                                    hairStyle, hairColor, facialFeatures,
                                    self->useFemaleModel)) {
            preview_->setSheathState(sheathState_);
            preview_->applyEquipment(worn, activeTabard.isValid() ? &activeTabard : nullptr);
            // After the model, because its bounds are what the framing is
            // measured against.
            if (framing_ == Framing::Face) preview_->setPortraitFraming();
        }
        // Logged because a portrait that rebuilds every frame looks like one
        // that flickers, and the two are indistinguishable from outside.
        LOG_INFO("UnitPortrait: rebuilt for guid ", self->guid,
                 " appearance ", appearanceBytes);
        loadedCreaturePath_.clear();
        loadedGuid_ = self->guid;
        loadedRace_ = static_cast<uint8_t>(self->race);
        loadedGender_ = (self->gender == game::Gender::FEMALE ||
                         (self->gender == game::Gender::NONBINARY && self->useFemaleModel)) ? 1u : 0u;
        loadedBake_.clear();
        loadedAppearance_ = appearanceBytes;
        loadedFacialFeatures_ = facialFeatures;
        loadedEquipHash_ = equipHash;
        loadedGuildTabard_ = activeTabard;
        loadedForceGuildTabard_ = forceGuildTabard_;
    }

    if (framing_ == Framing::FullBody && !applyModelFrameView()) return;
    preview_->update(deltaTime);
    preview_->render();
    preview_->requestComposite();
}

bool UnitPortrait::applyModelFrameView() {
    if (preview_->setModelFrameView(modelFacing_, modelCameraScale_)) {
        modelFrameViewMissing_ = false;
        return true;
    }
    if (preview_->isModelLoaded() && !modelFrameViewMissing_) {
        LOG_ERROR("UnitPortrait: player model has no character-sheet camera (M2 camera type 1)");
    }
    modelFrameViewMissing_ = true;
    return false;
}

void UnitPortrait::setSheathState(uint8_t sheathState) {
    sheathState_ = sheathState;
    if (preview_) preview_->setSheathState(sheathState);
}

bool UnitPortrait::updatePlayer(uint8_t race, uint8_t gender,
                                uint32_t appearanceBytes, uint8_t facialFeatures,
                                const std::vector<game::EquipmentItem>& equipment,
                                pipeline::AssetManager* assets,
                                rendering::Renderer* renderer, float deltaTime,
                                uint8_t textureRace, uint8_t textureGender) {
    if (!assets || !renderer) return false;

    if (!preview_) {
        preview_ = std::make_unique<rendering::CharacterPreview>();
        initialized_ = preview_->initialize(assets, targetWidth_, targetHeight_);
        if (!initialized_) {
            LOG_WARNING("UnitPortrait: could not build the offscreen view");
            preview_.reset();
            return false;
        }
        renderer->registerPreview(preview_.get());
        registered_ = true;
    }

    // The same three keys the player's own portrait compares, minus the guid -
    // this is asked per unit and the caller has already decided which.
    const uint64_t equipHash = game::hashEquipmentAppearance(equipment);
    const bool changed = (loadedAppearance_ != appearanceBytes) ||
                         (loadedFacialFeatures_ != facialFeatures) ||
                         (loadedRace_ != race) || (loadedGender_ != gender) ||
                         (loadedTextureRace_ != textureRace) ||
                         (loadedTextureGender_ != textureGender) ||
                         (loadedEquipHash_ != equipHash) ||
                         (loadedBake_ != pendingBake_) ||
                         (loadedGuildTabard_ != pendingGuildTabard_) ||
                         !loadedCreaturePath_.empty();
    if (changed) {
        const uint8_t skin      =  appearanceBytes        & 0xFF;
        const uint8_t face      = (appearanceBytes >> 8)  & 0xFF;
        const uint8_t hairStyle = (appearanceBytes >> 16) & 0xFF;
        const uint8_t hairColor = (appearanceBytes >> 24) & 0xFF;

        preview_->setBackgroundMode(
            rendering::portraitBackgroundModeForFraming(framing_ == Framing::Face));
        if (preview_->loadCharacter(static_cast<game::Race>(race),
                                    static_cast<game::Gender>(gender),
                                    skin, face, hairStyle, hairColor,
                                    facialFeatures, gender == 1,
                                    textureRace, textureGender)) {
            // After the model, because applyEquipment reads its geosets, and
            // only where there is something to apply - an empty list is
            // "nothing known yet", and dressing a model in it strips it.
            if (!equipment.empty()) {
                preview_->applyEquipment(
                    equipment,
                    (hasPendingGuildTabard_ && pendingGuildTabard_.isValid())
                        ? &pendingGuildTabard_
                        : nullptr);
            }
            // After the equipment, because both write the skin slot and the
            // bake is the more complete answer: it already has the armour on
            // it, which is the half applyEquipment cannot composite for an NPC.
            if (!pendingBake_.empty()) preview_->setBakedSkin(pendingBake_);
            if (framing_ == Framing::Face) preview_->setPortraitFraming();
        }
        LOG_INFO("UnitPortrait: rebuilt for player race ", static_cast<int>(race),
                 " appearance ", appearanceBytes);
        loadedCreaturePath_.clear();
        loadedGuid_ = 0;
        loadedRace_ = race;
        loadedGender_ = gender;
        loadedTextureRace_ = textureRace;
        loadedTextureGender_ = textureGender;
        loadedAppearance_ = appearanceBytes;
        loadedFacialFeatures_ = facialFeatures;
        loadedEquipHash_ = equipHash;
        loadedBake_ = pendingBake_;
        loadedGuildTabard_ = pendingGuildTabard_;
    }

    if (framing_ == Framing::FullBody && !applyModelFrameView()) return false;
    preview_->update(deltaTime);
    preview_->render();
    preview_->requestComposite();
    return preview_->isModelLoaded();
}

bool UnitPortrait::updateCreature(const std::string& m2Path,
                                  const std::vector<std::pair<uint32_t, std::string>>& skins,
                                  pipeline::AssetManager* assets,
                                  rendering::Renderer* renderer,
                                  float deltaTime) {
    if (!assets || !renderer || m2Path.empty()) return false;

    if (!preview_) {
        preview_ = std::make_unique<rendering::CharacterPreview>();
        initialized_ = preview_->initialize(assets, targetWidth_, targetHeight_);
        if (!initialized_) {
            LOG_WARNING("UnitPortrait: could not build the offscreen view");
            preview_.reset();
            return false;
        }
        renderer->registerPreview(preview_.get());
        registered_ = true;
    }

    if (loadedCreaturePath_ != m2Path) {
        // Declared before the model loads, so the racial backdrop is never
        // built in the first place - the same order loadCharacter needs.
        preview_->setBackgroundMode(
            rendering::portraitBackgroundModeForFraming(framing_ == Framing::Face));
        if (preview_->loadCreature(m2Path, skins)) {
            if (framing_ == Framing::Face) {
                preview_->setPortraitFraming();
            } else {
                preview_->resetView();
            }
        }
        LOG_INFO("UnitPortrait: rebuilt for creature ", m2Path);
        loadedCreaturePath_ = m2Path;
        // A player and a creature share one preview, so loading either has to
        // forget what the other was, or switching back would find nothing
        // changed and keep drawing the wrong one.
        loadedGuid_ = 0;
        loadedAppearance_ = 0;
        loadedFacialFeatures_ = 0;
        loadedEquipHash_ = 0;
    }

    preview_->update(deltaTime);
    preview_->render();
    preview_->requestComposite();
    return preview_->isModelLoaded();
}

uint64_t UnitPortrait::textureId() const {
    if (!preview_) return 0;
    return reinterpret_cast<uint64_t>(preview_->getTextureId());
}

bool UnitPortrait::updateKnownSubject(uint64_t guid, float deltaTime) {
    if (guid == 0 || guid != subjectGuid_ || !preview_ || !preview_->isModelLoaded())
        return false;
    preview_->update(deltaTime);
    preview_->render();
    preview_->requestComposite();
    return true;
}

void UnitPortrait::shutdown(rendering::Renderer* renderer) {
    if (preview_ && registered_ && renderer) renderer->unregisterPreview(preview_.get());
    registered_ = false;
    preview_.reset();
    initialized_ = false;
    subjectGuid_ = 0;
}

} // namespace wowee::ui
