#pragma once

#include "rendering/body_collision.hpp"
#include "rendering/collision_geometry.hpp"
#include "rendering/spatial_grid.hpp"
#include "rendering/shadow_params.hpp"
#include "rendering/legacy_light_direction.hpp"

#include "pipeline/m2_loader.hpp"
#include "pipeline/blp_loader.hpp"
#include "pipeline/grass_clearing.hpp"
#include "rendering/m2_model_classifier.hpp"
#include "rendering/m2_view_distance.hpp"
#include "rendering/projected_texture.hpp"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <array>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <deque>
#include <string>
#include <functional>
#include <optional>
#include <limits>
#include <random>
#include <chrono>
#include <future>
#include <algorithm>

namespace wowee {

namespace pipeline {
    class AssetManager;
}

namespace rendering {

class Camera;
class VkContext;
class VkTexture;
class RtScene;

// Ceiling on the bone matrices computed and uploaded for one M2 instance.
// WoW models run well past a hundred bones - the largest shipped model has
// 315 - and a vertex weighted to a bone above this ceiling would read outside
// its instance's range in the mega bone buffer, which is what threw stray
// spikes out of water elementals and other high-bone-count creatures.
inline constexpr uint32_t kMaxBonesPerInstance = 512;

/**
 * GPU representation of an M2 model
 */
struct M2ModelGPU {
    struct BatchGPU {
        VkTexture* texture = nullptr;  // from cache, NOT owned
        VkTexture* texture2 = nullptr;
        int16_t textureUnit2 = 0;
        uint16_t textureAnimIndex2 = 0xFFFF;
        // Texture stage modes from the model's combiner combos (global flag
        // 0x08) at the batch's shader id: 0 Opaque, 1 Mod, 2 Decal, 3 Add,
        // 4 Mod2x. Models without combos modulate both stages.
        int32_t combiner0 = 1;
        int32_t combiner1 = 1;
        VkDescriptorSet materialSet = VK_NULL_HANDLE;  // set 1
        ::VkBuffer materialUBO = VK_NULL_HANDLE;
        VmaAllocation materialUBOAlloc = VK_NULL_HANDLE;
        void* materialUBOMapped = nullptr;  // cached mapped pointer (avoids per-frame vmaGetAllocationInfo)
        uint32_t indexStart = 0;   // offset in indices (not bytes)
        uint32_t indexCount = 0;
        bool hasAlpha = false;
        /// The alpha is a real silhouette, not an atlas leftover.
        bool alphaIsSilhouette = false;
        glm::vec3 tint{1.0f};  ///< the batch's authored colour
        uint16_t textureAnimIndex = 0xFFFF; // 0xFFFF = no texture animation
        uint16_t blendMode = 0;   // 0=Opaque, 1=AlphaKey, 2=Alpha, 3=Add, etc.
        uint16_t materialFlags = 0; // M2 material flags (0x01=Unlit, 0x04=TwoSided, 0x10=NoDepthWrite)
        bool hasAuthoredMaterial = false; // missing flags must not be guessed as one-sided
        bool projected = false;   // skin batch flag 0x4: texture laid onto the ground under the quad
        uint16_t submeshLevel = 0; // LOD level: 0=base, 1=LOD1, 2=LOD2, 3=LOD3
        int16_t textureUnit = 0; // UV set, or -1 for environment mapping
        uint8_t texFlags = 0;     // M2Texture.flags (bit0=WrapS, bit1=WrapT)
        bool lanternGlowHint = false; // Texture/model hints this batch is a glow-card billboard
        bool glowCardLike = false; // Batch likely is a flat emissive card that should be sprite-replaced
        bool preserveGlowMesh = false; // Keep emissive glass/fixture mesh below its glow sprite
        // A sky model's star-point layer. Suppressed when the client draws its
        // own stars instead: the authored layer is a 256x256 compressed texture
        // magnified across the whole dome, which is soft at any resolution and
        // softer at a high one.
        bool starLayer = false;
        uint8_t glowTint = 0; // 0=warm, 1=cool, 2=red
        float batchOpacity = 1.0f; // Resolved texture weight opacity (0=transparent, skip batch)
        pipeline::M2AnimationTrack animatedColorRGB;
        pipeline::M2AnimationTrack animatedColorAlpha;
        pipeline::M2AnimationTrack animatedTextureWeight;
        glm::vec3 center = glm::vec3(0.0f); // Center of batch geometry (model space)
        // Submesh sort point and its bone; pass 2 draws a model's blended
        // batches far to near by it.
        uint16_t sortBone = 0;
        glm::vec3 sortCenter{0.0f};
        float glowSize = 1.0f;              // Approx radius of batch geometry

        struct LightBoneAnchor {
            uint16_t bone = 0;
            // Sum of (vertex position * skin weight, skin weight), divided by
            // sampled vertex count. Transforming and summing these reproduces
            // the animated center of the skinned glow geometry.
            glm::vec4 weightedPoint{0.0f};
        };
        std::vector<LightBoneAnchor> lightBoneAnchors;
        uint16_t lightSuspensionBone = UINT16_MAX;
        glm::vec3 lightSuspensionPoint{0.0f};
    };

    ::VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAlloc = VK_NULL_HANDLE;
    ::VkBuffer indexBuffer = VK_NULL_HANDLE;
    VmaAllocation indexAlloc = VK_NULL_HANDLE;
    uint32_t indexCount = 0;
    uint32_t vertexCount = 0;
    // Render geometry retained for cursor occlusion; collision hulls fill gaps.
    std::vector<pipeline::M2Vertex> pickVertices;
    std::vector<uint16_t> pickIndices;
    std::vector<BatchGPU> batches;

    glm::vec3 boundMin;
    glm::vec3 boundMax;
    float boundRadius = 0.0f;
    /// The M2 header's bounding (collision) box as authored; boundMin/boundMax
    /// above hold the tight render bounds instead. All zero when the model has
    /// no collision (Bush_Sansam.m2).
    glm::vec3 headerBoxMin{0.0f};
    glm::vec3 headerBoxMax{0.0f};
    /// What the render cull sphere encloses: the bind-pose vertex bounds,
    /// and for an animated model every sequence's authored bounds as well.
    glm::vec3 cullMin{0.0f};
    glm::vec3 cullMax{0.0f};
    bool isInvisibleTrap = false;   // Invisible trap objects (don't render, no collision)
    bool isGroundDetail = false;    // Ground clutter/detail doodads (special fallback render path)
    bool isBrazierOrFire = false;   // Brazier / campfire / bonfire model
    bool isGroundFire = false;      // Ground fire whose halo follows its lowest flame emitter
    bool isTorch = false;           // Wall-mounted or standing torch
    bool isForge = false;           // Smithy forge (contained fire, lights its surroundings)
    AmbientEmitterType ambientEmitterType = AmbientEmitterType::None;

    // Collision mesh with spatial grid (from M2 bounding geometry)
    struct CollisionMesh {
        std::vector<glm::vec3> vertices;
        std::vector<uint16_t> indices;
        uint32_t triCount = 0;
        glm::vec3 boundsMin{0.0f};
        glm::vec3 boundsMax{0.0f};

        struct TriBounds { float minZ, maxZ; };
        std::vector<TriBounds> triBounds;

        static constexpr float CELL_SIZE = 4.0f;
        glm::vec2 gridOrigin{0.0f};
        int gridCellsX = 0, gridCellsY = 0;
        std::vector<std::vector<uint32_t>> cellFloorTris;
        std::vector<std::vector<uint32_t>> cellWallTris;

        void build();
        /// The triangles of one of the two cell arrays that a query box
        /// reaches. The two queries below differ only in which they pass.
        void gatherTrisInRange(const std::vector<std::vector<uint32_t>>& cells,
                               float minX, float minY, float maxX, float maxY,
                               std::vector<uint32_t>& out) const;

        void getFloorTrisInRange(float minX, float minY, float maxX, float maxY,
                                 std::vector<uint32_t>& out) const;
        void getWallTrisInRange(float minX, float minY, float maxX, float maxY,
                                std::vector<uint32_t>& out) const;
        [[nodiscard]] bool valid() const { return triCount > 0; }
    };
    CollisionMesh collision;

    std::string name;

    // Skeletal animation data (kept from M2Model for bone computation)
    std::vector<pipeline::M2Bone> bones;
    std::vector<pipeline::M2Sequence> sequences;
    /// What the model plays as Stand (pipeline::standSequenceIndex).
    int standSequenceIndex = 0;
    std::vector<uint32_t> globalSequenceDurations;  // Loop durations for global sequence tracks
    bool hasAnimation = false;  // True if any bone has keyframes
    uint32_t rtMesh = ~0u;      // RtScene mesh, or ~0u when it does not cast
    bool isSpellEffect = false;  // True for spell effect models (skip particle dampeners)
    bool hasProjectedBatches = false;
    /// Loaded by SpellVisualSystem for a spell visual, not placed in the world.
    bool isRuntimeSpellVisual = false;
    bool disableAnimation = false; // Keep chest lids visually stable
    bool isElvenLike = false;       // Model name matches elf/elven/quel (precomputed)
    bool isLanternLike = false;     // Model name matches lantern/lamp/light (precomputed)
    bool isKoboldFlame = false;     // Model name matches kobold+(candle/torch/mine) (precomputed)
    bool isSkyBird = false;         // Flying bird/bat doodad - hide until animation range
    bool isLightBeam = false;       // Lighthouse/light-ray beam - distant bones must keep updating
    /// Play the sequence forward, then backward, rather than looping it.
    ///
    bool isTransportDoodad = false; // Animated ship sail/paddle child
    bool hasTextureAnimation = false; // True if any batch has UV animation
    bool hasTransparentBatches = false; // True if any batch uses alpha-blend or additive (blendMode >= 2)
    uint8_t availableLODs = 0;  // Bitmask: bit N set if any batch has submeshLevel==N

