#pragma once

#include <cassert>
#include <functional>

#include "rendering/vk_shader.hpp"
#include "rendering/animation/footstep_driver.hpp"
#include "rendering/animation/playable_animation.hpp"
#include "rendering/shadow_params.hpp"
#include "rendering/hand_grip.hpp"
#include "rendering/legacy_light_direction.hpp"

#include "pipeline/m2_loader.hpp"
#include "pipeline/blp_loader.hpp"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <memory>
#include <vector>
#include <optional>
#include <limits>
#include "pipeline/char_sections.hpp"
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <utility>
#include <future>
#include <deque>
#include <condition_variable>
#include <mutex>
#include <atomic>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

// Forward declarations
class FootprintRenderer;
class CameraShakeTable;
class CameraController;
class Camera;
class VkContext;
class VkTexture;
class M2Renderer;

// Enchant visual (glint, glow) attached to a weapon at one of the weapon model's
// own item-visual attachment points.
struct WeaponEffectAttachment {
    uint32_t effectModelId;
    uint32_t effectInstanceId;
    uint16_t boneIndex;        // weapon bone carrying the attachment
    glm::vec3 offset;          // attachment position on the weapon model
};

// Weapon attached to a character instance at a bone attachment point
struct WeaponAttachment {
    uint32_t weaponModelId;
    uint32_t weaponInstanceId;
    uint32_t attachmentId;     // 1=RightHand, 2=LeftHand
    uint16_t boneIndex;
    glm::vec3 offset;
    glm::mat4 localTransform{1.0f}; // orientation after the attachment point (shield, helm)
    // Weapons only: the unit sheath state (UNIT_FIELD_BYTES_2 byte 0) in which
    // the item sits at attachmentId; any other state puts it at
    // sheathAttachmentId, or hides it when that is UINT32_MAX. -1: not a
    // weapon, always at attachmentId (helm, shoulders).
    int8_t heldSheathState = -1;
    uint32_t sheathAttachmentId = UINT32_MAX;
    std::vector<WeaponEffectAttachment> effects;
};

/**
 * Character renderer for M2 models with skeletal animation
 *
 * Features:
 * - Skeletal animation with bone transformations
 * - Keyframe interpolation (linear position/scale, slerp rotation)
 * - Vertex skinning (GPU-accelerated via bone SSBO)
 * - Texture loading from BLP via AssetManager
 */
class CharacterRenderer {
public:
    /// Draw only units standing inside this box (a sealed interior, see
    /// WMORenderer::findSealedInterior); every unit when empty.
    void setVisibleRegion(std::optional<std::pair<glm::vec3, glm::vec3>> region) { visibleRegion_ = region; }
    /// How far units are drawn and animated: the scene's view distance, the
    /// same one terrain and WMOs stop at (Renderer::setViewDistance).
    ///
    /// 2.4.3 has no distance or size cut of its own for units: a unit is drawn
    /// as long as the server keeps it in range (cmangos Visibility.Distance:
    /// 100 yd on continents, 170 in instances, 533 in battlegrounds, 200/400
    /// for LARGE_AOI/GIGANTIC_AOI creatures) and it is inside the far clip,
    /// where it has faded into the fog. Its DistCull and SmallCull CVars do
    /// not touch units. Measured: _experiments/20261118T235900Z.unit_distance.
    void setViewDistance(float yards) {
        assert(yards > 0.0f);
        viewDistance_ = yards;
    }
    CharacterRenderer();
    ~CharacterRenderer();

