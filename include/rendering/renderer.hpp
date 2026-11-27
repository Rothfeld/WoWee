#pragma once
#include <optional>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <cstdint>
#include <vector>
#include <future>
#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include "rendering/pass_ablation.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/sky_system.hpp"
#include "rendering/unit_shadow.hpp"
#include "core/screen_recorder.hpp"
#include "pipeline/custom_zone_discovery.hpp"

#include "pipeline/grass_biomes.hpp"
#include "pipeline/grass_clearing.hpp"
#include "pipeline/grass_population.hpp"
#include "pipeline/grass_profile.hpp"
#include "ui/graphics_defaults.hpp"

namespace wowee {
namespace core { class Window; }
namespace rendering { class VkContext; }
namespace game { class World; class ZoneManager; class GameHandler; }
namespace audio { class AudioCoordinator; }
namespace pipeline { class AssetManager; }

namespace rendering {

class Camera;
class CameraController;
class TerrainRenderer;
class TerrainManager;
class PerformanceHUD;
class WaterRenderer;
class Skybox;
class Celestial;
class StarField;
class Clouds;
class LensFlare;
class Weather;
class SandStorm;
class Lightning;
class LightingManager;
class SwimEffects;
class MountDust;
class LevelUpEffect;
class ChargeEffect;
class CharacterRenderer;
class WMORenderer;
class M2Renderer;
class Minimap;
namespace world_map { class WorldMapFacade; }
using WorldMap = world_map::WorldMapFacade;
class QuestMarkerRenderer;
class FootprintRenderer;
class CameraShakeTable;
class CharacterPreview;
class AmdFsr3Runtime;
class SpellVisualSystem;
class PostProcessPipeline;
class AnimationController;
class LevelUpEffect;
class ChargeEffect;
class SwimEffects;
class RenderGraph;
class OverlaySystem;
class GrassRenderer;
class VolumetricFog;
class RtScene;
class RtLighting;
class LootSparkles;
class SunShafts;
class RendererBloom;
class ScreenCapture;

class Renderer {
public:
    Renderer();
    ~Renderer();

    bool initialize(core::Window* window);
    void shutdown();

    void beginFrame();
    void endFrame();

    /// A tick of the main loop in which no picture is drawn
    /// (core/draw_schedule.hpp, docs/draw-schedule.md). These two stand in
    /// for beginFrame and renderWorld on such a tick and do what those do
    /// besides drawing, at the same point of the tick: state that game code,
    /// the interface or a later drawn frame reads and that has to move once
    /// per tick, not once per picture. endFrame has no such part.
    ///
    /// passFrameUndrawn: finished GPU work is collected (frame slots,
    /// uploads, deferred destruction, recorded frames), the player's motion
    /// for grass and water is tracked, the grass population is advanced.
    /// passWorldUndrawn: which WMO rooms show (read by unit visibility,
    /// mouse targeting and nameplates through WMORenderer::roomHides) and
    /// the doodad draw distance's approach to its target.
    void passFrameUndrawn();
    void passWorldUndrawn();
    /// Whether the tick now running draws; set by the loop before update().
    void setTickDraws(bool draws) { tickDraws_ = draws; }
    [[nodiscard]] bool tickDraws() const { return tickDraws_; }

    /// Where the last endFrame spent its time, in milliseconds, for the frame
    /// budget report: recording the post-process and interface passes, the
    /// queue submit, and the present.
    /// presentUserCpu / presentSystemCpu: the main thread's processor time
    /// inside the present (negative where the platform cannot say); frameWait
    /// and acquire belong to the beginFrame of the same frame.
    struct EndFrameTimes {
        float postProcess = 0, interface = 0, submit = 0, present = 0;
        float presentUserCpu = -1, presentSystemCpu = -1;
        float frameWait = 0, acquire = 0;
        /// Presenting through shared memory only, else negative.
        float presentGpuWait = -1, presentCopy = -1, presentPut = -1;
    };
    [[nodiscard]] const EndFrameTimes& lastEndFrameTimes() const { return endFrameTimes_; }

    /// Recorded after the interface is drawn and before the frame is submitted,
    /// into the frame's own command buffer. A second window draws here - see
    /// AuxSwapchain - so the frame's one fence covers it. Empty clears it.
    void setAfterInterfaceRecorder(std::function<void(VkCommandBuffer)> recorder) {
        afterInterface_ = std::move(recorder);
    }

    void renderWorld(game::World* world, game::GameHandler* gameHandler = nullptr);

    /**
     * Update renderer (camera, etc.)
     */
    void update(float deltaTime);

    /**
     * Load test terrain for debugging
     * @param assetManager Asset manager to load terrain data
     * @param adtPath Path to ADT file (e.g., "World\\Maps\\Azeroth\\Azeroth_32_49.adt")
     */
    bool loadTestTerrain(pipeline::AssetManager* assetManager, const std::string& adtPath);