    // Particle emitter data (kept from M2Model)
    std::vector<pipeline::M2ParticleEmitter> particleEmitters;
    std::vector<VkTexture*> particleTextures;    // Resolved Vulkan textures per emitter
    std::vector<VkDescriptorSet> particleTexSets; // Pre-allocated descriptor sets per emitter (stable, avoids per-frame alloc)
    /// Per emitter: the bind-pose triangles of its geometry model, empty for
    /// sprite emitters. Drawn once per particle (renderM2Ribbons).
    struct ParticleGeometry {
        std::vector<glm::vec3> positions;
        std::vector<glm::vec2> uvs;
        std::vector<uint16_t> indices;
    };
    std::vector<ParticleGeometry> particleGeometry;

    // Ribbon emitter data (kept from M2Model)
    std::vector<pipeline::M2RibbonEmitter> ribbonEmitters;
    std::vector<pipeline::M2Light> lights;
    std::vector<VkTexture*> ribbonTextures;       // Resolved texture per ribbon emitter
    std::vector<VkDescriptorSet> ribbonTexSets;   // Descriptor sets per ribbon emitter
    // M2 material blend mode and render flags the ribbon's materialIndex names
    // (blendMode >= 3 additive; flags & 0x01 unlit, else lit by scene lighting).
    std::vector<uint16_t> ribbonBlendModes;
    std::vector<uint16_t> ribbonRenderFlags;

    // Texture transform data for UV animation
    std::vector<pipeline::M2TextureTransform> textureTransforms;
    std::vector<uint16_t> textureTransformLookup;
    std::vector<int> idleVariationIndices;  // Sequence indices for idle variations (animId 0)

    [[nodiscard]] bool isValid() const { return vertexBuffer != VK_NULL_HANDLE && indexCount > 0; }
};

/**
 * A single M2 particle emitted from a particle emitter
 */
struct M2Particle {
    glm::vec3 position;
    glm::vec3 velocity;
    glm::vec3 gravityDirection; // authored -Z transformed into this particle's simulation space
    float life;        // current age in seconds
    float maxLife;     // total lifespan
    int emitterIndex;  // which emitter spawned this
    /// Where it was born, in its simulation space (see kParticlePinned).
    glm::vec3 birthPosition{0.0f};
    /// Emitter orientation at birth, in the particle's simulation space;
    /// a geometry-model particle draws its mesh in it.
    glm::mat3 birthBasis{1.0f};
    /// Geometry particles: angular velocity (rad/s) from the emitter's tumble range.
    glm::vec3 tumbleRate{0.0f};
    /// Per-particle seed for the emitter's twinkle (particleTwinkle).
    uint32_t twinkleSeed = 0;
};

/**
 * Instance of an M2 model in the world
 */
struct M2Instance {
    uint32_t id = 0;     // Unique instance ID
    uint32_t modelId;
    // A WMO doodad that stands in rooms: drawn only while one of these groups
    // of WMO instance wmoRoomInstance is drawn. Empty for everything else.
    uint32_t wmoRoomInstance = 0;
    std::vector<uint16_t> wmoRooms;
    // Some WMO doodad (MODD entry) has been placed as this instance; see
    // rendering::addWmoDoodad.
    bool wmoDoodadPlaced = false;
    uint32_t indoorLighting = 0; // packed MODD RGB in bits 8..31, bit 1 enables
    bool outdoorShadowed = false;
    bool interiorFog = false;
    std::optional<DirectionalLightRig> customLightRig;
    // Game objects: where and how long ago their interior light was asked.
    glm::vec3 interiorLightQueriedAt{std::numeric_limits<float>::max()};
    float interiorLightAge = 0.0f;
    glm::vec3 position;
    glm::vec3 rotation;  // Euler angles in degrees
    float scale;
    glm::mat4 modelMatrix;
    glm::mat4 invModelMatrix;
    // Its RtScene instance and the matrix last given to it; see syncRtScene.
    uint32_t rtInstance = ~0u;
    glm::mat4 rtMatrix{0.0f};
    glm::vec3 worldBoundsMin;
    glm::vec3 worldBoundsMax;

    // Animation state
    float animTime = 0.0f;       // Current animation time (ms)
    float globalSequenceTime = 0.0f; // Independent clock for global animation tracks
    float animSpeed = 1.0f;      // Animation playback speed
    int currentSequenceIndex = 0;// Index into sequences array
    float animDuration = 0.0f;   // Duration of current animation (ms)
    std::vector<glm::mat4> boneMatrices;

    // Idle variation state
    int idleSequenceIndex = 0;   // Default idle sequence index
    float variationTimer = 0.0f; // Time until next variation attempt (ms)
    bool playingVariation = false;// Currently playing a one-shot variation

    /// Stop at the end of the sequence and stay there.
    ///
    /// A door is a pose, not a performance: the server says open or closed and
    /// the model holds that. Without this a one-shot ran to its end and went
    /// back to the idle sequence, which for a door is the closed pose - so an
    /// open door swung open and shut itself, over and over.
    bool holdAtEnd = false;
    bool stateEffectAnimation = false;

    /// Lit while the player is pressing on this instance: 0 none, 1 full.
    ///
    /// A game object has no selection circle - it is used rather than selected
    /// - so the press is the only thing that can say the click landed on this
    /// one and not the scenery beside it.
    float highlight = 0.0f;
    /// An alpha its owner sets on the whole model - the sky's crossfade
    /// between zones. Below one the instance is drawn in the blended pass
    /// alone, every layer of it, since an opaque layer cannot fade otherwise.
    float fade = 1.0f;
    /// Its effect has ended (retireInstance): the mesh and ribbons are gone,
    /// no particle is born, and the particles alive finish their lives; the
    /// instance is removed with the last of them.
    bool retiring = false;
    /// Retiring with its ribbon edges aging out instead of cleared (a landed missile's trail).
    bool ribbonsLiveOut = false;

    // Particle emitter state
    std::vector<float> emitterAccumulators;  // fractional particle counter per emitter
    int emitterPrevSequence = -1;            // sequence at previous emission step (Squirt crossings)
    float emitterPrevAnimTime = 0.0f;        // animTime (ms) at previous emission step
    // Birth transform of each emitter at the previous emission step. A birth
    // inside the frame is placed where the emitter was at that instant.
    std::vector<glm::mat4> emitterPreviousBirth;
    std::vector<M2Particle> particles;

    // Ribbon emitter state
    struct RibbonEdge {
        glm::vec3 worldPos;   // Spine world position when this edge was born
        // The edge's two ends in world space at birth: the emitter point
        // moved heightAbove along the emitter bone's +Z and heightBelow along
        // its -Z, through bone and model matrix (so the owner's scale and the
        // bone's turn are in them).
        glm::vec3 top;
        glm::vec3 bottom;
        glm::vec3 color;      // Interpolated color at birth
        float     alpha;      // Interpolated alpha at birth
        float     age;        // Seconds since spawned
    };
    // One deque of edges per ribbon emitter on this instance
    std::vector<std::deque<RibbonEdge>> ribbonEdges;
    std::vector<float> ribbonEdgeAccumulators; // fractional edge counter per emitter

    // Cached model flags (set at creation to avoid per-frame hash lookups)
    bool cachedHasAnimation = false;
    bool cachedDisableAnimation = false;
    bool cachedHasParticleEmitters = false;
    // Particle or ribbon emitters: the instance enters particleInstanceIndices_,
    // the pass that steps both. Holy_Precast_Low_Hand.m2 (Holy Smite, Scarlet
    // Adept) has three ribbons on spinning bones and no particle emitter.
    bool cachedHasEmitters = false;
    bool cachedIsGroundDetail = false;
    bool cachedIsInvisibleTrap = false;
    bool cachedIsSkyBird = false;
    bool cachedIsLightBeam = false;
    bool cachedIsTransportDoodad = false;
    bool cachedIsValid = false;
    bool skipCollision = false;    // Fully non-collidable visual/effect instance
    float cachedBoundRadius = 0.0f;
    glm::vec3 cachedCullCenter{0.0f};              // transformed visual-bounds center
    float cachedVisualRadius = 0.0f;               // transformed visual-bounds half diagonal
    // Pre-computed per-instance cull factors (depend only on static flags + scale +
    // bound radius), populated by recomputeCachedCullFactors(). Each frame's cull
    // multiplies them by the smoothed render distance.
    float cachedEffectiveMaxDistSqFactor = 1.0f;  // multiplied by maxRenderDistanceSq each frame
    float cachedPaddedRadius = 0.0f;              // frustum-test sphere radius
    float portalSpinAngle = 0.0f;  // Accumulated spin angle for portal rotation
    const M2ModelGPU* cachedModel = nullptr;  // Avoid per-frame hash lookups

    // Server game object (mailbox, chest, node, door). Set by the spawner.
    bool isGameObject = false;
    // Spell visual (SpellVisualSystem): a glow, beam end or state kit on a unit
    // or at a place the server named. Set by SpellVisualSystem.
    bool isSpellVisual = false;
    // Drawn to the view distance like the units CharacterRenderer draws, not to
    // the adaptive ambient-doodad distance (~200 yd in a city): the server
    // decides what is in range. Archimonde's Banish_Chest glow and his body
    // have to come and go together.
    [[nodiscard]] bool serverVisibility() const { return isGameObject || isSpellVisual || externalPose; }
    /// Posed by its owner rather than by this renderer: model matrix, bone
    /// matrices, sequence and clocks are written each frame through
    /// setExternalPose, and update() neither advances nor animates it. Its
    /// emitters run in updateExternalPoseEffects. Used for the particles and
    /// ribbons of models CharacterRenderer draws (creatures, players and
    /// their attached weapons), which share the character's skeleton.
    bool externalPose = false;
    /// The owner holds a finished one-shot sequence on its last frame (a
    /// corpse at the end of Death): emitters flagged
    /// kParticleStopsOnHeldFrame bear no particles there.
    bool externalClockHeld = false;
    /// Sprite size scale of an externally posed instance's particles, from
    /// its owner (ExternalPose::particleScale); its model matrix places them.
    float externalParticleScale = 1.0f;

