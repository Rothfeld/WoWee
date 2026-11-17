#include "ui/game_screen.hpp"
#include "ui/framexml_takeover.hpp"
#include "ui/sight_cache.hpp"
#include "addons/lua_api_registrations.hpp"
#include "ui/ui_texture_load.hpp"
#include "ui/ui_upload_budget.hpp"
#include "core/helm_visual.hpp"
#include "ui/ui_raid_icons.hpp"
#include "ui/ui_colors.hpp"
#include "ui/ui_helpers.hpp"
#include "ui/nameplate_stacking.hpp"
#include "game/summon_title.hpp"
#include "game/level_difficulty.hpp"
#include "rendering/vk_context.hpp"
#include "core/application.hpp"
#include "core/appearance_composer.hpp"
#include "core/entity_spawner.hpp"
#include "ui/map_window.hpp"
#include "addons/addon_manager.hpp"
#include "core/coordinates.hpp"
#include "core/input.hpp"
#include "rendering/renderer.hpp"
#include "rendering/post_process_pipeline.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/minimap.hpp"
#include "rendering/world_map.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/music_manager.hpp"
#include "game/zone_manager.hpp"
#include "game/game_utils.hpp"
#include "audio/footstep_manager.hpp"
#include "audio/activity_sound_manager.hpp"
#include "audio/mount_sound_manager.hpp"
#include "audio/npc_voice_manager.hpp"
#include "audio/ambient_sound_manager.hpp"
#include "audio/ui_sound_manager.hpp"
#include "audio/combat_sound_manager.hpp"
#include "audio/spell_sound_manager.hpp"
#include "audio/movement_sound_manager.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "core/geoset_rules.hpp"
#include "pipeline/item_textures.hpp"

#include "game/expansion_profile.hpp"
#include <cassert>
#include "game/character.hpp"
#include "core/logger.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_vulkan.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cctype>
#include <chrono>
#include <ctime>
#include <limits>

#include <unordered_set>
#include "ui/framexml_takeover.hpp"
#include "addons/lua_api_helpers.hpp"
#include "core/local_time.hpp"
#include "pipeline/spell_icon_paths.hpp"

namespace {
    using namespace wowee::ui::colors;
    using namespace wowee::ui::helpers;



}