    /**
     * Initialize all sub-renderers (WMO, M2, Character, terrain, water, minimap, etc.)
     * without loading any ADT tile.  Used by WMO-only maps (dungeons/raids/BGs).
     */
    bool initializeRenderers(pipeline::AssetManager* assetManager, const std::string& mapName);
    /// The map every sub-renderer is looking at.
    ///
    /// initializeRenderers set this on all four when it built them, and it
    /// builds them once - so on every later map change the two the loader
    /// remembered by hand moved and the two it did not stayed on the map the
    /// session started in. The minimap was one of those: in a dungeon it went
    /// on asking for the tiles of wherever the player had logged in, and got
    /// none, which is a minimap with no dungeon on it.
    void setActiveMapName(const std::string& name);

    /**
     * Enable/disable terrain rendering
     */
    void setTerrainEnabled(bool enabled) { terrainEnabled = enabled; }

    /**
     * Enable/disable wireframe mode
     */
    void setWireframeMode(bool enabled);



    /**
     * Render performance HUD
     */
    void renderHUD();

    Camera* getCamera() { return camera.get(); }
    CameraController* getCameraController() { return cameraController.get(); }
    TerrainRenderer* getTerrainRenderer() const { return terrainRenderer.get(); }
    TerrainManager* getTerrainManager() const { return terrainManager.get(); }
    PerformanceHUD* getPerformanceHUD() { return performanceHUD.get(); }
    WaterRenderer* getWaterRenderer() const { return waterRenderer.get(); }
    Skybox* getSkybox() const { return skySystem ? skySystem->getSkybox() : nullptr; }
    Celestial* getCelestial() const { return skySystem ? skySystem->getCelestial() : nullptr; }
    StarField* getStarField() const { return skySystem ? skySystem->getStarField() : nullptr; }
    Clouds* getClouds() const { return skySystem ? skySystem->getClouds() : nullptr; }
    LensFlare* getLensFlare() const { return skySystem ? skySystem->getLensFlare() : nullptr; }
    Weather* getWeather() const { return weather.get(); }
    Lightning* getLightning() const { return lightning.get(); }
    CharacterRenderer* getCharacterRenderer() const { return characterRenderer.get(); }
    WMORenderer* getWMORenderer() const { return wmoRenderer.get(); }
    M2Renderer* getM2Renderer() const { return m2Renderer.get(); }
    Minimap* getMinimap() const { return minimap.get(); }
    WorldMap* getWorldMap() const { return worldMap.get(); }
    QuestMarkerRenderer* getQuestMarkerRenderer() const { return questMarkerRenderer.get(); }
    FootprintRenderer* getFootprintRenderer() const { return footprintRenderer.get(); }
    CameraShakeTable* getCameraShakeTable() const { return cameraShakeTable_.get(); }
    SkySystem* getSkySystem() const { return skySystem.get(); }
    const std::string& getCurrentZoneName() const;
    uint32_t getCurrentZoneId() const;
    uint32_t getCurrentAreaId() const;
    void invalidateAreaAnnouncement() { announcedZoneId_ = 0; announcedAreaId_ = 0; }
    const std::string& getInteriorZoneName() const { return interiorZoneName_; }
    const std::string& getInteriorAreaName() const { return interiorAreaName_; }
    /// The area under the player, asked of AreaTable rather than resolved to a
    /// zone first - the world PvP flag is on the subzone. See ZoneManager.
    bool isOnOutdoorPvpObjective() const;
    /// AreaTable flag 0x400 of the area the character stands in (IsFlyableArea).
    bool isInFlyableArea() const;
    bool isPlayerIndoors() const { return playerIndoors_; }
    /// IsIndoors() and [indoors]: an interior group without MOGP 0x8000.
    bool isPlayerIndoorRules() const { return playerIndoorRules_; }
    VkContext* getVkContext() const { return vkCtx; }
    VkDescriptorSetLayout getPerFrameSetLayout() const { return perFrameSetLayout; }
    /// What a per-frame set allocated elsewhere binds at binding 2: a fog
    /// volume of clear air, for a set whose block leaves the fog off.
    VkImageView getNeutralFogVolumeView() const;
    /// What a per-frame set allocated elsewhere binds at 3 and 4.
    VkImageView getNeutralRtLightingView() const;
    /// The geometry the ray traced lighting sees; renderers register with it.
    RtScene* getRtScene() const { return rtScene_.get(); }
    VkRenderPass getShadowRenderPass() const { return shadowRenderPass; }

    // Third-person character follow
    void setCharacterFollow(uint32_t instanceId);
    glm::vec3& getCharacterPosition() { return characterPosition; }
    uint32_t getCharacterInstanceId() const { return characterInstanceId; }
    float getCharacterYaw() const { return characterYaw; }
    void setCharacterYaw(float yawDeg) { characterYaw = yawDeg; }
    /// Smoothed lower-body yaw offset relative to characterYaw, in [-pi/2, +pi/2] rad.
    float getCharacterLegYawOffsetRad() const { return characterLegYawOffsetRad_; }
    /// Visual body yaw offset relative to logical characterYaw during standing turns, in [-60, +60] deg.
    float getCharacterTurnBodyLagDeg() const { return characterTurnBodyLagDeg_; }
    /// Lower-body shuffle direction (+1 ShuffleLeft, -1 ShuffleRight, 0 none) resolved for standing turns.
    int getCharacterTurnShuffleDirection() const { return characterTurnShuffleDir_; }
    /// While the player moves a possessed unit, characterPosition and
    /// characterYaw are that unit's; the player's own model stays here.
    void setPlayerBodyHeld(std::optional<glm::vec4> positionAndYawDeg) { playerBodyHeld_ = positionAndYawDeg; }