    void recomputeCachedCullFactors();

    // Frame-skip optimization (update distant animations less frequently)
    uint8_t frameSkipCounter = 0;
    bool bonesDirty[2] = {false, false};  // Per-frame-index: set when bones recomputed, cleared after upload
    // Mega-bone slot this instance's bones were last uploaded to, per frame
    // index (0 = never uploaded; valid slots start at 1). Lets prepareRender
    // skip the memcpy when the bones are unchanged and still at the same slot.
    uint32_t megaBoneUploadedSlot[2] = {0, 0};

    // Per-instance bone SSBO (double-buffered) - legacy; see mega bone SSBO in M2Renderer
    ::VkBuffer boneBuffer[2] = {};
    VmaAllocation boneAlloc[2] = {};
    void* boneMapped[2] = {};
    VkDescriptorSet boneSet[2] = {};

    // Mega bone SSBO offset - base bone index for this instance (set per-frame in prepareRender)
    uint32_t megaBoneOffset = 0;

    void updateModelMatrix();
};

// M2 material UBO - matches M2Material in m2.frag.glsl (set 1, binding 2)
struct M2MaterialUBO {
    int32_t hasTexture;
    int32_t alphaTest;
    int32_t colorKeyBlack;
    float colorKeyThreshold;
    int32_t unlit;
    int32_t blendMode;
    float fadeAlpha;
    float interiorDarken;
    float specularIntensity;
    float emissiveBoost;
    // The batch's authored colour, from the M2's colour track, with any
    // flicker already folded in. Three scalars rather than a vec3 so the
    // std140 rules stay trivial.
    float tintR;
    float tintG;
    float tintB;
    /// Light drawn as geometry - a searchlight cone, a god ray, a shaft
    /// through a window. The fragment shader fades it out where the surface
    /// turns away from the eye, so the mesh's silhouette stops being an edge.
    int32_t volumetricBeam;
    /// A flame card. Fades out over the top of its own model, so the card's
    /// upper edge is not where the fire stops.
    int32_t fireCard;
    int32_t textureCount;
    int32_t combiner0;
    int32_t combiner1;
};

// M2 params UBO - matches M2Params in m2.vert.glsl (set 1, binding 1)
struct M2ParamsUBO {
    float uvOffsetX;
    float uvOffsetY;
    int32_t texCoordSet;
    int32_t useBones;
};

/**
 * M2 Model Renderer (Vulkan)
 *
 * Handles rendering of M2 models (doodads like trees, rocks, bushes)
 */
class M2Renderer {
public:
    M2Renderer();
    ~M2Renderer();

    [[nodiscard]] bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout,
                    pipeline::AssetManager* assets);
    /** Configure this renderer for camera-centered sky M2s before initialize(). */
    void setSkyMode(bool enabled) { skyMode_ = enabled; }
    void shutdown();

    [[nodiscard]] bool hasModel(uint32_t modelId) const;
    /// groundEffect: the terrain ground-effect scatterer places this model
    /// (see classifyM2Model). One model id is one role; loading the same id
    /// in the other role is an error.
    bool loadModel(const pipeline::M2Model& model, uint32_t modelId, bool groundEffect = false);
    /** Force-remove a model and all its GPU resources. Caller must ensure no instances reference it. */
    void unloadModel(uint32_t modelId);
    /** Mark a loaded model as a spell effect (full-brightness particles, no collision). */
    void markModelAsSpellEffect(uint32_t modelId);
    /// A game object's pose is its server state (Closed 147, Opened 149...),
    /// so its bones always run, whatever the name classifier guessed:
    /// G_PulsatingPlant.m2 (Warpwood Pod) reads as foliage, chest01.m2 as a
    /// chest, and both were frozen in bind pose.
    void markModelAsGameObject(uint32_t modelId);

    /// Instances of the same model at the same spot are treated as one placement,
    /// which is right for the static world and wrong for anything whose position
    /// arrives later from a parent. Pass allowPositionDedup=false for a child
    /// instance created at a placeholder position - see the note in
    /// setInstanceTransform on why the placeholder key outlives the placement.
    uint32_t createInstance(uint32_t modelId, const glm::vec3& position,
                            const glm::vec3& rotation = glm::vec3(0.0f),
                            float scale = 1.0f,
                            bool allowPositionDedup = true);
    uint32_t createInstanceWithMatrix(uint32_t modelId, const glm::mat4& modelMatrix,
                                       const glm::vec3& position,
                                       bool allowPositionDedup = true);

    /// One frame's pose for an externally posed instance (M2Instance::externalPose).
    struct ExternalPose {
        glm::mat4 modelMatrix{1.0f};
        const std::vector<glm::mat4>* boneMatrices = nullptr;  // model space, same skeleton
        int sequenceIndex = 0;
        float animTimeMs = 0.0f;
        float globalSequenceTimeMs = 0.0f;
        float fade = 1.0f;
        bool clockHeld = false;
        /// Multiplies the authored particle sizes: the emitter model's own
        /// scale. An item visual hung on a weapon is drawn at its own scale
        /// whatever the scale of the weapon, its attachment bone or the unit
        /// carrying it, which only place it (CharacterRenderer::syncEmitters).
        float particleScale = 0.0f;
    };
    /// An instance of an emitter model whose pose its owner supplies.
    uint32_t createExternalPoseInstance(uint32_t modelId, const glm::mat4& modelMatrix);
    void setExternalPose(uint32_t instanceId, const ExternalPose& pose);
    /// Simulate the particles and ribbons of externally posed instances. Call
    /// after update() has returned and after the owners have set this frame's
    /// pose, on the thread that owns this renderer's instance table.
    void updateExternalPoseEffects(float deltaTime);

    void update(float deltaTime, const glm::vec3& cameraPos, const glm::mat4& viewProjection);

    /// Height of the floor a projected texture lands on at (x, y), within
    /// [planeZ - reach, planeZ + reach]. See projected_texture.hpp.
    using ProjectedSurfaceQuery = std::function<std::optional<float>(float x, float y, float planeZ, float reach)>;
    /// Drape this frame's projected-texture quads over the ground. Main thread,
    /// after update() has posed the instances and before render().
    void updateProjectedTextures(const ProjectedSurfaceQuery& surface);

    /**
     * Render all visible instances (Vulkan)
     */
    /** Pre-allocate GPU resources (bone SSBOs, descriptors) on main thread before parallel render. */
    void prepareRender(uint32_t frameIndex, const Camera& camera);
    // Split command buffers let world opaque geometry execute before characters.
    // Inline recording uses afterOpaque to insert characters at the same boundary.
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera,
                VkCommandBuffer transparentCmd = VK_NULL_HANDLE,
                const std::function<void()>& afterOpaque = {},
                const std::function<bool(float)>& beforeTransparentDepth = {});

    /** Gather the nearest authored glow cards as inexpensive scene point lights. */
    uint32_t gatherLocalLights(const glm::vec3& cameraPos,
                               glm::vec4* outPosRadius,
                               glm::vec4* outColorIntensity,
                               uint32_t maxLights) const;

    /** Gather authored M2Light point lights (TBC model2.bls point lights). */
    uint32_t gatherAuthoredM2Lights(const glm::vec3& centerPos,
                                    glm::vec4* outPosRadius,
                                    glm::vec4* outColorIntensity,
                                    uint32_t maxLights) const;

    /// Where loaded models register their geometry for the ray traced
    /// lighting. Static doodads only - animated models would cast their
    /// bind pose. Not given to the sky's renderer.
    void setRtScene(RtScene* scene) { rtScene_ = scene; }
    /// Bring the scene's instances in line with this renderer's, once a frame
    /// while the lighting is on.
    void syncRtScene();
    void setForceNoCull(bool v) { forceNoCull_ = v; }

    /**
     * Initialize shadow pipeline (Phase 7)
     */
    [[nodiscard]] bool initializeShadow(VkRenderPass shadowRenderPass);
    [[nodiscard]] bool hasShadowPipeline() const { return shadowPipeline_ != VK_NULL_HANDLE; }

    /**
     * Render depth-only pass for shadow casting
     */
    void renderShadow(VkCommandBuffer cmd, const glm::mat4& lightSpaceMatrix,
                      const glm::vec3& shadowCenter = glm::vec3(0), float shadowRadius = 1e9f);

    /// Which part of a frame's particles one renderM2Particles call draws.
    /// Emitters with a negative priority plane sort before the transparent
    /// mesh batches (priority 0 and up); the rest after them.
    enum class ParticlePhase { BeforeTransparent, AfterTransparent };

    /**
     * Render M2 particle emitters (point sprites). Call BeforeTransparent
     * once, between the opaque and transparent mesh passes, then
     * AfterTransparent once after them, in the same frame.
     */
    void renderM2Particles(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, ParticlePhase phase);

    /**
     * Render smoke particles from chimneys etc.
     */

    /**
     * Render M2 ribbon emitters (spell trails / wing effects)
     */
    void renderM2Ribbons(VkCommandBuffer cmd, VkDescriptorSet perFrameSet);

    /// A SpellChainEffects.dbc beam for one frame: a camera-facing strip
    /// through spine points, drawn with the ribbon pipelines after the M2
    /// ribbons. u runs along the strip, v across it (0 one edge, 1 the other).
    struct ChainStrip {
        std::string texturePath;
        bool additive = true;
        glm::vec3 color{1.0f};
        float alpha = 1.0f;
        float halfWidth = 0.0f;
        std::vector<glm::vec3> spine;
        std::vector<float> u;
        std::vector<float> fade;  // alpha factor per spine vertex; empty: lit whole
    };
    /// Replaces the beams drawn from the next frame on.
    void setChainStrips(std::vector<ChainStrip> strips) { chainStrips_ = std::move(strips); }