namespace wowee { namespace ui {



void GameScreen::updateCharacterGeosets(game::Inventory& inventory) {
    auto& app = core::Application::getInstance();
    auto* renderer = app.getRenderer();
    if (!renderer) return;

    uint32_t instanceId = renderer->getCharacterInstanceId();
    if (instanceId == 0) return;

    auto* charRenderer = renderer->getCharacterRenderer();
    if (!charRenderer) return;

    auto* assetManager = app.getAssetManager();

    // Load ItemDisplayInfo.dbc for geosetGroup lookup
    std::shared_ptr<pipeline::DBCFile> displayInfoDbc;
    if (assetManager) {
        displayInfoDbc = assetManager->loadDBC("ItemDisplayInfo.dbc");
    }
    const auto* idiL = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
    const uint32_t geosetGroup1Field = idiL ? (*idiL)["GeosetGroup1"] : 7;
    const uint32_t geosetGroup3Field = idiL ? (*idiL)["GeosetGroup3"] : 9;

    auto getGeosetGroup = [&](uint32_t displayInfoId, uint32_t fieldIdx) -> uint32_t {
        if (!displayInfoDbc || displayInfoId == 0) return 0;
        int32_t recIdx = displayInfoDbc->findRecordById(displayInfoId);
        if (recIdx < 0) return 0;
        return displayInfoDbc->getUInt32(static_cast<uint32_t>(recIdx), fieldIdx);
    };

    // Helper: find first equipped item matching inventoryType, return its displayInfoId
    auto findEquippedDisplayId = [&](std::initializer_list<uint8_t> types) -> uint32_t {
        for (int s = 0; s < game::Inventory::NUM_EQUIP_SLOTS; s++) {
            const auto& slot = inventory.getEquipSlot(static_cast<game::EquipSlot>(s));
            if (!slot.empty()) {
                for (uint8_t t : types) {
                    if (slot.item.inventoryType == t)
                        return slot.item.displayInfoId;
                }
            }
        }
        return 0;
    };

    // Helper: check if any equipment slot has the given inventoryType
    auto hasEquippedType = [&](std::initializer_list<uint8_t> types) -> bool {
        for (int s = 0; s < game::Inventory::NUM_EQUIP_SLOTS; s++) {
            const auto& slot = inventory.getEquipSlot(static_cast<game::EquipSlot>(s));
            if (!slot.empty()) {
                for (uint8_t t : types) {
                    if (slot.item.inventoryType == t) return true;
                }
            }
        }
        return false;
    };

    std::unordered_set<uint16_t> geosets;
    if (appearanceComposer_) {
        if (auto* gh = app.getGameHandler()) {
            if (const auto* ch = gh->getActiveCharacter()) {
                const uint8_t raceId = static_cast<uint8_t>(ch->race);
                const uint8_t sexId = static_cast<uint8_t>(ch->gender);
                const uint8_t hairStyleId = static_cast<uint8_t>((ch->appearanceBytes >> 16) & 0xFF);
                const uint8_t facialId = ch->facialFeatures;
                geosets = appearanceComposer_->buildDefaultPlayerGeosets(raceId, sexId, hairStyleId, facialId);
            }
        }
    }
    if (geosets.empty()) {
        // The same bare set the character itself is built from, rather than a
        // shorter one written out here. This listed six ids and the real one
        // names twelve: no bare forearms, shins, sleeves, kneepads or pants,
        // and only one of the two feet spellings - so a portrait that fell
        // back to it drew a body missing the parts those groups carry. The
        // builder's own comment records the portrait and the player
        // disagreeing this way once before.
        //
        // Zeros for the hair and facial variants: this branch is reached when
        // there is no character to read them from, and a zero variant adds
        // nothing rather than guessing one.
        geosets = core::bareCharacterGeosets(0, 0, 0, 0, 0);
    }

    auto eraseGroup = [&](uint16_t group) {
        for (auto it = geosets.begin(); it != geosets.end();) {
            if ((*it / 100) == group) it = geosets.erase(it);
            else ++it;
        }
    };

    // Build set of geoset IDs present in the model for validation.
    // Races like Gnome (no 501) and Tauren (only 505) need fallback.
    std::unordered_set<uint16_t> modelGeosets;
    if (const auto* modelData = charRenderer->getInstanceModelData(instanceId)) {
        for (const auto& batch : modelData->batches) {
            modelGeosets.insert(batch.submeshId);
        }
    }

    // The same rule the player, NPC and portrait paths use, from
    // core::geoset_rules - ask for a variant, get the one this model has.
    auto pickGeoset = [&](uint16_t preferred) {
        return core::resolveGeoset(preferred, modelGeosets);
    };

    eraseGroup(4);
    eraseGroup(5);
    eraseGroup(8);
    eraseGroup(13);
    eraseGroup(15);
    eraseGroup(12);

    // CharGeosets mapping (verified via vertex bounding boxes):
    //   Group 4 (401+) = GLOVES (forearm area, Z~1.1-1.4)
    //   Group 5 (501+) = BOOTS  (shin area, Z~0.1-0.6)
    //   Group 8 (801+) = WRISTBANDS/SLEEVES (controlled by chest armor)
    //   Group 9 (901+) = KNEEPADS
    //   Group 13 (1301+) = TROUSERS/PANTS
    //   Group 15 (1501+) = CAPE/CLOAK
    //   Group 20 (2002) = FEET

    // Gloves: inventoryType 10 → group 4 (forearms)
    {
        uint32_t did = findEquippedDisplayId({10});
        uint32_t gg = getGeosetGroup(did, geosetGroup1Field);
        geosets.insert(pickGeoset(gg > 0 ? core::equippedGeoset(core::equipment::kGlovesBare, gg)
                                          : core::kGeosetBareForearms));
    }

    // Boots: inventoryType 8 → group 5 (shins/lower legs)
    {
        uint32_t did = findEquippedDisplayId({8});
        uint32_t gg = getGeosetGroup(did, geosetGroup1Field);
        uint16_t selectedShin = pickGeoset(gg > 0 ? core::equippedGeoset(core::equipment::kBootsBare, gg)
                                                  : core::kGeosetBareShins);
        geosets.insert(selectedShin);
    }

    // Chest/Shirt: inventoryType 4 (shirt), 5 (chest), 20 (robe)
    // Controls group 8 (wristbands/sleeve length): 801=bare wrists, 802+=sleeve styles
    // Also controls group 13 (trousers) via GeosetGroup[2] for robes
    {
        uint32_t did = findEquippedDisplayId({5, 20});
        if (did == 0) did = findEquippedDisplayId({4});
        uint32_t gg = getGeosetGroup(did, geosetGroup1Field);
        geosets.insert(gg > 0 ? core::equippedGeoset(core::equipment::kChestBare, gg)
                              : core::kGeosetBareSleeves);
        uint32_t gg3 = getGeosetGroup(did, geosetGroup3Field);
        if (gg3 > 0) {
            geosets.insert(core::equippedGeoset(core::equipment::kRobeKiltBare, gg3));
        }
    }

    // Kneepads: group 9 is not worn bare. 902 is a kneepad mesh - on the human
    // female a band twice the shin's radius from mid-shin to above the knee -
    // and on a character with no leg armour it reads as the top of a boot. The
    // knee is already carried by the bare pants (1301) and shins (501).

    // Legs/Pants: inventoryType 7 → group 13 (trousers/thighs)
    // 1301=bare legs, 1302+=pant/kilt styles
    {
        uint32_t did = findEquippedDisplayId({7});
        uint32_t gg = getGeosetGroup(did, geosetGroup1Field);
        // Only add if robe hasn't already set a kilt geoset
        // Only when the robe above has not already put a kilt on the legs.
        // 1302 and 1303 are the first two kilt variants; anything in group 13
        // beyond the bare one means something is already covering them.
        const bool kiltAlreadySet = std::any_of(
            geosets.begin(), geosets.end(), [](uint16_t g) {
                return core::geosetGroup(g) == core::geosetGroup(core::kGeosetBarePants) &&
                       !core::geosetMeansNone(g);
            });
        if (!kiltAlreadySet) {
            geosets.insert(gg > 0 ? core::equippedGeoset(core::equipment::kLegsBare, gg)
                                  : core::kGeosetBarePants);
        }
    }

    // Whether a helm and a cloak are *shown*, which is not the same question as
    // whether one is worn. Both toggles hide the model and neither was reaching
    // the geosets, so turning the cloak off left the cloak's own body geoset on
    // and turning the helm off left the player bald under nothing.
    bool helmShown = true, cloakShown = true;
    if (auto* gh = app.getGameHandler()) {
        helmShown = gh->isHelmVisible();
        cloakShown = gh->isCloakVisible();
    }

    // Back/Cloak: inventoryType 16 → group 15
    uint32_t capeGG = getGeosetGroup(findEquippedDisplayId({16}), geosetGroup1Field);
    geosets.insert((hasEquippedType({16}) && cloakShown) ? core::cloakGeoset(capeGG)
                                                         : core::kGeosetNoCape);

    // Tabard: inventoryType 19 → group 12
    if (hasEquippedType({19})) {
        geosets.insert(core::kGeosetDefaultTabard);
    }

    // Hide hair under a helm: drop the style scalp and put the bald cap on.
    // Group 0 holds the body plus one scalp; 0 itself is the body and stays.
    // The other-player path has always done this, so a helm covered their hair
    // and not yours.
    // Only while it is actually on the head. The show-helm toggle takes the
    // model away and left this branch running, so hiding a helm hid the hair
    // with it and the player stood there bald wearing nothing.
    if (hasEquippedType({1}) && helmShown) {
        const uint32_t headDisplayId = findEquippedDisplayId({1});
        uint8_t genderId = 0;
        if (auto* gh = app.getGameHandler()) {
            if (const auto* ch = gh->getActiveCharacter()) {
                genderId = static_cast<uint8_t>(ch->gender);
            }
        }
        // A circlet, tiara or crown sits over the hair rather than covering it,
        // and the data says which does what - see core::helmHidesHair.
        if (auto* assets = app.getAssetManager();
            assets && core::helmHidesHair(*assets, headDisplayId, genderId)) {
            for (auto it = geosets.begin(); it != geosets.end();) {
                if (*it != 0 && (*it / 100) == 0) it = geosets.erase(it);
                else ++it;
            }
            geosets.insert(1);
        }
    }

    // Bald with nothing on your head, which is the reported bug and the one
    // thing this function can say for certain about it.
    //
    // Group 0 holds the body (geoset 0) and one scalp. Wearing a helm that
    // covers hair replaces the style scalp with 1, the bald cap - that is the
    // branch above. With no helm equipped there is nothing to do that, so a
    // set whose only group-0 members are 0 and 1 means the scalp was never
    // selected rather than removed, and the fault is upstream in
    // buildDefaultPlayerGeosets or the hair map it reads.
    //
    // Silent whenever it is working: it can only fire when the character is
    // bare-headed and bald. The measurement that ruled out the DBC is at
    // entity_spawner.cpp, and what remains unproven is which of the two
    // possible answers this is.
    if (!hasEquippedType({1})) {
        bool hasStyleScalp = false;
        for (uint16_t g : geosets) {
            if (g / 100 == 0 && g != 0 && g != 1) { hasStyleScalp = true; break; }
        }
        if (!hasStyleScalp && geosets.count(1) > 0) {
            LOG_WARNING("Player geosets carry the bald cap with no helm equipped - "
                        "the hair scalp was never selected. Hair style byte and "
                        "CharHairGeosets lookup are the two places to look.");
        }
    }

    // Groups 17 and 18 are the Death Knight / Night Elf eye glow. Nothing here
    // should ever select one, and the renderer only auto-skips them when no
    // geoset filter is applied - with a filter, whatever is in this set is what
    // gets drawn. Glowing eyes on a character that should not have them come
    // from exactly this, so say so rather than leaving it to be spotted.
    for (uint16_t g : geosets) {
        const uint16_t group = g / 100;
        if (group == 17 || group == 18) {
            LOG_WARNING("Player geosets include eye-glow geoset ", g,
                        " (group ", group, ") - this will draw glowing eyes");
        }
    }

    charRenderer->setActiveGeosets(instanceId, geosets);
}

void GameScreen::updateCharacterTextures(game::Inventory& inventory) {
    auto& app = core::Application::getInstance();
    auto* renderer = app.getRenderer();
    if (!renderer) return;

    auto* charRenderer = renderer->getCharacterRenderer();
    if (!charRenderer) return;

    auto* assetManager = app.getAssetManager();
    if (!assetManager) return;

    const auto& bodySkinPath = app.getBodySkinPath();
    const auto& underwearPaths = app.getUnderwearPaths();
    uint32_t skinSlot = app.getSkinTextureSlotIndex();

    if (bodySkinPath.empty()) return;

    // Component directory names indexed by region

    // Load ItemDisplayInfo.dbc
    auto displayInfoDbc = assetManager->loadDBC("ItemDisplayInfo.dbc");
    if (!displayInfoDbc) return;
    const auto* idiL = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
    uint32_t texRegionFields[8];
    pipeline::getItemDisplayInfoTextureFields(*displayInfoDbc, idiL, texRegionFields);

    // Collect equipment texture regions from all equipped items
    std::vector<std::pair<int, std::string>> regionLayers;

    for (int s : {0, 1, 2, 3, 6, 7, 4, 8, 9, 5, 10, 11, 12, 13, 14, 15, 16, 17, 18}) {
        const auto& slot = inventory.getEquipSlot(static_cast<game::EquipSlot>(s));
        if (slot.empty() || slot.item.displayInfoId == 0) continue;

        int32_t recIdx = displayInfoDbc->findRecordById(slot.item.displayInfoId);
        if (recIdx < 0) continue;

        for (int region = 0; region < 8; region++) {
            std::string texName = displayInfoDbc->getString(
                static_cast<uint32_t>(recIdx), texRegionFields[region]);
            if (texName.empty()) continue;

            // Which of _M, _F, _U exists is not recorded anywhere, so the
            // order they are asked in is the rule - pipeline/item_textures.hpp.
            bool isFemale = false;
            if (auto* gh = app.getGameHandler()) {
                if (auto* ch = gh->getActiveCharacter()) {
                    isFemale = (ch->gender == game::Gender::FEMALE) ||
                               (ch->gender == game::Gender::NONBINARY && ch->useFemaleModel);
                }
            }
            const std::string fullPath = pipeline::resolveItemRegionTexture(
                *assetManager, region, texName, isFemale);
            if (fullPath.empty()) continue;
            regionLayers.emplace_back(region, fullPath);
        }
    }

    // Re-composite: base skin + underwear + equipment regions
    // Clear composite cache first to prevent stale textures from being reused
    charRenderer->clearCompositeCache();
    // Use per-instance texture override (not model-level) to avoid deleting cached composites.
    uint32_t instanceId = renderer->getCharacterInstanceId();
    auto* newTex = charRenderer->compositeWithRegions(bodySkinPath, underwearPaths, regionLayers);
    if (newTex != nullptr && instanceId != 0) {
        charRenderer->setTextureSlotOverride(instanceId, static_cast<uint16_t>(skinSlot), newTex);
    }

    // Cloak cape texture - separate from skin atlas, uses texture slot type-2 (Object Skin)
    uint32_t cloakSlot = app.getCloakTextureSlotIndex();
    if (cloakSlot > 0 && instanceId != 0) {
        // Find equipped cloak (inventoryType 16)
        uint32_t cloakDisplayId = 0;
        for (int s = 0; s < game::Inventory::NUM_EQUIP_SLOTS; s++) {
            const auto& slot = inventory.getEquipSlot(static_cast<game::EquipSlot>(s));
            if (!slot.empty() && slot.item.inventoryType == 16 && slot.item.displayInfoId != 0) {
                cloakDisplayId = slot.item.displayInfoId;
                break;
            }
        }

        if (cloakDisplayId > 0) {
            int32_t recIdx = displayInfoDbc->findRecordById(cloakDisplayId);
            if (recIdx >= 0) {
                // DBC field 3 = modelTexture_1 (cape texture name)
                const auto* dispL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
                std::string capeName = displayInfoDbc->getString(static_cast<uint32_t>(recIdx), dispL ? (*dispL)["LeftModelTexture"] : 3);
                if (!capeName.empty()) {
                    // This asked for one path only - ObjectComponents, no
                    // suffix - and a cape whose art is filed anywhere else
                    // showed white. The full list, in order, is in
                    // pipeline/item_textures.hpp, and the other three places
                    // that load a cape have always used all of it.
                    bool isFemale = false;
                    if (auto* gh = app.getGameHandler()) {
                        if (auto* ch = gh->getActiveCharacter()) {
                            isFemale = (ch->gender == game::Gender::FEMALE) ||
                                       (ch->gender == game::Gender::NONBINARY && ch->useFemaleModel);
                        }
                    }
                    const rendering::VkTexture* whiteTex = charRenderer->loadTexture("");
                    for (const auto& capePath : pipeline::capeTextureCandidates(capeName, isFemale)) {
                        auto* capeTex = charRenderer->loadTexture(capePath);
                        if (capeTex == nullptr || capeTex == whiteTex) continue;
                        charRenderer->setTextureSlotOverride(instanceId, static_cast<uint16_t>(cloakSlot), capeTex);
                        LOG_INFO("Cloak texture applied: ", capePath);
                        break;
                    }
                }
            }
        } else {
            // No cloak equipped - clear override so model's default (white) shows
            charRenderer->clearTextureSlotOverride(instanceId, static_cast<uint16_t>(cloakSlot));
        }
    }
}

// ============================================================
// World Map
// ============================================================

// Everything the map shows besides the land: the zone the player is in, what
// they have explored, their party, flight points, quests, their corpse and the
// rares nearby. For the in-game map and the second-window one alike, which is
// why it is on its own - the second window's map is fed every frame it is open,
// whatever the in-game one is doing.
void GameScreen::feedWorldMap(game::GameHandler& gameHandler,
                              rendering::world_map::WorldMapFacade& targetMap,
                              const std::function<bool(uint32_t)>& questAreaShown) {
    auto* renderer = core::Application::getInstance().getRenderer();
    if (!renderer) return;
    auto* wm = &targetMap;

    // Keep map name in sync with minimap's map name
    auto* minimap = renderer->getMinimap();
    if (minimap) {
        wm->setMapName(minimap->getMapName());
    }
    wm->setServerExplorationMask(
        gameHandler.getPlayerExploredZoneMasks(),
        gameHandler.hasPlayerExploredZoneMasks());
    // Which zone the player is actually in, rather than which WorldMapArea box
    // they happen to sit deepest inside. The boxes are axis-aligned rectangles
    // around irregular zones and overlap their neighbours heavily, so opening
    // the map could land on a zone the player was only near.
    wm->setPlayerZoneId(renderer->getCurrentZoneId());

    // Party member dots on world map
    {
        std::vector<rendering::WorldMapPartyDot> dots;
        if (gameHandler.isInGroup()) {
            const auto& partyData = gameHandler.getPartyData();
            for (const auto& member : partyData.members) {
                if (!member.isOnline || !member.hasPartyStats) continue;
                if (member.posX == 0 && member.posY == 0) continue;
                // posY → canonical X (north), posX → canonical Y (west)
                float wowX = static_cast<float>(member.posY);
                float wowY = static_cast<float>(member.posX);
                glm::vec3 rpos = core::coords::canonicalToRender(glm::vec3(wowX, wowY, 0.0f));
                auto ent = gameHandler.getEntityManager().getEntity(member.guid);
                uint8_t cid = entityClassId(ent.get());
                ImU32 col = (cid != 0)
                    ? classColorU32(cid, 230)
                    : (member.guid == partyData.leaderGuid
                       ? IM_COL32(255, 210, 0, 230)
                       : IM_COL32(100, 180, 255, 230));
                dots.push_back({ .renderPos = rpos, .color = col, .name = member.name });
            }
        }
        // Battleground team positions, which this client had only ever drawn
        // on the minimap.
        //
        // FrameXML draws them on its own world map - WorldMapRaid1..40, placed
        // from GetNumBattlefieldPositions and GetBattlefieldPosition - but that
        // is exactly the area this client's map surface covers, so nothing
        // FrameXML puts there can be seen. Handing the map over made the
        // interface responsible for a layer it cannot show, so the surface
        // that hides it has to draw them instead.
        //
        // The same two group colours the minimap uses, so a flag carrier is
        // the same colour on both.
        {
            for (const auto& bp : gameHandler.getBgPlayerPositions()) {
                // Packet coords are canonical: wowX north, wowY west.
                const glm::vec3 rpos =
                    core::coords::canonicalToRender(glm::vec3(bp.wowX, bp.wowY, 0.0f));
                dots.push_back({ .renderPos = rpos, .color = ui::bgGroupColor(bp.group),
                                 .name = gameHandler.lookupName(bp.guid) });
            }
        }
        wm->setPartyDots(std::move(dots));
    }

    // Taxi node markers on world map
    {
        std::vector<rendering::WorldMapTaxiNode> taxiNodes;
        const auto& nodes = gameHandler.getTaxiNodes();
        uint32_t currentTaxiNode = gameHandler.getTaxiCurrentNode();
        const bool playerAlliance = gameHandler.isPlayerAlliance();
        taxiNodes.reserve(nodes.size());
        for (const auto& [id, node] : nodes) {
            const bool known = gameHandler.isKnownTaxiNode(id);
            // Undiscovered nodes are shown so the player can see where flight
            // paths exist, but only ones their faction can actually use. A node's
            // faction is inferred from which taxi mount TaxiNodes.dbc lists:
            // own-faction mount → show; both mounts → neutral flight point, show;
            // opposite-faction-only OR no mount at all (boat/zeppelin/script
            // nodes) → hide. Known nodes are always shown.
            if (!known) {
                const bool hasAlliance = node.mountDisplayIdAlliance != 0;
                const bool hasHorde    = node.mountDisplayIdHorde != 0;
                const bool bothFactions = hasAlliance && hasHorde;   // neutral hub
                const bool ownFaction   = playerAlliance ? hasAlliance : hasHorde;
                if (!bothFactions && !ownFaction) continue;
            }
            rendering::WorldMapTaxiNode wtn;
            wtn.id    = node.id;
            wtn.mapId = node.mapId;
            // TaxiNodes.dbc stores server/wire-order coordinates - convert to
            // canonical (X=north, Y=west) like the taxi flight path code does,
            // or the markers land transposed on the map.
            glm::vec3 canonical = core::coords::serverToCanonical(
                glm::vec3(node.x, node.y, node.z));
            wtn.wowX  = canonical.x;
            wtn.wowY  = canonical.y;
            wtn.wowZ  = canonical.z;
            wtn.name  = node.name;
            wtn.known = known;
            wtn.costCopper = gameHandler.getTaxiCostTo(id);
            wtn.current    = (id == currentTaxiNode);
            wtn.reachable  = gameHandler.hasTaxiRouteTo(id);
            taxiNodes.push_back(std::move(wtn));
        }
        wm->setTaxiNodes(std::move(taxiNodes));
    }

    // Quest objective and quest-giver markers on the world map.
    {
        std::vector<rendering::WorldMap::QuestPoi> qpois;
        assert(services_.expansionRegistry && services_.expansionRegistry->getActive());
        const bool hasQuestMapPOIs = services_.expansionRegistry->getActive()->majorVersion >= 3;
        const auto& questStatuses = gameHandler.getNpcQuestStatuses();
        // Vanilla/TBC FrameXML draws landmarks and group positions, not quest
        // endpoints. Nearby NPC dialog status belongs on the minimap/world;
        // promoting it to world-map pins invents a quest tracking feature.
        // Addons remain responsible for their own quest-map textures.
        if (hasQuestMapPOIs) {
            // Add authoritative NPC statuses first. Some servers also emit a
            // generic POI at the NPC's position; that duplicate is filtered below
            // so it cannot leave a teal objective circle on a quest giver.
            for (const auto& [guid, status] : questStatuses) {
                auto entity = gameHandler.getEntityManager().getEntity(guid);
                if (!entity || entity->getType() != game::ObjectType::UNIT) continue;

                rendering::WorldMap::QuestPoi qp;
                qp.wowX = entity->getX();
                qp.wowY = entity->getY();
                qp.name = std::static_pointer_cast<game::Unit>(entity)->getName();
                const auto marker = game::questGiverMarker(status);
                if (!marker.symbol) continue;
                if (marker.symbol[0] == '!') {
                    qp.kind = marker.dim ? rendering::WorldMap::QuestPoi::Kind::AVAILABLE_LOW
                                         : rendering::WorldMap::QuestPoi::Kind::AVAILABLE;
                } else {
                    qp.kind = marker.dim ? rendering::WorldMap::QuestPoi::Kind::INCOMPLETE
                                         : rendering::WorldMap::QuestPoi::Kind::REWARD;
                }
                qpois.push_back(std::move(qp));
            }
        }

        constexpr float kQuestGiverPoiMergeDistance = 15.0f;
        constexpr float kQuestGiverPoiMergeDistanceSq =
            kQuestGiverPoiMergeDistance * kQuestGiverPoiMergeDistance;
        // Which quests the player is actually on. A quest POI outlives the
        // quest that asked for it - the points are kept until the next answer
        // for that quest, and abandoning one never comes with an answer - so
        // without this an abandoned quest's objectives stay on the map.
        std::unordered_set<uint32_t> questsInLog;
        for (const auto& quest : gameHandler.getQuestLog()) {
            if (quest.questId != 0) questsInLog.insert(quest.questId);
        }
        size_t objectivePois = 0;
        for (const auto& poi : gameHandler.getGossipPois()) {
            if (!hasQuestMapPOIs && poi.questObjectiveIndex != -2) continue;
            // Keep ordinary gossip navigation POIs, and quest points for a
            // quest in the log.
            //
            // This asked isQuestShownOnMap, a per-quest opt-in set by a
            // checkbox in the client's own quest tracker. That tracker was
            // handed to FrameXML and the checkbox went with it, leaving three
            // readers of a set nothing could add to: no objective has been
            // drawn on either map since.
            if (poi.questObjectiveIndex != -2 && !questsInLog.count(poi.data)) {
                continue;
            }
            if (poi.questObjectiveIndex >= 0) ++objectivePois;
            bool duplicatesQuestGiver = false;
            for (const auto& existing : qpois) {
                if (existing.kind == rendering::WorldMap::QuestPoi::Kind::OBJECTIVE) continue;
                const float dx = existing.wowX - poi.x;
                const float dy = existing.wowY - poi.y;
                if (dx * dx + dy * dy <= kQuestGiverPoiMergeDistanceSq) {
                    duplicatesQuestGiver = true;
                    break;
                }
            }
            if (duplicatesQuestGiver) continue;

            rendering::WorldMap::QuestPoi qp;
            qp.wowX = poi.x;
            qp.wowY = poi.y;
            qp.name = poi.name;
            // The shaded area, for the quest the map has selected. Every
            // objective of that quest gets its own, which is what the real
            // client shades: DrawQuestBlob names a quest, not an objective.
            if (poi.questObjectiveIndex >= 0 && !poi.area.empty() &&
                questAreaShown(poi.data)) {
                qp.area.reserve(poi.area.size());
                for (const auto& pt : poi.area) qp.area.emplace_back(pt.first, pt.second);
            }
            if (poi.questObjectiveIndex == -1) {
                // A quest POI with no objective index is the quest endpoint,
                // not an objective area. Completed quests use a yellow ?,
                // while in-progress endpoints use a gray ?.
                qp.kind = rendering::WorldMap::QuestPoi::Kind::INCOMPLETE;
                for (const auto& quest : gameHandler.getQuestLog()) {
                    if (quest.questId != poi.data) continue;
                    if (quest.complete) {
                        qp.kind = rendering::WorldMap::QuestPoi::Kind::REWARD;
                    }
                    break;
                }
            }
            qpois.push_back(std::move(qp));
        }
        // Said once, when the map has quests to show objectives for and no
        // points to show: the server answers CMSG_QUEST_POI_QUERY from its own
        // quest_poi table, and an empty one looks exactly like a client that
        // is not drawing them.
        static bool reportedNoObjectivePois = false;
        if (hasQuestMapPOIs && objectivePois == 0 && !questsInLog.empty() && !reportedNoObjectivePois) {
            reportedNoObjectivePois = true;
            LOG_WARNING("World map: ", questsInLog.size(), " quest(s) in the log and no "
                        "objective points for any of them - the server sent no quest POI "
                        "data, so only quest givers and endpoints can be drawn");
        }
        wm->setQuestPois(std::move(qpois));
    }

    // Corpse marker: show skull X on world map when ghost with unclaimed corpse
    {
        float corpseCanX = 0.0f, corpseCanY = 0.0f;
        bool ghostWithCorpse = gameHandler.isPlayerGhost() &&
                               gameHandler.getCorpseCanonicalPos(corpseCanX, corpseCanY);
        glm::vec3 corpseRender = ghostWithCorpse
            ? core::coords::canonicalToRender(glm::vec3(corpseCanX, corpseCanY, 0.0f))
            : glm::vec3{};
        wm->setCorpsePos(ghostWithCorpse, corpseRender);

        // And where releasing would put them. Shown while dead either way:
        // before releasing it is the choice being offered, and after it is the
        // place to walk back from.
        uint32_t healerMap = 0;
        glm::vec3 healerCanonical(0.0f);
        const bool haveHealer = gameHandler.isPlayerDead() &&
                                gameHandler.getDeathReleaseLocation(healerMap, healerCanonical) &&
                                healerMap == gameHandler.getCurrentMapId();
        wm->setGraveyardPos(haveHealer,
                            haveHealer ? core::coords::canonicalToRender(healerCanonical)
                                       : glm::vec3{});
    }

    // Rare tracker: mark every spawned rare / rare-elite the client currently has loaded.
    // Entities only exist while near the player, so a marker means that rare is out now.
    // Opt-in via the Interface setting; when off, feed an empty list so markers clear.
    {
        std::vector<rendering::WorldMapRareMark> rares;
        if (settingsPanel_.showRareTracker_)
        for (const auto& [guid, entity] : gameHandler.getEntityManager().getEntities()) {
            if (!entity || entity->getType() != game::ObjectType::UNIT) continue;
            auto unit = std::static_pointer_cast<game::Unit>(entity);
            const int rank = gameHandler.getCreatureRank(unit->getEntry());
            if (rank != 2 && rank != 4) continue;      // 2 = Rare Elite, 4 = Rare
            if (unit->getHealth() == 0) continue;       // skip dead/looted rares
            rendering::WorldMapRareMark m;
            m.renderPos = core::coords::canonicalToRender(
                glm::vec3(unit->getX(), unit->getY(), unit->getZ()));
            m.name = unit->getName();
            m.rank = rank;
            rares.push_back(std::move(m));
        }
        wm->setRares(std::move(rares));
    }

}

void GameScreen::renderWorldMap(game::GameHandler& gameHandler) {
    auto& app = core::Application::getInstance();
    auto* renderer = app.getRenderer();
    if (!renderer) return;

    // The map on the second window, under its own ImGui context: a setter can
    // free a texture, and that goes back through the context that made it.
    if (auto* mapWindow = app.getMapWindow(); mapWindow && mapWindow->map()) {
        mapWindow->withContext([&] {
            feedWorldMap(gameHandler, *mapWindow->map(), [mapWindow](uint32_t questId) {
                return questId != 0 && questId == mapWindow->selectedQuest();
            });
        });
    }

    auto* wm = renderer->getWorldMap();
    if (!wm) return;

    // Flight master window drives the world map's flight-map (taxi selection)
    // mode: opening SMSG_SHOWTAXINODES opens the map, activating a flight or
    // closing the gossip closes it. A user-dismissed map (Escape / X) closes
    // the flight master window through the onClose handler.
    // Not while FrameXML is drawing the flight map itself. The legacy taxi
    // list a few lines up already stands aside for that element; this mode did
    // not, so talking to a flight master put both on screen at once - TaxiFrame
    // over this client's own map, each with its own set of pins.
    const bool taxiWanted = gameHandler.isTaxiWindowOpen() &&
                            !frameXmlOwns(UiElement::Taxi);
    if (taxiWanted && !wm->isTaxiMapOpen()) {
        auto* gh = &gameHandler;
        wm->openTaxiMap(
            [gh](uint32_t dest) { return gh->getTaxiRouteTo(dest); },
            [gh](uint32_t dest) { gh->activateTaxi(dest); },
            [gh]() { gh->closeTaxi(); });
    } else if (!taxiWanted && wm->isTaxiMapOpen()) {
        wm->closeTaxiMap();
    }

    // Who says the map is wanted depends on who owns it. FrameXML's world map
    // is a frame it shows and hides, and application.cpp gives this one that
    // frame's rect while it is visible - so a rect being set is the same
    // statement as showWorldMap_ is for this client's own window.
    const bool frameXmlDrivesMap = frameXmlOwns(UiElement::WorldMap);
    const bool wanted = frameXmlDrivesMap
        ? (wm->hasFrameRect() || wm->isTaxiMapOpen())
        : (showWorldMap_ || wm->isTaxiMapOpen());
    if (!wanted) return;

    feedWorldMap(gameHandler, *wm, [&gameHandler](uint32_t questId) {
        return gameHandler.isQuestBlobShown(questId);
    });

    glm::vec3 playerPos = renderer->getCharacterPosition();
    float playerYaw = renderer->getCharacterYaw();
    auto* window = app.getWindow();
    int screenW = window ? window->getWidth() : 1280;
    int screenH = window ? window->getHeight() : 720;
    wm->render(playerPos, screenW, screenH, playerYaw);

    // Sync showWorldMap_ if the map closed itself (e.g. ESC key inside the overlay).
    // Only where that flag is what opened it: under FrameXML the frame's own
    // visibility is the state, and clearing this would say nothing.
    if (!frameXmlDrivesMap && !wm->isOpen()) showWorldMap_ = false;
}

// ============================================================
// Action Bar
// ============================================================

VkDescriptorSet GameScreen::getSpellIcon(uint32_t spellId, pipeline::AssetManager* am) {
    if (spellId == 0 || !am) return VK_NULL_HANDLE;

    // Check cache first
    auto cit = spellIconCache_.find(spellId);
    if (cit != spellIconCache_.end()) return cit->second;

    // Lazy-load SpellIcon.dbc and Spell.dbc icon IDs
    if (!spellIconDbLoaded_) {
        spellIconDbLoaded_ = true;

        // Load SpellIcon.dbc: field 0 = ID, field 1 = icon path
        pipeline::loadSpellIconPaths(am, spellIconPaths_);

        // Load Spell.dbc: SpellIconID field
        auto spellDbc = am->loadDBC("Spell.dbc");
        const auto* spellL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("Spell") : nullptr;
        if (spellDbc && spellDbc->isLoaded()) {
            uint32_t fieldCount = spellDbc->getFieldCount();
            // Helper to load icons for a given field layout
            auto tryLoadIcons = [&](uint32_t idField, uint32_t iconField) {
                spellIconIds_.clear();
                if (iconField >= fieldCount) return;
                for (uint32_t i = 0; i < spellDbc->getRecordCount(); i++) {
                    uint32_t id = spellDbc->getUInt32(i, idField);
                    uint32_t iconId = spellDbc->getUInt32(i, iconField);
                    if (id > 0 && iconId > 0) {
                        spellIconIds_[id] = iconId;
                    }
                }
            };

            // Use the active expansion layout when its fields are present in
            // the loaded DBC. TBC/WotLK/Classic place IconID in different
            // columns, so reading the WotLK default for every client leaves
            // action bars and spell UI without icons.
            uint32_t iconField = 133; // WotLK default
            uint32_t idField = 0;
            if (spellL) {
                try {
                    uint32_t layoutId = (*spellL)["ID"];
                    uint32_t layoutIcon = (*spellL)["IconID"];
                    if (layoutId < fieldCount && layoutIcon < fieldCount) {
                        iconField = layoutIcon;
                        idField = layoutId;
                    }
                } catch (...) {}
            }
            tryLoadIcons(idField, iconField);
        }
    }

    // Rate-limit GPU uploads per frame to prevent stalls when many icons are uncached
    // (e.g., first login, after loading screen, or many new auras appearing at once).
    if (!claimUiTextureUpload()) return VK_NULL_HANDLE;  // defer - do NOT cache null here

    // Look up spellId -> SpellIconID -> icon path
    auto iit = spellIconIds_.find(spellId);
    if (iit == spellIconIds_.end()) {
        spellIconCache_[spellId] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }

    auto pit = spellIconPaths_.find(iit->second);
    if (pit == spellIconPaths_.end()) {
        spellIconCache_[spellId] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }

    // Path from DBC has no extension - append .blp
    std::string iconPath = pit->second + ".blp";
    // Cached either way, failures included: the HUD asks for this every frame
    // an aura is up, so a missing icon must not be retried each time.
    VkDescriptorSet ds =
        uploadUiTextureFromBlp(am, iconPath, services_.window);
    spellIconCache_[spellId] = ds;
    return ds;
}

// ============================================================
// Cooldown Tracker - floating panel showing all active spell CDs
// ============================================================

// ============================================================
// Quest Objective Tracker (right-side HUD)
// ============================================================

// ============================================================
// Nameplates - world-space health bars projected to screen
// ============================================================

GameScreen::UnitLabelVisibility GameScreen::unitLabelVisibility(
        game::GameHandler& gameHandler, uint64_t guid,
        const glm::vec3& unitPosition, const glm::vec3& nameAnchor,
        const rendering::Camera& camera) const {
    const auto entity = gameHandler.getEntityManager().getEntity(guid);
    if (!entity || !entity->isUnit()) return {};
    const auto& unit = static_cast<const game::Unit&>(*entity);
    if (!unit.getMaxHealth() || (unit.getUnitFlags() & game::UNIT_FLAG_NOT_SELECTABLE)) return {};
    const bool isPlayer = entity->getType() == game::ObjectType::PLAYER;
    const bool own = guid == gameHandler.getPlayerGuid();
    const bool target = guid == gameHandler.getTargetGuid();
    const bool hostile = unit.isHostile();
    const uint32_t creatureType = isPlayer ? 0 : gameHandler.getCreatureType(unit.getEntry());
    const bool totem = creatureType == 11;
    const bool companion = creatureType == 12;
    const bool summon = !isPlayer && !totem && game::unitSummonedByGuid(*entity) != 0;
    const bool pet = summon && (unit.getUnitFlags() & game::UNIT_FLAG_PLAYER_CONTROLLED);
    const bool guardian = summon && !pet;
    const bool enemyPlate = hostile || (!isPlayer && gameHandler.unitReactionToPlayer(unit) <= 4);
    bool plate = enemyPlate ? settingsPanel_.showEnemyNameplates_ : settingsPanel_.showFriendlyNameplates_;
    if (totem) plate = plate && addons::storedCVarValue(
        hostile ? "nameplateShowEnemyTotems" : "nameplateShowFriendlyTotems", hostile ? "1" : "0") != "0";
    if (pet || guardian) {
        const char* key = pet ? (hostile ? "nameplateShowEnemyPets" : "nameplateShowFriendlyPets")
                             : (hostile ? "nameplateShowEnemyGuardians" : "nameplateShowFriendlyGuardians");
        plate = plate && addons::storedCVarValue(key, "1") != "0";
    }
    if (own || companion || (game::isPreWotlk() && !unit.getHealth())) plate = false;
    if (!game::isPreWotlk() && !unit.getHealth() && !target) return {};
    const glm::mat4 viewProj = camera.getProjectionMatrix() * camera.getViewMatrix();
    if (plate && game::isPreWotlk()) {
        const auto player = gameHandler.getEntityManager().getEntity(gameHandler.getPlayerGuid());
        if (!player) return {};
        const glm::vec3 playerPosition = core::coords::canonicalToRender(
            glm::vec3(player->getX(), player->getY(), player->getZ()));
        const glm::vec3 delta = unitPosition - playerPosition;
        const glm::vec4 clip = viewProj * glm::vec4(nameAnchor + glm::vec3(0, 0, 2.0f / 3.0f), 1);
        plate = glm::dot(delta, delta) <= 400.0f &&
            ui::legacyPlateAnchorVisible(clip.x, clip.y, clip.z, clip.w);
    }
    const char* nameCVar = isPlayer ? (hostile ? "unitNameEnemyPlayerName" : "unitNameFriendlyPlayerName")
        : companion ? "unitNameCompanionName"
        : totem ? (hostile ? "unitNameEnemyTotemName" : "unitNameFriendlyTotemName")
        : pet ? (hostile ? "unitNameEnemyPetName" : "unitNameFriendlyPetName")
        : guardian ? (hostile ? "unitNameEnemyGuardianName" : "unitNameFriendlyGuardianName")
        : creatureType == 8 ? "unitNameNonCombatCreatureName" : "unitNameNPC";
    // Hover is not a reason to show a name (Daelyshia, UnitNameNPC=0:
    // 20261117T102218Z.flight_marker_name_offset/h2-hover-original.png).
    const bool name = ui::unitWorldNameShown(own, plate, target,
        addons::storedCVarValue(nameCVar, "1") != "0");
    if (own && addons::storedCVarValue("unitNameOwn", "0") == "0") return {};
    return {plate, name};
}

void GameScreen::renderNameplates(game::GameHandler& gameHandler) {
    if (gameHandler.getState() != game::WorldState::IN_WORLD) return;

    // World picking and visible nameplates share one mouseover identity.
    uint64_t mouseoverGuid = worldHoverGuid_;
    plateHits_.clear();

    auto* appRenderer = services_.renderer;
    if (!appRenderer) return;
    rendering::Camera* camera = appRenderer->getCamera();
    if (!camera) return;

    auto* window = services_.window;
    if (!window) return;
    const float screenW = static_cast<float>(window->getWidth());
    const float screenH = static_cast<float>(window->getHeight());

    const glm::mat4 viewProj = camera->getProjectionMatrix() * camera->getViewMatrix();
    const glm::vec3 camPos   = camera->getPosition();
    const uint64_t  playerGuid = gameHandler.getPlayerGuid();
    const uint64_t  targetGuid = gameHandler.getTargetGuid();
    const bool legacyPlates = game::isPreWotlk();
    // Build 8606 WorldFrame plate geometry, queried at its 768-unit UI height.
    const float plateArtScale = screenH / 768.0f * (156.651184f / 128.0f)
                             * settingsPanel_.nameplateScale_;
    static VkDescriptorSet plateBorder = VK_NULL_HANDLE;
    static VkDescriptorSet plateFill = VK_NULL_HANDLE;
    static VkDescriptorSet plateGlow = VK_NULL_HANDLE;
    if (legacyPlates && (settingsPanel_.showFriendlyNameplates_ || settingsPanel_.showEnemyNameplates_)) {
        if (!plateBorder) plateBorder = uploadUiTextureFromBlp(services_.assetManager,
            "Interface/Tooltips/Nameplate-Border.blp", services_.window);
        if (!plateFill) plateFill = uploadUiTextureFromBlp(services_.assetManager,
            "Interface/TargetingFrame/UI-TargetingFrame-BarFill.blp", services_.window);
        if (!plateGlow) plateGlow = uploadUiTextureFromBlp(services_.assetManager,
            "Interface/Tooltips/Nameplate-Glow.blp", services_.window);
        if (!plateBorder || !plateFill || !plateGlow) throw std::runtime_error("Missing original nameplate textures");
    }

    refreshQuestObjectiveCache(gameHandler);
    const auto& questKillEntries = minimapQuestCreatureEntries_;

    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    static thread_local std::vector<std::shared_ptr<game::Entity>> nameplateEntities;
    glm::vec3 playerCanonical(0.0f);
    if (auto player = gameHandler.getEntityManager().getEntity(playerGuid))
        playerCanonical = glm::vec3(player->getX(), player->getY(), player->getZ());
    gameHandler.getEntityManager().getEntitiesNear(
        playerCanonical.x, playerCanonical.y, 150.0f, nameplateEntities);

    // Allow Nameplate Overlap. Off in the real client, and the boxes already
    // placed this frame are what "off" needs to know: a plate that would land
    // on one of them is lifted until it clears, so two units at the same
    // distance do not print one bar through another.
    //
    // Nearest first, so the plate that gets moved is the one further away. The
    // list is sorted by distance below for that reason and no other.
    const bool allowPlateOverlap =
        addons::storedCVarValue("nameplateAllowOverlap", "0") != "0";
    static thread_local std::vector<ui::PlateBox> placedPlates;
    placedPlates.clear();
    if (!allowPlateOverlap) {
        std::sort(nameplateEntities.begin(), nameplateEntities.end(),
                  [&](const std::shared_ptr<game::Entity>& a,
                      const std::shared_ptr<game::Entity>& b) {
                      if (!a || !b) return a != nullptr;
                      const glm::vec3 da(a->getX() - playerCanonical.x,
                                         a->getY() - playerCanonical.y,
                                         a->getZ() - playerCanonical.z);
                      const glm::vec3 db(b->getX() - playerCanonical.x,
                                         b->getY() - playerCanonical.y,
                                         b->getZ() - playerCanonical.z);
                      return glm::dot(da, da) < glm::dot(db, db);
                  });
    }

    for (const auto& entityPtr : nameplateEntities) {
        if (!entityPtr) continue;
        const uint64_t guid = entityPtr->getGuid();
        const bool hideOwnName = guid == playerGuid && addons::storedCVarValue("unitNameOwn", "0") == "0";

        if (!entityPtr->isUnit()) continue;
        auto* unit = static_cast<game::Unit*>(entityPtr.get());
        if (unit->getMaxHealth() == 0) continue;
        // A trigger carries health and a name like any unit, so without this
        // the invisible ones drew a bar over empty ground.
        if (unit->getUnitFlags() & game::UNIT_FLAG_NOT_SELECTABLE) continue;

        bool isPlayer = (entityPtr->getType() == game::ObjectType::PLAYER);
        bool isTarget = (guid == targetGuid);
        const bool isHostile = unit->isHostile();

        const uint32_t creatureType = isPlayer ? 0 : gameHandler.getCreatureType(unit->getEntry());

        // Legacy corpses retain plain names, governed by the usual name
        // settings and target/hover overrides, but never a health plate.
        const bool isCorpse = (unit->getHealth() == 0);
        if (isCorpse && !legacyPlates && !isTarget) continue;

        // Prefer the renderer's actual instance position so the nameplate tracks the
        // rendered model exactly (avoids drift from the parallel entity interpolator).
        glm::vec3 renderPos;
        if (!core::Application::getInstance().getRenderPositionForGuid(guid, renderPos)) {
            renderPos = core::coords::canonicalToRender(
                glm::vec3(unit->getX(), unit->getY(), unit->getZ()));
        }
        const glm::vec3 unitPosition = renderPos;
        auto& app = core::Application::getInstance();
        auto* spawner = app.getEntitySpawner();
        auto* characters = appRenderer->getCharacterRenderer();
        if (!spawner || !characters) continue;
        const auto instanceId = guid == playerGuid ? appRenderer->getCharacterInstanceId()
                                                   : spawner->characterInstanceIdForGuid(guid);
        if (!characters->getInstanceNamePosition(instanceId, unit->getMountDisplayId() != 0, renderPos))
            continue; // The model has not spawned yet.

        const auto labelVisibility = unitLabelVisibility(gameHandler, guid, unitPosition, renderPos, *camera);
        const bool showPlate = labelVisibility.plate;

        // Build 8606 draws the raid atlas on a one-yard camera-facing quad,
        // bottom anchored to the name attachment when no nameplate is visible.
        // Render before name filtering: own raid marker survives hidden names.
        const uint8_t raidMark = gameHandler.getEntityRaidMark(guid);
        static VkDescriptorSet texture = VK_NULL_HANDLE;
        if (raidMark < game::GameHandler::kRaidMarkCount) {
            if (!texture) texture = uploadUiTextureFromBlp(services_.assetManager,
                "Interface/TargetingFrame/UI-RaidTargetingIcons.blp", services_.window);
            if (!texture) throw std::runtime_error("Missing raid target atlas");
            const glm::vec4 anchor = viewProj * glm::vec4(renderPos, 1.0f);
            if (!showPlate && anchor.w > 0.01f && anchor.z >= 0.0f && anchor.z <= anchor.w) {
                const glm::vec2 bottom((anchor.x / anchor.w + 1.0f) * screenW * 0.5f,
                                       (anchor.y / anchor.w + 1.0f) * screenH * 0.5f);
                const float size = std::abs(camera->getProjectionMatrix()[1][1]) *
                                   screenH * 0.5f / anchor.w;
                drawList->AddImage((ImTextureID)(uintptr_t)texture,
                    ImVec2(bottom.x - size * 0.5f, bottom.y - size),
                    ImVec2(bottom.x + size * 0.5f, bottom.y),
                    ImVec2((raidMark % 4) * 0.25f, (raidMark / 4) * 0.25f),
                    ImVec2((raidMark % 4 + 1) * 0.25f, (raidMark / 4 + 1) * 0.25f));
            }
        }
        if (hideOwnName) continue;

        // Modern name visibility uses camera distance. Legacy plate range
        // was checked independently against the player above.
        glm::vec3 nameDelta = renderPos - camPos;
        float distSq = glm::dot(nameDelta, nameDelta);
        float cullDist = isTarget ? 60.0f : ((!showPlate || isPlayer) ? 40.0f : 20.0f);
        // Legacy plain names follow visible world units. The health-plate
        // range must not impose an additional camera-distance limit on them.
        if (!legacyPlates && distSq > cullDist * cullDist) continue;
        // CGUnit_C::GetNamePlatePosition in build 8606 adds 2/3 yard to
        // the authored name attachment. The frame's TOP is placed here.
        if (showPlate && legacyPlates) renderPos.z += 2.0f / 3.0f;
        // World names occupy a raised text line above the model's name attachment.
        // Dimensions calibrated against build 8606 at matching camera poses.
        else if (!showPlate) renderPos.z += 0.25f;


        // World names use the scene depth per text pixel below. Rejecting the
        // entire label at its anchor hides visible text when a beam crosses
        // only the anchor. Screen-space nameplates still need a sight query.
        static SightCache sight;
        if (showPlate && sight.blocked(
                services_.renderer ? services_.renderer->getWMORenderer() : nullptr,
                guid, camPos, renderPos)) {
            continue;
        }

        // Project to clip space
        glm::vec4 clipPos = viewProj * glm::vec4(renderPos, 1.0f);
        if (clipPos.w <= 0.01f) continue;  // Behind camera

        glm::vec3 ndc = glm::vec3(clipPos) / clipPos.w;
        if (ndc.z < 0.0f || ndc.z > 1.0f) continue;
        if (ndc.x < -1.2f || ndc.x > 1.2f || ndc.y < -1.2f || ndc.y > 1.2f) continue;

        // NDC → screen pixels.
        // The camera bakes the Vulkan Y-flip into the projection matrix, so
        // NDC y = -1 is the top of the screen and y = 1 is the bottom.
        // Map directly: sy = (ndc.y + 1) / 2 * screenH  (no extra inversion).
        float sx = (ndc.x * 0.5f + 0.5f) * screenW;
        float sy = (ndc.y * 0.5f + 0.5f) * screenH;
        if (showPlate && legacyPlates) {
            sx = ui::legacyPlateCenterX(sx, screenW, plateArtScale);
            sy = ui::legacyPlateBarTop(sy + 19.0f * plateArtScale, plateArtScale);
        }

        // Fade out in the last 5 units of cull range
        float fadeSq = (cullDist - 5.0f) * (cullDist - 5.0f);
        float dist = std::sqrt(distSq);
        float alpha = distSq < fadeSq ? 1.0f : 1.0f - (dist - (cullDist - 5.0f)) / 5.0f;
        if (legacyPlates) alpha = 1.0f;
        auto A = [&](int v) { return static_cast<int>(v * alpha); };

        // Bar colour by hostility (grey for corpses)
        ImU32 barColor, bgColor;
        if (isCorpse) {
            // Minimal grey bar for selected corpses (loot/skin targets)
            barColor = IM_COL32(140, 140, 140, A(200));
            bgColor  = IM_COL32(70,  70,  70,  A(160));
        } else if (isHostile) {
            // Check if mob is tapped by another player (grey nameplate)
            const bool tappedByOther = game::unitTappedByOther(unit->getDynamicFlags(), game::isPreWotlk());
            if (tappedByOther) {
                barColor = IM_COL32(160, 160, 160, A(200));
                bgColor  = IM_COL32(80,  80,  80,  A(160));
            } else {
                barColor = IM_COL32(220, 60,  60,  A(200));
                bgColor  = IM_COL32(100, 25,  25,  A(160));
            }
        } else if (isPlayer) {
            // World-space bars communicate reaction, and a red death knight or
            // a pink paladin reads as a hostility that is not there - so green
            // stays the default. The interface offers the other choice though,
            // and offering it and ignoring it is the worse of the two, so the
            // setting is honoured when it is asked for.
            const uint8_t classId = entityClassId(entityPtr.get());
            if (classId != 0 &&
                addons::storedCVarValue("showClassColorInNameplate", "0") != "0") {
                barColor = classColorU32(classId, A(200));
                bgColor  = classColorU32(classId, A(90));
            } else {
                barColor = IM_COL32(60,  200, 80,  A(200));
                bgColor  = IM_COL32(25,  100, 35,  A(160));
            }
        } else {
            barColor = IM_COL32(60,  200, 80,  A(200));
            bgColor  = IM_COL32(25,  100, 35,  A(160));
        }
        // Check if this unit is targeting the local player (threat indicator)
        bool isTargetingPlayer = false;
        if (unit->isHostile() && !isCorpse) {
            const uint64_t unitTarget = game::unitTargetGuid(*entityPtr);
            if (unitTarget != 0) {
                isTargetingPlayer = (unitTarget == playerGuid);
            }
        }
        // Creature rank for border styling (Elite=gold double border, Boss=red, Rare=silver)
        int creatureRank = -1;
        if (!isPlayer) creatureRank = gameHandler.getCreatureRank(unit->getEntry());

        // Border: gold = currently selected, orange = targeting player, dark = default
        ImU32 borderColor = isTarget
            ? IM_COL32(255, 215, 0,  A(255))
            : isTargetingPlayer
              ? IM_COL32(255, 140, 0,  A(220))   // orange = this mob is targeting you
              : IM_COL32(20,  20,  20, A(180));

        // Bar geometry
        const float barW = legacyPlates ? 103.0f * plateArtScale : 80.0f * settingsPanel_.nameplateScale_;
        const float barH = legacyPlates ? 9.0f * plateArtScale : 8.0f * settingsPanel_.nameplateScale_;
        const float barX = legacyPlates ? sx - 59.0f * plateArtScale : sx - barW * 0.5f;

        // ...and the lift itself, before anything is drawn with sy. The block a
        // plate occupies is its bar plus the name line above it; the bound is
        // generous rather than exact, because a plate that clears by a pixel
        // still reads as two bars touching.
        if (showPlate && !allowPlateOverlap) {
            const float topExtent = barH + 24.0f;
            sy = ui::plateTopClearOf(placedPlates, barX, barX + barW, sy, barH, topExtent);
            if (legacyPlates) sy = ui::legacyPlateBarTop(sy, plateArtScale);
            placedPlates.push_back({.x0 = barX, .y0 = sy - topExtent, .x1 = barX + barW, .y1 = sy + barH});
        }

        // Guard against division by zero when maxHealth hasn't been populated yet
        // (freshly spawned entity with default fields). 0/0 produces NaN which
        // poisons all downstream geometry; +inf is clamped but still wasteful.
        float healthPct = (unit->getMaxHealth() > 0)
            ? std::clamp(static_cast<float>(unit->getHealth()) / static_cast<float>(unit->getMaxHealth()), 0.0f, 1.0f)
            : 0.0f;

        const int plateVertexStart = drawList->VtxBuffer.Size;
        if (showPlate) {
        if (legacyPlates) {
            const int reaction = gameHandler.unitReactionToPlayer(*unit);
            ImU32 fill = isPlayer && !isHostile ? IM_COL32(0, 0, 255, 255)
                : reaction <= 2 ? IM_COL32(255, 0, 0, 255)
                : reaction <= 4 ? IM_COL32(255, 255, 0, 255) : IM_COL32(0, 255, 0, 255);
            if (game::unitTappedByOther(unit->getDynamicFlags(), true)) fill = IM_COL32(128, 128, 128, 255);
            // Original Nameplate-Border leaves its interior transparent. Empty
            // health shows the world through that opening, including ghosts.
            drawList->AddImage((ImTextureID)(uintptr_t)plateFill,
                ImVec2(barX, sy), ImVec2(barX + barW * healthPct, sy + barH),
                ImVec2(0, 0), ImVec2(healthPct, 1), fill);
            const float artX = sx - 64.0f * plateArtScale;
            const float artY = sy - 19.0f * plateArtScale;
            if (raidMark < game::GameHandler::kRaidMarkCount) {
                // Native nameplate: atlas texture RIGHT anchored to frame LEFT.
                const float size = 20.48f * plateArtScale;
                const float centerY = artY + 16.0f * plateArtScale;
                drawList->AddImage((ImTextureID)(uintptr_t)texture,
                    ImVec2(artX - size, centerY - size * 0.5f),
                    ImVec2(artX, centerY + size * 0.5f),
                    ImVec2((raidMark % 4) * 0.25f, (raidMark / 4) * 0.25f),
                    ImVec2((raidMark % 4 + 1) * 0.25f, (raidMark / 4 + 1) * 0.25f));
            }
            drawList->AddImage((ImTextureID)(uintptr_t)plateBorder,
                ImVec2(artX, artY), ImVec2(artX + 128.0f * plateArtScale, artY + 32.0f * plateArtScale));
            const uint32_t level = unit->getLevel();
            ImU32 levelColor;
            switch (game::creatureLevelDifficulty(gameHandler.getPlayerLevel(), level)) {
                case game::LevelDifficulty::Red: levelColor = IM_COL32(255, 25, 25, 255); break;
                case game::LevelDifficulty::Orange: levelColor = IM_COL32(255, 127, 63, 255); break;
                case game::LevelDifficulty::Yellow: levelColor = IM_COL32(255, 255, 0, 255); break;
                case game::LevelDifficulty::Green: levelColor = IM_COL32(63, 191, 63, 255); break;
                case game::LevelDifficulty::Gray: levelColor = IM_COL32(127, 127, 127, 255); break;
            }
            const std::string levelText = std::to_string(level);
            const float fontSize = 11.52f * plateArtScale;
            const ImVec2 size = ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, 0, levelText.c_str());
            const ImVec2 position(artX + 116.0f * plateArtScale - size.x * .5f, sy + (barH - size.y) * .5f);
            drawList->AddText(ImGui::GetFont(), fontSize, ImVec2(position.x + 1, position.y + 1), IM_COL32(0,0,0,255), levelText.c_str());
            drawList->AddText(ImGui::GetFont(), fontSize, position, levelColor, levelText.c_str());
        } else {
        drawList->AddRectFilled(ImVec2(barX,                 sy), ImVec2(barX + barW,               sy + barH), bgColor,    2.0f);
        // For corpses, don't fill health bar (just show grey background)
        if (!isCorpse) {
            drawList->AddRectFilled(ImVec2(barX,                 sy), ImVec2(barX + barW * healthPct,   sy + barH), barColor,   2.0f);
        }
        drawList->AddRect       (ImVec2(barX - 1.0f, sy - 1.0f), ImVec2(barX + barW + 1.0f, sy + barH + 1.0f), borderColor, 2.0f);

        // Elite/Boss/Rare decoration: extra outer border with rank-specific color
        if (creatureRank == 1 || creatureRank == 2) {
            // Elite / Rare Elite: gold double border
            drawList->AddRect(ImVec2(barX - 3.0f, sy - 3.0f),
                              ImVec2(barX + barW + 3.0f, sy + barH + 3.0f),
                              IM_COL32(255, 200, 50, A(200)), 3.0f);
        } else if (creatureRank == 3) {
            // Boss: red double border
            drawList->AddRect(ImVec2(barX - 3.0f, sy - 3.0f),
                              ImVec2(barX + barW + 3.0f, sy + barH + 3.0f),
                              IM_COL32(255, 40, 40, A(200)), 3.0f);
        } else if (creatureRank == 4) {
            // Rare: silver double border
            drawList->AddRect(ImVec2(barX - 3.0f, sy - 3.0f),
                              ImVec2(barX + barW + 3.0f, sy + barH + 3.0f),
                              IM_COL32(170, 200, 230, A(200)), 3.0f);
        }

        // HP % text centered on health bar (non-corpse, non-full-health for readability)
        if (!isCorpse && unit->getMaxHealth() > 0) {
            int hpPct = static_cast<int>(healthPct * 100.0f + 0.5f);
            char hpBuf[8];
            snprintf(hpBuf, sizeof(hpBuf), "%d%%", hpPct);
            ImVec2 hpTextSz = ImGui::CalcTextSize(hpBuf);
            float hpTx = sx - hpTextSz.x * 0.5f;
            float hpTy = sy + (barH - hpTextSz.y) * 0.5f;
            drawList->AddText(ImVec2(hpTx + 1.0f, hpTy + 1.0f), IM_COL32(0, 0, 0, A(140)), hpBuf);
            drawList->AddText(ImVec2(hpTx,         hpTy),         IM_COL32(255, 255, 255, A(200)), hpBuf);
        }

        } // Non-legacy plate artwork.

        // Cast bar below health bar when unit is casting
        float castBarBaseY = sy + barH + 2.0f;
        float nameplateBottom = castBarBaseY;  // tracks lowest drawn element for debuff dots
        {
            // Enemy Cast Bars on Nameplates. The control names enemies, so a
            // friendly caster's bar is not what it governs, and the current
            // target keeps its bar for the same reason it keeps its plate.
            //
            // Build 8606 nameplates carry no cast bar at all (border, health
            // bar, level and name only; showVKeyCastBar is a 3.x setting).
            const bool castBarAllowed = !legacyPlates && (
                isTarget || !isHostile ||
                addons::storedCVarValue("showVKeyCastBar", "1") != "0");
            const auto* cs = castBarAllowed ? gameHandler.getUnitCastState(guid) : nullptr;
            if (cs && cs->casting && cs->timeTotal > 0.0f) {
                float castPct = std::clamp((cs->timeTotal - cs->timeRemaining) / cs->timeTotal, 0.0f, 1.0f);
                const float cbH = 6.0f * settingsPanel_.nameplateScale_;

                // Spell icon + name above the cast bar
                const std::string& spellName = gameHandler.getSpellName(cs->spellId);
                {
                    auto* castAm = services_.assetManager;
                    VkDescriptorSet castIcon = (cs->spellId && castAm)
                        ? getSpellIcon(cs->spellId, castAm) : VK_NULL_HANDLE;
                    float iconSz = cbH + 8.0f;
                    if (castIcon) {
                        // Draw icon to the left of the cast bar
                        float iconX = barX - iconSz - 2.0f;
                        float iconY = castBarBaseY;
                        drawList->AddImage((ImTextureID)(uintptr_t)castIcon,
                                           ImVec2(iconX, iconY),
                                           ImVec2(iconX + iconSz, iconY + iconSz));
                        drawList->AddRect(ImVec2(iconX - 1.0f, iconY - 1.0f),
                                          ImVec2(iconX + iconSz + 1.0f, iconY + iconSz + 1.0f),
                                          IM_COL32(0, 0, 0, A(180)), 1.0f);
                    }
                    if (!spellName.empty()) {
                        ImVec2 snSz = ImGui::CalcTextSize(spellName.c_str());
                        float snX = sx - snSz.x * 0.5f;
                        float snY = castBarBaseY;
                        drawList->AddText(ImVec2(snX + 1.0f, snY + 1.0f), IM_COL32(0, 0, 0, A(140)), spellName.c_str());
                        drawList->AddText(ImVec2(snX,         snY),         IM_COL32(255, 210, 100, A(220)), spellName.c_str());
                        castBarBaseY += snSz.y + 2.0f;
                    }
                }

                // Cast bar: green = interruptible, red = uninterruptible; both pulse when >80% complete
                ImU32 cbBg = IM_COL32(30, 25, 40, A(180));
                ImU32 cbFill;
                if (castPct > 0.8f && unit->isHostile()) {
                    float pulse = 0.7f + 0.3f * std::sin(static_cast<float>(ImGui::GetTime()) * 8.0f);
                    cbFill = cs->interruptible
                        ? IM_COL32(static_cast<int>(40  * pulse), static_cast<int>(220 * pulse), static_cast<int>(40  * pulse), A(220))  // green pulse
                        : IM_COL32(static_cast<int>(255 * pulse), static_cast<int>(30  * pulse), static_cast<int>(30  * pulse), A(220)); // red pulse
                } else {
                    cbFill = cs->interruptible
                        ? IM_COL32(50,  190, 50,  A(200))   // green = interruptible
                        : IM_COL32(190, 40,  40,  A(200));  // red = uninterruptible
                }
                drawList->AddRectFilled(ImVec2(barX,                   castBarBaseY),
                                        ImVec2(barX + barW,             castBarBaseY + cbH), cbBg,    2.0f);
                drawList->AddRectFilled(ImVec2(barX,                   castBarBaseY),
                                        ImVec2(barX + barW * castPct,   castBarBaseY + cbH), cbFill,  2.0f);
                drawList->AddRect      (ImVec2(barX - 1.0f, castBarBaseY - 1.0f),
                                        ImVec2(barX + barW + 1.0f, castBarBaseY + cbH + 1.0f),
                                        IM_COL32(20, 10, 40, A(200)), 2.0f);

                // Time remaining text
                char timeBuf[12];
                snprintf(timeBuf, sizeof(timeBuf), "%.1fs", cs->timeRemaining);
                ImVec2 timeSz = ImGui::CalcTextSize(timeBuf);
                float timeX = sx - timeSz.x * 0.5f;
                float timeY = castBarBaseY + (cbH - timeSz.y) * 0.5f;
                drawList->AddText(ImVec2(timeX + 1.0f, timeY + 1.0f), IM_COL32(0, 0, 0, A(140)), timeBuf);
                drawList->AddText(ImVec2(timeX,         timeY),         IM_COL32(220, 200, 255, A(220)), timeBuf);
                nameplateBottom = castBarBaseY + cbH + 2.0f;
            }
        }

        // Debuff dot indicators: small colored squares below the nameplate showing
        // player-applied auras on the current hostile target.
        // Colors: Magic=blue, Curse=purple, Disease=yellow, Poison=green, Other=grey
        if (!legacyPlates && isTarget && unit->isHostile() && !isCorpse) {
            const auto& auras = gameHandler.getTargetAuras();
            const uint64_t pguid = gameHandler.getPlayerGuid();
            const float dotSize = 6.0f * settingsPanel_.nameplateScale_;
            const float dotGap  = 2.0f;
            float dotX = barX;
            for (const auto& aura : auras) {
                if (aura.isEmpty() || aura.casterGuid != pguid) continue;
                uint8_t dispelType = gameHandler.getSpellDispelType(aura.spellId);
                ImU32 dotCol;
                switch (dispelType) {
                    case 1:  dotCol = IM_COL32( 64, 128, 255, A(210)); break; // Magic   - blue
                    case 2:  dotCol = IM_COL32(160,  32, 240, A(210)); break; // Curse   - purple
                    case 3:  dotCol = IM_COL32(180, 140,  40, A(210)); break; // Disease - yellow-brown
                    case 4:  dotCol = IM_COL32( 50, 200,  50, A(210)); break; // Poison  - green
                    default: dotCol = IM_COL32(170, 170, 170, A(170)); break; // Other   - grey
                }
                drawList->AddRectFilled(ImVec2(dotX,          nameplateBottom),
                                        ImVec2(dotX + dotSize, nameplateBottom + dotSize), dotCol, 1.0f);
                drawList->AddRect      (ImVec2(dotX - 1.0f,          nameplateBottom - 1.0f),
                                        ImVec2(dotX + dotSize + 1.0f, nameplateBottom + dotSize + 1.0f),
                                        IM_COL32(0, 0, 0, A(150)), 1.0f);

                // Duration clock-sweep overlay (like target frame auras)
                uint64_t nowMs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                int32_t remainMs = aura.getRemainingMs(nowMs);
                if (aura.maxDurationMs > 0 && remainMs > 0) {
                    float pct = 1.0f - static_cast<float>(remainMs) / static_cast<float>(aura.maxDurationMs);
                    pct = std::clamp(pct, 0.0f, 1.0f);
                    float cx = dotX + dotSize * 0.5f;
                    float cy = nameplateBottom + dotSize * 0.5f;
                    float r  = dotSize * 0.5f;
                    float startAngle = -IM_PI * 0.5f;
                    float endAngle   = startAngle + pct * IM_PI * 2.0f;
                    ImVec2 center(cx, cy);
                    const int segments = 12;
                    for (int seg = 0; seg < segments; seg++) {
                        float a0 = startAngle + (endAngle - startAngle) * seg / segments;
                        float a1 = startAngle + (endAngle - startAngle) * (seg + 1) / segments;
                        drawList->AddTriangleFilled(
                            center,
                            ImVec2(cx + r * std::cos(a0), cy + r * std::sin(a0)),
                            ImVec2(cx + r * std::cos(a1), cy + r * std::sin(a1)),
                            IM_COL32(0, 0, 0, A(100)));
                    }
                }

                // Stack count on dot (upper-left corner)
                if (aura.charges > 1) {
                    char stackBuf[8];
                    snprintf(stackBuf, sizeof(stackBuf), "%d", aura.charges);
                    drawList->AddText(ImVec2(dotX + 1.0f, nameplateBottom), IM_COL32(0, 0, 0, A(200)), stackBuf);
                    drawList->AddText(ImVec2(dotX,         nameplateBottom - 1.0f), IM_COL32(255, 255, 255, A(240)), stackBuf);
                }

                // Duration text below dot
                if (remainMs > 0) {
                    char durBuf[8];
                    if (remainMs >= 60000)
                        snprintf(durBuf, sizeof(durBuf), "%dm", remainMs / 60000);
                    else
                        snprintf(durBuf, sizeof(durBuf), "%d", remainMs / 1000);
                    ImVec2 durSz = ImGui::CalcTextSize(durBuf);
                    float durX = dotX + (dotSize - durSz.x) * 0.5f;
                    float durY = nameplateBottom + dotSize + 1.0f;
                    drawList->AddText(ImVec2(durX + 1.0f, durY + 1.0f), IM_COL32(0, 0, 0, A(180)), durBuf);
                    // Color: red if < 5s, yellow if < 15s, white otherwise
                    ImU32 durCol = remainMs < 5000 ? IM_COL32(255, 60, 60, A(240))
                                 : remainMs < 15000 ? IM_COL32(255, 200, 60, A(240))
                                 : IM_COL32(230, 230, 230, A(220));
                    drawList->AddText(ImVec2(durX, durY), durCol, durBuf);
                }

                // Spell name + duration tooltip on hover
                {
                    ImVec2 mouse = ImGui::GetMousePos();
                    if (mouse.x >= dotX && mouse.x < dotX + dotSize &&
                        mouse.y >= nameplateBottom && mouse.y < nameplateBottom + dotSize) {
                        const std::string& dotSpellName = gameHandler.getSpellName(aura.spellId);
                        if (!dotSpellName.empty()) {
                            if (remainMs > 0) {
                                int secs = remainMs / 1000;
                                int mins = secs / 60;
                                secs %= 60;
                                char tipBuf[128];
                                if (mins > 0)
                                    snprintf(tipBuf, sizeof(tipBuf), "%s (%dm %ds)", dotSpellName.c_str(), mins, secs);
                                else
                                    snprintf(tipBuf, sizeof(tipBuf), "%s (%ds)", dotSpellName.c_str(), secs);
                                ImGui::SetTooltip("%s", tipBuf);
                            } else {
                                ImGui::SetTooltip("%s", dotSpellName.c_str());
                            }
                        }
                    }
                }

                dotX += dotSize + dotGap;
                if (dotX + dotSize > barX + barW) break;
            }
        }

        }

        // Name + level label above health bar
        //
        // Whether this kind of unit gets its name shown at all. The interface
        // offers one of these per category and read none of them, so every
        // unit was labelled alike. The health bar is a different question -
        // nameplateShowFriends and nameplateShowEnemies decide that, above -
        // so turning a name off here leaves the plate and takes the text,
        // which is what these settings mean. The current target keeps its
        // name whatever is set, as it keeps its plate.
        const bool showThisName = labelVisibility.name;

        uint32_t level = unit->getLevel();
        const std::string& unitName = unit->getName();

        // Player Titles. The chosen title travels with the player like any
        // other public field, so a nameplate can decorate the name the same
        // way the target frame does - with that player's name in the row's
        // own hole, not the reader's. A title this client has no row for
        // formats to nothing and the plain name is kept.
        std::string titledName;
        if (isPlayer && !unitName.empty() &&
            addons::storedCVarValue("unitNamePlayerPvPTitle", "1") != "0") {
            const auto& fields = entityPtr->getFields();
            auto tit = fields.find(game::fieldIndex(game::UF::PLAYER_CHOSEN_TITLE));
            if (tit != fields.end() && tit->second != 0) {
                titledName = gameHandler.getFormattedTitleFor(tit->second, unitName);
            }
        }
        const std::string& shownName = titledName.empty() ? unitName : titledName;

        char labelBuf[96];
        if (isPlayer) {
            // Player nameplates: show name only (no level clutter).
            // Fall back to level as placeholder while the name query is pending.
            if (!shownName.empty())
                snprintf(labelBuf, sizeof(labelBuf), "%s", shownName.c_str());
            else {
                // Name query may be pending; request it now to ensure it gets resolved
                gameHandler.queryPlayerName(unit->getGuid());
                if (level > 0)
                    snprintf(labelBuf, sizeof(labelBuf), "Player (%u)", level);
                else
                    snprintf(labelBuf, sizeof(labelBuf), "Player");
            }
        } else if (showPlate && !legacyPlates && level > 0) {
            uint32_t playerLevel = gameHandler.getPlayerLevel();
            // Show skull for units more than 10 levels above the player
            if (playerLevel > 0 && level > playerLevel + 10)
                snprintf(labelBuf, sizeof(labelBuf), "?? %s", unitName.c_str());
            else
                snprintf(labelBuf, sizeof(labelBuf), "%u %s", level, unitName.c_str());
        } else {
            snprintf(labelBuf, sizeof(labelBuf), "%s", unitName.c_str());
        }
        // Plain world names scale with perspective; nameplates remain screen UI.
        const glm::vec4 textTop = viewProj * glm::vec4(renderPos + glm::vec3(0, 0, 0.20f), 1.0f);
        const float nameFontSize = showPlate ? (legacyPlates ? 12.8f * plateArtScale : ImGui::GetFontSize())
            : std::clamp(std::abs(textTop.y / textTop.w - ndc.y) * screenH * 0.5f, 1.0f, 40.0f);
        ImFont* nameFont = ImGui::GetFont();
        ImVec2 textSize = nameFont->CalcTextSizeA(nameFontSize, FLT_MAX, 0.0f, labelBuf);
        float nameX = sx - textSize.x * 0.5f;
        float nameY = sy - (showPlate ? barH + 12.0f : textSize.y);
        if (showPlate && legacyPlates) nameY = sy - nameFontSize - 4.5f * plateArtScale;
        const ImVec2 pointer = ImGui::GetIO().MousePos;
        const bool plateHovered = showPlate && !ImGui::GetIO().WantCaptureMouse && !frameXmlOwnsMouse()
            && pointer.x >= std::min(nameX, barX) - 2.0f
            && pointer.x <= std::max(nameX + textSize.x, barX + barW) + 2.0f
            && pointer.y >= nameY - 1.0f && pointer.y <= sy + barH + 2.0f;
        // Name color communicates reaction too: friendly players blue, enemies red.
        // Every player-controlled unit takes the player colours: the original
        // draws a player's Imp, Stoneskin Totem, Skeletal Warrior and Boar
        // Spirit in the same blue as a friendly player
        // (_experiments/20261117T165200Z.pair4_kits/original-summons.png),
        // as it does companions.
        ImU32 nameColor;
        if (isCorpse && legacyPlates) {
            nameColor = IM_COL32(128, 128, 128, A(255));
        } else if (isPlayer || (unit->getUnitFlags() & game::UNIT_FLAG_PLAYER_CONTROLLED) != 0) {
            nameColor = isHostile
                ? IM_COL32(220, 80, 80, A(255))
                : IM_COL32(128, 128, 255, A(255));
        } else {
            const int reaction = gameHandler.unitReactionToPlayer(*unit);
            if (isHostile || reaction <= 2) nameColor = IM_COL32(255, 0, 0, A(255));
            else if (reaction == 3) nameColor = IM_COL32(255, 128, 0, A(255));
            else if (reaction == 4) nameColor = IM_COL32(255, 255, 0, A(255));
            else nameColor = IM_COL32(0, 255, 0, A(255));
        }
        if (showPlate && legacyPlates) {
            nameColor = (guid == worldHoverGuid_ || plateHovered) ? IM_COL32(255, 255, 0, 255) : IM_COL32(255, 255, 255, 255);
        }
        // Sub-label below the name: guild tag for players, subtitle for NPCs
        std::string subLabel;
        if (isPlayer) {
            // Guild Names, which this client already had and never asked about.
            // The NPC subtitle below is a different thing wearing the same
            // angle brackets, and is not what this control names.
            uint32_t guildId =
                addons::storedCVarValue("unitNamePlayerGuild", "1") != "0"
                    ? gameHandler.getEntityGuildId(guid) : 0;
            if (guildId != 0) {
                const std::string& gn = gameHandler.lookupGuildName(guildId);
                if (!gn.empty()) subLabel = "<" + gn + ">";
            }
        } else if (const auto title = game::summonTitle(creatureType, game::unitCreatedByGuid(*unit),
                                                            game::unitSummonedByGuid(*unit));
                   title != game::SummonTitle::None) {
            // "<Owner's Companion>" / "<Owner's Minion>" from GlobalStrings.
            const uint64_t creator = game::unitCreatedByGuid(*unit);
            std::string owner = gameHandler.lookupName(creator);
            if (owner.empty())
                if (const auto c = gameHandler.getEntityManager().getEntity(creator))
                    owner = game::entityDisplayName(c);
            auto* lua = services_.addonManager ? services_.addonManager->getLuaEngine() : nullptr;
            const std::string pattern = lua ? lua->globalString(game::summonTitleGlobal(title)) : std::string{};
            if (!owner.empty() && !pattern.empty())
                subLabel = "<" + game::formatSummonTitle(pattern, owner) + ">";
        } else {
            // NPC subtitle (e.g. "<Reagent Vendor>", "<Innkeeper>")
            std::string sub = gameHandler.getCachedCreatureSubName(unit->getEntry());
            if (!sub.empty()) subLabel = "<" + sub + ">";
        }
        if (showPlate && legacyPlates) subLabel.clear();
        if (!subLabel.empty()) nameY -= nameFontSize;  // shift name up for sub-label line

        if (showPlate && legacyPlates && (guid == worldHoverGuid_ || plateHovered)) {
            drawList->AddCallback([](const ImDrawList*, const ImDrawCmd* command) {
                auto* state = static_cast<ImGui_ImplVulkan_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
                assert(state);
                auto* context = static_cast<rendering::VkContext*>(command->UserCallbackData);
                vkCmdBindPipeline(state->CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  context->getImGuiAdditivePipeline(state->PipelineLayout));
            }, window->getVkContext());
            drawList->AddImage((ImTextureID)(uintptr_t)plateGlow,
                ImVec2(sx - 64.0f * plateArtScale, sy - 19.0f * plateArtScale),
                ImVec2(sx + 64.0f * plateArtScale, sy + 13.0f * plateArtScale));
            drawList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
        }

        if (showThisName && !showPlate) {
            struct DepthState { rendering::VkContext* context; float depth; };
            DepthState state{window->getVkContext(), ndc.z};
            state.context->requestWorldLabelDepth();
            drawList->AddCallback([](const ImDrawList*, const ImDrawCmd* command) {
                const auto* depth = static_cast<const DepthState*>(command->UserCallbackData);
                auto* render = static_cast<ImGui_ImplVulkan_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
                assert(render);
                depth->context->bindWorldLabelDepth(render->CommandBuffer, depth->depth);
            }, &state, sizeof(state));
        }
        if (showThisName) {
            drawList->AddText(nameFont, nameFontSize, ImVec2(nameX + 1.0f, nameY + 1.0f), IM_COL32(0, 0, 0, A(160)), labelBuf);
            drawList->AddText(nameFont, nameFontSize, ImVec2(nameX,         nameY),         nameColor, labelBuf);
        }

        // Sub-label below the name (WoW-style <Guild Name> or <NPC Title> in
        // lighter color). Hidden with the name it belongs to: a guild tag
        // floating alone over a nameless plate is not what anyone asked for.
        if (!subLabel.empty() && showThisName) {
            ImVec2 subSz = nameFont->CalcTextSizeA(nameFontSize, FLT_MAX, 0.0f, subLabel.c_str());
            float subX = sx - subSz.x * 0.5f;
            float subY = nameY + textSize.y + 1.0f;
            drawList->AddText(nameFont, nameFontSize, ImVec2(subX + 1.0f, subY + 1.0f), IM_COL32(0, 0, 0, A(120)), subLabel.c_str());
            drawList->AddText(nameFont, nameFontSize, ImVec2(subX,         subY),         (isPlayer ? IM_COL32(180, 180, 180, A(200)) : nameColor), subLabel.c_str());
        }

        {
            // Quest kill objective indicator: small yellow sword icon to the right of the name
            float questIconX = nameX + textSize.x + 4.0f;
            if (!game::isPreWotlk() && !isPlayer && questKillEntries.count(unit->getEntry())) {
                const char* objSym = "\xe2\x9a\x94";  // ⚔ crossed swords (UTF-8)
                drawList->AddText(ImVec2(questIconX + 1.0f, nameY + 1.0f), IM_COL32(0, 0, 0, A(160)), objSym);
                drawList->AddText(ImVec2(questIconX,         nameY),         IM_COL32(255, 220, 0, A(230)), objSym);
                questIconX += ImGui::CalcTextSize("\xe2\x9a\x94").x + 2.0f;
            }

            // TBC uses the world-space quest marker, without a second glyph beside the name.
            if (!game::isPreWotlk() && !isPlayer) {
                const auto mark = game::questGiverMarker(
                    gameHandler.getQuestGiverStatus(guid));
                const char* qSym = mark.symbol;
                const ImU32 qCol = mark.dim ? IM_COL32(160, 160, 160, A(220))
                                            : IM_COL32(255, 210, 0, A(255));
                if (qSym) {
                    drawList->AddText(ImVec2(questIconX + 1.0f, nameY + 1.0f), IM_COL32(0, 0, 0, A(160)), qSym);
                    drawList->AddText(ImVec2(questIconX,         nameY),         qCol, qSym);
                }
            }
        }

        if (showThisName && !showPlate) drawList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);