    [[nodiscard]] bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout, pipeline::AssetManager* am,
                    VkRenderPass renderPassOverride = VK_NULL_HANDLE,
                    VkSampleCountFlagBits msaaSamples = VK_SAMPLE_COUNT_1_BIT);
    [[nodiscard]] bool isInitialized() const { return vkCtx_ != nullptr; }
    void shutdown();
    void clear();  // Remove all models/instances/textures but keep pipelines/pools
    /// Drive the particle and ribbon emitters of every drawn instance through
    /// the M2 renderer (rendering/character_emitters.hpp). Main thread, after
    /// this frame's update() and after M2Renderer::update has returned.
    void syncEmitters(M2Renderer& m2Renderer);

    void setAssetManager(pipeline::AssetManager* am) { assetManager = am; }

    bool loadModel(const pipeline::M2Model& model, uint32_t id);

    uint32_t createInstance(uint32_t modelId, const glm::vec3& position,
                           const glm::vec3& rotation = glm::vec3(0.0f),
                           float scale = 1.0f);

    // oneShotReturnAnim: animation to resume (looping) when a non-looping
    // animation finishes; 0 = Stand. Lets NPC one-shot emotes return to their
    // persistent work/state loop instead of idling. holdFinalPose lets a caller
    // own completion and select the next animation without an intermediate pose.
    void playAnimation(uint32_t instanceId, uint32_t animationId, bool loop = true,
                       uint32_t oneShotReturnAnim = 0, bool holdFinalPose = false);
    /// The one-shot just started plays within `maxMs`: an authored sequence
    /// longer than that runs faster to end in time, a shorter one keeps its
    /// own speed. Lasts until the next playAnimation.
    void fitOneShotDuration(uint32_t instanceId, float maxMs);
    /// Replace the sequence just started by one of the model's variations of
    /// the same animation, drawn by their M2 frequency weights.
    void pickWeightedVariation(uint32_t instanceId);
    /// A running one-shot that would resume `from` resumes `to` instead.
    void retargetOneShotReturn(uint32_t instanceId, uint32_t from, uint32_t to);
    /// True when the instance is currently playing a non-looping one-shot
    /// (full-body or upper-body) that has not been frozen on its final pose.
    [[nodiscard]] bool isPlayingOneShot(uint32_t instanceId, uint32_t* outAnimId = nullptr) const;
    /// Set the looping animation to resume when the currently playing one-shot ends.
    void setOneShotReturn(uint32_t instanceId, uint32_t returnAnim);
    /// Play other animations on each arm alone, until the next playAnimation.
    void setArmAnimations(uint32_t instanceId, uint32_t leftArmAnim, uint32_t rightArmAnim);
    // Loop an animation on SpineLow and its descendants; zero removes the layer.
    void setUpperBodyAnimation(uint32_t instanceId, uint32_t animationId, bool loop = true);
    /// The instance rides a mount. While it does, playAnimation shows the
    /// seat poses (rider_pose.hpp riderKeepsSeat) as Mount (91) and plays
    /// every other animation above the spine over it; Death and Dead still
    /// take the whole body.
    void setInstanceRiding(uint32_t instanceId, bool riding);
    /// Seat the rider on the mount's MountMain (attachment 0, which the
    /// mount's model must author): every update() after the bones are posed
    /// places and turns the rider by the mount's attachment
    /// (rider_pose.hpp riderSeatPose), overriding its position and rotation.
    /// 0 unseats. Removing the mount unseats its riders.
    void setInstanceSeat(uint32_t riderInstanceId, uint32_t mountInstanceId);
    [[nodiscard]] uint32_t instanceSeat(uint32_t riderInstanceId) const;

    void update(float deltaTime, const glm::vec3& cameraPos = glm::vec3(0.0f),
                uint32_t onlyInstance = 0);

    /** Pre-allocate GPU resources (bone SSBOs, descriptors) on main thread before parallel render. */
    void prepareRender(uint32_t frameIndex);
    enum class RenderPhase { All, Solid, Blended };
    std::vector<std::pair<float, uint32_t>> drawOrder(const Camera& camera) const;
    std::vector<std::pair<float, uint32_t>> drawOrderForInstance(const Camera& camera,
                                                                 uint32_t onlyInstance) const;
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera,
                RenderPhase phase = RenderPhase::All, uint32_t onlyInstance = 0);
    void recreatePipelines();
    /// The five main-pass pipelines, which initialize() and
    /// recreatePipelines() both need and each used to describe.
    void buildMainPassPipelines(VkDevice device, VkRenderPass mainPass,
                                VkSampleCountFlagBits samples,
                                wowee::rendering::VkShaderModule& charVert,
                                wowee::rendering::VkShaderModule& charFrag);
    [[nodiscard]] bool initializeShadow(VkRenderPass shadowRenderPass);
    void renderShadow(VkCommandBuffer cmd, const glm::mat4& lightSpaceMatrix,
                      const glm::vec3& shadowCenter = glm::vec3(0), float shadowRadius = 1e9f);

    void setInstancePosition(uint32_t instanceId, const glm::vec3& position);
    void setInstanceRotation(uint32_t instanceId, const glm::vec3& rotation);
    void setInstanceTorsoYaw(uint32_t instanceId, float deltaYawRad);
    void moveInstanceTo(uint32_t instanceId, const glm::vec3& destination, float durationSeconds,
                        bool updateAnimation = true);
    void startFadeIn(uint32_t instanceId, float durationSeconds, float targetOpacity = 1.0f);
    /// Fade the instance, its weapons and their effects out from their
    /// current opacity, then remove it. The caller forgets the instance now.
    void fadeOutAndRemove(uint32_t instanceId, float durationSeconds);
    void setLocomotionSpeed(uint32_t instanceId, float speed);
    /// Print a footprint for every authored footfall event ($FL0, $RR1, ...)
    /// each unit's current sequence passed since the last call, at that foot.
    void emitFootprints(FootprintRenderer& footprints, const glm::vec3& viewerPosition);
    /// Trigger CameraShakes.dbc shakes for CreatureModelData FootstepShakeSize
    /// (on per-foot footfall events), DeathThudShakeSize (on $DTH), and M2 $SHK
    /// events passed since the last call, attenuated by 3D distance to `cameraPosition`.
    void emitCameraShakes(const CameraShakeTable& shakes, CameraController& cameraController,
                          const glm::vec3& cameraPosition);
    /// The unit's CreatureDisplayInfo row and the CreatureModelData id it names.
    void setInstanceCreatureModel(uint32_t instanceId, uint32_t displayId, uint32_t creatureModelDataId);
    /// CreatureDisplayInfo id last given to setInstanceCreatureModel; 0 = none.
    [[nodiscard]] uint32_t getInstanceDisplayId(uint32_t instanceId) const;
    [[nodiscard]] float getInstanceScale(uint32_t instanceId) const;
    void setInstanceScale(uint32_t instanceId, float scale);
    /// Grow or shrink to scale over kScaleTransitionMs, linearly from the size
    /// shown now (a new target restarts from there). Native 2.4.3 eases an
    /// OBJECT_FIELD_SCALE_X change in over about 1.2 s, the same for growing
    /// and shrinking and for steps of 0.25 (Furious Roar 42496), 0.5 and 1.0
    /// (.modify scale) (_experiments/20261118T1540Z.zulaman_warbringer).
    void setInstanceTargetScale(uint32_t instanceId, float scale);
    static constexpr float kScaleTransitionMs = 1200.0f;
    bool isInstanceScaleTransitioning(uint32_t instanceId) const {
        return instances.at(instanceId).scaleTransitionMs >= 0.0f;
    }
    void setInstanceOpacity(uint32_t instanceId, float opacity);
    void setInstanceCameraOpacity(uint32_t instanceId, float opacity);
    /// SpellVisualKit character procedures of the unit's auras: CharProc 1
    /// multiplies the shaded model by a colour, CharProc 14 sets its opacity.
    /// Kept apart from display alpha, stealth and spawn fades, which write
    /// setInstanceOpacity. Carried to the unit's weapons and their effects.
    /// The colour changes at once; the opacity moves from the drawn value to
    /// the new one over kKitOpacityTransition seconds, eased in as the cube
    /// of elapsed/duration, both ways (original 2.4.3: Transparency 25%/75%,
    /// Steal Magic, Banish, Stealth, whatever the kit's CharParamOne-Three;
    /// _experiments/20261118T001500Z.pair6_kit_colour_timing).
    void setInstanceKitAppearance(uint32_t instanceId, const glm::vec3& color, float opacity,
                                  bool freezeAnimation = false);
    /// SpellVisualKit CharProc 13 ("Glowy"): all light on the model (ambient,
    /// sun, interior and local lights) is multiplied by color for seconds,
    /// from now, whatever happens to the aura or spell that played the kit;
    /// fog still applies. Native 2.4.3: Shadow Image Visual (45263, kit 7441,
    /// black, 1e11 s) keeps working after .unaura; Dire Wolf Visual (31332,
    /// 0xFF09FF) leaves red and blue as unlit-by-kit
    /// (_experiments/20261118T101500Z.shadow_image_charproc13). Demonic
    /// Alignment (37713, same kit) turns a unit on GM Island at night pitch
    /// black (body 1,3,2) under an ambient of 0x637d5b, so ambient is scaled
    /// too (_experiments/20261118T1330Z.leotheras_glaives).
    /// The latest kit replaces an earlier one. Carried to weapons and effects.
    void startInstanceKitLight(uint32_t instanceId, const glm::vec3& color, float seconds);
    /// Light of a model standing at a world position in a WMO interior, or
    /// nothing outdoors (WMORenderer::interiorObjectLight).
    using InteriorLightQuery = std::function<std::optional<glm::vec3>(const glm::vec3&)>;
    void setInteriorLightQuery(InteriorLightQuery query) { interiorLightQuery_ = std::move(query); }
    struct UnitFloorLight {
        bool valid = false;
        bool isInterior = false;
        bool terrainShadowed = false;
        float exteriorBlend = 0.0f;
        DirectionalLightRig indoorRig{};
    };
    using UnitFloorLightQuery = std::function<UnitFloorLight(const glm::vec3& pos, float sampleHeight)>;
    void setUnitFloorLightQuery(UnitFloorLightQuery query) { unitFloorLightQuery_ = std::move(query); }
    void setOutdoorUnitLight(const glm::vec3& unitAmbient, const glm::vec3& diffuseBase,
                             const glm::vec3& lightDir) {
        outdoorUnitAmbient_ = unitAmbient;
        outdoorDiffuseBase_ = diffuseBase;
        outdoorLightDir_ = lightDir;
    }
    uint32_t gatherAuthoredM2Lights(const glm::vec3& centerPos,
                                    glm::vec4* outPosRadius,
                                    glm::vec4* outColorIntensity,
                                    uint32_t maxLights) const;
    static constexpr float kKitOpacityTransition = 1.0f;
    /// Highlight the unit under the pointer and the targeted unit (0 for
    /// none), with what they carry. 2.4.3 lights both alike.
    void setHighlightedInstances(uint32_t hoveredId, uint32_t targetedId);
    [[nodiscard]] const pipeline::M2Model* getModelData(uint32_t modelId) const;
    [[nodiscard]] const pipeline::M2Model* getInstanceModelData(uint32_t instanceId) const;
    void setActiveGeosets(uint32_t instanceId, const std::unordered_set<uint16_t>& geosets);
    /// Opt an instance into the Skin Extra head-detail batch. Only a character
    /// whose type 8 slot has been filled from CharSections should ask for it.
    void setDrawSkinExtra(uint32_t instanceId, bool enabled);

    /// Counts the head-batch diagnostic lines so it stops after a few.
    int headBatchCanaryCount_ = 0;
    void setGroupTextureOverride(uint32_t instanceId, uint16_t geosetGroup, VkTexture* texture);
    void setTextureSlotOverride(uint32_t instanceId, uint16_t textureSlot, VkTexture* texture);
    void clearTextureSlotOverride(uint32_t instanceId, uint16_t textureSlot);
    void setInstanceVisible(uint32_t instanceId, bool visible);
    void setInstanceReplaced(uint32_t instanceId, bool replaced);
    /// The instance's own visibility (first-person, taxi) and camera opacity.
    bool isInstanceDrawnVisible(uint32_t instanceId, float& cameraOpacity) const;
    /// A unit in a WMO room that is not drawn this frame is not drawn either,
    /// with its weapons, effects and emitters (WMORenderer::roomHides).
    void setInstanceRoomHidden(uint32_t instanceId, bool hidden);
    [[nodiscard]] bool isInstanceRoomHidden(uint32_t instanceId) const;
    void removeInstance(uint32_t instanceId);
    bool getAnimationState(uint32_t instanceId, uint32_t& animationId, float& animationTimeMs, float& animationDurationMs) const;
    /// Footfall ($FSD) event times in ms for the sequence the instance is
    /// currently playing; nullptr if the model has none for that sequence.
    [[nodiscard]] const std::vector<uint32_t>* getFootstepEventTimes(uint32_t instanceId) const;
    [[nodiscard]] const std::vector<uint32_t>* getWingFlapEventTimes(uint32_t instanceId) const;
    [[nodiscard]] const std::vector<uint32_t>* getWingGlideEventTimes(uint32_t instanceId) const;
    [[nodiscard]] bool hasAnimation(uint32_t instanceId, uint32_t animationId) const;
    // In the corpse pose: Death or Dead was played and no revival (Stand) since.
    // Native 2.4.3 keeps a corpse lying through emotes and spell cast/kit
    // animations aimed at it (_experiments/20261118T1320Z.death_over_emote).
    [[nodiscard]] bool isInstanceDead(uint32_t instanceId) const;
    /// Whether the instance still exists; clear() on a map change removes every
    /// instance while EntitySpawner still holds their ids.
    [[nodiscard]] bool hasInstance(uint32_t instanceId) const { return instances.count(instanceId) != 0; }
    /// Ground covered per second at authored rate: |moveSpeed| of the
    /// animation's primary sequence times the instance scale. nullopt when the
    /// model has no such sequence or it authors no movement.
    [[nodiscard]] std::optional<float> locomotionStride(uint32_t instanceId, uint32_t animationId) const;
    /// Whether the instance's model authors M2 attachment `attachmentId`.
    [[nodiscard]] bool hasAttachmentPoint(uint32_t instanceId, uint32_t attachmentId) const;
    bool getAnimationSequences(uint32_t instanceId, std::vector<pipeline::M2Sequence>& out) const;
    bool getInstanceBounds(uint32_t instanceId, glm::vec3& outCenter, float& outRadius) const;
    /// Ray hit against the unit's drawn triangles at the current pose, as
    /// build 8606 picks units (docs/unit-size-rules.md).
    bool intersectInstance(uint32_t instanceId, const glm::vec3& origin,
                           const glm::vec3& direction, float& distance) const;

    /// How tall this character stands, from its feet to the top of its head,
    /// with its scale applied. The model origin is at the feet.
    bool getInstanceNamePosition(uint32_t instanceId, bool mounted, glm::vec3& outPosition);
    bool getInstanceHeight(uint32_t instanceId, float& outHeight) const;
    bool getInstanceCollisionDimensions(uint32_t instanceId, float& radius, float& height) const;

    /// Where one of the skeleton's named bones sits, in model Z with the
    /// instance's scale applied. The key bone ids are WoW's own: 4 is the
    /// lower spine, 6 the head, 7 the jaw.
    ///
    /// A bone is where a thing actually is. A fraction of the bounding box is
    /// where it usually is, which is not the same for a race whose hair is
    /// half the distance from its chin to the top of the box.
    bool getInstanceKeyBonePivotZ(uint32_t instanceId, int32_t keyBoneId, float& outZ) const;
    bool getInstanceFootZ(uint32_t instanceId, float& outFootZ) const;
    bool getInstancePosition(uint32_t instanceId, glm::vec3& outPos) const;
    /// The instance's model yaw (rotation about world Z), radians.
    bool getInstanceYaw(uint32_t instanceId, float& outYaw) const;
    bool getInstanceRotation(uint32_t instanceId, glm::vec3& outRotation) const;

    /** Debug: Log all available animations for an instance */
    void dumpAnimations(uint32_t instanceId) const;

    /** Attach a weapon model to a character instance at the given attachment point. */
    bool attachWeapon(uint32_t charInstanceId, uint32_t attachmentId,
                      const pipeline::M2Model& weaponModel, uint32_t weaponModelId,
                      const std::string& texturePath,
                      const glm::mat4& localTransform = glm::mat4(1.0f),
                      int8_t heldSheathState = -1,
                      uint32_t sheathAttachmentId = UINT32_MAX);
    /** A gesture's animation: `sheathe` false keeps the weapons drawn through
     *  it (NOSHEATHE emotes share animations with sheathing ones). */
    void setGestureWeaponPose(uint32_t instanceId, uint32_t animationId, bool sheathe);
    /** The animation playing on the body or upper body whose
     *  AnimationData.WeaponFlags has 0x04 or 0x10 (spell casts, emotes,
     *  sitting, swimming, mounted), 0 if none: the weapons show sheathed
     *  while it plays. */
    uint32_t weaponSheathingAnimation(uint32_t instanceId) const;
    /** Unit sheath state (0 unarmed, 1 melee, 2 ranged) placing its attached weapons. */
    void setSheathState(uint32_t instanceId, uint8_t sheathState);
    /** Unit stand state (UNIT_FIELD_BYTES_1 byte 0): a pose whose animation's
     *  AnimationData.WeaponFlags has 0x04 or 0x10 shows the weapons sheathed
     *  whatever the sheath state, as gestures do (gestureSheathesWeapons). */
    void setStandState(uint32_t instanceId, uint8_t standState);

    /** Detach every item held at the given attachment point (drops their enchant effects too). */
    void detachWeapon(uint32_t charInstanceId, uint32_t attachmentId);
    void detachWeapons(uint32_t charInstanceId,
                       const std::function<bool(const WeaponAttachment&)>& match);

    // Free a model's GPU buffers and map entry once no instance references it.
    // For per-instance model ids (weapons/effects/player composites, which get
    // a fresh id per attach or spawn) - without this every reload or despawn
    // leaked the model. Do NOT call for displayId-keyed NPC models; those are
    // cached across despawn/respawn on purpose. Buffers are destroyed via the
    // frame-fence deferral path; shared textures stay in the cache.
    void unloadModelIfUnused(uint32_t modelId);

    // Model id an instance renders with (0 if the instance doesn't exist).
    [[nodiscard]] uint32_t getInstanceModelId(uint32_t instanceId) const {
        auto it = instances.find(instanceId);
        return it != instances.end() ? it->second.modelId : 0;
    }

    /**
     * Attach an item visual effect to the weapon held at the given attachment
     * point in the given sheath state (an off-hand weapon and a bow share the
     * left hand). visualSlot is the ItemVisuals.dbc slot (0-4): the attachment
     * point on the weapon model the effect hangs from. Returns false, drawing
     * nothing, when the weapon model has no such attachment (the original
     * draws no Frost Oil flame on Phosphorescent Blade, which has none).
     */
    bool attachWeaponEffect(uint32_t charInstanceId, uint32_t attachmentId, int8_t heldSheathState,
                            uint32_t visualSlot, const pipeline::M2Model& effectModel,
                            uint32_t effectModelId);

    /** Remove all item visual effects from that weapon. */
    void detachWeaponEffects(uint32_t charInstanceId, uint32_t attachmentId, int8_t heldSheathState);

    /** Mark an instance as a scene backdrop: no culling, no character material heuristics. */
    void setInstanceSceneModel(uint32_t instanceId, bool isScene);

    /** Use authored creature/vehicle material semantics instead of character heuristics. */
    void setInstanceCreatureMaterialRules(uint32_t instanceId, bool enabled);


    /** Get the world-space transform of an attachment point on an instance. */
    bool getAttachmentTransform(uint32_t instanceId, uint32_t attachmentId, glm::mat4& outTransform);
    bool getBonePointTransform(uint32_t instanceId, uint16_t boneIndex,
                               const glm::vec3& localOffset, glm::mat4& outTransform) const;

    /// World-space [base, tip] blade span of one currently drawn melee weapon.
    struct DrawnWeaponSpan {
        uint32_t weaponInstanceId = 0;
        glm::vec3 baseWorld{0.0f};
        glm::vec3 tipWorld{0.0f};
    };
    /// Returns world-space blade spans for all visible, drawn melee weapons
    /// currently held by charInstanceId (main-hand and/or off-hand).
    std::vector<DrawnWeaponSpan> getDrawnMeleeWeaponSpans(uint32_t charInstanceId) const;

    [[nodiscard]] size_t getInstanceCount() const { return instances.size(); }

    // Normal mapping / POM settings
    void setNormalMappingEnabled(bool enabled) { normalMappingEnabled_ = enabled; }
    void setNormalMapStrength(float strength) { normalMapStrength_ = strength; }
    void setPOMEnabled(bool enabled) { pomEnabled_ = enabled; }
    void setPOMQuality(int quality) { pomQuality_ = quality; }

    // Fog/lighting/shadow are now in per-frame UBO - keep stubs for callers that haven't been updated
    void setFog(const glm::vec3&, float, float) {}
    void setLighting(const float[3], const float[3], const float[3]) {}
    void setShadowMap(VkTexture*, const glm::mat4&) {}
    void clearShadowMap() {}

    // Pre-decoded BLP cache: set before calling loadModel() to skip main-thread BLP decode
    void setPredecodedBLPCache(std::unordered_map<std::string, pipeline::BLPImage>* cache) { predecodedBLPCache_ = cache; }