    // Screenshot capture - copies swapchain image to PNG file
    bool captureScreenshot(const std::string& outputPath);

    /// Start writing the screen, interface included, and what the client
    /// plays to a video file. False, with the reason in error, when it cannot.
    bool startRecording(const std::string& path, std::string& error);
    /// Finish the file and close it.
    core::ScreenRecorder::Stats stopRecording();
    [[nodiscard]] bool isRecording() const;
    /// Why a recording stopped by itself - the disk filled, the encoder gave
    /// up - once, for the interface to say; empty otherwise. The file is
    /// finished properly all the same.
    std::string takeRecordingFailure();

    // Spell visual effects (SMSG_PLAY_SPELL_VISUAL / SMSG_PLAY_SPELL_IMPACT)
    // Delegates to SpellVisualSystem (owned by Renderer)
    SpellVisualSystem* getSpellVisualSystem() const { return spellVisualSystem_.get(); }

    // Combat visual state (compound: resets AnimationController + SpellVisualSystem)
    void resetCombatVisualState();

    // Sub-system accessors (§4.2)
    AnimationController* getAnimationController() const { return animationController_.get(); }
    LevelUpEffect* getLevelUpEffect() const { return levelUpEffect.get(); }
    LootSparkles* getLootSparkles() const { return lootSparkles_.get(); }
    SwimEffects* getSwimEffects() const { return swimEffects.get(); }

    // Selection circle for targeted entity
    /// characterInstance: the target's CharacterRenderer instance, whose
    /// position at recording time centres the ring (0: pos is used).
    void setSelectionCircle(const glm::vec3& pos, float radius, const glm::vec3& color,
                            uint32_t characterInstance = 0);
    void clearSelectionCircle();
    /// The ground target reticle under the cursor (OverlaySystem), render coordinates.
    void setGroundTargetReticle(const glm::vec3& center, float radius, bool acceptable);
    void clearGroundTargetReticle();
    /// This frame's ground blob shadows (EntitySpawner::collectUnitShadows).
    void setUnitShadows(std::vector<UnitShadowBlob> blobs);
    void refreshSelectionCirclePosition();
    void renderUnitShadows(VkDescriptorSet perFrameSet, VkCommandBuffer cmd);

    // CPU timing stats (milliseconds, last frame).
    double getLastUpdateMs() const { return lastUpdateMs; }
    double getLastRenderMs() const { return lastRenderMs; }
    double getLastCameraUpdateMs() const { return lastCameraUpdateMs; }
    double getLastTerrainRenderMs() const { return lastTerrainRenderMs; }
    double getLastWMORenderMs() const { return lastWMORenderMs; }
    double getLastM2RenderMs() const { return lastM2RenderMs; }
    // Audio coordinator - owned by Application, set via setAudioCoordinator().
    void setAudioCoordinator(audio::AudioCoordinator* ac) { audioCoordinator_ = ac; }
    audio::AudioCoordinator* getAudioCoordinator() { return audioCoordinator_; }
    game::ZoneManager* getZoneManager() { return zoneManager.get(); }
    LightingManager* getLightingManager() { return lightingManager.get(); }

    const std::vector<pipeline::CustomZoneInfo>& getCustomZones() const { return customZones_; }

    /// Draw the minimap terrain disc inside an ImGui BackgroundDrawList callback
    /// (after WORLD-strata nameplates/labels/bubbles and before minimap markers
    /// and UIParent frames) so world labels/plates never bleed over the minimap.
    bool wasWorldDrawnThisFrame() const { return worldDrawnThisFrame_; }
    void deferMinimapOverlayToUi() { minimapOverlayDeferredToUi_ = true; }
    void renderMinimapOverlayInUiCallback(VkCommandBuffer cmd, game::GameHandler* gameHandler);

private:
    // Lays the M2 projected-texture quads onto the ground for this frame.
    void updateM2ProjectedTextures();
    std::function<void(VkCommandBuffer)> afterInterface_;

    // True when a 1x continuation pass with captured scene depth is available
    // (used for water refraction in WotLK+ and soft-depth weather mist across
    // all expansions).
    bool canRunContinuePass() const;

    // True when water is drawn in the scene continuation pass rather than in
    // the scene pass itself (see renderWorld).
    bool waterDrawsInContinuePass() const;

    /// Ghost tint, brightness and the minimap, in that order, at the end of
    /// the scene pass. The threaded and single-threaded paths both finish this
    /// way and differ only in which command buffer they are recording into.
    /// The underwater tint and its waterline. One implementation, called
    /// from both the parallel and the fallback recording paths, which had
    /// carried different ones.
    void renderUnderwaterOverlay(VkCommandBuffer cmd);
    void renderPostSceneOverlays(VkCommandBuffer cmd, game::GameHandler* gameHandler);
    void renderMinimapOverlay(VkCommandBuffer cmd, game::GameHandler* gameHandler);