    void setInstancePosition(uint32_t instanceId, const glm::vec3& position);
    /// WOWEE_M2_CENSUS: what a model is actually drawn at, said once each.
    void censusInstance(const M2Instance& instance);

    void setInstanceTransform(uint32_t instanceId, const glm::mat4& transform);
    void setInstanceAnimationFrozen(uint32_t instanceId, bool frozen);
    /// Pose the current sequence at a fraction (0-1) of its length and hold it
    /// there; the clock no longer advances it. A spell missile's sequence runs
    /// once over its flight (SpellVisualSystem::placeSpellMissile).
    void setInstanceAnimationProgress(uint32_t instanceId, float fraction);
    /// Play from the first frame, with bones to match.
    ///
    /// New instances start at a random point so placed doodads don't animate
    /// in lockstep, and take their first bones from a sibling that is already
    /// mid-animation. A spell cast has to start at its beginning instead.
    void restartInstanceAnimation(uint32_t instanceId);
    void startInstanceStateEffect(uint32_t instanceId);
    /// Returns authored Decay duration in seconds, or zero when absent.
    float finishInstanceStateEffect(uint32_t instanceId);
    /// Play an animation once and stay on its last frame.
    ///
    /// skipToEnd puts the model there immediately, which is what a door that
    /// was already open when it came into view wants - the swing belongs to
    /// the moment it opens, not to the moment it is first seen.
    void setInstanceAnimationHeld(uint32_t instanceId, uint32_t animationId,
                                  bool skipToEnd);
    /// Loop stateId, after one play of transitionId when the model has it.
    /// A game object's Closed (147) after Close (146), Opened (149) after
    /// Open (148).
    void setInstanceStateAnimation(uint32_t instanceId, uint32_t transitionId,
                                   uint32_t stateId, bool playTransition);
    /// Loop the model's Stand (M2ModelGPU::standSequenceIndex) from its start.
    void playInstanceStand(uint32_t instanceId);

    /// Set placement lighting on a WMO doodad instance (MODD + group MOLR rig),
    /// or outdoor terrain MCSH shadow state on an ADT doodad instance.
    void setInstanceIndoorLighting(uint32_t instanceId, const glm::vec3& color,
                                   const DirectionalLightRig* rig = nullptr);
    void setInstanceOutdoorShadowed(uint32_t instanceId, bool shadowed);
    /// Record a WMO doodad (MODD entry) placed as this instance, with the
    /// rooms that list it (pipeline::wmoDoodadRooms; empty: drawn with the
    /// building). An instance standing for several entries is drawn while any
    /// of them would be.
    void addInstanceWmoDoodad(uint32_t instanceId, uint32_t wmoInstanceId, const std::vector<uint16_t>& rooms);
    /// Answers whether any of the given groups of a WMO instance is drawn this frame.
    using WmoRoomQuery = std::function<bool(uint32_t wmoInstanceId, const std::vector<uint16_t>& groups)>;
    void setWmoRoomQuery(WmoRoomQuery query) { wmoRoomDrawn_ = std::move(query); }
    /// Light of a game object standing at a world position in a WMO interior,
    /// or nothing outdoors (WMORenderer::interiorObjectLight). Game objects
    /// take it as interior doodads take their MODD colour.
    using InteriorLightQuery = std::function<std::optional<glm::vec3>(const glm::vec3&)>;
    void setInteriorLightQuery(InteriorLightQuery query) { interiorLightQuery_ = std::move(query); }
    struct ObjectLightResolution {
        bool outdoorShadowed = false;
        bool interiorFog = false;
        std::optional<glm::vec3> particleLight;
        std::optional<DirectionalLightRig> customRig;
    };
    using ObjectLightQuery = std::function<ObjectLightResolution(const glm::vec3& pos, float sampleHeight)>;
    void setObjectLightQuery(ObjectLightQuery query) { objectLightQuery_ = std::move(query); }
    /// The WMO room a game object stands in (WMORenderer::interiorRoomAt).
    /// A game object there is drawn with that room, as a room doodad is:
    /// Orgrimmar's Mighty Blaze braziers stand in its interior groups, and
    /// 2.4.3 seen from above the city draws neither the groups nor them.
    using RoomAtQuery = std::function<std::optional<std::pair<uint32_t, std::vector<uint16_t>>>(const glm::vec3&)>;
    void setRoomAtQuery(RoomAtQuery query) { roomAtQuery_ = std::move(query); }
    /// Draw an instance with the room of another (an effect placed on a game
    /// object, such as its sparkle).
    void copyInstanceRoom(uint32_t instanceId, uint32_t sourceInstanceId);
    void setInstanceHighlight(uint32_t instanceId, float amount);
    /// See M2Instance::fade.
    void setInstanceFade(uint32_t instanceId, float alpha);
    /// Take the light off whatever has it, whichever instance that was.
    void clearInstanceHighlights();
    /// Set the animation sequence by animation ID (e.g. anim::OPEN, anim::CLOSE).
    /// Finds the first sequence with matching ID. Unfreezes the instance and resets time.
    void setInstanceAnimation(uint32_t instanceId, uint32_t animationId, bool loop = true);
    /// Check if a model instance has a specific animation ID in its sequence table.
    [[nodiscard]] bool hasAnimation(uint32_t instanceId, uint32_t animationId) const;
    [[nodiscard]] float getInstanceAnimDuration(uint32_t instanceId) const;
    /// World-space visual bounding sphere of an instance (center + radius),
    /// used for cursor picking and selection circles. Returns false for an
    /// unknown instance or a degenerate (zero-radius) model.
    bool getInstanceBounds(uint32_t instanceId, glm::vec3& outCenter, float& outRadius) const;
    /// World position of an instance's model origin.
    bool getInstancePosition(uint32_t instanceId, glm::vec3& outPosition) const;
    /// Height of the M2 header bounding box of an instance's model, times the
    /// instance scale.
    bool getInstanceCollisionBoxHeight(uint32_t instanceId, float& outHeight) const;

    /// The world-space box of an instance's collision geometry.
    ///
    /// What a lift's deck actually covers, as opposed to how far its origin is
    /// from you. Boarding a generic M2 transport is decided on a radius around
    /// that origin, which is why standing beside an Undercity lift shaft
    /// attaches you to the car.
    bool getInstanceWorldBounds(uint32_t instanceId, glm::vec3& outMin, glm::vec3& outMax) const;

    /// The floor one named instance offers under a point, ignoring every other.
    ///
    /// Asked of a lift car, this is "is its deck under your feet" - which a box
    /// around the car cannot answer, because an axis-aligned box covers the
    /// doorway and the ground just outside it.
    std::optional<float> getInstanceFloorHeight(uint32_t instanceId,
                                                float glX, float glY, float glZ) const;

    /// The id of an instance's only animation, where it has exactly one.
    ///
    /// A door with no OPEN or CLOSE sequence of its own carries one animation
    /// and that animation is the opening. Undercity's lift doors are the case:
    /// one bone, one sequence, id 0, 3333ms.
    [[nodiscard]] std::optional<uint32_t> soleSequenceId(uint32_t instanceId) const;
    /// The animation id of what the instance's model plays as Stand
    /// (M2ModelGPU::standSequenceIndex): 0, or its playable lookup's stand-in.
    [[nodiscard]] std::optional<uint32_t> standSequenceId(uint32_t instanceId) const;
    /// True while the instance is still live in the renderer. Owners that cache
    /// instance IDs (game objects, transports) use this to notice an instance
    /// that was dropped underneath them - e.g. by a renderer-wide clear - and
    /// respawn it instead of silently addressing a dead handle forever.
    [[nodiscard]] bool hasInstance(uint32_t instanceId) const {
        return instanceIndexById.find(instanceId) != instanceIndexById.end();
    }
    void removeInstance(uint32_t instanceId);
    /// End a spell effect as build 8606 does: its mesh and ribbons vanish at
    /// once, its emitters stop, and the particles already out live out their
    /// lifespans. Evocation 12051 interrupted: CycloneWater_State's cone and
    /// the hand lightning are gone in the next frame, the smoke at the feet
    /// stays and fades. Removed outright when it has no live particles.
    void retireInstance(uint32_t instanceId);
    /// End a spell missile at its landing: the mesh vanishes and its emitters
    /// stop, while the trail it already laid (ribbon edges) and its particles
    /// live out their lifetimes where they are. Build 8606, Ras Frostwhisper's
    /// Frostbolt Volley 8398 (Frostbolt.m2, ribbon edge lifetime 0.5 s): the
    /// straight trail from the hand stays at the target for about 0.3-0.4 s
    /// after the bolt arrives, at 7.8 and 15.7 yd alike.
    void retireMissileInstance(uint32_t instanceId);
    void removeInstances(const std::vector<uint32_t>& instanceIds);
    /// Mark an instance as a server game object so the adaptive doodad render
    /// distance can't cull it while the server still considers it visible.
    void setInstanceIsGameObject(uint32_t instanceId, bool isGameObject);
    void setInstanceIsSpellVisual(uint32_t instanceId);
    void setSkipCollision(uint32_t instanceId, bool skip);
    void clear();
    /** Drop all instances but keep models in GPU memory. Cheap path for the
     *  editor's rebuild loop where the same model is re-instanced repeatedly. */
    void clearInstances();
    void cleanupUnusedModels();
    /// Keep a model resident even while it has no instances. Game object models
    /// are a small, bounded set (one per display ID actually encountered) that
    /// goes instance-free every time the player leaves an area and comes back -
    /// exactly the reap/reload churn seen in the field. Pinning them keeps that
    /// cycle out of the picture entirely; ambient doodads still get reaped.
    void setModelPinned(uint32_t modelId, bool pinned);
    /** Return and clear the model IDs evicted by cleanupUnusedModels() since the
     *  last call. Lets callers that track uploaded models drop stale entries. */
    std::vector<uint32_t> drainReapedModelIds();