private:
    std::optional<std::pair<glm::vec3, glm::vec3>> visibleRegion_;
    float viewDistance_ = 1200.0f;  // Renderer::setViewDistance overrides it
    std::unordered_map<std::string, pipeline::BLPImage>* predecodedBLPCache_ = nullptr;
    // GPU representation of M2 model
    struct M2ModelGPU {
        VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VmaAllocation vertexAlloc = VK_NULL_HANDLE;
        VkBuffer indexBuffer = VK_NULL_HANDLE;
        VmaAllocation indexAlloc = VK_NULL_HANDLE;
        uint32_t indexCount = 0;
        uint32_t vertexCount = 0;

        pipeline::M2Model data;  // Original model data
        HandGripPose handGrip;
        bool hasBillboardBones = false;  // any bone turns toward the camera
        std::vector<glm::mat4> bindPose;  // Inverse bind pose matrices

        // Tight bind-pose bounds from rendered vertices. M2 header
        // boundingBox/boundingRadius describe collision geometry on many
        // creatures and can cover little more than their feet.
        glm::vec3 visualBoundMin{0.0f};
        glm::vec3 visualBoundMax{0.0f};
        float visualBoundRadius = 0.0f;

        // Textures loaded from BLP (indexed by texture array position)
        std::vector<VkTexture*> textureIds;

        // Cached batch render order sorted by (priorityPlane, materialLayer).
        // Built once at load time - the sort only depends on the model's static
        // batch metadata, so doing it per-instance per-frame in render() was
        // pure overhead.
        std::vector<size_t> sortedBatchIndices;

        // Pre-classified at load time to avoid per-batch string ops in render loop
        bool isSkyBird = false;
    };

    // Character instance
    std::vector<uint32_t> highlightedInstances_;

    struct CharacterInstance {
        uint32_t id;
        uint32_t modelId;

        glm::vec3 position;
        glm::vec3 rotation;
        float scale;
        // setInstanceTargetScale: size being eased to, the size it started
        // from and the milliseconds since (scaleTransitionMs < 0: none).
        float targetScale = 0.0f;
        float scaleFrom = 0.0f;
        float scaleTransitionMs = -1.0f;
        bool visible = true;  // For first-person camera hiding
        // Drawn by another instance in its place (a morphed player's creature
        // model); kept updated but never drawn, its weapons and emitters neither.
        bool replaced = false;
        // Standing in a WMO room the camera does not see (setInstanceRoomHidden).
        bool roomHidden = false;
        float torsoYawOverrideRad = 0.0f;

        // Animation state
        uint32_t currentAnimationId = 0;
        uint32_t gestureSheathAnimation = 0;
        uint32_t gestureHoldAnimation = 0;
        // Playing animation whose WeaponFlags sheathe (0x04 / 0x10), 0 if none.
        uint32_t poseSheathAnimation = 0;
        uint32_t poseSheathFlags = 0;  // AnimationData WeaponFlags of poseSheathAnimation
        // An animation with WeaponFlags 0x20 has drawn the melee weapons; they
        // stay drawn for the life of the instance (animationSheathState).
        bool weaponsDrawnByAnimation = false;
        uint8_t sheathState = 1;
        // The unit's stand state puts its weapons away (sit, sleep, kneel,
        // chairs): the AnimationData.WeaponFlags of its pose, see setStandState.
        bool standStateSheathes = false;
        int currentSequenceIndex = -1;  // Index into M2Model::sequences
        int armSequenceIndex[2] = {-1, -1};  // Left, right arm on their own sequences; -1 follows the body
        bool armSequenceLoops[2] = {false, false};  // Idle arm loops; a reaching arm holds its last frame
        std::vector<int8_t> boneArm;  // Per bone: 0 left arm, 1 right arm, -1 neither; built by setArmAnimations
        int upperBodySequenceIndex = -1;
        float upperBodyAnimationTime = 0.0f;
        bool upperBodyAnimationLoop = true;
        float upperBodyOneShotMaxDurationMs = 0.0f;
        std::vector<bool> upperBodyBones;
        // The upper body's outgoing pose, faded out over the incoming
        // sequence's authored blend time when a rider's swing starts or ends
        // (-1 none; upperBodyBlendTime is the outgoing sequence's frozen time).
        int upperBodyBlendSequence = -1;
        float upperBodyBlendTime = 0.0f;
        float upperBodyBlendElapsed = 0.0f;
        float upperBodyBlendDuration = 0.0f;
        // Rides a mount (setInstanceRiding); a rider's one-shot above the
        // spine goes on to this held animation, 0 for none.
        bool riding = false;
        // Mount instance whose MountMain places this rider (setInstanceSeat), 0 none.
        uint32_t seatMountId = 0;
        uint32_t upperBodyReturnAnim = 0;
        int blendSequenceIndex = -1;
        float blendAnimationTime = 0.0f;
        bool blendAnimationLoop = true;
        float blendPlaybackRate = 1.0f;
        float blendElapsed = 0.0f;
        float blendDuration = 0.0f;
        float playbackRate = 1.0f;
        float oneShotMaxDurationMs = 0.0f; // fitOneShotDuration; 0 = authored speed
        // CreatureModelData row this instance is drawn from; it names the
        // footprint mask and size. 0 = not a unit model, no prints.
        uint32_t creatureModelDataId = 0;
        uint32_t displayId = 0;
        // Where the footfall event scan stopped: sequence index and time.
        int footfallSequence = -1;
        float footfallTime = 0.0f;
        int shakeSequence = -1;
        float shakeTime = 0.0f;
        float locomotionSpeed = -1.0f; // Negative: caller does not supply movement speed.
        float animationTime = 0.0f;
        float globalSequenceTime = 0.0f; // Separate timer for global sequences (accumulates without wrapping at sequence duration)
        bool animationLoop = true;
        uint32_t sequenceRepeatsRemaining = 0;
        uint32_t oneShotReturnAnim = 0; // Anim to resume when a one-shot ends (0 = Stand)
        bool holdFinalPose = false; // Caller owns the transition after completion
        // The current sequence stands in for an animation the model lacks with
        // playable lookup flag 1: it plays from its last frame to its first.
        bool playReversed = false;
        bool isDead = false;  // Prevents movement while in death state
        std::vector<glm::mat4> boneMatrices;  // Current bone transforms

        // Geoset visibility - which submesh IDs to render
        // Empty = render all (for non-character models)
        std::unordered_set<uint16_t> activeGeosets;
        /// Draw the Skin Extra (texture type 8) head-detail batch. True only
        /// where the instance has been set up to composite it - the player.
        bool drawSkinExtra = false;

        // Per-geoset-group texture overrides (group → VkTexture*)
        std::unordered_map<uint16_t, VkTexture*> groupTextureOverrides;

        // Per-texture-slot overrides (slot → VkTexture*)
        std::unordered_map<uint16_t, VkTexture*> textureSlotOverrides;

        // Weapon attachments (weapons parented to this instance's bones)
        std::vector<WeaponAttachment> weaponAttachments;

        // Opacity (for fade-in)
        float opacity = 1.0f;
        float cameraOpacity = 1.0f;
        glm::vec3 kitColor{1.0f};
        // CharProc 13 light colour and the seconds it has left (startInstanceKitLight).
        glm::vec3 kitLight{1.0f};
        float kitLightRemaining = 0.0f;
        // Floor light of a locally lit WMO interior under the instance, or
        // nothing outdoors (setInteriorLightQuery); re-asked as it moves.
        std::optional<glm::vec3> interiorLight;
        UnitFloorLight floorLight{};
        glm::vec3 interiorLightQueriedAt{std::numeric_limits<float>::max()};
        float interiorLightAge = 0.0f;
        // Drawn kit opacity, moving from kitOpacityFrom to kitOpacityTarget
        // over kKitOpacityTransition seconds (see setInstanceKitAppearance).
        float kitOpacity = 1.0f;
        float kitOpacityFrom = 1.0f;
        float kitOpacityTarget = 1.0f;
        float kitOpacityTime = 0.0f;
        // SpellVisualKit CharProc 11: holds the instance's animation clock and
        // current pose motionless until cleared.
        bool kitAnimationFrozen = false;
        float fadeInTime = 0.0f;          // elapsed fade time (seconds)
        float fadeInDuration = 0.0f;      // total fade duration (0 = no fade)
        float fadeInTargetOpacity = 1.0f; // destination opacity at end of fade-in
        // Fade-out ahead of removal (fadeOutAndRemove); 0 = not fading out.
        float fadeOutDuration = 0.0f;
        float fadeOutTime = 0.0f;
        float fadeOutStartOpacity = 1.0f;

        // Movement interpolation
        bool isMoving = false;
        glm::vec3 moveStart{0.0f};
        glm::vec3 moveEnd{0.0f};
        float moveDuration = 0.0f;   // seconds
        float moveElapsed = 0.0f;

        // Override model matrix (used for weapon instances positioned by parent bone)
        bool hasOverrideModelMatrix = false;
        glm::mat4 overrideModelMatrix{1.0f};

        // Enchant visual attached to a weapon: a model of nothing but additive
        // FX batches, drawn unlit.
        bool isEffectModel = false;

        // A scene rather than a character: the glue-screen backdrops. Two things
        // follow. Their origin can sit hundreds of units from their geometry, so
        // culling on it would drop them. And the material heuristics below exist to
        // rescue character textures - applied to a scene they erase it, because
        // Stormwind's walls are DXT5 with an unused alpha channel that the opaque
        // batches must ignore, exactly as the blend mode says.
        bool isSceneModel = false;

        // Creatures, mounts and vehicles share this renderer with player
        // characters, but their geosets and atlas alpha mean different things.
        bool creatureMaterialRules = false;


        // Bone update throttling for characters outside normal gameplay range.
        uint32_t boneUpdateCounter = 0;
        const M2ModelGPU* cachedModel = nullptr;  // Avoid per-frame hash lookups

        // Per-instance bone SSBO (double-buffered per frame)
        VkBuffer boneBuffer[2] = {};
        VmaAllocation boneAlloc[2] = {};
        void* boneMapped[2] = {};
        VkDescriptorSet boneSet[2] = {};
    };

    void setupModelBuffers(M2ModelGPU& gpuModel);
    void calculateBindPose(M2ModelGPU& gpuModel);
    void calculateBoneMatrices(CharacterInstance& instance);
    glm::mat4 getBoneTransform(const pipeline::M2Bone& bone, float animTime, float globalSeqTime,
                               int sequenceIndex, const std::vector<uint32_t>& globalSeqDurations,
                               int blendSequence = -1, float blendTime = 0.0f, float blendWeight = 0.0f);
    [[nodiscard]] glm::mat4 getModelMatrix(const CharacterInstance& instance) const;
    void destroyModelGPU(M2ModelGPU& gpuModel, bool defer = false);
    void destroyInstanceBones(CharacterInstance& inst, bool defer = false);

    // Attachment point lookup helper - shared by attachWeapon() and getAttachmentTransform()
    static bool weaponSheathed(const CharacterInstance& instance, const WeaponAttachment& weapon);
    bool findAttachmentBone(uint32_t modelId, uint32_t attachmentId,
                           uint16_t& outBoneIndex, glm::vec3& outOffset) const;

