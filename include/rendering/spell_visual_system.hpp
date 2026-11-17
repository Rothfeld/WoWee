#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <glm/glm.hpp>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class M2Renderer;
class Renderer;
class CharacterRenderer;

class SpellVisualSystem {
public:
    SpellVisualSystem() = default;
    ~SpellVisualSystem() = default;

    // Initialize with references to the M2 renderer and parent renderer
    void initialize(M2Renderer* m2Renderer, Renderer* renderer);
    void shutdown();

    // Spawn a spell visual at a world position.
    // useImpactKit=false → CastKit path; useImpactKit=true → ImpactKit path
    // attachInstanceId: the CASTER's CharacterRenderer instance for hand/chest/
    // head bone tracking (0 = static effect at worldPosition). Effects used to
    // attach to the local player unconditionally, so every nearby unit's cast
    // kit landed on the player's hands.
    void playSpellVisual(uint32_t visualId, const glm::vec3& worldPosition,
                         bool useImpactKit = false, uint32_t attachInstanceId = 0);

    void playSpellVisualKit(uint32_t kitId, const glm::vec3& worldPosition, uint32_t attachInstanceId);
    // SpellVisual InstantAreaKit (TBC column 22), at the caster when the spell
    // goes. Original 2.4.3 (_experiments/20261117T165200Z.pair4_kits): Summon
    // Boar Spirit 8286 kit 9209 DarkRitual_PreCast_Base under the caster while
    // the spirit appears yards away; Summon Skeleton 261 kit 9161
    // CallLightning_Impact at the caster; Quillboar Channeling 7083 kit 9191
    // Frost_Nova_state under the casting Ward Keeper, not its target.
    void playInstantAreaKit(uint32_t visualId, const glm::vec3& casterPosition,
                            uint32_t casterInstanceId);
    /// One SpellVisualEffectName.dbc model at a unit's M2 attachment, e.g. a
    /// UnitBlood.dbc combat blood spurt at ChestBloodFront (15).
    void playEffectAtAttachment(uint32_t effectNameId, uint32_t attachInstanceId, uint32_t attachment);
    void startChannelVisual(uint32_t visualId, const glm::vec3& position,
                            uint32_t durationMs, uint32_t attachInstanceId);
    void updateChannelVisual(uint32_t attachInstanceId, uint32_t remainingMs);
    // SpellVisual.dbc Precast/Cast/ChannelKit -> SpellVisualKit.dbc AnimID
    // (column 2, an AnimationData.dbc id). kNoAnimation when the stage has no
    // kit or its kit names no animation (-1). Column 1 is not an animation:
    // it holds 0-7 and groups kits by stage (0 precast/cast, 1 impact,
    // 2 state/channel in TBC 2.4.3 data).
    static constexpr uint32_t kNoAnimation = 0xffffffffu;
    uint32_t precastAnimation(uint32_t visualId);
    uint32_t castAnimation(uint32_t visualId);
    uint32_t channelAnimation(uint32_t visualId);
    // Persistent StateKit effects follow the unit's current aura set.
    void syncAuraVisuals(uint32_t attachInstanceId, const std::vector<uint32_t>& visualIds,
                         bool observerConcealed);

    // Spawn a precast visual effect at a world position.
    // castTimeMs: server cast time in milliseconds (0 = use anim duration).
    // attachInstanceId: see playSpellVisual.
    void playSpellVisualPrecast(uint32_t visualId, const glm::vec3& worldPosition,
                                uint32_t castTimeMs = 0, uint32_t attachInstanceId = 0);

    // Whether this visual has a missile model (SpellVisual.dbc MissileModel).
    bool hasSpellMissile(uint32_t visualId);

    // Fly the visual's missile model from start to end at speed yards a second
    // (Spell.dbc Speed). With impactAtEnd the impact kit plays where it lands,
    // instead of when the server reports the hit. False when nothing launched.
    bool playSpellMissile(uint32_t visualId, const glm::vec3& start, const glm::vec3& end,
                          float speed, bool impactAtEnd);

    // Launch a physical weapon projectile (arrow, bullet, or thrown item)
    // without invoking the spell visual pipeline.
    void playPhysicalProjectile(const std::string& modelPath,
                                const std::string& texturePath,
                                const glm::vec3& start,
                                const glm::vec3& end,
                                float duration,
                                bool spin);

    // Advance lifetime timers and remove expired instances.
    void update(float deltaTime);

    // Remove all active precast visual instances (cast canceled/interrupted).
    void cancelAllPrecastVisuals();