        // TBC dims every non-target plate while a target is selected. Apply
        // frame opacity to border, fill, name, level and hover glow together.
        if (showPlate && legacyPlates && targetGuid != 0 && !isTarget) {
            for (int vertex = plateVertexStart; vertex < drawList->VtxBuffer.Size; ++vertex) {
                auto& color = drawList->VtxBuffer[vertex].col;
                const ImU32 alphaByte = ((color >> IM_COL32_A_SHIFT) & 0xffu) / 2u;
                color = (color & ~IM_COL32_A_MASK) | (alphaByte << IM_COL32_A_SHIFT);
            }
        }

        // The plate's click box: the wider of name text or health bar, so short
        // names like "Wolf" don't produce a strip narrower than the bar. Clicks
        // are routed by processTargetInput through plateUnitAt, the same way as
        // a click on the unit itself: left targets, right targets and
        // interacts or attacks. Recorded for the next frame's input.
        if (showPlate) {
            const ui::PlateBox hitBox{.x0 = std::min(nameX, barX) - 2.0f, .y0 = nameY - 1.0f,
                                      .x1 = std::max(nameX + textSize.x, barX + barW) + 2.0f,
                                      .y1 = sy + barH + 2.0f};
            plateHits_.push_back({.guid = guid, .box = hitBox});
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            if (!ImGui::GetIO().WantCaptureMouse && !frameXmlOwnsMouse() &&
                mouse.x >= hitBox.x0 && mouse.x <= hitBox.x1 &&
                mouse.y >= hitBox.y0 && mouse.y <= hitBox.y1) {
                // Track mouseover for [target=mouseover] macro conditionals
                mouseoverGuid = guid;
            }
        }
    }