public:
    /**
     * Build a composited character skin texture by alpha-blending overlay
     * layers onto a base skin BLP. Returns the resulting VkTexture*.
     */
    /// layers[0] is the body skin; each later layer goes to the atlas region
    /// its CharSections section and column name (pipeline::SkinLayer).
    VkTexture* compositeTextures(const std::vector<pipeline::SkinLayer>& layers);

    /**
     * Build a composited character skin with explicit region-based equipment overlays.
     */
    VkTexture* compositeWithRegions(const std::string& basePath,
                                const std::vector<pipeline::SkinLayer>& baseLayers,
                                const std::vector<std::pair<int, std::string>>& regionLayers);

    /** Clear the composite texture cache (forces re-compositing on next call). */
    void clearCompositeCache();

    /** Load a BLP texture from MPQ and return VkTexture* (cached). */
    VkTexture* loadTexture(const std::string& path);
    [[nodiscard]] VkTexture* getTransparentTexture() const { return transparentTexture_.get(); }

    /** Replace a loaded model's texture at the given slot. */
    void setModelTexture(uint32_t modelId, uint32_t textureSlot, VkTexture* texture);



private:
    // Create 1×1 fallback textures used when real textures are missing or still loading.
    // Called during both init and clear to ensure valid descriptor bindings at all times.
    void createFallbackTextures(VkDevice device);

    VkContext* vkCtx_ = nullptr;
    VkRenderPass renderPassOverride_ = VK_NULL_HANDLE;
    VkSampleCountFlagBits msaaSamplesOverride_ = VK_SAMPLE_COUNT_1_BIT;
    pipeline::AssetManager* assetManager = nullptr;
    /// AnimationData.dbc ID -> Fallback and ID -> Flags, loaded on first use.
    std::unordered_map<uint32_t, uint32_t> animationFallback_;
    std::unordered_map<uint32_t, uint32_t> animationFlags_;
    std::unordered_map<uint32_t, uint32_t> animationWeaponFlags_;
    std::unordered_map<uint32_t, uint32_t> animationBodyFlags_;
    bool animationFallbackLoaded_ = false;
    void loadAnimationData();
    std::optional<uint32_t> animationFallback(uint32_t animationId);
    uint32_t animationFlags(uint32_t animationId);
    uint32_t animationBodyFlags(uint32_t animationId);
    /// The sequence animation and play mode shown for `animationId` on a
    /// model without it (rendering/animation/playable_animation.hpp).
    std::optional<PlayableStandIn> playableStandIn(const pipeline::M2Model& model, uint32_t animationId);

    // Vulkan pipelines (one per blend mode)
    VkPipeline opaquePipeline_ = VK_NULL_HANDLE;
    VkPipeline alphaTestPipeline_ = VK_NULL_HANDLE;
    VkPipeline alphaPipeline_ = VK_NULL_HANDLE;
    VkPipeline additivePipeline_ = VK_NULL_HANDLE;
    VkPipeline modPipeline_ = VK_NULL_HANDLE;    // M2Blend 5
    VkPipeline mod2xPipeline_ = VK_NULL_HANDLE;  // M2Blend 6
    // Whole-instance fades (ghost form, spawn fade-in): alpha blend with depth
    // write kept on, so the faded model still self-occludes instead of showing
    // backfaces and under-armor skin through the body.
    VkPipeline translucentPipeline_ = VK_NULL_HANDLE;
    VkPipeline cameraDepthPipeline_ = VK_NULL_HANDLE;
    VkPipeline cameraFadePipeline_ = VK_NULL_HANDLE;
    // A fading instance's blended batches (alpha, additive, mod, mod2x) test
    // LESS_OR_EQUAL against its own depth prepass: a material layer drawn on
    // the same triangles as the faded base composites over it, as in 2.4.3
    // (Warglaive of Azzinoth's alpha layer keeps the stealthed blade denser
    // than the body).
    VkPipeline fadeAlphaPipeline_ = VK_NULL_HANDLE;
    VkPipeline fadeAdditivePipeline_ = VK_NULL_HANDLE;
    VkPipeline fadeModPipeline_ = VK_NULL_HANDLE;
    VkPipeline fadeMod2xPipeline_ = VK_NULL_HANDLE;
    // Back-face culled twin of each pipeline above, for one-sided materials.
    std::unordered_map<VkPipeline, VkPipeline> culledPipelines_;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;

    // Descriptor set layouts
    VkDescriptorSetLayout perFrameLayout_ = VK_NULL_HANDLE;  // set 0 (owned by Renderer)
    VkDescriptorSetLayout materialSetLayout_ = VK_NULL_HANDLE;  // set 1
    VkDescriptorSetLayout boneSetLayout_ = VK_NULL_HANDLE;  // set 2

    // Descriptor pool
    VkDescriptorPool materialDescPools_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    struct MaterialDescriptorKey {
        VkImageView diffuse = VK_NULL_HANDLE;
        VkImageView normal = VK_NULL_HANDLE;
        VkSampler diffuseSampler = VK_NULL_HANDLE;
        VkSampler normalSampler = VK_NULL_HANDLE;
        VkImageView second = VK_NULL_HANDLE;
        VkSampler secondSampler = VK_NULL_HANDLE;
        bool operator==(const MaterialDescriptorKey&) const = default;
    };
    struct MaterialDescriptorKeyHash {
        size_t operator()(const MaterialDescriptorKey& key) const {
            const size_t a = std::hash<VkImageView>{}(key.diffuse);
            const size_t b = std::hash<VkImageView>{}(key.normal);
            const size_t c = std::hash<VkSampler>{}(key.diffuseSampler);
            const size_t d = std::hash<VkSampler>{}(key.normalSampler);
            const size_t e = std::hash<VkImageView>{}(key.second);
            const size_t f = std::hash<VkSampler>{}(key.secondSampler);
            return a ^ (b << 1) ^ (c << 2) ^ (d << 3) ^ (e << 4) ^ (f << 5);
        }
    };
    std::unordered_map<MaterialDescriptorKey, VkDescriptorSet, MaterialDescriptorKeyHash>
        materialDescriptorCache_[2];
    VkDescriptorPool boneDescPool_ = VK_NULL_HANDLE;
    std::shared_ptr<std::atomic<uint64_t>> boneDescPoolGeneration_ =
        std::make_shared<std::atomic<uint64_t>>(0);
    uint32_t lastMaterialPoolResetFrame_ = 0xFFFFFFFFu;

    // Material UBO ring buffer - pre-allocated per frame slot, sub-allocated each draw
    VkBuffer materialRingBuffer_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VmaAllocation materialRingAlloc_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    void* materialRingMapped_[2] = {nullptr, nullptr};
    uint32_t materialRingOffset_[2] = {0, 0};
    uint32_t materialUboAlignment_ = 256;  // minUniformBufferOffsetAlignment
    static constexpr uint32_t MATERIAL_RING_CAPACITY = 4096;

    // Texture cache
    struct TextureCacheEntry {
        std::unique_ptr<VkTexture> texture;
        std::unique_ptr<VkTexture> normalHeightMap;
        float heightMapVariance = 0.0f;
        size_t approxBytes = 0;
        uint64_t lastUse = 0;
        bool hasAlpha = false;
        bool normalMapPending = false;  // deferred normal map generation
    };
    std::unordered_map<std::string, TextureCacheEntry> textureCache;
    struct NormalMapInfo {
        VkTexture* normalMap = nullptr;
        float heightMapVariance = 0.0f;
    };
    std::unordered_map<VkTexture*, NormalMapInfo> normalMapByTexPtr_;
    struct TextureProperties {
        bool hasAlpha = false;
    };
    std::unordered_map<VkTexture*, TextureProperties> texturePropsByPtr_;
    std::unordered_map<std::string, VkTexture*> compositeCache_;  // key → texture for reuse
    std::unordered_set<std::string> failedTextureCache_;  // negative cache for budget exhaustion
    std::unordered_map<std::string, uint64_t> failedTextureRetryAt_;
    std::unordered_set<std::string> loggedTextureLoadFails_;  // dedup warning logs
    uint64_t textureLookupSerial_ = 0;
    size_t textureCacheBytes_ = 0;
    uint64_t textureCacheCounter_ = 0;
    size_t textureCacheBudgetBytes_ = 1024ull * 1024 * 1024;
    uint32_t textureBudgetRejectWarnings_ = 0;
    std::unique_ptr<VkTexture> whiteTexture_;
    std::unique_ptr<VkTexture> transparentTexture_;
    std::unique_ptr<VkTexture> flatNormalTexture_;

    std::unordered_map<uint32_t, M2ModelGPU> models;
    std::unordered_map<uint32_t, CharacterInstance> instances;
    // Emitter instance in the M2 renderer per character instance, and the
    // emitter-only models this renderer has loaded there.
    struct EmitterCompanion {
        uint32_t characterModelId = 0;
        uint32_t m2InstanceId = 0;
        uint32_t particleColorId = 0;
    };
    /// CreatureDisplayInfo ParticleColorID of a display (0: none), and the
    /// ParticleColor rows, read on first use.
    uint32_t displayParticleColorId(uint32_t displayId);
    std::unordered_map<uint32_t, uint32_t> displayParticleColor_;
    std::unordered_map<uint32_t, std::array<uint32_t, 9>> particleColorRows_;
    bool particleColorsLoaded_ = false;
    std::unordered_map<uint32_t, EmitterCompanion> emitterCompanions_;
    std::unordered_set<uint32_t> emitterModelsLoaded_;

    uint32_t nextInstanceId = 1;

    // Normal map generation (same algorithm as WMO renderer)
    std::unique_ptr<VkTexture> generateNormalHeightMap(
        const uint8_t* pixels, uint32_t width, uint32_t height, float& outVariance);

    // Background normal map generation - CPU work on thread pool, GPU upload on main thread
    struct NormalMapResult {
        std::string cacheKey;
        std::vector<uint8_t> pixels;  // RGBA normal map output
        uint32_t width, height;
        float variance;
    };
    // Completed results ready for GPU upload (populated by background threads)
    std::mutex normalMapResultsMutex_;
    std::condition_variable normalMapDoneCV_;  // signaled when pendingNormalMapCount_ reaches 0
    std::deque<NormalMapResult> completedNormalMaps_;
    std::atomic<int> pendingNormalMapCount_{0};  // in-flight background tasks

    // Pure CPU normal map generation (thread-safe, no GPU access)
    /// Start deriving a normal/height map for a texture already in the cache.
    /// Called for every surface this renderer draws, whether it came from a
    /// file or was composited in memory.
    bool queueNormalMapGeneration(const std::string& cacheKey,
                                  std::vector<uint8_t> pixels,
                                  uint32_t width, uint32_t height);

    static NormalMapResult generateNormalHeightMapCPU(
        std::string cacheKey, std::vector<uint8_t> pixels, uint32_t width, uint32_t height);