    /// Point the swim spray at whichever pass the water ends up drawing in, so
    /// it can be recorded after the water rather than under it. Must run before
    /// the spray's pipelines are built, and again whenever they are rebuilt.
    void syncSwimEffectsTargetPass();
    /// syncSwimEffectsTargetPass, then rebuild the spray if its pass moved.
    /// Runs from beginFrame every frame, once the
    /// post-process target for the frame is settled; free when nothing moved.
    void refreshSwimEffectsPass();

    /// True when the spray's pipelines were built for the water continuation
    /// pass. Draw sites test this rather than waterDrawsInContinuePass() so a
    /// mid-run mode change cannot record a pipeline into an incompatible pass.
    bool swimEffectsDrawWithWater_ = false;
    /// True when WeatherMist pipelines (weather & sandstorm) were built for
    /// the 1x continuation pass so they sample captured scene depth for soft
    /// terrain feathering even when pre-WotLK water stays in the scene pass.
    bool mistDrawsInContinuePass_ = false;

    void runDeferredWorldInitStep(float deltaTime);

    core::Window* window = nullptr;
    std::unique_ptr<Camera> camera;
    std::unique_ptr<CameraController> cameraController;
    std::unique_ptr<TerrainRenderer> terrainRenderer;
    /// How much of the line from the eye to the sun is blocked, smoothed.
    ///
    /// Sampled once a frame in update() and eased, because the raw answer is a
    /// yes or a no and a hill edge crossing it would snap the flare on and off.
    float sunOcclusion_ = 0.0f;
    float sunOcclusionEaseSeconds_ = 0.0f;  // since the last sample (taken on drawn ticks)

    /// That line, asked of the terrain, the buildings and where the camera is.
    [[nodiscard]] float sampleSunOcclusion() const;

    std::unique_ptr<TerrainManager> terrainManager;
    std::unique_ptr<PerformanceHUD> performanceHUD;
    std::unique_ptr<WaterRenderer> waterRenderer;
    std::unique_ptr<Skybox> skybox;
    std::unique_ptr<Celestial> celestial;
    std::unique_ptr<StarField> starField;
    std::unique_ptr<Clouds> clouds;
    std::unique_ptr<LensFlare> lensFlare;
    std::unique_ptr<Weather> weather;
    std::unique_ptr<SandStorm> sandStorm;
    /// Server sandstorm grade while outdoors, else 0; set before update.
    float sandStormTarget_ = 0.0f;
    std::unique_ptr<Lightning> lightning;
    std::unique_ptr<LightingManager> lightingManager;
    std::unique_ptr<SkySystem> skySystem;  // Coordinator for sky rendering
    std::unique_ptr<SwimEffects> swimEffects;
    std::unique_ptr<MountDust> mountDust;
    std::unique_ptr<LevelUpEffect> levelUpEffect;
    std::unique_ptr<LootSparkles> lootSparkles_;
    std::unique_ptr<ChargeEffect> chargeEffect;
    std::unique_ptr<CharacterRenderer> characterRenderer;
    std::unique_ptr<WMORenderer> wmoRenderer;
    std::unique_ptr<M2Renderer> m2Renderer;
    std::unique_ptr<M2Renderer> skyboxModelRenderer_;
    /// The last zone a terrain chunk actually named. Many chunks carry an area
    /// id of zero, and a tile being loaded carries none at all, so the lookup
    /// answers "do not know" often - and answering from somewhere else instead
    /// made the zone flip at chunk boundaries while walking. Mutable because
    /// getCurrentZoneId() is const and this is a cache of what it last learnt.
    mutable uint32_t lastResolvedZoneId_ = 0;
    uint32_t announcedAreaId_ = 0;
    uint32_t announcedZoneId_ = 0;
    std::string interiorZoneName_;
    std::string interiorAreaName_;
    uint32_t interiorAreaId_ = 0;
    /// The map that answer belongs to, so it is dropped on a
    /// continent change rather than held across one.
    mutable uint32_t lastResolvedZoneMapId_ = 0xFFFFFFFFu;