    // Remove all active spell visual instances and reset caches.
    // Called on map change / combat reset.
    void reset();

private:
    // Spell visual effects - transient M2 instances spawned by SMSG_PLAY_SPELL_VISUAL/IMPACT
    struct SpellVisualInstance {
        uint32_t instanceId;
        float elapsed;
        float duration;  // per-instance lifetime in seconds (from M2 anim or default)
        bool isPrecast;  // true for precast effects (removed on cancel/interrupt)
        uint32_t attachmentId;  // character attachment point to track (0=none/static)
        uint32_t attachInstanceId;  // CharacterRenderer instance the attachment belongs to
        bool isChannel = false;
        uint32_t auraVisualId = 0;
        bool concealmentDetectionMarker = false;
        bool decaying = false;
        // Follows the attachment's full transform, not only its position.
        bool orientWithAttachment = false;
    };

    struct PhysicalProjectile {
        uint32_t instanceId = 0;
        glm::vec3 start{0.0f};
        glm::vec3 end{0.0f};
        glm::vec3 rotation{0.0f};
        float elapsed = 0.0f;
        float duration = 0.0f;
        bool spin = false;
    };

    struct SpellMissile {
        uint32_t instanceId = 0;
        uint32_t visualId = 0;
        glm::vec3 start{0.0f};
        glm::vec3 end{0.0f};
        float elapsed = 0.0f;
        float duration = 0.0f;
        bool impactAtEnd = false;
    };

    void loadSpellVisualDbc();
    uint32_t kitAnimation(const std::unordered_map<uint32_t, uint32_t>& kits, uint32_t visualId);
    // duration <= 0: the model's own animation length.
    void spawnKitEffects(uint32_t kitId, const glm::vec3& worldPosition, uint32_t attachInstanceId,
                         float duration, bool isPrecast);
    std::unordered_map<uint32_t, uint32_t> kitAnimations_;
    struct KitEffect { std::string path; uint32_t attachment; };
    std::unordered_map<uint32_t, std::vector<KitEffect>> kitEffects_;
    std::unordered_map<uint32_t, std::string> effectPaths_;  // SpellVisualEffectName ID -> .m2 path
    std::unordered_map<uint32_t, uint32_t> channelKits_;
    std::unordered_map<uint32_t, uint32_t> precastKits_;
    std::unordered_map<uint32_t, uint32_t> stateKits_;
    std::unordered_map<uint32_t, uint32_t> castKits_;
    std::unordered_map<uint32_t, uint32_t> impactKits_;
    std::unordered_map<uint32_t, uint32_t> instantAreaKits_;
    /// M2 attachment 17, named Breath. SpellVisualKit's BreathEffect slot is
    /// placed there and aimed along it: Creature\FireElemental\FireElemental.m2
    /// carries it on bone 82 in front of the face, bone +X forward.
    static constexpr uint32_t kAttachBreath = 17;
    struct UnitAuraVisuals {
        std::vector<uint32_t> ids;
        bool observerConcealed = false;
    };
    std::unordered_map<uint32_t, UnitAuraVisuals> unitAuraVisuals_;
    // Model ID for a cast, impact or missile model, loading it on first use.
    // 0 when it cannot be loaded.
    uint32_t loadSpellModel(const std::string& modelPath);
    void placeSpellMissile(const SpellMissile& missile, float t);

    M2Renderer* m2Renderer_ = nullptr;
    Renderer* renderer_ = nullptr;
    pipeline::AssetManager* cachedAssetManager_ = nullptr;

    std::vector<SpellVisualInstance> activeSpellVisuals_;
    void placeOnUnit(const SpellVisualInstance& visual, CharacterRenderer* characters,
                     const glm::vec3& position);
    std::vector<PhysicalProjectile> physicalProjectiles_;
    std::vector<SpellMissile> spellMissiles_;
    std::unordered_map<uint32_t, std::string> spellVisualMissilePath_; // visualId → missile M2 path
    std::unordered_map<std::string, uint32_t> spellVisualModelIds_;   // M2 path → M2Renderer modelId
    std::unordered_set<uint32_t> spellVisualFailedModels_;           // modelIds that failed to load (negative cache)
    uint32_t nextSpellVisualModelId_ = 999000; // Reserved range 999000-999799
    uint32_t nextProjectileModelId_ = 998000;  // Reserved range 998000-998999
    std::unordered_map<std::string, uint32_t> projectileModelIds_;
    bool spellVisualDbcLoaded_ = false;
    static constexpr float SPELL_MISSILE_MAX_DURATION = 4.0f;
};

} // namespace rendering
} // namespace wowee