public:
    void processPendingNormalMaps(int budget = 4);
private:
    InteriorLightQuery interiorLightQuery_;
    UnitFloorLightQuery unitFloorLightQuery_;
    glm::vec3 outdoorUnitAmbient_{0.5f};
    glm::vec3 outdoorDiffuseBase_{1.0f};
    glm::vec3 outdoorLightDir_{0.3f, -0.7f, 0.6f};

    // Normal mapping / POM settings
    bool normalMappingEnabled_ = true;
    float normalMapStrength_ = 0.8f;
    bool pomEnabled_ = true;
    int pomQuality_ = 1;  // 0=Low(16), 1=Medium(32), 2=High(64)

    // Maximum bones supported
    static constexpr int MAX_BONES = 240;
    uint32_t numAnimThreads_ = 1;
    std::vector<std::future<void>> animFutures_;
    std::vector<std::reference_wrapper<CharacterInstance>> toUpdate_;  // reused across frames
    glm::vec3 boneCameraPos_{0.0f};  // what billboard bones turn toward this frame
    std::vector<uint32_t> fadedOut_;  // fade-out finished; removed at the next update

    // Shadow pipeline resources
    VkPipeline shadowPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout shadowPipelineLayout_ = VK_NULL_HANDLE;
    /// The set the shadow pass binds. Five separate members before,
    /// built and torn down here and in three other renderers.
    ShadowParamsSet shadowParams_;

    /// The texture of one entry of a batch's texture combo, with the
    /// instance's slot overrides applied.
    [[nodiscard]] VkTexture* resolveComboTexture(const CharacterInstance& inst,
                                                 const M2ModelGPU& gm,
                                                 const pipeline::M2Batch& b,
                                                 uint32_t comboIndex) const;
    /// Which texture a batch draws with. Shared by the main pass and the shadow
    /// pass, which needs it to cut a silhouette rather than a rectangle.
    [[nodiscard]] VkTexture* resolveBatchTexture(const CharacterInstance& inst,
                                   const M2ModelGPU& gm,
                                   const pipeline::M2Batch& b) const;

    /// Per-batch texture sets for alpha-keyed shadow casters, one pool per
    /// frame in flight and reset at the top of each frame's shadow pass. Same
    /// arrangement M2Renderer uses for its foliage shadows.
    static constexpr uint32_t kShadowTexPoolFrames = 2;
    VkDescriptorPool shadowTexPool_[kShadowTexPoolFrames] = {};
    std::unordered_map<VkImageView, VkDescriptorSet> shadowTexSetCache_;
    VkDescriptorSet shadowTexDescSet(VkTexture* tex, uint32_t frameIndex);
};

} // namespace rendering
} // namespace wowee