    if (frameXmlOwnsMouse() || ImGui::GetIO().WantCaptureMouse) mouseoverGuid = 0;
    gameHandler.setMouseoverGuid(mouseoverGuid);
    const uint64_t objectTooltipGuid = frameXmlOwnsMouse() || ImGui::GetIO().WantCaptureMouse
        || mouseoverGuid ? 0 : worldObjectHoverGuid_;
    const uint64_t tooltipGuid = mouseoverGuid ? mouseoverGuid : objectTooltipGuid;
    if (services_.addonManager && worldTooltipGuid_ != tooltipGuid) {
        auto* lua = services_.addonManager->getLuaEngine();
        std::string script;
        if (objectTooltipGuid) {
            const auto entity = gameHandler.getEntityManager().getEntity(objectTooltipGuid);
            assert(entity);
            std::string objectLabel = game::entityDisplayName(entity);
            const auto object = std::static_pointer_cast<game::GameObject>(entity);
            const auto* info = gameHandler.getCachedGameObjectInfo(object->getEntry());
            if (info && info->type == 23) {
                const std::string area = gameHandler.getWhoAreaName(info->data[2]);
                if (!area.empty()) objectLabel += "\n" + area;
            }
            // Encode server text as Lua data, including quotes and control bytes.
            std::string literal = "'";
            for (const unsigned char c : objectLabel) {
                literal += '\\';
                literal += static_cast<char>('0' + c / 100);
                literal += static_cast<char>('0' + c / 10 % 10);
                literal += static_cast<char>('0' + c % 10);
            }
            literal += "'";
            script = worldObjectTooltipFloating_
                ? "GameTooltip:SetOwner(WorldFrame, 'ANCHOR_CURSOR'); "
                : "GameTooltip_SetDefaultAnchor(GameTooltip, WorldFrame); ";
            script += "GameTooltip:SetText(" + literal + "); GameTooltip:Show()";
        } else {
            script = mouseoverGuid
                ? "GameTooltip_SetDefaultAnchor(GameTooltip, WorldFrame); GameTooltip:SetUnit('mouseover')"
                : "if GameTooltip:GetOwner() == WorldFrame then GameTooltip:Hide() end";
        }
        if (!lua->executeString(script)) {
            LOG_ERROR("World tooltip update failed: ", lua->lastError());
        } else {
            worldTooltipGuid_ = tooltipGuid;
        }
    }
    if (auto* characters = appRenderer->getCharacterRenderer()) {
        auto* spawner = core::Application::getInstance().getEntitySpawner();
        characters->setHoveredInstance(spawner && mouseoverGuid
            ? spawner->characterInstanceIdForGuid(mouseoverGuid) : 0);
    }
}