    bool checkCollision(const glm::vec3& from, const glm::vec3& to,
                        glm::vec3& adjustedPos, float playerRadius = 0.5f,
                        float playerHeight = 2.0f, bool allowHorizontalRise = true) const;
    std::optional<float> getFloorHeight(float glX, float glY, float glZ, float* outNormalZ = nullptr) const;
    // Constrain a downward grounding snap against the complete upright body.
    // Returns true only when authored geometry blocks the requested descent.
    bool constrainFloorSnap(const glm::vec3& from, float requestedZ,
                            float radius, float height,
                            float& supportZ, float& supportNormalZ) const;
    // Where a grounded body ends after stepping up by at most maxStepUp,
    // across to `to` and back down onto authored M2 collision faces.
    // `landingFace` as movement::stepBodyAcross.
    [[nodiscard]] glm::vec3 stepBody(const glm::vec3& from, const glm::vec3& to,
                                     float radius, float height, float maxStepUp,
                                     glm::vec3* landingFace = nullptr) const;

    /// Every doodad the floor query would consider here, and what each one
    /// contributes. The WMO side has had debugDumpGroupsAtPosition for this
    /// since the Undercity pull was first chased; the M2 side had nothing, so
    /// a floor that should have come from a lift deck and did not left no
    /// trace at all beyond "m2=-99999".
    void debugDumpFloorCandidatesAt(float glX, float glY, float glZ) const;
    [[nodiscard]] float raycastBoundingBoxes(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, uint32_t excludedInstanceId = 0) const;
    [[nodiscard]] float raycastVisibleGeometry(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, uint32_t excludedInstanceId = 0, uint32_t requiredInstanceId = 0) const;
    void setCollisionFocus(const glm::vec3& worldPos, float radius);

    void resetQueryStats();
    [[nodiscard]] double getQueryTimeMs() const { return queryTimeMs; }
    [[nodiscard]] uint32_t getQueryCallCount() const { return queryCallCount; }

    void recreatePipelines();

    /// Build the nine main-pass pipelines. Called by initialize() and again by
    /// recreatePipelines() after a device loss, which is the reason it exists:
    /// the two used to be separate copies of the same 190 lines.
    bool buildMainPassPipelines(VkDescriptorSetLayout perFrameLayout);

    // Stats
    [[nodiscard]] bool isInitialized() const { return initialized_; }
    [[nodiscard]] uint32_t getModelCount() const { return static_cast<uint32_t>(models.size()); }
    [[nodiscard]] uint32_t getInstanceCount() const { return static_cast<uint32_t>(instances.size()); }
    [[nodiscard]] uint32_t getTotalTriangleCount() const;
    [[nodiscard]] uint32_t getDrawCallCount() const { return lastDrawCallCount; }

    /**
     * Append the 2D footprints of placed props touching the given window,
     * for the grass generator's clearings. Ground detail is skipped - it is
     * the clutter itself - and the footprint radius is capped so a tree
     * clears the ground at its trunk, not under its whole crown.
     */
    void collectGrassClearings(float minX, float minY, float maxX, float maxY,
                               std::vector<pipeline::GrassClearingSource>& out) const;

    // Lighting/fog/shadow are in per-frame UBO; setLighting also supplies
    // outdoor ambient/diffuse/direction for lit outdoor particles (!(flags & 0x1)).
    void setFog(const glm::vec3& /*color*/, float /*start*/, float /*end*/) {}
    void setLighting(const float lightDirIn[3], const float lightColorIn[3],
                     const float ambientColorIn[3]) {
        if (lightDirIn) outdoorLightDir_ = glm::vec3(lightDirIn[0], lightDirIn[1], lightDirIn[2]);
        if (lightColorIn) outdoorDiffuse_ = glm::vec3(lightColorIn[0], lightColorIn[1], lightColorIn[2]);
        if (ambientColorIn) outdoorAmbient_ = glm::vec3(ambientColorIn[0], ambientColorIn[1], ambientColorIn[2]);
    }
    void setShadowMap(uint32_t /*depthTex*/, const glm::mat4& /*lightSpace*/) {}
    void clearShadowMap() {}

    /// How far the furthest doodad drawn this frame was. See
    /// Renderer::logViewDistanceDiag.
    [[nodiscard]] float getFurthestDrawnDistance() const { return std::sqrt(furthestDrawnSq_); }
    /// How many particles to emit, as a fraction of what a model asks for -
    /// the game's Particle Density.
    ///
    /// Applied to the authored rate before the floor that holds flames
    /// together, so turning it down thins smoke, dust and spell effects while
    /// a candle keeps enough particles alive to still read as a flame. See the
    /// note in m2_renderer_particles.cpp: below that floor a fire does not get
    /// smaller, it disappears.
    void setParticleDensity(float density) {
        particleDensity_ = std::clamp(density, 0.05f, 1.0f);
    }
    [[nodiscard]] float particleDensity() const { return particleDensity_; }

    void setSuppressBakedStars(bool suppress) { suppressBakedStars_ = suppress; }
    void setInsideInterior(bool inside) { insideInterior = inside; }
    /// Draw only instances standing inside this box (a sealed interior, see
    /// WMORenderer::findSealedInterior); every instance when empty.
    void setVisibleRegion(std::optional<std::pair<glm::vec3, glm::vec3>> region) { visibleRegion_ = region; }
    /// When the camera is inside an indoor WMO room with no visible portal path
    /// to an exterior group, hide ADT terrain doodads, ground clutter, and
    /// exterior WMO doodads while keeping interior WMO doodads, game objects,
    /// and spell effects.
    void setExteriorHidden(bool hidden) { exteriorHidden_ = hidden; }
    [[nodiscard]] bool exteriorCulled(const M2Instance& instance) const {
        if (!exteriorHidden_) return false;
        if (instance.cachedIsGroundDetail) return true;
        if (instance.cachedModel && instance.cachedModel->isSpellEffect) return false;
        if (instance.wmoRoomInstance != 0) return false;
        return !instance.isGameObject;
    }
    /// A WMO room doodad whose room is not drawn this frame: not its mesh,
    /// particles or ribbons (Karazhan's upper-floor braziers showed their
    /// flames in the Master's Terrace sky).
    [[nodiscard]] bool roomCulled(const M2Instance& instance) const {
        return instance.wmoRoomInstance != 0 && wmoRoomDrawn_ &&
               !wmoRoomDrawn_(instance.wmoRoomInstance, instance.wmoRooms);
    }
    [[nodiscard]] bool outsideVisibleRegion(const glm::vec3& position) const {
        return visibleRegion_ && (glm::any(glm::lessThan(position, visibleRegion_->first)) ||
                                  glm::any(glm::greaterThan(position, visibleRegion_->second)));
    }

    /// Drop the ground clutter from this pass, for the ablation's clutter
    /// phase. Clutter is drawn by the thousand and shares this renderer with
    /// the world's doodads, so switching the whole pass off says what the two
    /// cost together and nothing about which of them it was.
    void setSkipGroundDetail(bool skip) { skipGroundDetail_ = skip; }

    /// Hold the doodads to this many yards, on top of whatever the density
    /// rule and the instance's own size allow. Zero lifts it.
    ///
    /// The ablation's far-doodad phase: these models have no LOD skins, so a
    /// tree a thousand yards off is drawn with every triangle it has to cover
    /// a few pixels. Whether that is where the time goes is a question about
    /// distance alone, and switching the whole pass off cannot answer it.
    void setDoodadDistanceCap(float yards) { doodadDistanceCapYards_ = yards; }

    /// The doodad view distance with that cap applied.
    /// Squared distance one instance draws to (m2InstanceMaxDistSq), given
    /// this frame's ambient doodad distance.
    float maxDistSqOf(const M2Instance& inst, float baseMaxDistSq) const {
        return m2InstanceMaxDistSq(baseMaxDistSq, inst.cachedEffectiveMaxDistSqFactor, inst.serverVisibility(),
                                   inst.serverVisibility() ? viewDistanceAbsolute_ : cappedViewDistance(),
                                   inst.cachedIsGroundDetail, groundDetailMaxDistance_);
    }
    /// Squared distance an instance's particles and ribbons are drawn and
    /// stepped to: the scene's model distance, or the view distance for a
    /// server object (its particles go wherever it is drawn).
    float particleMaxDistSq(const M2Instance& inst) const {
        return inst.serverVisibility() ? viewDistanceAbsolute_ * viewDistanceAbsolute_ : cachedMaxRenderDistSq_;
    }
    float cappedViewDistance() const {
        return doodadDistanceCapYards_ > 0.0f
                   ? std::min(viewDistanceAbsolute_, doodadDistanceCapYards_)
                   : viewDistanceAbsolute_;
    }
    void setViewDistance(float distance) {
        viewDistanceRaw_ = std::clamp(distance, 400.0f, 2400.0f);
        // And the distance itself, as a ceiling. The scale multiplies a
        // constant tuned for scene density - 2800 yards where models are
        // sparse - so at 2400 it put doodads 5600 yards out while the terrain
        // and the WMOs both stop at the 2400 the player asked for. Distant
        // trees and buildings then stood on nothing.
        viewDistanceAbsolute_ = viewDistanceRaw_;
        recomputeViewDistanceScale();
    }