    /// The original client's sky models that are up, one instance each, faded
    /// by the weight LightingManager gives them. See updateSkyboxLayers.
    struct SkyLayerInstance {
        std::string path;
        uint32_t instanceId = 0;
    };
    std::vector<SkyLayerInstance> skyLayers_;
    /// Sky models already uploaded, by path, so one that fades out and back in
    /// is not read off disk again.
    std::unordered_map<std::string, uint32_t> loadedSkyModels_;
    /// Sky paths that did not resolve to a usable model, so a failing path is
    /// not read off disk again on every frame it is wanted.
    std::unordered_set<std::string> failedSkyboxPaths_;
    std::unique_ptr<Minimap> minimap;
    std::unique_ptr<WorldMap> worldMap;
    std::unique_ptr<QuestMarkerRenderer> questMarkerRenderer;
    std::unique_ptr<FootprintRenderer> footprintRenderer;
    std::unique_ptr<CameraShakeTable> cameraShakeTable_;
    audio::AudioCoordinator* audioCoordinator_ = nullptr;  // Owned by Application
    std::unique_ptr<AnimationController> animationController_;  // §4.2
    std::unique_ptr<game::ZoneManager> zoneManager;
    // Shadow mapping (Vulkan)
    /// The shadow map is square and this is its side, chosen before the
    /// per-frame resources are built and not changed after.
    ///
    /// Deliberately not live. Those resources are created once at start-up and
    /// destroyed at shutdown - they are not part of the swapchain-resize path,
    /// so rebuilding them mid-session would be a new and unexercised one, and
    /// this renderer has lost a device to exactly that before. The setting is
    /// marked as needing a restart in the panel instead, which is what the
    /// original client does for the settings it cannot change live.
    uint32_t SHADOW_MAP_SIZE = 4096;
    void setShadowMapSize(uint32_t side) {
        // Powers of two between 512 and 4096: the quality slider has five
        // steps and these are they.
        SHADOW_MAP_SIZE = std::clamp(side, 512u, 4096u);
    }
    // Per-frame shadow resources: each in-flight frame has its own depth image and
    // framebuffer so that frame N's shadow read and frame N+1's shadow write don't
    // race on the same image across concurrent GPU submissions.
    // Array size must match MAX_FRAMES (= 2, defined in the private section below).
    VkImage shadowDepthImage[2] = {};
    VmaAllocation shadowDepthAlloc[2] = {};
    VkImageView shadowDepthView[2] = {};
    VkSampler shadowSampler = VK_NULL_HANDLE;
    VkRenderPass shadowRenderPass = VK_NULL_HANDLE;
    VkFramebuffer shadowFramebuffer[2] = {};
    VkImageLayout shadowDepthLayout_[2] = {};
    glm::mat4 lightSpaceMatrix = glm::mat4(1.0f);
    glm::vec3 shadowCenter = glm::vec3(0.0f);
    bool shadowCenterInitialized = false;
    bool shadowsEnabled = true;
    float shadowDistance_ = 300.0f;  // Shadow frustum half-extent (default: 300 units)
    float viewDistance_ = 1200.0f;
    std::optional<float> groundDetailDistance_;  // unset: nobody has configured one
    bool sharpStars_ = true;
    float diagTerrainFurthest_ = -1.0f;
    float diagM2Furthest_ = -1.0f;


public:
    // Character preview registration (for off-screen composite pass)
    void registerPreview(CharacterPreview* preview);
    void unregisterPreview(CharacterPreview* preview);
    std::shared_ptr<CharacterRenderer> getOrCreatePreviewCharacterRenderer(
        pipeline::AssetManager* assetManager,
        VkRenderPass renderPass,
        VkSampleCountFlagBits samples);

    /// Off stops the scene sampling the shadow map; the shadow pass itself
    /// still runs whole, casters and all. See renderShadowPass.
    void setShadowsEnabled(bool enabled) { shadowsEnabled = enabled; }
    bool areShadowsEnabled() const { return shadowsEnabled; }

