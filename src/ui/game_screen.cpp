#include "ui/graphics_choices.hpp"
#include "game/game_utils.hpp"
#include "game/game_object_lock.hpp"
#include "game/game_object_interaction.hpp"
#include "game/game_object_cursor.hpp"
#include "ui/game_screen.hpp"
#include "ui/nameplate_stacking.hpp"
#include "ui/gamepad_controls.hpp"
#include "core/gamepad.hpp"

#include <set>
#include "core/click_drag.hpp"
#include "addons/lua_api_registrations.hpp"
#include "ui/escape_action.hpp"
#include "ui/display_modes.hpp"
#include "ui/framexml_takeover.hpp"
#include "ui/scene_pick.hpp"
#include "ui/world_cursor.hpp"
#include "ui/corpse_harvesting.hpp"
#include "ui/ui_colors.hpp"
#include "ui/ui_helpers.hpp"
#include "ui/ui_texture_load.hpp"
#include "rendering/vk_context.hpp"
#include "core/application.hpp"
#include "core/appearance_composer.hpp"
#include "addons/addon_manager.hpp"
#include "ui/link_hit.hpp"
#include "core/coordinates.hpp"
#include "core/input.hpp"
#include "rendering/renderer.hpp"
#include "rendering/post_process_pipeline.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/lens_flare.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/minimap.hpp"
#include "rendering/world_map.hpp"
#include "rendering/character_renderer.hpp"
#include "core/entity_spawner.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/music_manager.hpp"
#include "game/zone_manager.hpp"
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

#include "game/expansion_profile.hpp"
#include "game/character.hpp"
#include "game/shapeshift_forms.hpp"
#include "game/pet_action.hpp"
#include "core/logger.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <sstream>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cctype>
#include <chrono>
#include <ctime>

#include <unordered_set>

namespace {
    using namespace wowee::ui::colors;
    using namespace wowee::ui::helpers;

    // Upper bound on a game object's cursor-pick sphere. A wide/tall M2 GO (a forge, an
    // anvil, a large brazier) reports a legitimately big visual radius, but its bounding
    // sphere is a poor click target: the half-diagonal reaches well past the geometry and
    // swallows NPCs standing next to it, so those units become unclickable. Clamping the
    // GO pick radius keeps large objects clickable near their center without stealing the
    // click from a neighbor. Units are never clamped - this is a GO-only correction, the
    // same reasoning that already makes WMO GOs fall back to a conservative fixed sphere.



    // Draw a four-edge screen vignette (gradient overlay along each edge).
    // Used for damage flash, low-health pulse, and level-up golden burst.
    void drawScreenEdgeVignette(uint8_t r, uint8_t g, uint8_t b,
                                int alpha, float thicknessRatio) {
        if (alpha <= 0) return;
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const float W = ImGui::GetIO().DisplaySize.x;
        const float H = ImGui::GetIO().DisplaySize.y;
        const float thickness = std::min(W, H) * thicknessRatio;
        const ImU32 edgeCol = IM_COL32(r, g, b, alpha);
        const ImU32 fadeCol = IM_COL32(r, g, b, 0);
        // Top
        fg->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, thickness),
                                    edgeCol, edgeCol, fadeCol, fadeCol);
        // Bottom
        fg->AddRectFilledMultiColor(ImVec2(0, H - thickness), ImVec2(W, H),
                                    fadeCol, fadeCol, edgeCol, edgeCol);
        // Left
        fg->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(thickness, H),
                                    edgeCol, fadeCol, fadeCol, edgeCol);
        // Right
        fg->AddRectFilledMultiColor(ImVec2(W - thickness, 0), ImVec2(W, H),
                                    fadeCol, edgeCol, edgeCol, fadeCol);
    }

}