    /// How far the world's clutter is drawn, against how far the world is -
    /// the game's Environment Detail.
    ///
    /// View distance is how far there is anything to see; this is how much of
    /// the scenery inside that is worth drawing, and it only ever takes away.
    /// Above 1 it changes nothing: the ceiling above is the terrain's own, and
    /// a doodad past that is a tree standing on nothing, which is the fault
    /// that ceiling was added for.
    /// How far the ground cover is drawn - the game's Ground Clutter Radius.
    ///
    /// Its own distance rather than a share of the doodad one: grass stops
    /// between 70 and 140 yards in the original client while doodads run to
    /// the horizon, and the setting names those yards outright.
    void setGroundDetailDistance(float yards) {
        groundDetailMaxDistance_ = std::clamp(yards, 0.0f, 500.0f);
    }
    [[nodiscard]] float groundDetailDistance() const { return groundDetailMaxDistance_; }

    void setEnvironmentDetail(float detail) {
        environmentDetail_ = std::clamp(detail, 0.25f, 1.5f);
        recomputeViewDistanceScale();
    }
    [[nodiscard]] float environmentDetail() const { return environmentDetail_; }


    // Pre-decoded BLP cache: set by terrain manager before calling loadModel()
    // so loadTexture() can skip the expensive assetManager->loadTexture() call.
    void setPredecodedBLPCache(std::unordered_map<std::string, pipeline::BLPImage>* cache) { predecodedBLPCache_ = cache; }

private:
    // Authored collision faces of every movement-blocking instance overlapping
    // the world box, transformed to world space.
    void appendBodyCollisionTriangles(const glm::vec3& worldLow, const glm::vec3& worldHigh,
                                      std::vector<movement::BodyTriangle>& triangles) const;
    bool initialized_ = false;
    bool insideInterior = false;
    std::optional<std::pair<glm::vec3, glm::vec3>> visibleRegion_;
    bool exteriorHidden_ = false;
    pipeline::AssetManager* assetManager = nullptr;

    // Vulkan context
    VkContext* vkCtx_ = nullptr;

    // Vulkan pipelines (one per blend mode)
    VkPipeline opaquePipeline_ = VK_NULL_HANDLE;       // blend mode 0
    /// Cutout: leaves, ground clutter, anything the fragment shader alpha-tests.
    /// Blend disabled with alpha-to-coverage on, which is what turns the
    /// shader's sharpened alpha into per-sample coverage.
    VkPipeline cutoutPipeline_ = VK_NULL_HANDLE;
    // Opt-in one-sided foliage variants. Other materials retain their pipelines.
    VkPipeline foliageCullOpaquePipeline_ = VK_NULL_HANDLE;
    VkPipeline foliageCullCutoutPipeline_ = VK_NULL_HANDLE;
    // The same for blended batches: one-sided alpha, additive and multiply materials.
    VkPipeline cullAlphaPipeline_ = VK_NULL_HANDLE;
    VkPipeline cullAdditivePipeline_ = VK_NULL_HANDLE;
    VkPipeline cullModPipeline_ = VK_NULL_HANDLE;
    VkPipeline cullMod2xPipeline_ = VK_NULL_HANDLE;
    /// The back-face-culling twin of a main-pass pipeline, or the pipeline
    /// itself where there is none.
    [[nodiscard]] VkPipeline culledPipeline(VkPipeline p) const {
        if (p == opaquePipeline_ && foliageCullOpaquePipeline_) return foliageCullOpaquePipeline_;
        if (p == cutoutPipeline_ && foliageCullCutoutPipeline_) return foliageCullCutoutPipeline_;
        if (p == alphaPipeline_ && cullAlphaPipeline_) return cullAlphaPipeline_;
        if (p == additivePipeline_ && cullAdditivePipeline_) return cullAdditivePipeline_;
        if (p == modPipeline_ && cullModPipeline_) return cullModPipeline_;
        if (p == mod2xPipeline_ && cullMod2xPipeline_) return cullMod2xPipeline_;
        return p;
    }
    /// The ablation's clutter phase; see setSkipGroundDetail.
    bool skipGroundDetail_ = false;
    /// The ablation's far-doodad phase; see setDoodadDistanceCap.
    float doodadDistanceCapYards_ = 0.0f;
    /// Said once: a cut-out batch reached the shadow pass with no texture.
    bool warnedShadowNoTexture_ = false;
    VkPipeline alphaTestPipeline_ = VK_NULL_HANDLE;     // blend mode 1
    VkPipeline alphaPipeline_ = VK_NULL_HANDLE;         // blend mode 2
    VkPipeline additivePipeline_ = VK_NULL_HANDLE;      // blend modes 3, 4
    VkPipeline modPipeline_ = VK_NULL_HANDLE;           // blend mode 5: dst * src
    VkPipeline mod2xPipeline_ = VK_NULL_HANDLE;         // blend mode 6: dst * src * 2
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;

    // Shadow rendering (Phase 7)
    VkPipeline shadowPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout shadowPipelineLayout_ = VK_NULL_HANDLE;
    /// The set the shadow pass binds. Five separate members before,
    /// built and torn down here and in three other renderers.
    ShadowParamsSet shadowParams_;
    // Per-frame pools for foliage shadow texture descriptor sets (one per frame-in-flight)
    static constexpr uint32_t kShadowTexPoolFrames = 2;
    VkDescriptorPool shadowTexPool_[kShadowTexPoolFrames] = {};

    // Particle pipelines
    VkPipeline particlePipeline_ = VK_NULL_HANDLE;       // M2 emitter particles
    VkPipeline particleAdditivePipeline_ = VK_NULL_HANDLE; // Additive particle blend
    VkPipeline particleModPipeline_ = VK_NULL_HANDLE;      // M2Blend 5
    VkPipeline particleMod2xPipeline_ = VK_NULL_HANDLE;    // M2Blend 6
    VkPipelineLayout particlePipelineLayout_ = VK_NULL_HANDLE;

    // Ribbon pipelines (additive + alpha-blend)
    VkPipeline ribbonPipeline_ = VK_NULL_HANDLE;          // Alpha-blend ribbons
    VkPipeline ribbonAdditivePipeline_ = VK_NULL_HANDLE;  // Additive ribbons
    // Geometry-model particles: ribbon vertex layout and shaders, triangle lists.
    VkPipeline particleMeshPipeline_ = VK_NULL_HANDLE;
    VkPipeline particleMeshAdditivePipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout ribbonPipelineLayout_ = VK_NULL_HANDLE;
    /// The per-frame set layout initialize() was given. recreatePipelines()
    /// runs long after that call and needs the same one.
    VkDescriptorSetLayout perFrameLayout_ = VK_NULL_HANDLE;

    // Descriptor set layouts
    VkDescriptorSetLayout materialSetLayout_ = VK_NULL_HANDLE;  // set 1
    VkDescriptorSetLayout boneSetLayout_ = VK_NULL_HANDLE;      // set 2
    VkDescriptorSetLayout particleTexLayout_ = VK_NULL_HANDLE;  // particle set 1 (texture only)

    // Descriptor pools
    VkDescriptorPool materialDescPool_ = VK_NULL_HANDLE;
    VkDescriptorPool boneDescPool_ = VK_NULL_HANDLE;
    std::shared_ptr<std::atomic<uint64_t>> boneDescPoolGeneration_ =
        std::make_shared<std::atomic<uint64_t>>(0);
    static constexpr uint32_t MAX_MATERIAL_SETS = 16384;
    static constexpr uint32_t MAX_BONE_SETS = 16384;

    // Dummy identity bone buffer + descriptor set for non-animated models.
    // The pipeline layout declares set 2 (bones) and some drivers (Intel ANV)
    // require all declared sets to be bound even when the shader doesn't access them.
    ::VkBuffer dummyBoneBuffer_ = VK_NULL_HANDLE;
    VmaAllocation dummyBoneAlloc_ = VK_NULL_HANDLE;
    VkDescriptorSet dummyBoneSet_ = VK_NULL_HANDLE;

    // Mega bone SSBO - consolidates all per-instance bone matrices into a single buffer per frame.
    // Replaces per-instance bone SSBOs for fewer descriptor binds and enables GPU instancing.
    static constexpr uint32_t MEGA_BONE_MAX_INSTANCES = 4096;
    // Per-instance bone ranges are packed at their model's true bone count
    // rather than at a fixed stride, so the buffer is sized in matrices. The
    // budget matches the old 4096 x 128 slots; models under the old stride
    // now leave room for the ones above it.
    static constexpr uint32_t MEGA_BONE_MATRIX_CAPACITY = MEGA_BONE_MAX_INSTANCES * 128;
    ::VkBuffer megaBoneBuffer_[2] = {};
    VmaAllocation megaBoneAlloc_[2] = {};
    void* megaBoneMapped_[2] = {};
    VkDescriptorSet megaBoneSet_[2] = {};