    /// Whether scene shaders should sample set 0 binding 1 (`uShadowMap`) this frame.
    /// False whenever shadows are disabled, skipped (`WOWEE_SKIP_SHADOWS=1` or pass
    /// ablation), before a valid light-space matrix exists, or if the slot's shadow
    /// depth image is not in `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`.
    [[nodiscard]] static constexpr bool shouldSampleShadowMap(
            bool shadowsOn, bool skipShadowPass, bool passAblated,
            bool hasLightSpaceMatrix, VkImageLayout shadowLayout) noexcept {
        return shadowsOn && !skipShadowPass && !passAblated &&
               hasLightSpaceMatrix &&
               shadowLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    void setShadowDistance(float dist) { shadowDistance_ = glm::clamp(dist, 40.0f, 500.0f); }
    float getShadowDistance() const { return shadowDistance_; }
    void setViewDistance(float distance);
    float getViewDistance() const { return viewDistance_; }
    /// Ground Clutter Radius in yards (M2Renderer::setGroundDetailDistance).
    /// Kept here as well: the settings are read at startup and the M2 renderer
    /// is created with the first world load, so a value handed straight to it
    /// had nobody to take it. Until the interface set it again after the
    /// loading screen the M2 renderer ran with its own default, no radius, and
    /// the terrain manager instantiated the clutter of every loaded tile.
    void setGroundDetailDistance(float yards);
    /// Draw the client's own point stars in place of the sky model's baked
    /// star layer, which is a 256x256 compressed texture stretched across the
    /// whole dome. See Renderer::setSharpStars.
    /// WOWEE_VIEW_DIAG=1: one line naming how far terrain and doodads each
    /// actually drew, so the two can be compared rather than reasoned about.
    void logViewDistanceDiag();
    void setSharpStars(bool enabled);
    bool areSharpStars() const { return sharpStars_; }
    /// Fog lit by the sun through the shadow map and by nearby torches: 0 is
    /// off, 1-3 the volume's resolution. Applied at the start of the next
    /// frame. See VolumetricFog.
    void setVolumetricFogQuality(int quality);
    /// Ray traced lighting: 0 off, 1 sun shadows, 2 + ambient occlusion,
    /// 3 + one-bounce diffuse. See RtLighting.
    void setRtLightingMode(int mode);
    /// A multiplier on how thick that air is; 1 is the default mist.
    void setVolumetricFogDensity(float density) { volumetricFogDensity_ = glm::clamp(density, 0.0f, 3.0f); }
    /// Rays streaming from the sun across the finished picture. See SunShafts.
    void setSunShaftsEnabled(bool enabled) { sunShaftsEnabled_ = enabled; }
    /// TBC 2.4.3 full-screen scene glow (`ffxGlow` / `LightParams.dbc` col 4).
    void setFullScreenGlowEnabled(bool enabled) { fullScreenGlowEnabled_ = enabled; }
    [[nodiscard]] bool isFullScreenGlowEnabled() const { return fullScreenGlowEnabled_; }
    int getTerrainLoadRadius() const;
    int getTerrainUnloadRadius() const { return getTerrainLoadRadius() + 3; }
    void setMsaaSamples(VkSampleCountFlagBits samples);

    // Post-process pipeline API - delegates to PostProcessPipeline (§4.3)
    PostProcessPipeline* getPostProcessPipeline() const;
    void setFSREnabled(bool enabled);
    void setFSR2Enabled(bool enabled);

    void setWaterRefractionEnabled(bool enabled);

private:
    void applyMsaaChange();
    bool updateSkyboxLayers();
    uint32_t loadSkyboxModel(const std::string& path);
    VkSampleCountFlagBits pendingMsaaSamples_ = VK_SAMPLE_COUNT_1_BIT;
    bool msaaChangePending_ = false;
    void renderShadowPass();
    glm::mat4 computeLightSpaceMatrix();

    std::vector<pipeline::CustomZoneInfo> customZones_;
    pipeline::AssetManager* cachedAssetManager = nullptr;

    // Spell visual effects - owned SpellVisualSystem (extracted from Renderer §4.4)
    std::unique_ptr<SpellVisualSystem> spellVisualSystem_;

    // Post-process pipeline - owns all FSR/FXAA/FSR2 state (extracted §4.3)
    std::unique_ptr<PostProcessPipeline> postProcessPipeline_;

    bool playerIndoors_ = false;  // Cached WMO inside state (interior minimap scale)
    bool playerIndoorRules_ = false;  // IsIndoors() / [indoors]
    bool deferredWorldInitEnabled_ = true;
    bool deferredWorldInitPending_ = false;
    uint8_t deferredWorldInitStage_ = 0;
    float deferredWorldInitCooldown_ = 0.0f;

    // Third-person character state
    glm::vec3 characterPosition = glm::vec3(0.0f);
    uint32_t characterInstanceId = 0;
    float characterYaw = 0.0f;
    float characterLegYawOffsetRad_ = 0.0f;
    float characterTorsoYawOffsetRad_ = 0.0f;
    float characterTurnBodyLagDeg_ = 0.0f;
    bool characterTurnBodyAligning_ = false;
    int characterTurnShuffleDir_ = 0;
    float lastLogicalCharacterYaw_ = 0.0f;
    bool hasLastLogicalCharacterYaw_ = false;

    // Where the player was, for shaders that react to them moving through the
    // world. playerWakePos_ chases characterPosition with a fixed time
    // constant; the lag is what gives brushed-past foliage its springback.
    glm::vec3 playerWakePos_ = glm::vec3(0.0f);
    glm::vec3 prevPlayerPos_ = glm::vec3(0.0f);
    float playerSpeed_ = 0.0f;
    bool playerMotionTracked_ = false;



    // Selection circle + overlay rendering (owned by OverlaySystem)
    std::unique_ptr<OverlaySystem> overlaySystem_;
    uint32_t selectionInstance_ = 0;



    // Vulkan frame state
    VkContext* vkCtx = nullptr;
    VkCommandBuffer currentCmd = VK_NULL_HANDLE;
    uint32_t currentImageIndex = 0;

    // Per-frame UBO + descriptors (set 0)
    static constexpr uint32_t MAX_FRAMES = 2;
    VkDescriptorSetLayout perFrameSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool sceneDescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet perFrameDescSets[MAX_FRAMES] = {};
    VkBuffer perFrameUBOs[MAX_FRAMES] = {};
    VmaAllocation perFrameUBOAllocs[MAX_FRAMES] = {};
    void* perFrameUBOMapped[MAX_FRAMES] = {};
    GPUPerFrameData currentFrameData{};
    float globalTime = 0.0f;

    // Per-frame reflection UBO (mirrors camera for planar reflections)
    VkBuffer reflPerFrameUBO = VK_NULL_HANDLE;
    VmaAllocation reflPerFrameUBOAlloc = VK_NULL_HANDLE;
    void* reflPerFrameUBOMapped = nullptr;
    VkDescriptorSet reflPerFrameDescSet[MAX_FRAMES] = {};

    bool createPerFrameResources();
    void destroyPerFrameResources();
    void updatePerFrameUBO();
    void setupWater1xPass();
    void renderReflectionPass();

    // ── Multithreaded secondary command buffer recording ──
    // Indices into secondaryCmds_ arrays
    static constexpr uint32_t SEC_SKY       = 0;  // sky (main thread)
    static constexpr uint32_t SEC_TERRAIN   = 1;  // terrain (worker 0)
    static constexpr uint32_t SEC_WMO       = 2;  // WMO (worker 1)
    static constexpr uint32_t SEC_SELECTION = 3;  // selection circle (main thread)
    static constexpr uint32_t SEC_CHARS     = 4;  // characters (worker 2)
    static constexpr uint32_t SEC_M2        = 5;  // M2 + particles + glow (worker 3)
    static constexpr uint32_t SEC_POST      = 6;  // water + weather + effects (worker 4)
    static constexpr uint32_t SEC_IMGUI     = 7;  // ImGui (main thread, non-FSR only)
    static constexpr uint32_t SEC_M2_OPAQUE = 8;  // same worker/pool as SEC_M2
    static constexpr uint32_t NUM_SECONDARIES = 9;
    static constexpr uint32_t NUM_WORKERS = 5;

    // Per-worker command pools (thread-safe: one pool per thread)
    VkCommandPool workerCmdPools_[NUM_WORKERS] = {};
    // Main-thread command pool for its secondary buffers
    VkCommandPool mainSecondaryCmdPool_ = VK_NULL_HANDLE;
    // Pre-allocated secondary command buffers [secondaryIndex][frameInFlight]
    VkCommandBuffer secondaryCmds_[NUM_SECONDARIES][MAX_FRAMES] = {};

    bool parallelRecordingEnabled_ = false;  // set true after pools/buffers created
    // WOWEE_PASS_ABLATION: switches one world pass off at a time and reports
    // what the frame did without it. See pass_ablation.hpp for why the GPU's
    // own timestamps cannot answer that on this platform.
    std::unique_ptr<PassAblation> passAblation_;
    bool passAblationReported_ = false;
    bool worldDrawnLastFrame_ = false;
    std::chrono::steady_clock::time_point lastFrameStart_{};
    float lastDeltaTime_ = 0.0f;           // the tick's delta, as update() got it
    // Seconds since the last drawn frame ended: what the draw path's own
    // easing and the post-process pass step by. Equal to lastDeltaTime_ when
    // every tick is drawn.
    float sinceDrawSeconds_ = 0.0f;
    bool tickDraws_ = true;
    /// Player motion for water ripples and grass, stepped once per tick.
    void trackPlayerMotion(float deltaTime);
    /// Sealed interiors and the rooms each WMO shows from the camera; what
    /// renderWorld decides before it records anything. Returns whether the
    /// world outside a sealed interior is hidden.
    bool updateWorldVisibility();
    EndFrameTimes endFrameTimes_;
    bool createSecondaryCommandResources();
    void destroySecondaryCommandResources();
    VkCommandBuffer beginSecondary(uint32_t secondaryIndex);
    void setSecondaryViewportScissor(VkCommandBuffer cmd);

    // Cached render pass state for secondary buffer inheritance
    VkRenderPass activeRenderPass_ = VK_NULL_HANDLE;
    VkFramebuffer activeFramebuffer_ = VK_NULL_HANDLE;
    VkExtent2D activeRenderExtent_ = {.width = 0, .height = 0};

    // Active character previews for off-screen rendering
    std::vector<CharacterPreview*> activePreviews_;
    std::shared_ptr<CharacterRenderer> previewCharacterRenderer_;
    VkSampleCountFlagBits previewCharacterSamples_ = VK_SAMPLE_COUNT_1_BIT;

    bool terrainEnabled = true;
    bool terrainLoaded = false;


    // Render Graph - declarative pass ordering with automatic barriers
    std::unique_ptr<RenderGraph> renderGraph_;
    void buildFrameGraph(game::GameHandler* gameHandler);

    // Volumetric fog: a froxel volume built after the shadow pass and read by
    // every world shader through set 0 binding 2.
    std::unique_ptr<VolumetricFog> volumetricFog_;
    // Ray traced lighting. Created with the per-frame resources and destroyed
    // after every renderer that registers geometry with the scene.
    std::unique_ptr<RtScene> rtScene_;
    std::unique_ptr<RtLighting> rtLighting_;
    void writeRtLightingBindings();
    VkExtent2D sceneRenderExtent() const;
    void recordRtLighting(VkImage sceneDepth, VkExtent2D sceneExtent, bool depthIsMsaa);
    bool rtRecordedThisFrame_ = false;
    float volumetricFogDensity_ = 1.0f;
    /// Whether this frame builds the volume, decided where the per-frame block
    /// is written so the block's switch and the dispatch cannot disagree.
    bool volumetricThisFrame_ = false;
    /// The ground the mist lies on, chased toward the ground under the player.
    float fogLayerBase_ = 0.0f;
    bool fogLayerBaseValid_ = false;
    /// The extinction the volume is built with, chased toward what the zone,
    /// the weather and the hour ask for so walking indoors does not switch it.
    float fogExtinction_ = -1.0f;
    void renderVolumetricFog();
    void writeFogVolumeBindings();
    float volumetricFogExtinction() const;

    // Screen recording: the GPU side reads frames back, the recorder encodes.
    std::unique_ptr<ScreenCapture> screenCapture_;
    std::unique_ptr<core::ScreenRecorder> recorder_;
    std::string recordingFailure_;
    /// The frame recorded with `frameSlot`, once the GPU has finished it.
    void collectRecordedFrame(uint32_t frameSlot);
    void recordScreenCapture();

    // Screen-space sun shafts, built from the finished frame at the end of
    // endFrame and added in the overlay pass ahead of the interface.
    std::unique_ptr<SunShafts> sunShafts_;
    bool sunShaftsEnabled_ = true;
    std::unique_ptr<RendererBloom> bloom_;
    bool fullScreenGlowEnabled_ = true;
    /// renderWorld ran this frame. The shafts are built from the world's
    /// picture, and a login screen or a loading screen is not one.
    bool worldDrawnThisFrame_ = false;
    bool minimapOverlayDeferredToUi_ = false;
    void recordBloom();
    void recordSunShafts();

    // GPU-driven grass: compute cull with atomic compaction feeding an
    // indirect draw, over a population generated from terrain suitability.
    std::unique_ptr<GrassRenderer> grassRenderer_;
    // Where the live population was generated for. Rebuilt when the player
    // leaves it; the generator's lattice is world-anchored, so a rebuild
    // reproduces every blade that is still in range rather than reshuffling.
    glm::vec3 grassWindowCenter_{0.0f};
    bool grassWindowValid_ = false;
    // The window being generated, a bounded number of lattice cells per
    // frame. Small windows still finish inside one; the ones the distance
    // slider allows take as many frames as they take, and upload when done.
    pipeline::GrassPopulationBuilder grassBuilder_;
    glm::vec3 grassBuildCenter_{0.0f};
    // Clearings around placed WMOs and props for the window being built,
    // gathered once per rebuild from the renderers that own the instances.
    pipeline::GrassClearingField grassClearing_;
    void updateGrassPopulation();
public:
    /** Grass density and height, as fractions of the generator's defaults.
     * Set from the settings panel; either changing rebuilds the field. */
    void setGrassScales(float density, float height);
    /** How far out grass draws, in yards. Density thins with distance past
     * the near field, so range costs blades logarithmically, not by area. */
    void setGrassDistance(float yards);
    /** Turn grass on or off. Off is the default: it is new, it costs
     * generation time on the main thread, and turning it off has to release
     * what it was holding rather than merely stop drawing. */
    void setGrassEnabled(bool enabled);
    /** Ground-clutter density scale (0.0..1.5 = 0%..150% UI slider).
     * Stored on Renderer so settings loaded at startup reach TerrainManager
     * before initial world tiles stream in, and forwarded live on change. */
    void setGroundClutterDensityScale(float scale);
    [[nodiscard]] float getGroundClutterDensityScale() const { return groundClutterDensityScale_; }
private:
    bool grassEnabled_ = false;
    float grassDensityScale_ = 1.0f;
    float grassHeightScale_ = 1.0f;
    float grassDistance_ = 150.0f;
    float groundClutterDensityScale_ = static_cast<float>(ui::kDefaultGroundClutter) / 100.0f;
    // (biome, effectId) -> index into the profile table, built as effects are
    // met. Grass profiles are a blend of five categories crossed with the
    // biome overrides, so distinct ones stay far fewer than the hundreds of
    // ground effects that map onto them.
    std::unordered_map<uint64_t, uint32_t> grassProfileIndex_;
    std::vector<pipeline::GrassProfile> grassProfiles_;
    uint32_t grassProfileFor(uint32_t effectId, uint32_t areaId);
    // The per-zone look table from assets/grass_biomes.json, loaded on first
    // use, and a memo of which biome each area resolved to - resolution walks
    // AreaTable parentage and runs per blade sample without it.
    pipeline::GrassBiomeSet grassBiomes_;
    bool grassBiomesLoaded_ = false;
    std::unordered_map<uint32_t, uint32_t> grassBiomeForArea_;
    // Session totals, reported once at shutdown. The log is bounded and the
    // per-rebuild lines rotate out of it whenever the player stands still for
    // a minute, so the tail is the only place a "check the log" can rely on.
    uint32_t grassRebuilds_ = 0;
    size_t grassLastCount_ = 0;
    double grassWorstGenerateMs_ = 0.0;

    // CPU timing stats (last frame/update).
    double lastUpdateMs = 0.0;
    double lastRenderMs = 0.0;
    double lastCameraUpdateMs = 0.0;
    double lastTerrainRenderMs = 0.0;
    bool lastFrameSealedInterior_ = false;
    bool lastFrameExteriorHidden_ = false;
    glm::vec3 sealedClearColor_{0.0f};
    double lastWMORenderMs = 0.0;
    double lastM2RenderMs = 0.0;
    std::optional<glm::vec4> playerBodyHeld_;
};

} // namespace rendering
} // namespace wowee