// ============================================================
// Durability Warning (equipment damage indicator)
// ============================================================

// The settings panel keeps brightness as 0-100 with 50 neutral, and the post
// process pipeline wants that over 50 - so the number the video options call
// gamma is exactly what the pipeline is already given, and this converts
// between the two rather than introducing a third scale.
float GameScreen::getGamma() const {
    return static_cast<float>(settingsPanel_.pendingBrightness) / 50.0f;
}

void GameScreen::setGamma(float gamma) {
    // WoW's own slider runs 0.3 to 2.8; clamped to what the 0-100 setting can
    // hold so a value from outside cannot push the slider off its own track.
    const float clamped = std::clamp(gamma, 0.0f, 2.0f);
    const int stored = static_cast<int>(std::lround(clamped * 50.0f));
    // Saved here, because nothing else was going to.
    //
    // Every other route into these settings goes through the settings window,
    // which writes the file when it is done. The video options' Gamma slider
    // reaches this directly from Lua instead, so the value applied, looked
    // right for the rest of the session, and was gone the next time the client
    // started - the file had never been written.
    //
    // Only when the stored number actually moves. The slider reports every
    // frame it is dragged, and the setting is a whole number out of a hundred,
    // so this is a handful of writes across a drag rather than one per frame.
    const bool changed = (stored != settingsPanel_.pendingBrightness);
    settingsPanel_.pendingBrightness = stored;
    if (auto* renderer = services_.renderer) {
        renderer->getPostProcessPipeline()->setBrightness(clamped);
    }
    if (changed) saveSettings();
}