    // GPU instance data SSBO - per-instance transforms, fade, bones for instanced draws.
    // Shader reads instanceData[push.instanceDataOffset + gl_InstanceIndex].
    struct M2InstanceGPU {
        glm::mat4 model;                 // 64 bytes @ offset 0
        glm::vec2 uvOffset;              //  8 bytes @ offset 64
        float fadeAlpha;                 //  4 bytes @ offset 72
        int32_t useBones;                //  4 bytes @ offset 76
        int32_t boneBase;                //  4 bytes @ offset 80
        int32_t boneCount;               //  4 bytes @ offset 84 - clamps skinning reads
        float highlight = 0.0f;          //  4 bytes @ offset 88 - pressed-on / hover lift
        int32_t flags = {};              //  4 bytes @ offset 92 - bit0: MCSH shadow, bit1: customRig, bit2: interiorFog
        glm::vec2 uvOffset2{};           //  8 bytes @ offset 96 - second texture translation
        uint32_t tintPacked = 0;         //  4 bytes @ offset 104 - bit 24 + RGB8 animated M2Color tint
        float boundsRadius = 0.5f;       //  4 bytes @ offset 108 - worldBoundsRadius
        glm::vec4 customAmbient{0.0f};   // 16 bytes @ offset 112 - xyz: ambient, w: worldBoundsCenter.x
        glm::vec4 customDiffuse{0.0f};   // 16 bytes @ offset 128 - xyz: diffuse, w: worldBoundsCenter.y
        glm::vec4 customLightDir{0.0f};  // 16 bytes @ offset 144 - xyz: lightDir, w: worldBoundsCenter.z
        glm::vec4 uvScale{1.0f};         // 16 bytes @ offset 160 - xy: tex0 UV scale, zw: tex1 UV scale
    };
    // How many instances one frame may hand the GPU, not how many exist. Ground
    // clutter is what fills it: it is drawn by the thousand and every tuft
    // takes a slot of its own, so at 16384 a dwarf standing in Dun Morogh
    // grass was already losing whole models for a frame at a time - which reads
    // as clutter blinking rather than as anything being over budget. Two
    // buffers of 96 bytes a slot, so this costs 6 MB rather than 3.
    static constexpr uint32_t MAX_INSTANCE_DATA = 32768;
    VkDescriptorSetLayout instanceSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool instanceDescPool_ = VK_NULL_HANDLE;
    ::VkBuffer instanceBuffer_[2] = {};
    VmaAllocation instanceAlloc_[2] = {};
    void* instanceMapped_[2] = {};
    VkDescriptorSet instanceSet_[2] = {};
    uint32_t instanceDataCount_ = 0; // reset each frame in render()

    RtScene* rtScene_ = nullptr;
    // RtScene instances this renderer owns, and per RtScene instance id the
    // sync generation that last saw it and the mesh it places.
    std::vector<uint32_t> rtOwned_;
    std::vector<uint64_t> rtSeen_;
    std::vector<uint32_t> rtMeshOf_;
    uint64_t rtSyncGeneration_ = 0;
    void registerRtModel(M2ModelGPU& gpuModel, const pipeline::M2Model& model);
    void releaseRtModel(M2ModelGPU& gpuModel);

    // Dynamic ribbon vertex buffer (CPU-written triangle strip)
    static constexpr size_t MAX_RIBBON_VERTS = 2048;  // initial ribbonVB_ size, 9 floats each
    /// Frames in flight; equal to VkContext's MAX_FRAMES_IN_FLIGHT (asserted in m2_renderer.cpp).
    static constexpr uint32_t kParticleFrames = 2;
    // One per frame in flight: the GPU may still read frame N's vertices
    // while the CPU writes frame N+1's. A single shared buffer let a run of
    // one emitter draw another emitter's sizes and colours (#108: Stone
    // Rumbler mist drawn as giant rocks and glows).
    ::VkBuffer ribbonVB_[kParticleFrames] = {};
    VmaAllocation ribbonVBAlloc_[kParticleFrames] = {};
    void* ribbonVBMapped_[kParticleFrames] = {};
    // Vertices ribbonVB_ holds; grows to fit every visible trail and beam.
    size_t ribbonVertexCapacity_[kParticleFrames] = {};
    // Geometry-model particle triangles (ribbon layout), grown on demand.
    ::VkBuffer particleMeshVB_[kParticleFrames] = {};
    VmaAllocation particleMeshVBAlloc_[kParticleFrames] = {};
    void* particleMeshVBMapped_[kParticleFrames] = {};
    size_t particleMeshVertexCapacity_[kParticleFrames] = {};

    // Dynamic particle buffers, one per frame in flight
    ::VkBuffer m2ParticleVB_[kParticleFrames] = {};
    VmaAllocation m2ParticleVBAlloc_[kParticleFrames] = {};
    void* m2ParticleVBMapped_[kParticleFrames] = {};
    // Dedicated glow sprite vertex buffer (separate from particle VB to avoid data race)
    static constexpr size_t MAX_GLOW_SPRITES = 8000;
    ::VkBuffer glowVB_[kParticleFrames] = {};
    VmaAllocation glowVBAlloc_[kParticleFrames] = {};
    void* glowVBMapped_[kParticleFrames] = {};
    // Projected-texture quads draped over the ground, rebuilt per frame.
    static constexpr size_t MAX_PROJECTED_VERTS = 65536;
    ::VkBuffer projectedVB_[kParticleFrames] = {};
    VmaAllocation projectedVBAlloc_[kParticleFrames] = {};
    void* projectedVBMapped_[kParticleFrames] = {};
    struct ProjectedDraw {
        uint32_t firstVertex = 0;
        uint32_t vertexCount = 0;
        glm::vec3 origin{0.0f};
    };
    // Key: instance id << 16 | batch index.
    std::unordered_map<uint64_t, ProjectedDraw> projectedDraws_[kParticleFrames];
    struct ProjectedCache {
        std::array<glm::vec3, 4> corners{};
        glm::vec3 origin{0.0f};
        std::vector<projected_texture::Vertex> vertices;
        uint32_t lastFrameSerial = 0;
    };
    std::unordered_map<uint64_t, ProjectedCache> projectedCache_;
    uint32_t projectedFrameSerial_ = 0;
    bool warnedProjectedOverflow_ = false;

    std::unordered_map<uint32_t, M2ModelGPU> models;
    // Grace period for model cleanup: track when a model first became instanceless.
    // Models are only evicted after 60 seconds with no instances.
    std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> modelUnusedSince_;
    std::unordered_set<uint32_t> pinnedModelIds_;
    // Model IDs evicted by cleanupUnusedModels() since the last drain. Owners that
    // cache "already uploaded" model IDs (e.g. TerrainManager) reconcile against
    // this so a reaped model is re-loaded instead of skipped as a stale hit.
    std::vector<uint32_t> reapedModelIds_;
    std::vector<M2Instance> instances;

    // O(1) dedup: key = (modelId, quantized x, quantized y, quantized z) → instanceId
    struct DedupKey {
        uint32_t modelId;
        int32_t qx, qy, qz; // position quantized to 0.1 units
        bool operator==(const DedupKey& o) const {
            return modelId == o.modelId && qx == o.qx && qy == o.qy && qz == o.qz;
        }
    };
    struct DedupHash {
        size_t operator()(const DedupKey& k) const {
            size_t h = std::hash<uint32_t>()(k.modelId);
            h ^= std::hash<int32_t>()(k.qx) * 2654435761u;
            h ^= std::hash<int32_t>()(k.qy) * 40503u;
            h ^= std::hash<int32_t>()(k.qz) * 12289u;
            return h;
        }
    };
    std::unordered_map<DedupKey, uint32_t, DedupHash> instanceDedupMap_;

    uint32_t nextInstanceId = 1;
    uint32_t lastDrawCallCount = 0;
    size_t modelCacheLimit_ = 6000;
    uint32_t modelLimitRejectWarnings_ = 0;

    VkTexture* loadTexture(const std::string& path, uint32_t texFlags = 0);
    std::unordered_map<std::string, pipeline::BLPImage>* predecodedBLPCache_ = nullptr;

    struct TextureCacheEntry {
        std::unique_ptr<VkTexture> texture;
        size_t approxBytes = 0;
        uint64_t lastUse = 0;
    };
    std::unordered_map<std::string, TextureCacheEntry> textureCache;
    struct TextureProperties {
        bool hasAlpha = false;
        bool alphaIsSilhouette = false;
    };
    std::unordered_map<VkTexture*, TextureProperties> texturePropsByPtr_;
    size_t textureCacheBytes_ = 0;
    uint64_t textureCacheCounter_ = 0;
    size_t textureCacheBudgetBytes_ = 2048ull * 1024 * 1024;
    /// Make room for `bytesNeeded` by dropping textures nothing is looking at,
    /// oldest first. Returns what it actually freed.
    ///
    /// The cache hands out raw VkTexture* and a model's batches, particle
    /// emitters and ribbons all keep one, so age alone cannot decide: a
    /// referenced texture would leave those pointers dangling whatever
    /// lastUse says about it. What model unloading leaves behind is exactly
    /// what this collects.
    size_t evictUnreferencedTextures(size_t bytesNeeded);

    std::unordered_set<std::string> failedTextureCache_;
    /// Keys refused for want of budget rather than because the file is bad.
    /// Cleared when eviction frees something, so they are retried at once
    /// instead of waiting out a timer set when the cache was full.
    std::unordered_set<std::string> budgetRejected_;
    std::unordered_map<std::string, uint64_t> failedTextureRetryAt_;
    std::unordered_set<std::string> loggedTextureLoadFails_;
    uint64_t textureLookupSerial_ = 0;
    uint32_t textureBudgetRejectWarnings_ = 0;
    std::unique_ptr<VkTexture> whiteTexture_;
    std::unique_ptr<VkTexture> glowTexture_;
    VkDescriptorSet glowTexDescSet_ = VK_NULL_HANDLE;  // cached glow texture descriptor (allocated once)

    // Optional query-space culling for collision/raycast hot paths.
    CollisionFocus collisionFocus;

    void rebuildSpatialIndex();
    void gatherCandidates(const glm::vec3& queryMin, const glm::vec3& queryMax, std::vector<size_t>& outIndices) const;

    SpatialGrid spatialGrid;
    std::unordered_map<uint32_t, size_t> instanceIndexById;
    // Instance to copy bone matrices from when spawning another of the same
    // model. Finding one by scanning every instance is O(n) per spawn, which
    // became 18ms for a single createInstance once a zone held tens of
    // thousands of them.
    std::unordered_map<uint32_t, uint32_t> boneSeedInstanceByModel_;
    // Collision scratch buffers are thread_local (see m2_renderer.cpp) for thread-safety.