namespace wowee { namespace ui {

GameScreen::GameScreen() {
    loadSettings();
}

void GameScreen::openInspectWindow(game::GameHandler& gameHandler) {
    // Loads Blizzard_InspectUI and shows it. The inspect request itself is
    // already on its way from the caller; this only puts a window up.
    gameHandler.runInterfaceCommand("InspectUnit(\"target\")");
}

void GameScreen::applySavedAntiAliasing(rendering::Renderer* renderer) {
    if (!renderer) return;
    settingsPanel_.msaaSettingsApplied_ = true;
    if (settingsPanel_.pendingAntiAliasing <= 0) return;
    renderer->setMsaaSamples(msaaSamplesForChoice(settingsPanel_.pendingAntiAliasing));
}

void GameScreen::applySavedDisplayMode(core::Window* window) {
    if (!window) return;
    // Saved on every change and read back by the panel, and applied by nobody
    // at startup: the window is built from a WindowConfig with the field left
    // at its default, so every session began windowed however it was left.
    //
    // Resolution is deliberately not set with it. This is a desktop
    // fullscreen - Window::applyResolution keeps the desktop mode for one -
    // and the panel's width and height are not saved at all, so they still
    // hold their 1920x1080 defaults this early and would be a guess rather
    // than a setting.
    //
    // Vsync goes the same way for the same reason: saved, and the config it
    // would be read into is written by hand above the window.
    window->setFullscreen(settingsPanel_.pendingFullscreen);
    window->setVsync(settingsPanel_.pendingVsync);
}

// Set UI services and propagate to child components

namespace {
using core::clickDragThreshold;
}

bool GameScreen::getFullscreen() const {
    return services_.window ? services_.window->isFullscreen()
                            : settingsPanel_.pendingFullscreen;
}

void GameScreen::setFullscreen(bool enabled) {
    const bool changed = settingsPanel_.pendingFullscreen != enabled;
    settingsPanel_.pendingFullscreen = enabled;
    if (services_.window) services_.window->setFullscreen(enabled);
    // Kept for the next start, as the Display page's own row is: applied and
    // not saved, the choice lasted until the client was closed.
    if (changed) saveSettings();
}

// The resolution chosen, not the size the window happens to be.
//
// The game's video panel keeps the resolution it read when it was set up and
// hands it back to SetScreenResolution on every Okay, changed or not - in the
// real client that is harmless, because going full screen does not change the
// chosen resolution. Answered from the window's size, going full screen did:
// the desktop's size matched another row, or none, and Okay then applied the
// row read before - which, full screen, switched the monitor to that mode, and
// windowed moved and resized the window off the screen it had been on.
int GameScreen::getResolutionIndex() const {
    return displayResolutionIndexFor(settingsPanel_.pendingResolutionWidth,
                                     settingsPanel_.pendingResolutionHeight);
}

void GameScreen::setResolutionIndex(int index) {
    if (index < 0 || index >= kNumDisplayResolutions) return;
    // Asked for what is already chosen: the panel replaying its value on Okay.
    if (index == getResolutionIndex()) return;
    settingsPanel_.pendingResolutionWidth  = kDisplayResolutions[index][0];
    settingsPanel_.pendingResolutionHeight = kDisplayResolutions[index][1];
    if (services_.window)
        services_.window->applyResolution(settingsPanel_.pendingResolutionWidth,
                                          settingsPanel_.pendingResolutionHeight);
    saveSettings();
}

/// Anti-aliasing, shared with the interface's own Multisampling dropdown.
///
/// Through applySettingSideEffects rather than by writing the field, so the
/// renderer is told: the field alone is the value with nothing applying it,
/// which is the shape [[state_without_an_event]] describes.
int GameScreen::getAntiAliasingIndex() const {
    return settingsPanel_.pendingAntiAliasing;
}

void GameScreen::setAntiAliasingIndex(int index) {
    if (index < 0 || index > 3) return;
    settingsPanel_.pendingAntiAliasing = index;
    settingsPanel_.applySettingSideEffects("antialiasing");
}

void GameScreen::setServices(const UIServices& services) {
    services_ = services;
    // Settings are loaded by the constructor before services are injected.
    // Apply the saved display pacing as soon as the actual window is available.
    if (services_.window &&
        services_.window->isVsyncEnabled() != settingsPanel_.pendingVsync) {
        services_.window->setVsync(settingsPanel_.pendingVsync);
    }
    // The chat channels the player wants joined, pushed as soon as the handler
    // exists rather than only from the in-game frame loop.
    //
    // The handler joins its default channels once, on first world entry, and
    // that happens during the world load - before the first in-game frame and
    // so before the per-frame sync had ever run. The handler was therefore reading
    // its own defaults, which are all true, and a channel the player had turned
    // off was joined anyway. Being the only entry that joins, there was no
    // second chance to get it right.
    if (services_.gameHandler) {
        auto& caj = services_.gameHandler->chatAutoJoin;
        caj.general      = chatPanel_.chatAutoJoinGeneral;
        caj.trade        = chatPanel_.chatAutoJoinTrade;
        caj.localDefense = chatPanel_.chatAutoJoinLocalDefense;
        caj.lfg          = chatPanel_.chatAutoJoinLFG;
        caj.local        = chatPanel_.chatAutoJoinLocal;
    }

    if (services_.window && settingsPanel_.displaySettingsLoaded_) {
        services_.window->setFullscreen(settingsPanel_.pendingFullscreen);
        services_.window->applyResolution(settingsPanel_.pendingResolutionWidth,
                                          settingsPanel_.pendingResolutionHeight);
    }
    // Update legacy pointer for compatibility
    appearanceComposer_ = services.appearanceComposer;
    // Propagate to child panels
    chatPanel_.setServices(services);
    toastManager_.setServices(services);
    settingsPanel_.setServices(services);
    // The chat settings are the chat panel's, and the options panels ask the
    // settings panel for every setting by name. Handing it the chat's own
    // struct is what lets those keys be answered from the same place as the
    // rest rather than through a second bridge with its own list of names.
    settingsPanel_.setChatSettings(&chatPanel_.settings);
    combatUI_.setServices(services);
    windowManager_.setServices(services);
    applyCameraControlSettings();
    // The settings file is read in the constructor, before there is a renderer
    // to hand the graphics values to. This is where one arrives.
    settingsPanel_.applyLoadedSettings();
}

void GameScreen::applyCameraControlSettings() {
    auto* renderer = services_.renderer;
    if (!renderer) return;

    // Field of view is on the camera rather than its controller, and the
    // camera is built asking for 60 - so a saved fov, and the 70 the schema
    // defaults to, only ever reached it when the slider was moved.
    if (auto* cam = renderer->getCamera()) cam->setFov(settingsPanel_.pendingFov);

    if (auto* cam = renderer->getCameraController()) {
        cam->setInvertMouse(settingsPanel_.pendingInvertMouse);
        cam->setCameraSmoothSpeed(settingsPanel_.pendingCameraStiffness);
        cam->setPivotHeight(settingsPanel_.pendingPivotHeight);
        cam->setIdleOrbitEnabled(settingsPanel_.pendingIdleCameraOrbit);
        cam->setSmoothCameraFollow(settingsPanel_.pendingSmoothCameraFollow);
        cam->setShakeScale(settingsPanel_.pendingCameraShake);
        cam->setDistanceMaxFactor(settingsPanel_.pendingCameraDistanceMaxFactor);
    }

    // The controller reads its own settings here for the same reason the
    // camera does: the file is read in the constructor, and these are the
    // values it found rather than the ones the class was built with.
    gamepadControls().setEnabled(settingsPanel_.pendingGamepadEnabled);
    gamepadControls().setLookDegreesPerSecond(settingsPanel_.pendingGamepadLookSpeed);
    gamepadControls().setInvertLook(settingsPanel_.pendingGamepadInvertLook);
    core::gamepad().setStickDeadzone(settingsPanel_.pendingGamepadDeadzone);
}

namespace {

/// Put the bags up, for a vendor or the guild bank.
///
/// Not OpenAllBags: that one toggles, and its reopen path hides every open bag
/// before showing them all again. ContainerFrame_GenerateFrame indexes its bag
/// list by a counter ContainerFrame_OnHide maintains, and this client runs
/// OnHide at the end of the frame rather than from Hide, so the count is stale
/// for the whole sequence and the reopened bags write themselves over one
/// index. The anchor pass then had one name where three belonged, and every
/// bag but the first kept the position its XML gave it - the same one. Walking
/// up to a vendor with bags already open stacked them.
///
/// OpenBackpack and OpenBag are the interface's own openers and no-op on a bag
/// that is already showing, so nothing is hidden and the list stays whole.
/// This is what FrameXML's own merchant frame does with the backpack.
///
/// The bag *keybind* still goes through OpenAllBags, deliberately: that one is
/// a toggle and the player is asking it to toggle. It is still exposed to the
/// same fault, which goes away when OnHide runs from Hide.
constexpr const char* kOpenBagsCommand =
    "OpenBackpack() for i = 1, 4 do OpenBag(i) end";

void openBagsForTrading(game::GameHandler& gameHandler) {
    gameHandler.runInterfaceCommand(kOpenBagsCommand);
}

/// Where a ground-targeted spell lands: the first terrain or WMO surface the
/// cursor ray meets, render coordinates. Units do not stop it: native 2.4.3,
/// Blizzard clicked on a Target Dummy's chest landed on the ground 6.5 yards
/// behind the dummy. Nothing within reach (the sky): empty.
std::optional<glm::vec3> groundUnderRay(rendering::Renderer& renderer, const rendering::Ray& ray) {
    // Further than any spell reaches and than the terrain streamed around
    // the camera; the step only finds the crossing, bisection places it.
    constexpr float kReach = 1000.0f;
    constexpr float kStep = 1.0f;
    float hitT = kReach;
    if (const auto* wmo = renderer.getWMORenderer())
        hitT = wmo->raycastVisibleGeometry(ray.origin, ray.direction, kReach);
    if (const auto* terrain = renderer.getTerrainManager()) {
        const auto clearance = [&](float t) -> std::optional<float> {
            const glm::vec3 p = ray.origin + ray.direction * t;
            if (terrain->isHoleAt(p.x, p.y)) return std::nullopt;
            const auto h = terrain->getHeightAt(p.x, p.y);
            if (!h) return std::nullopt;
            return p.z - *h;
        };
        std::optional<float> previous = clearance(0.0f);
        float previousT = 0.0f;
        for (float t = kStep; t < hitT; t += kStep) {
            const auto now = clearance(t);
            if (previous && now && *previous >= 0.0f && *now < 0.0f) {
                float above = previousT, below = t;
                for (int i = 0; i < 16; ++i) {
                    const float mid = 0.5f * (above + below);
                    const auto c = clearance(mid);
                    if (c && *c >= 0.0f) above = mid; else below = mid;
                }
                hitT = std::min(hitT, below);
                break;
            }
            previous = now;
            previousT = t;
        }
    }
    if (hitT >= kReach) return std::nullopt;
    return ray.origin + ray.direction * hitT;
}

}  // namespace

void GameScreen::render(game::GameHandler& gameHandler) {
    // Apply before any Begin() calls so a scale change cannot alter style
    // metrics halfway through an ImGui frame.
    settingsPanel_.applyWindowUiScale();

    // Set up chat bubble callback (once) and cache game handler in ChatPanel
    chatPanel_.setupCallbacks(gameHandler);

    // Set up appearance-changed callback to refresh inventory preview (barber shop, etc.)
    if (!appearanceCallbackSet_) {
        gameHandler.setAppearanceChangedCallback([this]() {
            inventoryScreenCharGuid_ = 0;  // force preview re-sync on next frame
        });
        appearanceCallbackSet_ = true;
    }

    // Set up UI error frame callback (once)
    if (!uiErrorCallbackSet_) {
        gameHandler.setUIErrorCallback([this](const std::string& msg) {
            // The sound first, and outside the gate. It belongs to no
            // element - the error is the same error whoever draws the text -
            // and it sat after the return, so handing the errors over would
            // have taken the sound with them.
            if (auto* ac = services_.audioCoordinator) {
                if (auto* sfx = ac->getUiSoundManager()) sfx->playError();
            }
            // UIErrorsFrame draws the text now. The sound above is still
            // ours: FrameXML plays none for these, so it would go silent.
            (void)msg;
        });
        uiErrorCallbackSet_ = true;
    }

    // No flash callback here. The red overlay on a failed cast belonged to
    // this client's own action bar; FrameXML draws the bars and flashes its
    // own button from UI_ERROR_MESSAGE.

    // Apply UI transparency setting
    float prevAlpha = ImGui::GetStyle().Alpha;
    ImGui::GetStyle().Alpha = settingsPanel_.uiOpacity_;

    // Minimap terrain is opaque; custom ImGui panel opacity does not fade it.
    {
        auto* renderer = services_.renderer;
        if (renderer) {
            if (auto* minimap = renderer->getMinimap()) {
                minimap->setOpacity(1.0f);
            }
        }
    }

    // Apply initial settings when renderer becomes available
    if (!settingsPanel_.minimapSettingsApplied_ || !settingsPanel_.zoneSettingsApplied_ ||
        !settingsPanel_.terrainSettingsApplied_) {
        auto* renderer = services_.renderer;
        if (renderer) {
            // Re-read from disk once. This screen is constructed by UIManager's
            // constructor, which runs before the login screen exists, so
            // loadSettings() in the constructor saw the file as it was at
            // startup; the login screen's graphics page writes the same file.
            // Once, not every frame: the subsystems below arrive at different
            // moments, and re-reading until the slowest one turned up threw
            // away anything the player changed while waiting.
            if (!settingsPanel_.settingsRereadFromDisk_) {
                settingsPanel_.settingsRereadFromDisk_ = true;
                loadSettings();
            }

            // Each subsystem latches when it has been given its settings. One
            // latch for all three meant the first to exist marked the whole
            // apply done and the others never got theirs, which is how a saved
            // soundtrack setting could be ignored for a whole run.
            if (!settingsPanel_.minimapSettingsApplied_) {
                if (auto* minimap = renderer->getMinimap()) {
                    // The saved rotation, not off. This forced it off at every
                    // start, and the next save wrote the off back to the file.
                    minimap->setRotateWithCamera(settingsPanel_.minimapRotate_);
                    minimap->setSquareShape(settingsPanel_.minimapSquare_);
                    settingsPanel_.minimapSettingsApplied_ = true;
                }
            }
            if (!settingsPanel_.zoneSettingsApplied_) {
            if (auto* zm = renderer->getZoneManager()) {
                zm->setUseOriginalSoundtrack(settingsPanel_.pendingUseOriginalSoundtrack);
                // Setting the flag alone is not the whole apply: music that
                // already started during the loading screen keeps playing
                // through a disabled setting.
                if (!settingsPanel_.pendingUseOriginalSoundtrack) {
                    if (auto* ac = renderer->getAudioCoordinator()) {
                        ac->onOriginalSoundtrackDisabled(zm);
                    }
                }
                settingsPanel_.zoneSettingsApplied_ = true;
            }
            }
            if (!settingsPanel_.terrainSettingsApplied_) {
            if (auto* tm = renderer->getTerrainManager()) {
                tm->setGroundClutterDensityScale(
                    static_cast<float>(settingsPanel_.pendingGroundClutterDensity) / 100.0f);
                renderer->setGrassEnabled(settingsPanel_.pendingGrassEnabled);
                renderer->setGrassScales(
                    static_cast<float>(settingsPanel_.pendingGrassDensity) / 100.0f,
                    static_cast<float>(settingsPanel_.pendingGrassHeight) / 100.0f);
                renderer->setGrassDistance(
                    static_cast<float>(settingsPanel_.pendingGrassDistance));
                settingsPanel_.terrainSettingsApplied_ = true;
            }
            }
            // Restore mute state: save actual master volume first, then apply mute
            if (settingsPanel_.soundMuted_) {
                float actual = audio::AudioEngine::instance().getMasterVolume();
                settingsPanel_.preMuteVolume_ = (actual > 0.0f) ? actual
                    : static_cast<float>(settingsPanel_.pendingMasterVolume) / 100.0f;
                audio::AudioEngine::instance().setMasterVolume(0.0f);
            }
        }
    }

    // Settings are loaded before renderer services are injected. Apply the
    // saved lighting state once the renderer exists so opening the settings
    // window cannot become the first operation that changes scene lighting.
    if (!settingsPanel_.lightingSettingsApplied_) {
        if (auto* renderer = services_.renderer) {
            renderer->setShadowsEnabled(settingsPanel_.pendingShadows);
            renderer->setShadowDistance(settingsPanel_.pendingShadowDistance);
            renderer->setViewDistance(settingsPanel_.pendingViewDistance);
            // The latch waits for the pipeline, not just the renderer.
            //
            // Settings are loaded in the constructor, before the renderer is
            // injected, so the apply that happens while reading the file does
            // nothing and this is the only place brightness ever reaches the
            // pipeline. Marking the work done while the pipeline was still null
            // meant it never did: the value was read from the file, held
            // correctly in the settings panel, and never handed to anything
            // that draws - which reads as gamma not being saved at all.
            auto* post = renderer->getPostProcessPipeline();
            if (post) {
                post->setBrightness(static_cast<float>(settingsPanel_.pendingBrightness) / 50.0f);
                settingsPanel_.lightingSettingsApplied_ = true;
            }
        }
    }

    // Apply saved volume settings once when audio managers first become available
    if (!settingsPanel_.volumeSettingsApplied_) {
        auto* ac = services_.audioCoordinator;
        if (ac && ac->getUiSoundManager()) {
            settingsPanel_.applyAudioVolumes(ac);
            settingsPanel_.volumeSettingsApplied_ = true;
        }
    }

    // Normally already done at startup - see applySavedAntiAliasing, which the
    // application calls before the first frame. This is the fallback for a
    // renderer that was not there yet, and it latches either way.
    if (!settingsPanel_.msaaSettingsApplied_ && settingsPanel_.pendingAntiAliasing > 0) {
        if (auto* renderer = services_.renderer) {
            applySavedAntiAliasing(renderer);
        }
    } else {
        settingsPanel_.msaaSettingsApplied_ = true;
    }

    // Apply saved FXAA setting once the post-process pipeline is available.
    //
    // The pipeline, not the renderer. It is built after the renderer is, and
    // the same shape of mistake next door - latching as soon as the renderer
    // existed - is why gamma was read from the file and never reached anything
    // that draws. Dereferencing it unchecked was the other half: on the frames
    // where it is genuinely absent this was a null call, not a missed setting.
    if (!settingsPanel_.fxaaSettingsApplied_) {
        auto* renderer = services_.renderer;
        auto* post = renderer ? renderer->getPostProcessPipeline() : nullptr;
        if (post) {
            post->setFXAAEnabled(settingsPanel_.pendingFXAA);
            settingsPanel_.fxaaSettingsApplied_ = true;
        }
    }

    // Apply saved water refraction setting once when renderer is available
    if (!settingsPanel_.waterRefractionApplied_) {
        auto* renderer = services_.renderer;
        if (renderer) {
            renderer->setWaterRefractionEnabled(settingsPanel_.pendingWaterRefraction);
            settingsPanel_.waterRefractionApplied_ = true;
        }
    }

    // The saved frame limit, once the window exists to take it.
    if (!settingsPanel_.frameCapApplied_) {
        if (services_.window) {
            services_.window->setFrameCap(frameCapFpsForChoice(settingsPanel_.pendingFrameCap));
            settingsPanel_.frameCapApplied_ = true;
        }
    }

    // The saved sun flare strength, once the sky exists to take it.
    //
    // Its own latch, and it waits for the flare rather than the renderer: the
    // sky is built later than the renderer is, and marking the work done while
    // it was still absent is how brightness came to be read from the file and
    // never handed to anything.
    if (!settingsPanel_.lensFlareApplied_) {
        if (auto* renderer = services_.renderer) {
            if (auto* lf = renderer->getLensFlare()) {
                lf->setIntensity(settingsPanel_.pendingLensFlare);
                settingsPanel_.lensFlareApplied_ = true;
            }
        }
    }

    // Apply saved normal mapping / POM settings once when WMO renderer is available
    if (!settingsPanel_.normalMapSettingsApplied_) {
        auto* renderer = services_.renderer;
        if (renderer) {
            if (auto* wr = renderer->getWMORenderer()) {
                wr->setNormalMappingEnabled(settingsPanel_.pendingNormalMapping);
                wr->setNormalMapStrength(settingsPanel_.pendingNormalMapStrength);
                wr->setPOMEnabled(settingsPanel_.pendingPOM);
                wr->setPOMQuality(settingsPanel_.pendingPOMQuality);
                if (auto* cr = renderer->getCharacterRenderer()) {
                    cr->setNormalMappingEnabled(settingsPanel_.pendingNormalMapping);
                    cr->setNormalMapStrength(settingsPanel_.pendingNormalMapStrength);
                    cr->setPOMEnabled(settingsPanel_.pendingPOM);
                    cr->setPOMQuality(settingsPanel_.pendingPOMQuality);
                    // Both told before this is done with. The two renderers are
                    // built one after the other in the same function today, so
                    // the character one is never missing while the WMO one is
                    // here - but marking it applied from inside the outer guard
                    // meant that if it ever were, characters would keep the
                    // defaults and nothing would try again. Re-telling the WMO
                    // renderer on a later frame costs four setters.
                    settingsPanel_.normalMapSettingsApplied_ = true;
                }
            }
        }
    }

    // Apply saved upscaling setting once when renderer is available
    if (!settingsPanel_.fsrSettingsApplied_) {
        auto* renderer = services_.renderer;
        // Every branch below reaches through getPostProcessPipeline(), so the
        // pipeline is what this waits for - see the FXAA block above.
        auto* fsrPost = renderer ? renderer->getPostProcessPipeline() : nullptr;
        if (renderer && fsrPost) {
#ifdef __APPLE__
            // Frame generation only. It is AMD's runtime and there is none on
            // this platform, so the saved flag must not survive into a session
            // that cannot honour it.
            //
            // Upscaling itself is no longer turned off here. This used to force
            // the whole mode to Off on every start, which left the control
            // working for the session and back at Off at the next one - and the
            // next save wrote that Off over what the player had chosen. Neither
            // upscaler needs the FidelityFX SDK: FSR 1 is a shader of this
            // client's own and FSR 3 falls back to its own compute path, which
            // is what a macOS log shows initialising. Where the device really
            // cannot carry FSR 3, setFSR2Enabled refuses it by name and says so.
            settingsPanel_.pendingAMDFramegen = false;
#endif
            renderer->getPostProcessPipeline()->setFSRQuality(
                fsrScaleForChoice(settingsPanel_.pendingFSRQuality));
            renderer->getPostProcessPipeline()->setFSRSharpness(settingsPanel_.pendingFSRSharpness);
            renderer->getPostProcessPipeline()->setFSR2DebugTuning(settingsPanel_.pendingFSR2JitterSign, settingsPanel_.pendingFSR2MotionVecScaleX, settingsPanel_.pendingFSR2MotionVecScaleY);
            renderer->getPostProcessPipeline()->setAmdFsr3FramegenEnabled(settingsPanel_.pendingAMDFramegen);
            int effectiveMode = settingsPanel_.pendingUpscalingMode;

            // Defer FSR2/FSR3 activation until fully in-world to avoid
            // init issues during login/character selection screens.
            if (effectiveMode == 2 && gameHandler.getState() != game::WorldState::IN_WORLD) {
                renderer->setFSREnabled(false);
                renderer->setFSR2Enabled(false);
            } else {
                renderer->setFSREnabled(effectiveMode == 1);
                renderer->setFSR2Enabled(effectiveMode == 2);
                settingsPanel_.fsrSettingsApplied_ = true;
            }
        }
    }

    // Apply auto-loot / auto-sell settings to GameHandler every frame (cheap bool sync)
    gameHandler.setAutoLoot(settingsPanel_.pendingAutoLoot);
    gameHandler.setAutoFaceTarget(settingsPanel_.pendingAutoFaceTarget);
    // Pushed rather than applied once: a /reload makes a new engine.
    if (services_.addonManager) {
        if (auto* engine = services_.addonManager->getLuaEngine()) {
            engine->setWheelSensitivity(settingsPanel_.pendingScrollSpeed);
        }
    }
    gameHandler.setAutoSellGrey(settingsPanel_.pendingAutoSellGrey);
    gameHandler.setAutoRepair(settingsPanel_.pendingAutoRepair);

    // Sync chat auto-join settings to GameHandler
    gameHandler.chatAutoJoin.general = chatPanel_.chatAutoJoinGeneral;
    gameHandler.chatAutoJoin.trade = chatPanel_.chatAutoJoinTrade;
    gameHandler.chatAutoJoin.localDefense = chatPanel_.chatAutoJoinLocalDefense;
    gameHandler.chatAutoJoin.lfg = chatPanel_.chatAutoJoinLFG;
    gameHandler.chatAutoJoin.local = chatPanel_.chatAutoJoinLocal;

    // Process targeting input before UI windows
    processTargetInput(gameHandler);


    // The chat window is FrameXML's and this client no longer draws one, but
    // the slash commands still belong here: FrameXML's edit box hands an
    // unknown command to runClientChatCommand, which is this client's own
    // registry, and the handlers set the flags read below.
    //
    // These used to sit inside the same gate as the window that is gone, so
    // every one of them ran, set its flag, and had it read by nobody:
    // /inspect, /threat, /bgscore, /gm and /who all did nothing at all.
    {
        auto cmds = chatPanel_.consumeSlashCommands();
        if (cmds.showInspect) openInspectWindow(gameHandler);
        // Both windows are gated on their element, so with either handed over
        // the slash command set a flag whose only reader is switched off.
        if (cmds.showBgScore) {
            gameHandler.runInterfaceCommand("ToggleWorldStateScoreFrame()");
        }
        if (cmds.showGmTicket) {
            gameHandler.runInterfaceCommand("ToggleHelpFrame()");
        }
        // Tab two is the who list. The client's own who window is gated on
        // the social element like the rest of that panel's windows.
        if (cmds.showWho) {
            gameHandler.runInterfaceCommand("ToggleFriendsFrame(2)");
        }
        // The combat log is the chat frame's second window in every
        // interface: Blizzard_CombatLog's from 2.4 on, and 1.12's own
        // ChatFrame2 before it. COMBATLOG names it where it exists.
        if (cmds.toggleCombatLog) {
            gameHandler.runInterfaceCommand(
                "local f = COMBATLOG or ChatFrame2 "
                "if f and FCF_SelectDockFrame then FCF_SelectDockFrame(f) end");
        }
        if (cmds.takeScreenshot) takeScreenshot();
        switch (cmds.recording) {
            case ChatPanel::SlashCommands::Recording::Toggle: toggleRecording(); break;
            case ChatPanel::SlashCommands::Recording::Start:  startRecording();  break;
            case ChatPanel::SlashCommands::Recording::Stop:   stopRecording();   break;
            case ChatPanel::SlashCommands::Recording::None:   break;
        }
    }
    reportRecordingFailure();

    // ---- New UI elements ----
    renderNameplates(gameHandler);  // player names always shown; NPC plates gated by the enemyplates row
    combatUI_.playSoundsForNewChat(gameHandler);
    offerPendingTradeItem(gameHandler);
    // The numbers over the world: build 8606 draws them whatever
    // enableCombatText says, which governs Blizzard_CombatText only.
    combatUI_.renderCombatText(gameHandler);
    // FriendsFrame is what "social" suppresses, and its tabs are the friends
    // list, the who list and the guild roster. Only the friends list was gated
    // on this side, so handing social over left two of this client's three
    // windows drawing beside FrameXML's tabs.
    // The blips, whoever draws the ring around them.
    //
    // This was gated on the element the way every other pass is, and the
    // minimap is the one place that is wrong: WoW's minimap blips come from
    // the C client, not from the interface. minimap.xml declares the border,
    // the buttons, the mail and battlefield icons and the north tag, and not
    // one frame for a party member, a flight master or a corpse - so handing
    // the minimap over and standing this down left the ring drawn, the map
    // inside it drawn, and nothing on it at all.
    //
    // The pass stands down where FrameXML genuinely does own the work: its
    // input, and the mail and battlefield indicators. Both gates are inside
    // it now.
    if (showMinimap_) {
        renderMinimapMarkers(gameHandler);
    }
    chatPanel_.renderBubbles(gameHandler);
    // Not gated on GameMenu ownership. FrameXML's own options frames are shells
    // - its menu buttons are routed here through ShowUIPanel - and this window
    // draws nothing unless something opened it, so leaving it out cost every
    // setting in it the moment the menu was handed over.

    // Always, because this is the only thing that feeds the map its data and
    // the only thing that draws it. Skipping it when FrameXML owns the map
    // left the panel FrameXML drew with nothing in it - the half of that
    // handover that positions the map inside WorldMapDetailFrame was built and
    // the half that renders it was not. renderWorldMap decides for itself
    // whether the map is wanted; under FrameXML that is FrameXML's frame being
    // on screen rather than this client's own flag.
    renderWorldMap(gameHandler);

    // Insert spell link into chat if player shift-clicked a spellbook entry
    {
        std::string pendingSpellLink = spellbookScreen.getAndClearPendingChatLink();
        if (!pendingSpellLink.empty()) {
            chatPanel_.insertChatLink(pendingSpellLink);
        }
    }


    // Set up inventory screen asset manager + player appearance (re-init on character switch)
    {
        uint64_t activeGuid = gameHandler.getActiveCharacterGuid();
        if (activeGuid != 0 && activeGuid != inventoryScreenCharGuid_) {
            auto* am = services_.assetManager;
            if (am) {
                inventoryScreen.setAssetManager(am);
                inventoryScreenCharGuid_ = activeGuid;
            }
        }
    }

    // Set vendor mode before rendering inventory
    inventoryScreen.setGameHandler(&gameHandler);

    // Auto-open bags once when vendor window first opens
    if (gameHandler.isVendorWindowOpen()) {
        if (!windowManager_.vendorBagsOpened_) {
            windowManager_.vendorBagsOpened_ = true;
            openBagsForTrading(gameHandler);
        }
    } else {
        windowManager_.vendorBagsOpened_ = false;
    }

    // Auto-open bags once when the guild bank first opens, so items can be
    // right-clicked to deposit into the vault.
    if (gameHandler.isGuildBankOpen()) {
        if (!windowManager_.guildBankBagsOpened_) {
            windowManager_.guildBankBagsOpened_ = true;
            openBagsForTrading(gameHandler);
        }
    } else {
        windowManager_.guildBankBagsOpened_ = false;
    }

    inventoryScreen.setGameHandler(&gameHandler);
    // Item-target cursor (sharpening stone / oil awaiting the item it applies to)
    inventoryScreen.renderItemTargetCursor();

    // Insert item link into chat if player shift-clicked any inventory/equipment slot
    {
        std::string pendingLink = inventoryScreen.getAndClearPendingChatLink();
        if (!pendingLink.empty()) {
            chatPanel_.insertChatLink(pendingLink);
        }
    }

    if (inventoryScreen.consumeEquipmentDirty() || gameHandler.consumeOnlineEquipmentDirty()) {
        updateCharacterGeosets(gameHandler.getInventory());
        updateCharacterTextures(gameHandler.getInventory());
        if (appearanceComposer_) appearanceComposer_->loadEquippedWeapons();
        // Update renderer weapon type for animation selection
        auto* r = services_.renderer;
        if (r) {
            const auto& mh = gameHandler.getInventory().getEquipSlot(game::EquipSlot::MAIN_HAND);
            const auto& oh = gameHandler.getInventory().getEquipSlot(game::EquipSlot::OFF_HAND);
            if (mh.empty()) {
                if (auto* ac = r->getAnimationController()) ac->setEquippedWeaponType(0, false);
            } else {
                // Polearms and staves use ATTACK_2H_LOOSE instead of ATTACK_2H
                bool is2HLoose = (game::isWeaponSubclass(mh.item.itemClass, mh.item.subClass, game::WEAPON_SUBCLASS_POLEARM) ||
                                  game::isWeaponSubclass(mh.item.itemClass, mh.item.subClass, game::WEAPON_SUBCLASS_STAFF));
                bool isFist = game::isWeaponSubclass(mh.item.itemClass, mh.item.subClass, game::WEAPON_SUBCLASS_FIST);
                bool isDagger = game::isWeaponSubclass(mh.item.itemClass, mh.item.subClass, game::WEAPON_SUBCLASS_DAGGER);
                bool hasOffHand = !oh.empty() &&
                    game::isOffHandWeaponInventoryType(oh.item.inventoryType);
                bool hasShield = !oh.empty() && oh.item.inventoryType == game::InvType::SHIELD;
                bool offHandIsFist = hasOffHand && game::isWeaponSubclass(oh.item.itemClass, oh.item.subClass, game::WEAPON_SUBCLASS_FIST);
                bool offHandIsDagger = hasOffHand && game::isWeaponSubclass(oh.item.itemClass, oh.item.subClass, game::WEAPON_SUBCLASS_DAGGER);
                if (auto* ac = r->getAnimationController()) ac->setEquippedWeaponType(mh.item.inventoryType, is2HLoose, isFist, isDagger, hasOffHand, hasShield, offHandIsFist, offHandIsDagger);
            }
            // Detect ranged weapon type from RANGED slot
            const auto& rangedSlot = gameHandler.getInventory().getEquipSlot(game::EquipSlot::RANGED);
            if (rangedSlot.empty()) {
                if (auto* ac = r->getAnimationController()) ac->setEquippedRangedType(rendering::RangedWeaponType::NONE);
            } else if (rangedSlot.item.inventoryType == game::InvType::RANGED_BOW) {
                // The subclass distinguishes Bow vs Crossbow
                if (game::isWeaponSubclass(rangedSlot.item.itemClass, rangedSlot.item.subClass, game::WEAPON_SUBCLASS_CROSSBOW)) {
                    if (auto* ac = r->getAnimationController()) ac->setEquippedRangedType(rendering::RangedWeaponType::CROSSBOW);
                } else {
                    if (auto* ac = r->getAnimationController()) ac->setEquippedRangedType(rendering::RangedWeaponType::BOW);
                }
            } else if (rangedSlot.item.inventoryType == game::InvType::RANGED_GUN) {
                // That inventory type is guns, crossbows and wands together, so
                // the subclass is what tells them apart - as it already does for
                // a crossbow above. Without this a wand was shouldered and fired
                // like a rifle.
                if (game::isWeaponSubclass(rangedSlot.item.itemClass, rangedSlot.item.subClass, game::WEAPON_SUBCLASS_WAND)) {
                    if (auto* ac = r->getAnimationController()) ac->setEquippedRangedType(rendering::RangedWeaponType::WAND);
                } else if (auto* ac = r->getAnimationController()) {
                    ac->setEquippedRangedType(rendering::RangedWeaponType::GUN);
                }
            } else if (rangedSlot.item.inventoryType == game::InvType::THROWN) {
                if (auto* ac = r->getAnimationController()) ac->setEquippedRangedType(rendering::RangedWeaponType::THROWN);
            } else {
                if (auto* ac = r->getAnimationController()) ac->setEquippedRangedType(rendering::RangedWeaponType::NONE);
            }
        }
    }

    // Update renderer face-target position and selection circle
    auto* renderer = services_.renderer;
    if (renderer) {
        // Receiving damage enters combat without enabling melee attack stance.
        if (auto* ac = renderer->getAnimationController()) ac->setInCombat(gameHandler.hasAutoAttackIntent() &&
                              !gameHandler.isPlayerDead() &&
                              !gameHandler.isPlayerGhost());
        static glm::vec3 targetGLPos;
        if (gameHandler.hasTarget()) {
            auto target = gameHandler.getTarget();
            if (target) {
                // Prefer the renderer's actual instance position so the selection
                // circle tracks the rendered model (not a parallel entity-space
                // interpolator that can drift from the visual position).
                glm::vec3 instPos;
                if (core::Application::getInstance().getRenderPositionForGuid(target->getGuid(), instPos)) {
                    targetGLPos = instPos;
                    // Override Z with foot position to sit the circle on the ground.
                    float footZ = 0.0f;
                    if (core::Application::getInstance().getRenderFootZForGuid(target->getGuid(), footZ)) {
                        targetGLPos.z = footZ;
                    }
                } else {
                    // Fallback: entity game-logic position (no CharacterRenderer instance yet)
                    targetGLPos = core::coords::canonicalToRender(
                        glm::vec3(target->getX(), target->getY(), target->getZ()));
                    float footZ = 0.0f;
                    if (core::Application::getInstance().getRenderFootZForGuid(target->getGuid(), footZ)) {
                        targetGLPos.z = footZ;
                    }
                }
                if (auto* ac = renderer->getAnimationController()) ac->setTargetPosition(&targetGLPos);

                // Selection circle color by reaction (NPCs) and PvP flag (players), as the original client.
                bool showSelectionCircle = false;
                glm::vec3 circleColor(1.0f, 1.0f, 0.3f); // default yellow
                // Build 8606 sizes the ring from CreatureModelData's GeoBox
                // (CGUnit_C 0x60e560, docs/unit-size-rules.md). A unit whose
                // model size is not known yet - display not loaded, or an
                // expansion whose layout carries no GeoBox - draws no ring
                // rather than one of a guessed size, and says so once.
                float circleRadius = 0.0f;
                const bool knownSize = core::Application::getInstance().getSelectionRadiusForGuid(
                    target->getGuid(), circleRadius);
                if (!knownSize && (target->getType() == game::ObjectType::UNIT ||
                                   target->getType() == game::ObjectType::PLAYER)) {
                    static std::unordered_set<uint64_t> saidUnknownSize;
                    if (saidUnknownSize.size() < 64 && saidUnknownSize.insert(target->getGuid()).second) {
                        LOG_WARNING("Selection circle: no CreatureModelData size for target 0x",
                                    std::hex, target->getGuid(), std::dec, " display=",
                                    std::static_pointer_cast<game::Unit>(target)->getDisplayId());
                    }
                }
                if (target->getType() == game::ObjectType::UNIT ||
                    target->getType() == game::ObjectType::PLAYER) {
                    showSelectionCircle = true;
                    auto unit = std::static_pointer_cast<game::Unit>(target);
                    // Reaction colors; unflagged player rings use pale violet in the TBC reference.
                    const glm::vec3 kRed(1.0f, 0.0f, 0.0f), kOrange(1.0f, 0.5f, 0.0f),
                        kYellow(1.0f, 1.0f, 0.0f), kGreen(0.0f, 1.0f, 0.0f), kBlue(0.5f, 0.5f, 1.0f);
                    auto pvpFlagged = [&](uint64_t guid) {
                        auto e = gameHandler.getEntityManager().getEntity(guid);
                        return e && (e->getField(game::fieldIndex(game::UF::UNIT_FIELD_FLAGS)) & 0x00001000u);
                    };
                    if (unit->getHealth() == 0 && unit->getMaxHealth() > 0) {
                        circleColor = glm::vec3(0.5f, 0.5f, 0.5f); // gray (dead)
                    } else if (target->getType() == game::ObjectType::PLAYER ||
                               (unit->getUnitFlags() & game::UNIT_FLAG_PLAYER_CONTROLLED)) {
                        // Player-controlled units: blue unflagged, green friendly flagged, yellow hostile
                        // flagged while we are not, red when both are flagged.
                        if (gameHandler.isDuelOpponent(*unit)) circleColor = kRed;
                        else if (!pvpFlagged(target->getGuid())) circleColor = kBlue;
                        else if (!unit->isHostile()) circleColor = kGreen;
                        else circleColor = pvpFlagged(gameHandler.getPlayerGuid()) ? kRed : kYellow;
                    } else {
                        const int reaction = gameHandler.unitReactionToPlayer(*unit);
                        if (reaction <= 2 || gameHandler.isAggressiveTowardPlayer(target->getGuid()))
                            circleColor = kRed;
                        else if (reaction == 3) circleColor = kOrange;
                        else if (reaction == 4) circleColor = kYellow;
                        else circleColor = kGreen;
                    }
                }
                // Nothing else gets one. WoW draws a selection circle under a
                // unit and a player and under nothing else - a door, a mailbox,
                // a bank vault is used rather than selected, and a ring on the
                // floor around one reads as a creature being fought.
                // None under the local player: native 2.4.3 targeting itself
                // (Edrike on the GM Island hilltop, standing on the ground)
                // draws no ring, while another player there gets one.
                if (target->getGuid() == gameHandler.getPlayerGuid()) showSelectionCircle = false;
                if (showSelectionCircle && knownSize) {
                    if (gameHandler.hasAutoAttackIntent() &&
                        gameHandler.getAutoAttackTargetGuid() == target->getGuid()) {
                        // Build 8606 attack selection alternates pink/red and
                        // pale yellow once per second, even outside melee range.
                        const float pulse = 0.5f + 0.5f * std::sin(
                            static_cast<float>(ImGui::GetTime() * 6.283185307179586));
                        circleColor = glm::vec3(1.0f, 0.5f + 0.5f * pulse, 0.5f);
                    }
                    // Centred on the unit's own position (the mount's when
                    // riding), not its feet: the ring is where the selection
                    // sphere meets the ground (OverlaySystem). The renderer
                    // re-reads the instance when it records the ring.
                    auto* spawner = core::Application::getInstance().getEntitySpawner();
                    const uint32_t groundInstance = spawner ? spawner->groundInstanceIdForGuid(target->getGuid()) : 0;
                    glm::vec3 circleCenter = core::coords::canonicalToRender(
                        glm::vec3(target->getX(), target->getY(), target->getZ()));
                    if (groundInstance && renderer->getCharacterRenderer())
                        renderer->getCharacterRenderer()->getInstancePosition(groundInstance, circleCenter);
                    renderer->setSelectionCircle(circleCenter, circleRadius, circleColor, groundInstance);
                } else {
                    renderer->clearSelectionCircle();
                }
            } else {
                if (auto* ac = renderer->getAnimationController()) ac->setTargetPosition(nullptr);
                renderer->clearSelectionCircle();
            }
        } else {
            if (auto* ac = renderer->getAnimationController()) ac->setTargetPosition(nullptr);
            renderer->clearSelectionCircle();
        }
    }

    // Screen edge damage flash - red vignette that fires on HP decrease
    {
        const bool deadOrGhost = gameHandler.isPlayerDead() || gameHandler.isPlayerGhost();
        auto playerEntity = gameHandler.getEntityManager().getEntity(gameHandler.getPlayerGuid());
        uint32_t currentHp = 0;
        if (playerEntity && (playerEntity->getType() == game::ObjectType::PLAYER ||
                             playerEntity->getType() == game::ObjectType::UNIT)) {
            auto unit = std::static_pointer_cast<game::Unit>(playerEntity);
            if (unit->getMaxHealth() > 0)
                currentHp = unit->getHealth();
        }

        // Detect HP drop (ignore transitions from 0 - entity just spawned or uninitialized)
        if (!deadOrGhost && settingsPanel_.damageFlashEnabled_ &&
            lastPlayerHp_ > 0 && currentHp < lastPlayerHp_ && currentHp > 0) {
            damageFlashAlpha_ = 1.0f;
        }
        lastPlayerHp_ = currentHp;

        // Spirit release can leave a low/non-zero health value on the local
        // entity. Never carry a pre-death damage flash into ghost form.
        if (deadOrGhost) {
            damageFlashAlpha_ = 0.0f;
        }

        // Fade out over ~0.5 seconds
        if (damageFlashAlpha_ > 0.0f) {
            damageFlashAlpha_ -= ImGui::GetIO().DeltaTime * 2.0f;
            if (damageFlashAlpha_ < 0.0f) damageFlashAlpha_ = 0.0f;
            drawScreenEdgeVignette(200, 0, 0,
                                   static_cast<int>(damageFlashAlpha_ * 100.0f), 0.12f);
        }
    }

    // Persistent low-health vignette - pulsing red edges when HP < 20%
    {
        auto playerEntity = gameHandler.getEntityManager().getEntity(gameHandler.getPlayerGuid());
        const bool deadOrGhost = gameHandler.isPlayerDead() || gameHandler.isPlayerGhost();
        float hpPct = 1.0f;
        if (!deadOrGhost && playerEntity &&
            (playerEntity->getType() == game::ObjectType::PLAYER ||
             playerEntity->getType() == game::ObjectType::UNIT)) {
            auto unit = std::static_pointer_cast<game::Unit>(playerEntity);
            if (unit->getMaxHealth() > 0)
                hpPct = static_cast<float>(unit->getHealth()) / static_cast<float>(unit->getMaxHealth());
        }

        // Only show when alive and below 20% HP; intensity increases as HP drops
        if (settingsPanel_.lowHealthVignetteEnabled_ && !deadOrGhost &&
            hpPct < 0.20f && hpPct > 0.0f) {
            // Base intensity from HP deficit (0 at 20%, 1 at 0%); pulse at ~1.5 Hz
            float danger = (0.20f - hpPct) / 0.20f;
            float pulse  = 0.55f + 0.45f * std::sin(static_cast<float>(ImGui::GetTime()) * 9.4f);
            int   alpha  = static_cast<int>(danger * pulse * 90.0f);  // max ~90 alpha, subtle
            drawScreenEdgeVignette(200, 0, 0, alpha, 0.15f);
        }
    }

    // Restore previous alpha
    ImGui::GetStyle().Alpha = prevAlpha;
}

/// Put the item dropped on a player into the trade it opened.
///
/// CMSG_INITIATE_TRADE is a request and the window is the server's answer, so
/// the item cannot be offered at the moment of the drop. It waits here until
/// the trade is up, and is dropped if the trade never opens - a refusal, or a
/// player who walked away - rather than being offered into the next one.
void GameScreen::offerPendingTradeItem(game::GameHandler& gameHandler) {
    if (!pendingTradeItem_.active) return;
    if (!gameHandler.isTradeOpen()) {
        // Only while the request could still be answered. Leaving the world
        // ends that, and so does putting the item somewhere else.
        if (!gameHandler.isInWorld()) pendingTradeItem_ = PendingTradeItem{};
        return;
    }
    gameHandler.setTradeItem(0, pendingTradeItem_.bag, pendingTradeItem_.slot);
    pendingTradeItem_ = PendingTradeItem{};
}

/// A cursor picture, uploaded once and kept.
///
/// Only a successful upload is cached, so a transient descriptor-pool failure
/// is retried rather than leaving the cursor plain for good, and a file that
/// will not load says so once - a cursor that fails and a thing that has no
/// cursor of its own both end as the plain hand, and from the chair they look
/// the same.
VkDescriptorSet cursorTexture(pipeline::AssetManager* assets, core::Window* window,
                              const char* path) {
    static std::unordered_map<std::string, VkDescriptorSet> loaded;
    auto it = loaded.find(path);
    if (it != loaded.end() && it->second != VK_NULL_HANDLE) return it->second;

    ui::UiTextureLoad why = ui::UiTextureLoad::Ok;
    VkDescriptorSet tex = ui::uploadUiTextureFromBlp(assets, path, window, &why);
    if (tex) {
        loaded[path] = tex;
        return tex;
    }
    static std::set<std::string> said;
    if (said.insert(path).second) {
        LOG_WARNING("Cursor art ", path, " did not load (", static_cast<int>(why),
                    ") - the pointer stays the plain hand there");
    }
    return VK_NULL_HANDLE;
}

/// Draw one in place of the pointer. The tip sits at the mouse position, the
/// way the art is authored.
void GameScreen::selectCursorTexture(VkDescriptorSet tex) {
    SDL_Window* focused = SDL_GetMouseFocus();
    if (!focused || SDL_GetWindowRelativeMouseMode(focused) ||
        !ImGui::IsMousePosValid()) return;
    ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    cursorTexture_ = tex;
}

void GameScreen::renderCursor() {
    const auto tex = cursorTexture_;
    const auto held = heldIconTexture_;
    cursorTexture_ = VK_NULL_HANDLE;
    heldIconTexture_ = VK_NULL_HANDLE;
    if (!tex) return;
    SDL_Window* focused = SDL_GetMouseFocus();
    if (!focused || SDL_GetWindowRelativeMouseMode(focused) ||
        !ImGui::IsMousePosValid()) return;
    const ImVec2 at = ImGui::GetIO().MousePos;
    constexpr float kSize = 32.0f;
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    if (held) {
        fg->AddImage((ImTextureID)(uintptr_t)held, at, ImVec2(at.x + kSize, at.y + kSize));
    }
    fg->AddImage((ImTextureID)(uintptr_t)tex, at, ImVec2(at.x + kSize, at.y + kSize));
}

bool GameScreen::drawVendorCursor(game::GameHandler& gameHandler,
                                  const ui::ScenePick& pick) {
    const uint64_t guid = pick.resolve();
    const auto entity = gameHandler.getEntityManager().getEntity(guid);
    if (!entity || (entity->getType() != game::ObjectType::UNIT &&
                    entity->getType() != game::ObjectType::PLAYER)) return false;
    const auto unit = std::static_pointer_cast<game::Unit>(entity);
    const auto questStatus = gameHandler.getQuestGiverStatus(guid);
    const auto& creatureInfo = gameHandler.getCreatureInfoCache();
    const auto info = creatureInfo.find(unit->getEntry());
    const std::string_view iconName = info != creatureInfo.end() ? std::string_view(info->second.iconName) : std::string_view{};
    const auto harvesting = ui::corpseHarvestPresentation(gameHandler, *unit);
    const std::string path = ui::unitCursorPath(
        !game::isUnitCorpseState(unit->getHealth(), unit->getMaxHealth(), unit->getDynamicFlags()),
        unit->getDynamicFlags(), gameHandler.unitReactionToPlayer(*unit),
        unit->getNpcFlags(), questStatus, iconName, gameHandler.canPlayerAttackUnit(*unit), unit->getUnitFlags(),
        harvesting && harvesting->difficulty != game::HarvestDifficulty::Unavailable,
        harvesting && harvesting->knowsProfession, gameHandler.unitWithinInteractionReach(*unit));
    const auto texture = cursorTexture(services_.assetManager, services_.window, path.c_str());
    if (!texture) return false;
    selectCursorTexture(texture);
    return true;
}

namespace {

enum class LockSpellAim { Invalid, TooLow, Ok };

/// What the spell waiting for its lock (Pick Lock) would do at a game
/// object: Invalid ("UnableCast": not a game object, beyond the spell's range
/// from the object's origin, or no applying lock slot of the spell's
/// LockType), TooLow (the player's rank is below that slot's Skill) or Ok.
/// Original 2.4.3 (pair 22): Herod's Door shows Cast 4.94 yards from its
/// origin and UnableCast at 5.09 (Pick Lock's Interact Range, 5), while a
/// right click still reaches it there; Cast over Workshop Door 90566 at
/// Lockpicking 1 and 200; UnableCast over the ground, a Practice Lockbox 11
/// yards away, Herod's Door standing open (Quick Close only), a barrel and a
/// Scarlet Myrmidon.
LockSpellAim lockSpellAim(game::GameHandler& gameHandler, uint64_t objectGuid) {
    const uint32_t spellId = gameHandler.pendingLockTargetSpell();
    assert(spellId != 0);
    const auto object = gameHandler.getEntityManager().getEntity(objectGuid);
    if (!object || object->getType() != game::ObjectType::GAMEOBJECT) return LockSpellAim::Invalid;
    const auto* info = gameHandler.getCachedGameObjectInfo(
        std::static_pointer_cast<game::GameObject>(object)->getEntry());
    if (!info) return LockSpellAim::Invalid;
    const auto& me = gameHandler.getMovementInfo();
    const float distance = glm::length(glm::vec3(object->getX() - me.x, object->getY() - me.y,
                                                 object->getZ() - me.z));
    if (distance > gameHandler.getSpellData(spellId).maxRange) return LockSpellAim::Invalid;
    switch (gameHandler.lockSpellTarget(spellId, info, *object)) {
        case game::GameHandler::LockSpellTarget::NoSlot: return LockSpellAim::Invalid;
        case game::GameHandler::LockSpellTarget::TooLow: return LockSpellAim::TooLow;
        case game::GameHandler::LockSpellTarget::Ok: return LockSpellAim::Ok;
    }
    return LockSpellAim::Invalid;
}

} // namespace

bool GameScreen::drawWorldObjectCursor(game::GameHandler& gameHandler,
                                       const ui::ScenePick& pick) {
    // resolve(), for the reason the vendor cursor gives: it is what a click
    // would act on, and asking the hover a different question puts the two
    // back into disagreement.
    //
    uint64_t guid = pick.resolve();
    const bool usable = guid != 0;
    if (guid == 0) guid = pick.namedObjectGuid;
    if (guid == 0) return false;
    auto entity = gameHandler.getEntityManager().getEntity(guid);
    if (!entity || entity->getType() != game::ObjectType::GAMEOBJECT) return false;

    auto go = std::static_pointer_cast<game::GameObject>(entity);
    const auto* info = gameHandler.getCachedGameObjectInfo(go->getEntry());
    if (usable) {
        assert(info);  // resolve() only answers an object whose template is known
        const auto& rule = game::gameObjectTypeRule(info->type);
        assert(rule.use && rule.cursor);
        game::GameObjectCursorFacts facts;
        facts.typeCursor = rule.cursor;
        constexpr uint32_t kGameObjectTypeQuestGiver = 2;
        if (info->type == kGameObjectTypeQuestGiver) {
            facts.questCursor = game::questGiverCursor(gameHandler.getQuestGiverStatus(guid));
        }
        const uint32_t lockId = game::gameObjectLockId(info);
        if (lockId != 0) {
            assert(services_.assetManager);
            const auto locks = services_.assetManager->loadDBC("Lock.dbc");
            const auto types = services_.assetManager->loadDBC("LockType.dbc");
            assert(locks && locks->isLoaded() && locks->getFieldCount() >= 33);
            assert(types && types->isLoaded() && types->getFieldCount() > 1);
            const int32_t row = locks->findRecordById(lockId);
            assert(row >= 0);
            // Lock slot 0 alone names the cursor, and only a LockType does
            // (kind 2); a key item there (Hidden Strongbox 2039, Lock 46) or
            // an empty slot 0 (Alliance Strongbox 4095, Lock 23) is the gear.
            constexpr uint32_t kLockKeySkill = 2;
            const uint32_t typeId = locks->getUInt32(row, 9);
            if (locks->getUInt32(row, 1) == kLockKeySkill && typeId != 0) {
                const int32_t typeRow = types->findRecordById(typeId);
                assert(typeRow >= 0);
                // CursorName is the final, non-localized LockType column;
                // blank for Open, Treasure, Quick Open and the rest.
                facts.lockCursor = types->getString(typeRow, types->getFieldCount() - 1);
            }
        }
        // GO_FLAG_LOCKED greys the gear unless the player can open it: a key
        // of the slots that apply now carried, or a known spell opening one
        // (game::GameObjectLockStatus). Original 2.4.3 (pair 22): Gauntlet
        // Gate 175357 greyed without Key to the City, the gear with it;
        // King's Square Gate spawned open (Closing) the gear; Practice
        // Lockbox (Lockpicking 1) the gear with Pick Lock known, 3 yards away.
        if (lockId != 0) {
            const auto lock = gameHandler.gameObjectLockStatus(info, *go);
            facts.flaggedLocked = lock->flaggedLocked && lock->difficulty == game::LockDifficulty::Impossible;
        }
        // Fishing has a single cursor asset; bobbers use their own reach.
        facts.inRange = info->type == 17 || gameHandler.isGameObjectInRange(guid) != false;
        const std::string path = game::gameObjectCursorPath(facts);
        VkDescriptorSet tex = cursorTexture(services_.assetManager, services_.window, path.c_str());
        if (!tex) return false;
        selectCursorTexture(tex);
    }

    // Generic signs opt into labels through their server template: a floating
    // label at the cursor, or with highlight alone the default tooltip (guild
    // master sign, ledger #04). Chairs and unlabelled scenery keep the cursor.
    const auto tooltip = info ? game::objectNameTooltip(info->type, info->data[0], info->data[1])
                              : game::ObjectNameTooltip::Default;
    if (tooltip == game::ObjectNameTooltip::None) return usable;

    const std::string name = game::entityDisplayName(entity);
    if (name.empty() || name == "Unknown") return usable;
    worldObjectHoverGuid_ = guid;
    worldObjectTooltipFloating_ = tooltip == game::ObjectNameTooltip::Floating;
    return true;
}

void GameScreen::processTargetInput(game::GameHandler& gameHandler) {
    auto& io = ImGui::GetIO();
    auto& input = core::Input::getInstance();

    auto& bindings = KeybindingManager::getInstance();
    if (bindings.isCommandPressed("OPENCHATSLASH")) {
        gameHandler.runInterfaceCommand("ChatFrame_OpenChat(\"/\")");
        noteChatOpenedBySlash();
    }
    if (bindings.isCommandPressed("OPENCHAT"))
        gameHandler.runInterfaceCommand("ChatFrame_OpenChat(\"\")");

    // Anything at all is taking typed input: this client's chat box, an ImGui
    // field, or one of the interface's own edit boxes.
    //
    // That third one was left out, and the block of game hotkeys further down
    // asks only this. So every one of them fired while the player was typing
    // into a FrameXML box - H opened the titles window from the middle of a
    // mail recipient, C opened the character sheet, I the bags, and the digits
    // fired action bar slots. The two places that had noticed named the
    // interface's box separately, which is how the third came to be missing
    // from the rest.
    const bool textFocus = io.WantTextInput || interfaceTakingTypedInput();

    // Game hotkeys - gate on textFocus (chat/text-input active) rather than
    // WantCaptureKeyboard so that toggle keys like M, C, I still work when an
    // ImGui window (character panel, map, etc.) happens to have focus.
    {
        // The manager filters consumed physical presses, not unrelated pad
        // sources of the same command.
        if (!textFocus && bindings.isCommandPressed("TARGETNEARESTENEMY")) {
            gameHandler.tabTarget();
        }

        if (!textFocus &&
            KeybindingManager::getInstance().isActionPressed(KeybindingManager::Action::TOGGLE_SETTINGS, true)) {
            // Gathered, then decided, then done - rather than decided while
            // being done. The order of these branches is the whole of what
            // Escape means, and as a chain of else-if inside a draw there was
            // no way to ask what the key would do without being in the
            // situation. resolveEscape is that question on its own, and its
            // test states each situation directly.
            EscapeState st;
            // Capture was checked per source by the manager. A consumed
            // keyboard Escape must not swallow a simultaneous controller B.
            st.interfaceConsumedKey  = false;
            // Native 2.4.3 runs ToggleGameMenu and nothing before it:
            // StaticPopups, the option/menu frames, CloseMenus,
            // SpellStopCasting, SpellStopTargeting, CloseAllWindows (whose
            // panels close the server side in their OnHide: CloseMerchant,
            // CloseLoot, CloseGossip ...), ClearTarget, then the game menu.
            // An item on the cursor stays there (measured: backpack open,
            // item held - Escape closed the backpack, then cleared the
            // target, then opened the menu with the item still held). So this
            // client only acts first for windows it still draws itself.
            const auto ours = [](UiElement e) { return !frameXmlOwns(e); };
            st.holdingItem           = inventoryScreen.isHoldingItem();
            st.casting               = ours(UiElement::GameMenu) && gameHandler.isCasting();
            st.lootOpen              = ours(UiElement::Loot) && gameHandler.isLootWindowOpen();
            st.gossipOpen            = ours(UiElement::Gossip) && gameHandler.isGossipWindowOpen();
            st.vendorOpen            = ours(UiElement::Vendor) && gameHandler.isVendorWindowOpen();
            st.barberShopOpen        = ours(UiElement::BarberShop) && gameHandler.isBarberShopOpen();
            st.bankOpen              = ours(UiElement::Bank) && gameHandler.isBankOpen();
            st.guildBankOpen         = ours(UiElement::GuildBank) && gameHandler.isGuildBankOpen();
            st.trainerOpen           = ours(UiElement::ClassTrainer) && gameHandler.isTrainerWindowOpen();
            st.mailboxOpen           = ours(UiElement::Mail) && gameHandler.isMailboxOpen();
            st.auctionHouseOpen      = ours(UiElement::AuctionHouse) && gameHandler.isAuctionHouseOpen();
            st.questDetailsOpen      = ours(UiElement::QuestGiver) && gameHandler.isQuestDetailsOpen();
            st.questOfferRewardOpen  = ours(UiElement::QuestGiver) && gameHandler.isQuestOfferRewardOpen();
            st.questRequestItemsOpen = ours(UiElement::QuestGiver) && gameHandler.isQuestRequestItemsOpen();
            st.tradeOpen             = ours(UiElement::Trade) && gameHandler.isTradeOpen();

            const EscapeAction action = resolveEscape(st);
            // At warning, like the three in the pump it pairs with.
            //
            // This chain has been read end to end more times than any other in
            // the client and every link checks out on paper; what is missing is
            // which branch actually runs, and that is the one thing reading
            // cannot tell you. It was said at info, and the log a report
            // arrives with is warnings only - so a session that reproduced
            // "Escape does nothing" came back with no Escape line in it at all,
            // and silence there meant nothing, because it is also exactly what
            // a working press sounds like.
            //
            // One line per press, and Escape is not a key anyone holds down.
            LOG_WARNING("Escape: ", escapeActionName(action));
            switch (action) {
                case EscapeAction::CancelCast:             gameHandler.cancelCast(); break;
                case EscapeAction::CloseLoot:              gameHandler.closeLoot(); break;
                case EscapeAction::CloseGossip:            gameHandler.closeGossip(); break;
                case EscapeAction::ReturnHeldItem:
                    if (inventoryScreen.isHoldingItem())
                        inventoryScreen.returnHeldItem(gameHandler.getInventory());
                    else
                        frameXmlPutCursorDown();
                    break;
                case EscapeAction::CloseVendor:            gameHandler.closeVendor(); break;
                case EscapeAction::CloseBarberShop:        gameHandler.closeBarberShop(); break;
                case EscapeAction::CloseBank:              gameHandler.closeBank(); break;
                case EscapeAction::CloseGuildBank:         gameHandler.closeGuildBank(); break;
                case EscapeAction::CloseTrainer:           gameHandler.closeTrainer(); break;
                case EscapeAction::CloseMailbox:           gameHandler.closeMailbox(); break;
                case EscapeAction::CloseAuctionHouse:      gameHandler.closeAuctionHouse(); break;
                case EscapeAction::DeclineQuest:           gameHandler.declineQuest(); break;
                case EscapeAction::CloseQuestOfferReward:  gameHandler.closeQuestOfferReward(); break;
                case EscapeAction::CloseQuestRequestItems: gameHandler.closeQuestRequestItems(); break;
                case EscapeAction::CancelTrade:            gameHandler.cancelTrade(); break;
                case EscapeAction::None: break;
                case EscapeAction::AskTheInterface: {
                    // Asked only here, because asking closes things. Everything
                    // above is a window the *server* knows about and each has
                    // to go through the client so the closing packet is sent -
                    // CloseAllWindows would hide the frame and leave the server
                    // believing the vendor was still open.
                    //
                    // Ask ToggleGameMenu and nothing else, when the
                    // interface owns the menu.
                    //
                    // This used to close menus, special windows and all
                    // windows itself first, and skip ToggleGameMenu whenever
                    // one of them answered - which is a fragment of the very
                    // cascade ToggleGameMenu already runs, in the same order,
                    // and ending differently. Blizzard's version tries static
                    // popups, the option frames, dropdowns, casting, targeting
                    // and CloseAllWindows in turn and then, if none of them
                    // wanted the key, opens the game menu. Ours consumed the
                    // press before it could reach that last step, so Escape
                    // could shut the menu and never raise it.
                    //
                    // The order the old comment cared about - a dropdown over
                    // a panel closing before the panel - is ToggleGameMenu's
                    // own order, and it keeps it.
                    // Nothing is asked of the interface first. ToggleGameMenu
                    // closes menus, special windows and all windows in that
                    // order before it opens anything, and it is the interface
                    // that draws the menu now - so asking here would only be
                    // this client doing the first half of ToggleGameMenu's own
                    // work, in ToggleGameMenu's own order, ahead of it.
                    const EscapeOutcome outcome = resolveAfterInterface(
                        /*interfaceClosedAPanel=*/false, /*frameXmlOwnsMenu=*/true);
                    // At warning, because this is the press the report is
                    // about and the default log is warnings only.
                    //
                    // Every other branch of this chain speaks at info, so a
                    // session that reproduces "Escape does nothing" came back
                    // with no Escape line in it at all - and silence there
                    // meant nothing, since it is also what a working press
                    // sounds like. One line per press that gets this far, and
                    // it names which of the two menus was chosen. If that line
                    // is absent from a log where Escape was pressed with
                    // nothing open, the key never reached this chain and the
                    // fault is in the input path rather than here.
                    if (outcome == EscapeOutcome::InterfaceClosedAPanel) {
                        LOG_INFO("Escape: ", escapeOutcomeName(outcome));
                    } else {
                        LOG_WARNING("Escape: ", escapeOutcomeName(outcome));
                    }
                    switch (outcome) {
                        case EscapeOutcome::InterfaceClosedAPanel:
                            break;
                        case EscapeOutcome::ToggleInterfaceMenu:
                            // The interface's own way in, and the same function
                            // its own Escape binding calls. This branch used to
                            // set the flag behind *this* client's menu, and
                            // that menu is only drawn while the element is not
                            // handed over - so with it handed over, Escape set
                            // a flag nobody read and nothing appeared.
                            gameHandler.runInterfaceCommand("ToggleGameMenu()");
                            // And whether it worked, which the line above
                            // cannot say. Asked straight afterwards so the two
                            // faults separate: shown but not on screen is a
                            // drawing problem, not shown is a problem in
                            // ToggleGameMenu - which runs clean headlessly.
                            //
                            // At warning when it did not work, because that is
                            // the one outcome nobody would think to look for.
                            // The whole of this chain has been read seven
                            // times and every link checks out; the interface
                            // side is settled too - headlessly ToggleGameMenu
                            // shows GameMenuFrame at 195x240, visible, alpha
                            // one, parented to UIParent. So the answer is in
                            // the running client, and a line only the info
                            // level shows is a line nobody has. This says it
                            // where the default log will keep it.
                            // rawget, not truth: a missing GameMenuFrame is
                            // answered by the no-op object, whose IsShown
                            // returns another one, so this reported the menu
                            // shown whether or not the frame existed.
                            if (gameHandler.askInterface(
                                    "type(rawget(_G, 'GameMenuFrame')) == 'table' "
                                    "and GameMenuFrame:IsShown()")) {
                                LOG_INFO("Escape: the interface's menu is now shown");
                            } else {
                                LOG_WARNING(
                                    "Escape: asked the interface for its game "
                                    "menu and GameMenuFrame is still not shown "
                                    "- ToggleGameMenu ran and left it hidden");
                            }
                            break;
                    }
                    break;
                }
            }
        }

        if (!textFocus) {
            // Toggle character screen (C) and inventory/bags (I)
            if (KeybindingManager::getInstance().isActionPressed(KeybindingManager::Action::TOGGLE_CHARACTER_SCREEN)) {
                // The route table in application.cpp calls ToggleCharacter for
                // this key. Calling it here as well was the whole of a bug
                // reported three times: IsKeyPressed does not consume, so both
                // sites saw the same press and toggled in the same frame -
                // open, then shut, and nothing on screen.
                if (gameHandler.isConnected()) gameHandler.requestPlayedTime();
            }

            if (KeybindingManager::getInstance().isActionPressed(KeybindingManager::Action::TOGGLE_INVENTORY)) {
                // The binding's own FrameXML body (OpenAllBags in 2.4.3 and 3.3.5).
                gameHandler.runInterfaceCommand(bindingScriptLua("OPENALLBAGS"));
            }

            if (bindings.isCommandPressed("NAMEPLATES") ||
                bindings.isCommandPressed("FRIENDNAMEPLATES") ||
                bindings.isCommandPressed("ALLNAMEPLATES")) {
                // Match TBC 2.4.3 Bindings.xml toggle semantics (V and Shift+V
                // are mutually exclusive, Ctrl+V toggles both) and persist both
                // CVars via the settings panel so the choice survives restart.
                const ui::NameplateToggleCommand cmd =
                    bindings.isCommandPressed("ALLNAMEPLATES") ? ui::NameplateToggleCommand::All :
                    bindings.isCommandPressed("FRIENDNAMEPLATES") ? ui::NameplateToggleCommand::Friends :
                    ui::NameplateToggleCommand::Enemies;
                const ui::NameplateToggleState next = ui::toggleNameplates(
                    {settingsPanel_.showEnemyNameplates_, settingsPanel_.showFriendlyNameplates_},
                    cmd, game::isPreWotlk());
                settingsPanel_.setSettingValue("enemyplates", next.showEnemies ? "1" : "0");
                settingsPanel_.setSettingValue("friendlyplates", next.showFriends ? "1" : "0");
                saveSettings();
            }

            if (KeybindingManager::getInstance().isActionPressed(KeybindingManager::Action::TOGGLE_WORLD_MAP)) {
                if (frameXmlOwns(UiElement::WorldMap)) {
                    // Nothing: the route table owns this key too. The call
                    // written here named ToggleWorldMap, which 3.3.5 does not
                    // have, so it silently did nothing and left the map
                    // working on the route table's single toggle.
                } else {
                    showWorldMap_ = !showWorldMap_;
                }
            }

            if (KeybindingManager::getInstance().isActionPressed(KeybindingManager::Action::TOGGLE_MINIMAP)) {
                showMinimap_ = !showMinimap_;
                // This flag gates this client's marker pass, which is drawn
                // over FrameXML's minimap on purpose rather than instead of it
                // - so on its own the key hid the markers and left the minimap
                // underneath them up. ToggleMinimap is what the interface's own
                // TOGGLEMINIMAP binding calls, and it hides the frame the
                // markers sit on, which is what makes the two agree.
                if (frameXmlOwns(UiElement::Minimap)) {
                    gameHandler.runInterfaceCommand(bindingScriptLua("TOGGLEMINIMAP"));
                }
            }

            if (KeybindingManager::getInstance().isActionPressed(KeybindingManager::Action::TOGGLE_ACHIEVEMENTS)) {
                gameHandler.runInterfaceCommand(bindingScriptLua("TOGGLEACHIEVEMENT"));
            }
            if (KeybindingManager::getInstance().isActionPressed(KeybindingManager::Action::TOGGLE_SKILLS)) {
                // The skills list is a tab of the character sheet in FrameXML
                // rather than a window of its own.
                gameHandler.runInterfaceCommand(bindingScriptLua("TOGGLECHARACTER1"));
            }

            // H, which in WoW is TOGGLECHARACTER4: the Player vs Player panel,
            // where battlegrounds and arenas are queued for.
            //
            // It was toggling a titles window this client drew, under a comment
            // saying H had no keybinding to conflict with - it has had one since
            // the game shipped. The titles window went with it: the character
            // sheet has a title picker of its own, on the panel FrameXML draws
            // in every run, so keeping a second one on a key that belongs to
            // something else was two faults holding each other up.
            if (KeybindingManager::getInstance().isActionPressed(KeybindingManager::Action::TOGGLE_PVP)) {
                // 2.4.3 binds it to ToggleCharacter("PVPFrame"); TogglePVPFrame
                // is 3.3.5's and does not exist there, so H did nothing.
                gameHandler.runInterfaceCommand(bindingScriptLua("TOGGLECHARACTER4"));
            }

            if (bindings.isCommandPressed("SCREENSHOT")) takeScreenshot();
            if (bindings.isCommandPressed("WOWEETOGGLERECORDING")) toggleRecording();

            // Cache only immutable names, as strings to avoid per-poll conversions.
            // Bindings and the character's known forms must stay live.
            static const std::array<std::string, 10> shapeshiftCommands = {
                "SHAPESHIFTBUTTON1", "SHAPESHIFTBUTTON2", "SHAPESHIFTBUTTON3", "SHAPESHIFTBUTTON4",
                "SHAPESHIFTBUTTON5", "SHAPESHIFTBUTTON6", "SHAPESHIFTBUTTON7", "SHAPESHIFTBUTTON8",
                "SHAPESHIFTBUTTON9", "SHAPESHIFTBUTTON10"};
            std::vector<game::ShapeshiftForm> forms;
            bool formsLoaded = false;
            for (size_t i = 0; i < shapeshiftCommands.size(); ++i) {
                if (!bindings.isCommandPressed(shapeshiftCommands[i])) continue;
                // One snapshot per frame with a press, even if several forms fire.
                if (!formsLoaded) {
                    forms = game::playerShapeshiftForms(gameHandler);
                    formsLoaded = true;
                }
                if (i < forms.size()) gameHandler.castSpell(forms[i].spellId);
            }

            static const std::array<std::string, 12> actionCommands = {
                "ACTIONBUTTON1", "ACTIONBUTTON2", "ACTIONBUTTON3", "ACTIONBUTTON4",
                "ACTIONBUTTON5", "ACTIONBUTTON6", "ACTIONBUTTON7", "ACTIONBUTTON8",
                "ACTIONBUTTON9", "ACTIONBUTTON10", "ACTIONBUTTON11", "ACTIONBUTTON12"};
            static const std::array<std::string, 12> bottomCommands = {
                "MULTIACTIONBAR1BUTTON1", "MULTIACTIONBAR1BUTTON2", "MULTIACTIONBAR1BUTTON3",
                "MULTIACTIONBAR1BUTTON4", "MULTIACTIONBAR1BUTTON5", "MULTIACTIONBAR1BUTTON6",
                "MULTIACTIONBAR1BUTTON7", "MULTIACTIONBAR1BUTTON8", "MULTIACTIONBAR1BUTTON9",
                "MULTIACTIONBAR1BUTTON10", "MULTIACTIONBAR1BUTTON11", "MULTIACTIONBAR1BUTTON12"};
            static_assert(actionCommands.size() == game::GameHandler::SLOTS_PER_BAR);
            static_assert(bottomCommands.size() == game::GameHandler::SLOTS_PER_BAR);
            lua_State* luaState = (services_.addonManager && services_.addonManager->getLuaEngine())
                ? services_.addonManager->getLuaEngine()->getState()
                : nullptr;
            const SDL_Keymod sdlMods = SDL_GetModState();
            const auto& bar = gameHandler.getActionBar();
            for (int i = 0; i < game::GameHandler::SLOTS_PER_BAR; ++i) {
                for (const bool bottom : {false, true}) {
                    const std::string& cmd = (bottom ? bottomCommands : actionCommands)[i];
                    if (!bindings.isCommandPressed(cmd)) continue;
                    const auto qMods = bindings.pressedCommandModifiers(cmd);
                    const bool shift = qMods ? qMods->shift : (io.KeyShift || (sdlMods & SDL_KMOD_SHIFT) != 0);
                    const bool ctrl  = qMods ? qMods->ctrl  : (io.KeyCtrl  || (sdlMods & SDL_KMOD_CTRL)  != 0);
                    const bool alt   = qMods ? qMods->alt   : (io.KeyAlt   || (sdlMods & SDL_KMOD_ALT)   != 0);
                    uint64_t namedTarget = 0;
                    if (addons::isModifiedClickActive(luaState, "SELFCAST", shift, ctrl, alt))
                        namedTarget = gameHandler.getPlayerGuid();
                    else if (addons::isModifiedClickActive(luaState, "FOCUSCAST", shift, ctrl, alt))
                        namedTarget = gameHandler.getFocusGuid();
                    const int page = bottom ? ActionBarPanel::kBottomLeftActionPage : actionBarPanel_.getMainActionBarPage();
                    int slot = ActionBarPanel::actionSlotForPage(page, i);
                    if (!bottom && page == 1) {
                        if (const uint32_t bonus = gameHandler.getBonusActionBarOffset(); bonus > 0)
                            slot = ActionBarPanel::actionSlotForPage(ActionBarPanel::kFrameXmlActionBarPages + static_cast<int>(bonus), i);
                    }
                    // Possessed unit bonus bar 5 (0-based slots 120..129, 1-based 121..130):
                    // route through sendPetAction just like lua_UseAction.
                    if (slot >= 120 && slot < 120 + game::GameHandler::PET_ACTION_BAR_SLOTS &&
                        gameHandler.getPossessedGuid() != 0) {
                        const uint32_t packed = gameHandler.getPetActionSlot(slot - 120);
                        if (packed != 0) {
                            const auto type = game::pet::petActionType(packed);
                            const uint32_t spell = (type == game::pet::ActionType::Command ||
                                                    type == game::pet::ActionType::Reaction)
                                ? 0 : game::pet::petActionId(packed);
                            if (spell != 0) {
                                gameHandler.getSpellName(spell);
                                const auto it = gameHandler.spellNameCacheRef().find(spell);
                                if (it != gameHandler.spellNameCacheRef().end() &&
                                    (it->second.targetFlags & 0x40u)) {
                                    gameHandler.beginPetGroundTarget(spell);
                                    continue;
                                }
                            }
                            const uint64_t target = namedTarget != 0
                                ? namedTarget
                                : (gameHandler.hasTarget() ? gameHandler.getTargetGuid() : 0);
                            gameHandler.sendPetAction(packed, target);
                        }
                        continue;
                    }
                    if (slot < 0 || slot >= static_cast<int>(bar.size())) continue;
                    if (bar[slot].type == game::ActionBarSlot::SPELL && bar[slot].isReady()) {
                        if (const uint32_t itemForSpell = gameHandler.getItemIdForSpell(bar[slot].id)) {
                            gameHandler.useItemById(
                                itemForSpell,
                                namedTarget != 0 ? namedTarget
                                                 : (gameHandler.hasTarget() ? gameHandler.getTargetGuid() : 0));
                        } else {
                            gameHandler.castSpell(bar[slot].id, namedTarget);
                        }
                    } else if (bar[slot].type == game::ActionBarSlot::ITEM && bar[slot].id != 0) {
                        gameHandler.useItemById(bar[slot].id, namedTarget);
                    } else if (bar[slot].type == game::ActionBarSlot::MACRO) {
                        chatPanel_.executeMacroText(gameHandler, gameHandler.getMacroText(bar[slot].id));
                    }
                }
            }
        }

    }

    // A link says it can be clicked.
    //
    // The chat window takes no clicks at all - it is click-through by design -
    // so nothing about hovering an item link in it looked any different from
    // hovering the text beside it, and the only way to find out a link was
    // there was to click and see.
    //
    // Tested against the rects the last draw filed, which is the same list the
    // click itself is resolved against, so the pointer cannot promise a link
    // that clicking would miss.
    if (services_.addonManager) {
        if (auto* engine = services_.addonManager->getLuaEngine()) {
            auto& widgets = engine->widgets();
            const glm::vec2 mouse = input.getMousePosition();
            float lx = mouse.x, ly = mouse.y;
            const float screenH = services_.window
                ? static_cast<float>(services_.window->getHeight()) : 0.0f;
            ui::mouseToTreeSpace(lx, ly, screenH, widgets.uiScale());
            if (widgets.linkAt(lx, ly)) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
    }

    worldHoverGuid_ = 0;
    worldObjectHoverGuid_ = 0;
    // Drawn again below for this frame's cursor while a ground spell is armed.
    if (services_.renderer) services_.renderer->clearGroundTargetReticle();
    uint64_t highlightedObject = 0;
    // Hover and press light an object alike (m2.vert.glsl: one more unit of light gain).
    const float objectHoverStrength = 1.0f;
    // Cursor affordance: show hand cursor over interactable entities.
    // Not while the cursor is over a frame FrameXML owns: ImGui has never heard
    // of those, so its own answer is no wherever they are.
    if (!io.WantCaptureMouse && !frameXmlOwnsMouse()) {
        auto* renderer = services_.renderer;
        auto* camera = renderer ? renderer->getCamera() : nullptr;
        auto* window = services_.window;
        if (camera && window) {
            glm::vec2 mousePos = input.getMousePosition();
            float screenW = static_cast<float>(window->getWidth());
            float screenH = static_cast<float>(window->getHeight());
            rendering::Ray ray = camera->screenToWorldRay(mousePos.x, mousePos.y, screenW, screenH);
            // The same picker the click uses, so the cursor affordance cannot
            // disagree with what clicking would actually select - including the
            // tighter sphere critters get, which this copy did not have.
            const bool awaitingUnitTarget = gameHandler.isAwaitingUnitTarget();
            const ui::ScenePick hoverPick = ui::pickScene(
                gameHandler, ray,
                ui::ScenePickParams{.includeLocalPlayer = ui::shouldPickLocalPlayer(awaitingUnitTarget)});
            const auto hovered = gameHandler.getEntityManager().getEntity(hoverPick.resolve());
            if (hovered && hovered->isUnit()) worldHoverGuid_ = hovered->getGuid();
            // 2.4.3 lights the object nearest under the pointer
            // (ScenePick::highlightObjectGuid), whether a click uses it or
            // not: a chest, a herb, a mailbox, a wanted poster and a campfire
            // came out 2x in Elwynn and Hellfire; Workshop Door 90566 and a
            // Practice Lockbox brighten alike, greyed or with Pick Lock armed
            // (pair 22). Not when a unit is nearer. How much is the M2
            // light's gain (m2.vert.glsl).
            if (hoverPick.highlightObjectGuid != 0 && hoverPick.highlightObjectT <= hoverPick.closestT)
                highlightedObject = hoverPick.highlightObjectGuid;
            if (const auto armed = gameHandler.pendingGroundTarget()) {
                // A spell cast at a place: Spell-Shadow-Acceptable at the
                // spell's radius on the floor under the cursor and the "Cast"
                // cursor within reach; the red Spell-Shadow-Unacceptable and
                // "UnableCast" beyond it; "UnableCast" alone over the sky.
                // Native 2.4.3, Blizzard (8 yd, 30 yd range) from the
                // Shimmering Flats.
                const auto floor = groundUnderRay(*renderer, ray);
                bool reachable = false;
                if (floor) {
                    reachable = gameHandler.groundDestinationInRange(
                        *armed, core::coords::renderToCanonical(*floor));
                    if (const float radius = gameHandler.groundTargetRadius(armed->spellId); radius > 0.0f)
                        renderer->setGroundTargetReticle(*floor, radius, reachable);
                }
                if (const auto texture = cursorTexture(services_.assetManager, services_.window,
                        reachable ? "Interface\\Cursor\\Cast.blp" : "Interface\\Cursor\\UnableCast.blp"))
                    selectCursorTexture(texture);
            } else if (gameHandler.pendingLockTargetSpell() != 0) {
                // A spell waiting for its lock (Pick Lock): "Cast" where
                // lockSpellAim allows a cast, whatever the player's rank;
                // "UnableCast" elsewhere.
                const uint64_t objectGuid = hoverPick.resolve() ? hoverPick.resolve() : hoverPick.namedObjectGuid;
                const bool aimed = lockSpellAim(gameHandler, objectGuid) != LockSpellAim::Invalid;
                if (const auto texture = cursorTexture(services_.assetManager, services_.window,
                        aimed ? "Interface\\Cursor\\Cast.blp" : "Interface\\Cursor\\UnableCast.blp"))
                    selectCursorTexture(texture);
            } else if (awaitingUnitTarget) {
                const uint64_t unitGuid = hoverPick.resolve();
                const uint32_t unitSpell = gameHandler.pendingUnitTargetSpell();
                bool aimed = false;
                if (unitGuid != 0) {
                    if (unitSpell != 0) {
                        aimed = gameHandler.canSpellTargetUnit(unitSpell, unitGuid) &&
                                gameHandler.unitSpellRange(unitSpell, unitGuid)
                                    .value_or(game::spellclass::SpellRangeCheck::InRange) ==
                                    game::spellclass::SpellRangeCheck::InRange;
                    } else {
                        aimed = unitGuid == gameHandler.getPlayerGuid() ||
                                (hovered && hovered->isUnit());
                    }
                }
                if (const auto texture = cursorTexture(services_.assetManager, services_.window,
                        aimed ? "Interface\\Cursor\\Cast.blp" : "Interface\\Cursor\\UnableCast.blp"))
                    selectCursorTexture(texture);
            } else if (hoverPick.closestGuid != 0) {
                if (!drawVendorCursor(gameHandler, hoverPick) &&
                    !drawWorldObjectCursor(gameHandler, hoverPick)) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                }
            }
        }
    }

    if (!input.isMouseButtonPressed(SDL_BUTTON_LEFT))
        core::Application::getInstance().setGameObjectHighlight(highlightedObject, objectHoverStrength);

    // World and FrameXML share the WoW gauntlet when no contextual cursor
    // was drawn. ImGui otherwise restores its desktop arrow each frame.
    // Without a window there is no pointer to dress: the headless GameScreen
    // in test_input_integration has neither window nor assets.
    //
    // Something held on FrameXML's cursor is the pointer instead, wherever it
    // is - over the world, a unit, a frame, an action button, a valid drop or
    // not: build 8606 draws the held icon, 32 pixels with its top left at the
    // hotspot, and Interface\Cursor\Item.blp (the gold corner) over it, and
    // no gauntlet or service cursor. Spells, items, macros, an action swapped
    // off a bar and money (its coin icon) all look the same way.
    if (services_.window) {
        if (std::string held = frameXmlCursorItem(); !held.empty()) {
            if (!held.ends_with(".blp") && !held.ends_with(".BLP")) held += ".blp";
            const auto corner = cursorTexture(services_.assetManager, services_.window,
                                              "Interface\\Cursor\\Item.blp");
            if (!corner) throw std::runtime_error("Missing WoW held-item cursor texture");
            selectCursorTexture(corner);
            heldIconTexture_ = cursorTexture(services_.assetManager, services_.window,
                                             held.c_str());
        } else if (std::string fxCursor = frameXmlCursorTexture();
                   !fxCursor.empty() && ImGui::GetMouseCursor() != ImGuiMouseCursor_None) {
            const auto texture = cursorTexture(services_.assetManager, services_.window,
                                               fxCursor.c_str());
            if (texture) {
                selectCursorTexture(texture);
            } else {
                const auto fallback = cursorTexture(services_.assetManager, services_.window,
                                                    "Interface\\Cursor\\Point.blp");
                if (!fallback) throw std::runtime_error("Missing default WoW cursor texture");
                selectCursorTexture(fallback);
            }
        } else if (ImGui::GetMouseCursor() != ImGuiMouseCursor_None) {
            const bool spellTargeting = gameHandler.isAwaitingUnitTarget() ||
                                        gameHandler.pendingLockTargetSpell() != 0 ||
                                        gameHandler.isAwaitingGroundTarget();
            bool frameUnitAimed = false;
            if (frameXmlOwnsMouse() && gameHandler.isAwaitingUnitTarget() && services_.addonManager) {
                if (auto* engine = services_.addonManager->getLuaEngine()) {
                    frameUnitAimed = engine->evaluateBoolean(
                        "(function() local f = GetMouseFocus() "
                        "for i = 1, 4 do if not f then break end "
                        "local u = (f.GetAttribute and f:GetAttribute('unit')) or f.unit "
                        "if type(u) == 'string' and u ~= '' then return SpellCanTargetUnit(u) end "
                        "f = f.GetParent and f:GetParent() end return false end)()");
                }
            }
            const char* art = spellTargeting
                ? (frameUnitAimed ? "Interface\\Cursor\\Cast.blp" : "Interface\\Cursor\\UnableCast.blp")
                : "Interface\\Cursor\\Point.blp";
            const auto texture = cursorTexture(services_.assetManager, services_.window, art);
            if (!texture) throw std::runtime_error("Missing default WoW cursor texture");
            selectCursorTexture(texture);
        }
    }

    // Left-click targeting: only on mouse-up if the mouse didn't drag (camera rotate)
    // Record press position on mouse-down
    if (!io.WantCaptureMouse && !frameXmlOwnsMouse() &&
        input.isMouseButtonJustPressed(SDL_BUTTON_LEFT) && !input.isMouseButtonPressed(SDL_BUTTON_RIGHT)) {
        leftClickPressPos_ = input.getMousePressPosition(SDL_BUTTON_LEFT);
        leftClickWasPress_ = true;

        // Light the object being pressed on.
        //
        // A game object is used rather than selected, so nothing else says the
        // press landed on it - a unit has its circle and its frame, and an
        // object had only the cursor, which does not change when the button
        // goes down. Lit on the press and dark again on the release, which is
        // where the real client puts it too.
        if (auto* camera = services_.renderer ? services_.renderer->getCamera() : nullptr) {
            if (auto* window = services_.window) {
                const rendering::Ray pressRay = camera->screenToWorldRay(
                    leftClickPressPos_.x, leftClickPressPos_.y,
                    static_cast<float>(window->getWidth()),
                    static_cast<float>(window->getHeight()));
                const uint64_t pressed =
                    ui::pickScene(gameHandler, pressRay, ui::ScenePickParams{}).resolve();
                auto entity = pressed ? gameHandler.getEntityManager().getEntity(pressed)
                                      : nullptr;
                core::Application::getInstance().setGameObjectHighlight(
                    entity && entity->getType() == game::ObjectType::GAMEOBJECT ? pressed : 0, 1.0f);
            }
        }
    }

    // A touch screen has no right button, so interacting is a second tap on
    // the unit already targeted. It runs the right-click path below.
    bool tapInteract = false;

    // On mouse-up, check if it was a click (not a drag)
    if (input.isMouseButtonJustReleased(SDL_BUTTON_LEFT)) {
        // Restore the current hover strength after releasing pressed feedback.
        core::Application::getInstance().setGameObjectHighlight(highlightedObject, objectHoverStrength);
    }

    if (leftClickWasPress_ && input.isMouseButtonJustReleased(SDL_BUTTON_LEFT)) {
        leftClickWasPress_ = false;
        glm::vec2 releasePos = input.getMouseReleasePosition(SDL_BUTTON_LEFT);
        glm::vec2 dragDelta = releasePos - leftClickPressPos_;
        float dragDistSq = glm::dot(dragDelta, dragDelta);
        const float CLICK_THRESHOLD = clickDragThreshold();

        // A plate is screen UI in front of the world: a click on it targets
        // its unit, not what the ray behind it would hit.
        const uint64_t plateUnit =
            ui::plateUnitAt(plateHits_, leftClickPressPos_.x, leftClickPressPos_.y);
        if (dragDistSq < CLICK_THRESHOLD * CLICK_THRESHOLD && plateUnit != 0 &&
            !gameHandler.isAwaitingGroundTarget()) {
            if (const uint32_t unitSpell = gameHandler.pendingUnitTargetSpell(); unitSpell != 0) {
                if (gameHandler.canSpellTargetUnit(unitSpell, plateUnit))
                    gameHandler.completeItemUseOnUnit(plateUnit);
            } else if (gameHandler.isAwaitingUnitTarget()) {
                gameHandler.completeItemUseOnUnit(plateUnit);
            } else {
                gameHandler.setTarget(plateUnit);
            }
        } else if (dragDistSq < CLICK_THRESHOLD * CLICK_THRESHOLD) {
            auto* renderer = services_.renderer;
            auto* camera = renderer ? renderer->getCamera() : nullptr;
            auto* window = services_.window;

            if (camera && window) {
                float screenW = static_cast<float>(window->getWidth());
                float screenH = static_cast<float>(window->getHeight());

                rendering::Ray ray = camera->screenToWorldRay(leftClickPressPos_.x, leftClickPressPos_.y, screenW, screenH);

                // A ground spell takes the click as its destination: the
                // floor under the cursor, whatever unit stands in front of it.
                // Over the sky there is no place and it stays armed.
                if (gameHandler.isAwaitingGroundTarget()) {
                    if (const auto floor = groundUnderRay(*renderer, ray))
                        gameHandler.castGroundTarget(core::coords::renderToCanonical(*floor));
                    else
                        LOG_INFO("Ground target: nothing under the cursor");
                    return;
                }

                // Use the same authoritative position and forgiving click volume
                // as right-click for a hooked bobber. Its tiny, partly submerged
                // render bounds must not let a click fall through to a character.
                const uint64_t hookedBobber = gameHandler.getHookedFishingBobberGuid();
                if (hookedBobber != 0) {
                    auto bobber = gameHandler.getEntityManager().getEntity(hookedBobber);
                    if (bobber && bobber->getType() == game::ObjectType::GAMEOBJECT) {
                        glm::vec3 bobberCenter = core::coords::canonicalToRender(
                            glm::vec3(bobber->getX(), bobber->getY(), bobber->getZ()));
                        bobberCenter.z += 0.35f;
                        float hitT = 0.0f;
                        constexpr float kFishingBobberClickRadius = 4.0f;
                        if (raySphereIntersect(ray, bobberCenter, kFishingBobberClickRadius, hitT)) {
                            LOG_WARNING("Fishing bobber direct left click: guid=0x", std::hex,
                                        hookedBobber, std::dec, " hitDistance=", hitT);
                            gameHandler.interactWithGameObject(hookedBobber);
                            return;
                        }
                    }
                }

                const bool awaitingUnitTarget = gameHandler.isAwaitingUnitTarget();
                const ui::ScenePick pick = ui::pickScene(
                    gameHandler, ray, ui::ScenePickParams{
                        .trace = true,
                        .includeLocalPlayer = ui::shouldPickLocalPlayer(awaitingUnitTarget),
                    });
                uint64_t closestGuid = pick.resolve();

                // Dropping what the cursor is carrying onto another player
                // opens a trade with them, which is how a trade begins in WoW
                // and was reachable here only by typing /trade.
                // Either cursor again: the item being offered usually comes
                // from FrameXML's bags now, and asking only the bag window's
                // meant a drag onto a player did nothing.
                const bool carrying = inventoryScreen.isHoldingItem() ||
                                      !frameXmlCursorItem().empty();
                if (closestGuid != 0 && carrying) {
                    auto entity = gameHandler.getEntityManager().getEntity(closestGuid);
                    if (entity && entity->getType() == game::ObjectType::PLAYER) {
                        uint8_t srcBag = 0xFF, srcSlot = 0;
                        const bool haveSource =
                            inventoryScreen.isHoldingItem()
                                ? inventoryScreen.heldItemSource(srcBag, srcSlot)
                                : frameXmlCursorWireSlot(srcBag, srcSlot);
                        if (haveSource) {
                            pendingTradeItem_ = PendingTradeItem{.active = true, .bag = srcBag, .slot = srcSlot};
                            gameHandler.initiateTrade(closestGuid);
                            // Whichever cursor was carrying it.
                            if (inventoryScreen.isHoldingItem())
                                inventoryScreen.releaseHeldItem();
                            else
                                frameXmlPutCursorDown();
                            return;
                        }
                    }
                }

                // A spell waiting for its lock takes a click on a game object
                // and casts there; anywhere else the cursor stays armed.
                if (gameHandler.pendingLockTargetSpell() != 0) {
                    // Over anything the cursor shows UnableCast for, the
                    // click sends nothing and the spell stays armed: original
                    // 2.4.3 (pair 22) clicking the ground, or a Bound Fel Iron
                    // Chest beyond 5 yards. Below the slot's Skill it says
                    // "Skill not high enough" (SPELL_FAILED_LOW_CASTLEVEL),
                    // sends nothing and puts the spell down: Workshop Door
                    // 90566 at Lockpicking 1.
                    const uint64_t objectGuid = closestGuid ? closestGuid : pick.namedObjectGuid;
                    switch (lockSpellAim(gameHandler, objectGuid)) {
                        case LockSpellAim::Invalid: break;
                        case LockSpellAim::TooLow:
                            gameHandler.cancelItemTargeting();
                            gameHandler.errorGlobal("SPELL_FAILED_LOW_CASTLEVEL");
                            break;
                        case LockSpellAim::Ok: gameHandler.completeLockTargetOnGameObject(objectGuid); break;
                    }
                    return;
                }

                // A spell or item waiting for a unit takes this click instead of
                // selecting anyone or using scenery; over empty space or an
                // invalid target a waiting spell stays armed and preserves any
                // current selection.
                if (awaitingUnitTarget) {
                    if (closestGuid != 0) {
                        const uint32_t unitSpell = gameHandler.pendingUnitTargetSpell();
                        if (unitSpell == 0 || gameHandler.canSpellTargetUnit(unitSpell, closestGuid)) {
                            gameHandler.completeItemUseOnUnit(closestGuid);
                            frameXmlSetCursorTexture({});
                        } else {
                            gameHandler.errorGlobal("SPELL_FAILED_BAD_TARGETS");
                        }
                    }
                    return;
                }
                if (closestGuid != 0) {
#ifdef __ANDROID__
                    if (gameHandler.getTargetGuid() == closestGuid) {
                        auto tapped = gameHandler.getEntityManager().getEntity(closestGuid);
                        if (tapped && tapped->getType() == game::ObjectType::UNIT) {
                            tapInteract = true;
                            rightClickPressPos_ = leftClickPressPos_;
                        }
                    }
#endif
                    // A game object is used, not targeted.
                    //
                    // The client set every click's subject as the target, so
                    // clicking a door made the door the target: a selection
                    // circle on the floor around it, a name where a creature's
                    // belongs, and CMSG_SET_SELECTION sent for a guid that is
                    // not a unit. WoW targets units and players; a left click
                    // on an object is a use, which is what the hand cursor over
                    // one has been promising. The hooked fishing bobber was the
                    // one object this already did, as a special case.
                    auto picked = gameHandler.getEntityManager().getEntity(closestGuid);
                    if (picked && picked->getType() == game::ObjectType::GAMEOBJECT) {
                        auto* luaState = (services_.addonManager && services_.addonManager->getLuaEngine())
                            ? services_.addonManager->getLuaEngine()->getState()
                            : nullptr;
                        const bool autoLootMod = addons::isModifiedClickActive(
                            luaState, "AUTOLOOTTOGGLE", io.KeyShift, io.KeyCtrl, io.KeyAlt);
                        gameHandler.prepareLoot(closestGuid, autoLootMod);
                        gameHandler.interactWithGameObject(closestGuid);
                    } else {
                        gameHandler.setTarget(closestGuid);
                    }
                } else if (addons::storedCVarValue("deselectOnClick", "1") != "0") {
                    // Clicked empty space - deselect current target, unless the
                    // player asked otherwise. The panel calls this Sticky
                    // Targeting and ticks it to mean "do not", which is why the
                    // sense reads backwards here: self.invert on the checkbox
                    // turns the tick into a zero.
                    //
                    // Clearing was unconditional, so the setting moved and the
                    // target went anyway. The default is the clearing this
                    // client already did, so nobody's targeting changes by the
                    // switch beginning to work.
                    gameHandler.clearTarget();
                }
            }
        }
    }

    // Right-click: select NPC (if needed) then interact / loot / auto-attack.
    // Record the press position; the action only fires on release for a tap (below),
    // never for a right-drag camera rotate - otherwise turning the view toward a nearby
    // mob would auto-attack it without the player intending to engage.
    if (!io.WantCaptureMouse && !frameXmlOwnsMouse() &&
        input.isMouseButtonJustPressed(SDL_BUTTON_RIGHT) && !input.isMouseButtonPressed(SDL_BUTTON_LEFT)) {
        rightClickPressPos_ = input.getMousePressPosition(SDL_BUTTON_RIGHT);
        rightClickWasPress_ = true;
    }

    // On right mouse-up, act only if it was a click (not a drag / camera rotate).
    if (tapInteract || (rightClickWasPress_ && input.isMouseButtonJustReleased(SDL_BUTTON_RIGHT))) {
        rightClickWasPress_ = false;
        const glm::vec2 rightClickReleasePos = input.getMouseReleasePosition(
            tapInteract ? SDL_BUTTON_LEFT : SDL_BUTTON_RIGHT);
        glm::vec2 rDragDelta = rightClickReleasePos - rightClickPressPos_;
        const float RCLICK_THRESHOLD = clickDragThreshold();
        if (glm::dot(rDragDelta, rDragDelta) >= RCLICK_THRESHOLD * RCLICK_THRESHOLD) {
            // Treated as a camera rotate - do not interact/attack.
            return;
        }
        // A right-click puts an armed ground or unit spell down and does
        // nothing else: native 2.4.3 fires UNIT_SPELLCAST_FAILED for it.
        if (gameHandler.isAwaitingGroundTarget()) {
            gameHandler.cancelGroundTarget(true);
            return;
        }
        if (gameHandler.isAwaitingUnitTarget()) {
            gameHandler.cancelUnitTargeting(true);
            frameXmlSetCursorTexture({});
            return;
        }
        // A right-click on a plate is a right-click on its unit: target it,
        // then interact or attack below. Without this the world ray missed
        // (the plate floats above the unit) and the click fell back to the
        // current target - a selected corpse got looted (ledger #94).
        const uint64_t plateUnit =
            ui::plateUnitAt(plateHits_, rightClickReleasePos.x, rightClickReleasePos.y);
        if (plateUnit != 0) gameHandler.setTarget(plateUnit);
        // Fishing bobbers are tiny and partly submerged, so their model bounds can
        // miss a cursor ray that visibly lands on the float. Test the authoritative
        // water position first with a forgiving sphere and reel directly, before
        // normal unit targeting can fall through to the player/self target.
        const uint64_t hookedBobber = plateUnit ? 0 : gameHandler.getHookedFishingBobberGuid();
        if (hookedBobber != 0) {
            auto bobber = gameHandler.getEntityManager().getEntity(hookedBobber);
            auto* renderer = services_.renderer;
            auto* camera = renderer ? renderer->getCamera() : nullptr;
            auto* window = services_.window;
            if (bobber && bobber->getType() == game::ObjectType::GAMEOBJECT && camera && window) {
                const glm::vec2 mousePos = rightClickReleasePos;
                const rendering::Ray ray = camera->screenToWorldRay(
                    mousePos.x, mousePos.y,
                    static_cast<float>(window->getWidth()),
                    static_cast<float>(window->getHeight()));
                glm::vec3 bobberCenter = core::coords::canonicalToRender(
                    glm::vec3(bobber->getX(), bobber->getY(), bobber->getZ()));
                bobberCenter.z += 0.35f;
                float hitT = 0.0f;
                constexpr float kFishingBobberClickRadius = 4.0f;
                if (raySphereIntersect(ray, bobberCenter, kFishingBobberClickRadius, hitT)) {
                    LOG_WARNING("Fishing bobber direct click: guid=0x", std::hex,
                                hookedBobber, std::dec, " hitDistance=", hitT);
                    gameHandler.interactWithGameObject(hookedBobber);
                    return;
                }
            }
        }

        // If a gameobject is already targeted, prioritize interacting with that target
        // instead of re-picking under cursor (which can hit nearby decorative GOs).
        // Exclude chair-type GOs (type 7): otherwise any right-click (including the
        // start of a camera rotate) auto-sits the player whenever a chair is targeted.
        if (plateUnit == 0 && gameHandler.hasTarget()) {
            auto target = gameHandler.getTarget();
            if (target && target->getType() == game::ObjectType::GAMEOBJECT) {
                auto go = std::static_pointer_cast<game::GameObject>(target);
                auto* goInfo = gameHandler.getCachedGameObjectInfo(go->getEntry());
                if (!goInfo || goInfo->type != 7) {
                    LOG_DEBUG("[GO-DIAG] Right-click: re-interacting with targeted GO 0x",
                                std::hex, target->getGuid(), std::dec);
                    gameHandler.setTarget(target->getGuid());
                    gameHandler.interactWithGameObject(target->getGuid());
                    return;
                }
            }
        }

        // The unit this click is on, if any. Only that unit is acted on below.
        uint64_t clickedUnit = plateUnit;

        // If no target or right-clicking in world, try to pick one under cursor
        if (plateUnit == 0) {
            auto* renderer = services_.renderer;
            auto* camera = renderer ? renderer->getCamera() : nullptr;
            auto* window = services_.window;
            if (camera && window) {
                // If a quest objective gameobject is under the cursor, prefer it over
                // hostile units so quest pickups (e.g. "Bundle of Wood") are reliable.
                std::unordered_set<uint32_t> questObjectiveGoEntries;
                {
                    const auto& ql = gameHandler.getQuestLog();
                    questObjectiveGoEntries.reserve(32);
                    for (const auto& q : ql) {
                        if (q.complete) continue;
                        for (const auto& obj : q.killObjectives) {
                            if (obj.npcOrGoId >= 0 || obj.required == 0) continue;
                            uint32_t entry = static_cast<uint32_t>(-obj.npcOrGoId);
                            uint32_t cur = 0;
                            auto it = q.killCounts.find(entry);
                            if (it != q.killCounts.end()) cur = it->second.first;
                            if (cur < obj.required) questObjectiveGoEntries.insert(entry);
                        }
                    }
                }

                glm::vec2 mousePos = rightClickReleasePos;
                float screenW = static_cast<float>(window->getWidth());
                float screenH = static_cast<float>(window->getHeight());
                rendering::Ray ray = camera->screenToWorldRay(mousePos.x, mousePos.y, screenW, screenH);
                // Drag rejection above separates camera rotation from interaction.
                ui::ScenePickParams params;
                params.unitHitRadius = 1.5f;
                params.unitHeightOffset = 1.5f;

                float closestQuestGoT = 1e30f;
                uint64_t closestQuestGoGuid = 0;
                float hookedBobberT = 1e30f;
                uint64_t hookedBobberGuid = 0;
                const ui::ScenePick pick = ui::pickScene(
                    gameHandler, ray, params,
                    [&](uint64_t guid, const std::shared_ptr<game::Entity>& entity,
                        float hitT, float /*centerT*/) {
                        if (entity->getType() != game::ObjectType::GAMEOBJECT) return;
                        if (guid == gameHandler.getHookedFishingBobberGuid() &&
                            hitT < hookedBobberT) {
                            hookedBobberT = hitT;
                            hookedBobberGuid = guid;
                        }
                        if (questObjectiveGoEntries.empty()) return;
                        auto go = std::static_pointer_cast<game::GameObject>(entity);
                        if (questObjectiveGoEntries.count(go->getEntry()) &&
                            hitT < closestQuestGoT) {
                            closestQuestGoT = hitT;
                            closestQuestGoGuid = guid;
                        }
                    });

                uint64_t closestGuid = 0;
                game::ObjectType closestType = game::ObjectType::OBJECT;

                // A hooked fishing bobber is time-sensitive and intentionally small:
                // if its click sphere was hit, reel it rather than selecting an
                // overlapping unit or decorative object.
                if (hookedBobberGuid != 0) {
                    closestGuid = hookedBobberGuid;
                    closestType = game::ObjectType::GAMEOBJECT;
                } else if (closestQuestGoGuid != 0) {
                    closestGuid = closestQuestGoGuid;
                    closestType = game::ObjectType::GAMEOBJECT;
                } else {
                    closestGuid = pick.resolve();
                    const auto picked = gameHandler.getEntityManager().getEntity(closestGuid);
                    closestType = (picked && picked->getType() == game::ObjectType::GAMEOBJECT)
                                      ? game::ObjectType::GAMEOBJECT
                                      : game::ObjectType::UNIT;
                }

                if (closestGuid != 0) {
                    if (closestType == game::ObjectType::GAMEOBJECT) {
                        LOG_DEBUG("[GO-DIAG] Right-click: raypick hit GO 0x",
                                    std::hex, closestGuid, std::dec);
                        // Object use must preserve the selected unit. Meeting
                        // stones summon that player through the server's selection.
                        auto* luaState = (services_.addonManager && services_.addonManager->getLuaEngine())
                            ? services_.addonManager->getLuaEngine()->getState()
                            : nullptr;
                        const bool autoLootMod = addons::isModifiedClickActive(
                            luaState, "AUTOLOOTTOGGLE", io.KeyShift, io.KeyCtrl, io.KeyAlt);
                        gameHandler.prepareLoot(closestGuid, autoLootMod);
                        gameHandler.interactWithGameObject(closestGuid);
                        return;
                    }
                    gameHandler.setTarget(closestGuid);
                    clickedUnit = closestGuid;
                }
            }
        }
        // A right-click on nothing does nothing. It used to fall back to the
        // current target, so a click that missed corpse B looted the selected
        // corpse A again, and one beside a selected enemy attacked it.
        // Original 2.4.3, right-click on bare ground: no packet with a corpse
        // or a living hostile selected. A touch tap is already on its target.
        if (!tapInteract &&
            (clickedUnit == 0 || gameHandler.getTargetGuid() != clickedUnit)) {
            return;
        }
        if (gameHandler.hasTarget()) {
            auto target = gameHandler.getTarget();
            if (target) {
                if (target->getType() == game::ObjectType::UNIT) {
                    // Check if unit is dead (health == 0) → loot, otherwise interact/attack
                    auto unit = std::static_pointer_cast<game::Unit>(target);
                    if (unit->getHealth() == 0 && unit->getMaxHealth() > 0) {
                        auto* luaState = (services_.addonManager && services_.addonManager->getLuaEngine())
                            ? services_.addonManager->getLuaEngine()->getState()
                            : nullptr;
                        const bool autoLootMod = addons::isModifiedClickActive(
                            luaState, "AUTOLOOTTOGGLE", io.KeyShift, io.KeyCtrl, io.KeyAlt);
                        gameHandler.prepareLoot(target->getGuid(), autoLootMod);
                        gameHandler.lootTarget(target->getGuid());
                    } else {
                        // Service flags describe interaction, not permission to attack.
                        // The service flags alone (CMaNGOS UNIT_NPC_FLAG_SPIRITHEALER
                        // 0x4000, SPIRITGUIDE 0x8000). Every spirit healer and
                        // battleground spirit guide carries one; the name match
                        // this also did only added Ancient Grimtotem Spirit
                        // Guide, a quest creature with no service at all.
                        auto isSpiritNpc = [&]() -> bool {
                            constexpr uint32_t NPC_FLAG_SPIRIT_HEALER = 0x00004000;
                            constexpr uint32_t NPC_FLAG_SPIRIT_GUIDE = 0x00008000;
                            return (unit->getNpcFlags() & (NPC_FLAG_SPIRIT_HEALER | NPC_FLAG_SPIRIT_GUIDE)) != 0;
                        };
                        bool allowSpiritInteract = (gameHandler.isPlayerDead() || gameHandler.isPlayerGhost()) && isSpiritNpc();
                        bool canInteractNpc = unit->isInteractable() || allowSpiritInteract;
                        if (!unit->isHostile() && canInteractNpc) {
                            gameHandler.interactWithNpc(target->getGuid());
                        } else if (gameHandler.canPlayerAttackUnit(*unit)) {
                            gameHandler.startAutoAttack(target->getGuid());
                        }
                    }
                } else if (target->getType() == game::ObjectType::GAMEOBJECT) {
                    // Skip chairs: auto-sit must only happen when the chair is
                    // under the cursor, never as a side effect of right-click.
                    auto go = std::static_pointer_cast<game::GameObject>(target);
                    auto* goInfo = gameHandler.getCachedGameObjectInfo(go->getEntry());
                    if (!goInfo || goInfo->type != 7) {
                        gameHandler.interactWithGameObject(target->getGuid());
                    }
                } else if (target->getType() == game::ObjectType::PLAYER) {
                    const auto unit = std::static_pointer_cast<game::Unit>(target);
                    if (gameHandler.canPlayerAttackUnit(*unit)) {
                        gameHandler.startAutoAttack(target->getGuid());
                    }
                }
            }
        }
    }
}


}} // namespace wowee::ui