namespace {

/// ~/.wowee/<folder>/WoWee_YYYYMMDD_HHMMSS.<extension>, the name a screenshot
/// or a recording is saved under.
std::string capturePath(const char* folder, const char* extension) {
    const char* home = std::getenv("HOME");
    if (!home) home = std::getenv("USERPROFILE");
    if (!home) home = "/tmp";
    std::string dir = std::string(home) + "/.wowee/" + folder;

    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    tm = core::localTime(tt);

    char filename[128];
    std::snprintf(filename, sizeof(filename),
                  "WoWee_%04d%02d%02d_%02d%02d%02d.%s",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, extension);
    return dir + "/" + filename;
}

}  // namespace

void GameScreen::startRecording() {
    auto* renderer = services_.renderer;
    if (!renderer || !services_.gameHandler || renderer->isRecording()) return;
    const std::string path = capturePath("recordings", "mp4");
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::string error;
    if (renderer->startRecording(path, error)) {
        recordingPath_ = path;
        services_.gameHandler->addSystemChatMessage(
            "Recording to " + path + ". Type /record again to stop.");
    } else {
        services_.gameHandler->addSystemChatMessage("Could not start recording: " + error + ".");
    }
}

void GameScreen::stopRecording() {
    auto* renderer = services_.renderer;
    if (!renderer || !services_.gameHandler || !renderer->isRecording()) return;
    const auto stats = renderer->stopRecording();
    const int seconds = static_cast<int>(stats.seconds + 0.5);
    char length[32];
    std::snprintf(length, sizeof(length), "%d:%02d", seconds / 60, seconds % 60);
    std::string message = "Recording saved: " + recordingPath_ + " (" + length;
    if (!stats.hasAudio) message += ", no sound";
    if (stats.framesDropped > 0) {
        message += ", " + std::to_string(stats.framesDropped) + " frames dropped";
    }
    services_.gameHandler->addSystemChatMessage(message + ").");
}

void GameScreen::toggleRecording() {
    auto* renderer = services_.renderer;
    if (!renderer) return;
    if (renderer->isRecording()) {
        stopRecording();
    } else {
        startRecording();
    }
}

void GameScreen::reportRecordingFailure() {
    auto* renderer = services_.renderer;
    if (!renderer || !services_.gameHandler) return;
    const std::string failure = renderer->takeRecordingFailure();
    if (failure.empty()) return;
    services_.gameHandler->addSystemChatMessage(
        "Recording stopped: " + failure + ". What was recorded is saved in " + recordingPath_ + ".");
}

void GameScreen::takeScreenshot() {
    auto* renderer = services_.renderer;
    if (!renderer) return;

    const std::string path = capturePath("screenshots", "png");

    if (renderer->captureScreenshot(path)) {
        game::MessageChatData sysMsg;
        sysMsg.type = game::ChatType::SYSTEM;
        sysMsg.language = game::ChatLanguage::UNIVERSAL;
        sysMsg.message = "Screenshot saved: " + path;
        services_.gameHandler->addLocalChatMessage(sysMsg);
    }
}

}} // namespace wowee::ui