    // Collision query profiling - atomic because getFloorHeight is dispatched
    // on async threads from camera_controller while the main thread reads these.
    mutable std::atomic<double> queryTimeMs{0.0};
    mutable std::atomic<uint32_t> queryCallCount{0};

    // Persistent render buffers (avoid per-frame allocation/deallocation)
    struct VisibleEntry {
        uint32_t index;
        uint32_t modelId;
        float distSq;
        float effectiveMaxDistSq;
        bool groundDetail = false;  // blended clutter: drawn last, far to near
    };
    WmoRoomQuery wmoRoomDrawn_;
    InteriorLightQuery interiorLightQuery_;
    ObjectLightQuery objectLightQuery_;
    RoomAtQuery roomAtQuery_;
    std::vector<VisibleEntry> sortedVisible_;  // Reused each frame
    std::vector<VisibleEntry> transparentVisible_; // Only models needing pass 2
    std::vector<std::pair<float, uint32_t>> transparentBatchOrder_; // pass 2: view z, batch index
    struct GlowSprite {
        glm::vec3 worldPos;
        glm::vec4 color;
        float size;
    };
    std::vector<GlowSprite> glowSprites_;  // Reused each frame

    // Shadow-pass texture descriptor cache (reused each frame, cleared via pool reset)
    std::unordered_map<VkImageView, VkDescriptorSet> shadowTexSetCache_;
    /// The casters this frame, grouped by model: [0] solid, [1] foliage.
    ///
    /// Shadow casters of one frame as (model, instance), sorted by model so
    /// buffers and textures are bound once per model and batch.
    ///
    /// Kept as a member so a frame's worth of casters costs no allocation.
    std::vector<std::pair<uint32_t, uint32_t>> shadowCasters_;

    // Ribbon draw-call list (reused each frame)
    struct RibbonDrawCall {
        VkDescriptorSet texSet;
        VkPipeline      pipeline;
        uint32_t        firstVertex;
        uint32_t        vertexCount;
    };
    std::vector<RibbonDrawCall> ribbonDraws_;
    std::vector<ChainStrip> chainStrips_;
    /// Ribbon-layout descriptor set per chain texture path, from materialDescPool_
    /// (dropped with the pool's reset in clear()).
    std::unordered_map<std::string, VkDescriptorSet> chainTexSets_;
    VkDescriptorSet chainTextureSet(const std::string& path);
    void renderParticleMeshes(VkCommandBuffer cmd, VkDescriptorSet perFrameSet);

    // Particle group structures (reused each frame)
    struct ParticleGroupKey {
        VkTexture* texture;
        uint8_t blendType;
        uint16_t tilesX;
        uint16_t tilesY;
        bool operator==(const ParticleGroupKey& other) const {
            return texture == other.texture &&
                   blendType == other.blendType &&
                   tilesX == other.tilesX &&
                   tilesY == other.tilesY;
        }
    };
    struct ParticleGroupKeyHash {
        size_t operator()(const ParticleGroupKey& key) const {
            size_t h1 = std::hash<uintptr_t>{}(reinterpret_cast<uintptr_t>(key.texture));
            size_t h2 = std::hash<uint32_t>{}((static_cast<uint32_t>(key.tilesX) << 16) | key.tilesY);
            size_t h3 = std::hash<uint8_t>{}(key.blendType);
            return h1 ^ (h2 * 0x9e3779b9u) ^ (h3 * 0x85ebca6bu);
        }
    };
    struct ParticleGroup {
        VkTexture* texture;
        uint8_t blendType;
        uint16_t tilesX;
        uint16_t tilesY;
        VkDescriptorSet preAllocSet = VK_NULL_HANDLE;
    };
    std::unordered_map<ParticleGroupKey, ParticleGroup, ParticleGroupKeyHash> particleGroups_;

    /// One run of particles written to the vertex buffer that share a group.
    ///
    /// The vertices used to be accumulated into a std::vector<float> per
    /// group - nine push_backs a particle - and then memcpy'd into the mapped
    /// buffer. They are written straight into it now, in the order they are
    /// walked, and a run is closed whenever the group changes. Particles from
    /// one emitter cluster together, so a run is roughly an emitter.
    ///
    /// That also fixes the upload: every group used to memcpy to offset zero
    /// and draw from vertex zero, so with more than one group on screen they
    /// all drew whichever group was copied last.
    struct ParticleRun {
        ParticleGroup* group = nullptr;
        uint32_t first = 0;
        uint32_t count = 0;
    };
    std::vector<ParticleRun> particleRuns_;
    // Vertices the BeforeTransparent call wrote this frame; AfterTransparent appends after them.
    uint32_t particleBeforeTransparentEnd_ = 0;
    /// Vertices the particle buffer holds. Not MAX_M2_PARTICLES, which is the
    /// cap on one instance's particles: this has to hold a whole frame's.
    static constexpr size_t MAX_M2_PARTICLE_VERTS = 32768;
    size_t particleVertexCapacity_[kParticleFrames] = {MAX_M2_PARTICLE_VERTS, MAX_M2_PARTICLE_VERTS};

    // Animation update buffers (avoid per-frame allocation)
    std::vector<uint32_t> retiredInstanceIds_;  // drained this update, removed after the particle loop
    std::vector<size_t> boneWorkIndices_;        // Reused each frame
    std::vector<std::future<void>> animFutures_; // Reused each frame
    bool spatialIndexDirty_ = false;

    // Fast-path instance index lists (rebuilt in rebuildSpatialIndex / on create)
    std::vector<size_t> animatedInstanceIndices_;   // hasAnimation && !disableAnimation
    std::vector<size_t> particleOnlyInstanceIndices_; // !hasAnimation && hasParticleEmitters
    std::vector<size_t> particleInstanceIndices_;    // ALL instances with particle emitters


    // M2 particle emitter system
    static constexpr size_t MAX_M2_PARTICLES = 4000;
    std::mt19937 particleRng_{123};
    bool skyMode_ = false;
    // What the sky-model clock diagnostic last reported, so it prints on a
    // restart or once a second rather than every frame. See M2Renderer::update.
    uint32_t skyDiagInstanceId_ = 0;
    float skyDiagAnimTime_ = 0.0f;
    /// Whether the sky model was drawn last frame, so the cull
    /// diagnostic prints on a change rather than every frame.
    bool skyDiagWasDrawn_ = false;
    /// Sky batches that reached a draw call this frame, and the last counts
    /// reported, so the line prints on a change rather than every frame.
    uint32_t skyDiagDrawsOpaque_ = 0;
    uint32_t skyDiagDrawsTransparent_ = 0;
    uint32_t skyDiagLastOpaque_ = 0xFFFFFFFFu;
    uint32_t skyDiagLastTransparent_ = 0xFFFFFFFFu;

    // Cached camera state from update() for frustum-culling bones
    glm::vec3 cachedCamPos_ = glm::vec3(0.0f);
    float cachedMaxRenderDistSq_ = 0.0f;
    float smoothedRenderDist_ = 1000.0f;  // Smoothed render distance to prevent flickering
    /// The distance as asked for, kept so the scale can be rebuilt when the
    /// detail setting changes without the caller having to say it again.
    float viewDistanceRaw_ = 1200.0f;
    float environmentDetail_ = 1.0f;
    float groundDetailMaxDistance_ = 0.0f;   // 0 = no cap of its own
    void recomputeViewDistanceScale() {
        viewDistanceScale_ = (viewDistanceRaw_ / 1200.0f) * environmentDetail_;
    }
    float viewDistanceScale_ = 1.0f;
    float viewDistanceAbsolute_ = 1200.0f;
    /// Drop the sky model's baked star layer, because something else is
    /// drawing stars. See BatchGPU::starLayer.
    bool suppressBakedStars_ = false;
    float particleDensity_ = 1.0f;
    glm::vec3 outdoorLightDir_{0.0f, 0.0f, -1.0f};
    glm::vec3 outdoorDiffuse_{0.0f, 0.0f, 0.0f};
    glm::vec3 outdoorAmbient_{1.0f, 1.0f, 1.0f};
    float furthestDrawnSq_ = 0.0f;
    bool forceNoCull_ = false;

    // Thread count for parallel bone animation
    uint32_t numAnimThreads_ = 1;

    float interpFloat(const pipeline::M2AnimationTrack& track, float animTime,
                      float globalTime, int seqIdx,
                      const std::vector<uint32_t>& globalSeqDurations);
    float interpFBlockFloat(const pipeline::M2FBlock& fb, float lifeRatio);
    glm::vec3 interpFBlockVec3(const pipeline::M2FBlock& fb, float lifeRatio);
    void emitParticles(M2Instance& inst, const M2ModelGPU& gpu, float dt);
    void updateParticles(M2Instance& inst, float dt);
    // emit: lay edges at edgesPerSecond (the first at once); finalEdge: one more
    // edge where the emitter is now, then no more.
    void updateRibbons(M2Instance& inst, const M2ModelGPU& gpu, float dt, bool emit = true, bool finalEdge = false);

    // Helper to allocate descriptor sets
    VkDescriptorSet allocateMaterialSet();
    VkDescriptorSet allocateBoneSet();

    // Helper to destroy model GPU resources
    void destroyModelGPU(M2ModelGPU& model);
    // Helper to destroy instance bone buffers.
    // When defer=true, destruction is scheduled via deferAfterFrameFence so
    // in-flight command buffers are not invalidated (use for streaming unload).
    /// Starts a new instance's animation and seeds its bones from a
    /// sibling of the same model, so it draws on the frame it spawns.
    /// Both spawn paths need it and each used to have its own copy.
    void seedInstanceAnimation(const M2ModelGPU& model, uint32_t modelId,
                               M2Instance& instance);
    void seedUnanimatedSequence(const M2ModelGPU& model, M2Instance& instance);

    void destroyInstanceBones(M2Instance& inst, bool defer = false);
};

} // namespace rendering
} // namespace wowee
