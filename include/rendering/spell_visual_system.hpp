#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <glm/glm.hpp>
#include "rendering/camera_shake.hpp"
#include "rendering/chain_effect.hpp"
#include "rendering/missile_homing.hpp"
#include "rendering/spell_missile_motion.hpp"
#include "rendering/spell_visual_kit_policy.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/spell_launch_origin.hpp"
#include "rendering/weapon_trail.hpp"

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
    /// One end of a cast kit's beam: a unit (its render instance, chest 34,
    /// followed while the beam lasts) or, with instanceId 0, a fixed point.
    struct ChainEnd {
        uint32_t instanceId = 0;
        glm::vec3 position{0.0f};
        uint32_t attachment = 34;  // M2 attachment followed while the unit is drawn
    };
    /// SMSG_SPELL_GO's CastKit: its effects at the caster, and, where the
    /// kit's CharProc 0 names a SpellChainEffects row, a beam from the caster
    /// to every end for as long as the kit plays (its longest effect model).
    /// Build 8606, pair 12 (_experiments/20261118T124000Z.astromancer_glow
    /// fork-*.jpg): Forked Lightning 38145 (CastKit 965, chain 95
    /// ShockLightning, Lightning_Cast_Hand 1 s) from a Voidwalker to both
    /// players it missed, lit 0.92-1.0 s. A kit without effect models lasts
    /// the spell's duration (spellSeconds): Essence of Anger's Spite 41376
    /// (kit 7979, chain 220, 6 s aura) held its beams 6.1 s, until 41377
    /// (spite2.json). With neither (42784's kit 8183) the beam is not drawn.
    /// `chained`: a jumping spell's beam runs caster -> ends[0] -> ends[1] ...
    void playCastKit(uint32_t visualId, const glm::vec3& casterPosition, uint32_t casterInstanceId,
                     const std::vector<ChainEnd>& ends, bool chained, float spellSeconds);
    // SpellVisual InstantAreaKit (TBC column 22) plays at the SMSG_SPELL_GO
    // destination (TARGET_FLAG_DEST_LOCATION), unattached; a spell with no
    // destination plays none. Build 8606, pair 12
    // (_experiments/20261118T124000Z.astromancer_glow): Bonechewer Behemoth's
    // Meteor 41276 (dest = its target player) drops Meteor_Impact_Base on the
    // player; Smoke Bomb 7964 and Summon Burning Exile 4929 (dest = the
    // caster's own position) at the caster; Volcanic Geyser 42052 (source
    // location only) shows none (_experiments/20261118T141843Z.supremus_ground).
    void playInstantAreaKit(uint32_t visualId, const glm::vec3& destination);
    /// SpellVisual CasterImpactKit (TBC column 13) on the caster at
    /// SMSG_SPELL_GO. Build 8606, pair 24: a player under Pillar of Fire
    /// 43218 casts Burn 43217 every second, and kit 8255 (Flamestrike
    /// Impact (2x), Fire ImpactDD Uber Base) puts a column of fire on them
    /// each time; Zul'jin's invisible Column of Fire (24187) does the same.
    /// The caller plays it only for a spell that hit at least one unit.
    void playCasterImpactKit(uint32_t visualId, const glm::vec3& casterPosition, uint32_t casterInstanceId);
    /// SpellVisual ImpactAreaKit at the spell's destination. Spectral Blast
    /// 46648 (visual 10441, kit 9254 Shadow_Fissure_Base): the pink beams and
    /// dark shards native draws where the Spectral Rift opens.
    void playImpactAreaKit(uint32_t visualId, const glm::vec3& destination);
    /// One SpellVisualEffectName.dbc model at a unit's M2 attachment, e.g. a
    /// UnitBlood.dbc combat blood spurt at ChestBloodFront (15).
    void playEffectAtAttachment(uint32_t effectNameId, uint32_t attachInstanceId, uint32_t attachment);
    /// HARDCODED Resist Spell (SpellVisualEffectName 3207, spells\resist_immune_effect.m2):
    /// directional dark shockwave dome placed at `impactPosition` and oriented
    /// along `incomingDirection` (caster -> victim, so its -X dome faces the caster).
    /// Native 2.4.3 plays this at missile arrival on resisted/immune spell missiles.
    void playResistImmuneEffect(const glm::vec3& impactPosition, const glm::vec3& incomingDirection);
    void startChannelVisual(uint32_t visualId, const glm::vec3& position,
                            uint32_t durationMs, uint32_t attachInstanceId);
    void updateChannelVisual(uint32_t attachInstanceId, uint32_t remainingMs);
    /// A unit's channel as its UNIT_CHANNEL_SPELL / UNIT_FIELD_CHANNEL_OBJECT
    /// fields hold it. start and end are the beam's ends, valid with hasTarget.
    struct UnitChannel {
        uint64_t casterGuid = 0;
        uint32_t casterInstanceId = 0;
        uint32_t visualId = 0;
        glm::vec3 casterPosition{0.0f};
        bool hasTarget = false;
        glm::vec3 start{0.0f};
        glm::vec3 end{0.0f};
        uint32_t spellId = 0;
        // Further targets a jumping channel's beam runs on to: end -> hops[0] -> hops[1] ...
        std::vector<glm::vec3> hops;
    };
    /// A jumping channel's targets (EffectChainTarget above 1) from its
    /// SMSG_SPELL_GO, hit then missed, in packet order. Native High Priestess Jeklik's Mind
    /// Flay 23953 (chain targets 5, ChannelKit 430 -> chain 4) runs from her
    /// to the first player and on to the second; the channel fields name
    /// only the first.
    void setChannelTargets(uint64_t casterGuid, uint32_t spellId, std::vector<uint64_t> targets);
    /// The targets set for this caster's channel of `spellId`, else none.
    std::vector<uint64_t> channelTargets(uint64_t casterGuid, uint32_t spellId) const;
    /// Every unit channelling now, once a frame. A channel that began before
    /// its caster came into view (no MSG_CHANNEL_START) gets its ChannelKit
    /// effects here, held until the fields clear; one with a target draws the
    /// ChannelKit's SpellChainEffects beam (CharProc 0) from start to end.
    void syncChannels(const std::vector<UnitChannel>& channels);
    /// MSG_CHANNEL_UPDATE 0 ended this unit's channel: its beam and held
    /// kit go now, though the fields still name the channel until the next
    /// object update (0.27-0.35 s later with CMaNGOS). Ends at the unit's
    /// next MSG_CHANNEL_START or once its fields clear.
    void endChannel(uint64_t casterGuid);
    void startChannel(uint64_t casterGuid);
    [[nodiscard]] bool channelEnded(uint64_t casterGuid) const { return endedChannels_.contains(casterGuid); }
    // SpellVisual.dbc Precast/Cast/ChannelKit -> SpellVisualKit.dbc AnimID
    // (column 2, an AnimationData.dbc id). kNoAnimation when the stage has no
    // kit or its kit names no animation (-1). Column 1 is not an animation:
    // it holds 0-7 and groups kits by stage (0 precast/cast, 1 impact,
    // 2 state/channel in TBC 2.4.3 data).
    static constexpr uint32_t kNoAnimation = 0xffffffffu;
    uint32_t precastAnimation(uint32_t visualId);
    uint32_t castAnimation(uint32_t visualId);
    uint32_t channelAnimation(uint32_t visualId);
    [[nodiscard]] bool hasPrecastSound(uint32_t visualId);
    [[nodiscard]] bool hasCastSound(uint32_t visualId);
    [[nodiscard]] bool hasImpactSound(uint32_t visualId);
    // Persistent StateKit effects follow the unit's current aura set, and so
    // do the kits' character procedures that change the unit's own model:
    // SpellVisualKit CharProc 1 multiplies the model's colour by CharParamZero
    // (an 0xRRGGBB integer stored as a float: Ghost kit 989 0x8CB9FD,
    // Immolate kit 235 0xE75641, Vertex Color Black kit 7641 0x000000) and
    // CharProc 14 sets its opacity to CharParamZero (Transparency (75%) 37802
    // kit 6858 0.25, Ghost 0.5). The most recently applied aura's kit wins for
    // each, and removing it returns the previous one (original 2.4.3, pair 0,
    // _experiments/20261117T181105Z.pair0_ghost_names_gossip).
    void syncAuraVisuals(uint32_t attachInstanceId, const std::vector<uint32_t>& visualIds,
                         bool observerConcealed);
    // The animation the unit holds while its auras last: the StateKit
    // animation (column 2) of its most recently applied aura that has one,
    // else kNoAnimation. Brood Power: Bronze (22291, kit 5769, Whirlwind 126)
    // applied with .aura spins the original 2.4.3 character for its five
    // seconds without any cast or channel; Rend Blackhand's Whirlwind (13736,
    // kit 370) spins through its two-second aura the same way.
    uint32_t auraStateAnimation(uint32_t attachInstanceId) const;

    /// Grace window past the server cast time so a precast visual does not
    /// flicker out from network/tick jitter before SMSG_SPELL_GO arrives.
    /// Explicit cancellation on SMSG_SPELL_GO / SMSG_SPELL_FAILURE / cancel
    /// retires the precast immediately when the cast ends.
    static constexpr float kPrecastGraceSec = 2.0f;

    /// Initial lifetime in seconds for a PrecastKit spawned from SMSG_SPELL_START.
    /// Returns -1 when castTimeMs == 0 (fall back to the M2's authored length).
    static constexpr float precastDurationSeconds(uint32_t castTimeMs) {
        return castTimeMs > 0
            ? (static_cast<float>(castTimeMs) / 1000.0f) + kPrecastGraceSec
            : -1.0f;
    }

    // Spawn a precast visual effect at a world position.
    // castTimeMs: server cast time in milliseconds (0 = use anim duration).
    // attachInstanceId: see playSpellVisual.
    void playSpellVisualPrecast(uint32_t visualId, const glm::vec3& worldPosition,
                                uint32_t castTimeMs = 0, uint32_t attachInstanceId = 0);

    /// Extend active precast visuals on attachInstanceId by delayMs when
    /// SMSG_SPELL_DELAYED pushes a cast back (local or remote caster).
    void delayPrecastVisuals(uint32_t attachInstanceId, uint32_t delayMs);

    /// Ensure active precast visuals on attachInstanceId last at least
    /// remainingMs from now (SMSG_RESUME_CAST_BAR).
    void extendPrecastVisualsToRemaining(uint32_t attachInstanceId, uint32_t remainingMs);

    // A DynamicObject's ground effect: the visual's PersistentAreaKit models at
    // worldPosition, held until stopPersistentArea with the same key, and the
    // kit's area impacts (CharProc 9) falling within radius yards of it.
    void playPersistentArea(uint64_t key, uint32_t visualId, const glm::vec3& worldPosition, float radius);
    void stopPersistentArea(uint64_t key);

    // Whether this visual has a missile model (SpellVisual.dbc MissileModel).
    bool hasSpellMissile(uint32_t visualId);

    // SpellVisual MissileAttachment (-1 default) and MissileDestinationAttachment
    // of a missile visual. Only TBC declares the columns.
    struct MissileAttachments { int32_t launch = -1; uint32_t destination = 0; };
    std::optional<MissileAttachments> missileAttachments(uint32_t visualId);
    /// The caster's attachment a channel or cast kit beam starts at.
    uint32_t beamStartAttachment(uint32_t visualId);
    /// Caster-side beam start origin resolved against the caster's M2 ($CSL/$CSR/$CST
    /// on the channel animation when MissileAttachment == -1, else chest 34).
    SpellLaunchOrigin beamStartOrigin(uint32_t visualId, const pipeline::M2Model* casterModel);

    using MissileTargetResolver = std::function<std::optional<MissileTargetSample>()>;

    // Missile of the visual from start to end. It lands distance / speed
    // seconds from now (Spell.dbc Speed, yards a second) and leaves the caster
    // launchDelay seconds from now, flying whatever speed that leaves. A delay
    // at or past the landing time draws it at the hand, then at the target in
    // the next frame. With impactAtEnd the impact
    // kit plays at the landing instead of when the server reports the hit.
    // False when nothing was scheduled.
    // areaImpactAtEnd: a missile to a ground point plays the visual's
    // ImpactAreaKit where it lands instead of the impact kit.
    // When casterInstanceId != 0, start is re-sampled live from the caster's
    // animated bone/attachment at t = launchDelay so the missile leaves the
    // extended hand instead of the stale Precast/Stand pose from SMSG_SPELL_GO.
    // targetResolver: when non-empty, samples the target unit's live impact
    // position every frame so unit-targeted missiles home onto moving targets.
    bool playSpellMissile(uint32_t visualId, const glm::vec3& start, const glm::vec3& end,
                          float speed, float launchDelay, bool impactAtEnd, bool areaImpactAtEnd = false,
                          bool resistEffectAtEnd = false, std::function<void()> onArrive = {},
                          uint32_t casterInstanceId = 0, SpellLaunchOrigin launchOrigin = {},
                          uint32_t targetInstanceId = 0, uint32_t destinationAttachment = kAttachChestBloodFrontId,
                          MissileTargetResolver targetResolver = {});

    // Launch a physical weapon projectile (arrow, bullet, or thrown item)
    // without invoking the spell visual pipeline.
    // targetResolver + speed > 0 enables per-frame homing toward a moving unit.
    void playPhysicalProjectile(const std::string& modelPath,
                                const std::string& texturePath,
                                const glm::vec3& start,
                                const glm::vec3& end,
                                float duration,
                                bool spin,
                                MissileTargetResolver targetResolver = {},
                                float speed = 0.0f,
                                bool stickOnHit = false);

    // Advance lifetime timers and remove expired instances.
    void update(float deltaTime);

    /// The unit drawn by attachInstanceId died: its own precast, cast and
    /// channel kits, cast kit beams, and CharProc 13 kit light go at once, the
    /// particles already out with them, also those of such kits that ended just
    /// before (the interrupt's SMSG_SPELL_FAILURE comes ahead of the death).
    void removeCasterStages(uint32_t attachInstanceId);

    /// Clear any active SpellVisualKit CharProc 13 light tint on attachInstanceId.
    void clearUnitKitLight(uint32_t attachInstanceId);

    /// The precast kit of the unit drawn by attachInstanceId ends: its cast
    /// was interrupted or failed (SMSG_SPELL_FAILURE), its caster died
    /// mid-cast included. Other units' precasts go on.
    void cancelPrecastVisuals(uint32_t attachInstanceId);

    /// Re-bind active spell visuals, aura visuals, channel visuals, and beams
    /// when a unit swaps render instance (e.g. player/creature morph IN/OUT).
    void transferUnitVisuals(uint32_t fromInstanceId, uint32_t toInstanceId);

    /// True if `playSpellVisual(..., useImpactKit=true, attachInstanceId)` has
    /// already fired `visualId` on this unit within `kRecentImpactDedupSeconds`,
    /// avoiding duplicate ImpactKit playback when `SMSG_SPELL_GO` precedes the
    /// `SMSG_UPDATE_OBJECT` aura field update.
    static constexpr float kRecentImpactDedupSeconds = 1.0f;
    bool hasRecentImpactVisual(uint32_t attachInstanceId, uint32_t visualId) const;

    // Remove all active spell visual instances and reset caches.
    // Called on map change / combat reset.
    void reset();

    /// M2 attachment IDs used by SpellVisualKit slots.
    static constexpr uint32_t kAttachRightWeapon = 1;
    static constexpr uint32_t kAttachLeftWeapon = 2;
    static constexpr uint32_t kAttachBreath = 17;
    static constexpr uint32_t kAttachSpellLeftHand = 21;
    static constexpr uint32_t kAttachSpellRightHand = 22;
    static constexpr uint32_t kAttachSpecial1 = 23;
    static constexpr uint32_t kAttachSpecial3 = 25;

    // Slots whose effect takes the attachment bone's full orientation, every
    // frame, and not only the unit's yaw: HandRight/HandLeft 1-2 (held weapon
    // effects such as Brewfest Mug, Flaming Torch, Mining Pick), Breath 17,
    // SpellLeftHand/SpellRightHand 21-22, and Special 23-25.
    // - Onyxia's Flame Breath 18435 (cast kit 4450 SpecialEffect0,
    //   dragonflamebreath.m2): the fire first dives with her head to the ground
    //   ahead of her, then climbs as the SpellCastDirected head lifts.
    // - Mage Cone of Cold 120 (cast kit 1027 LeftHandEffect, ConeofCold_Hand.m2):
    //   all 11 emitters fire along local -Z, which SpellLeftHand (21) aims
    //   forward during SpellCastDirected (anim 53); yaw-only placement aimed it
    //   straight down into the ground.
    // Head 18 (overhead anchor), Base 19, Chest 34, and World 0 stay upright
    // with the unit's yaw only.
    static constexpr bool slotFollowsBone(uint32_t attachment) {
        return attachment == kAttachRightWeapon ||
               attachment == kAttachLeftWeapon ||
               attachment == kAttachBreath ||
               attachment == kAttachSpellLeftHand ||
               attachment == kAttachSpellRightHand ||
               (attachment >= kAttachSpecial1 && attachment <= kAttachSpecial3);
    }

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
        // SpellVisualEffectName.Scale, times whatever the placement scales by.
        float effectScale = 1.0f;
        bool isCast = false;  // the caster's CastKit (SMSG_SPELL_GO)
    };

    struct PhysicalProjectile {
        uint32_t instanceId = 0;
        glm::vec3 start{0.0f};
        glm::vec3 end{0.0f};
        glm::vec3 pos{0.0f};
        glm::vec3 dir{1.0f, 0.0f, 0.0f};
        glm::vec3 rotation{0.0f};
        float elapsed = 0.0f;
        float duration = 0.0f;
        float speed = 0.0f;
        float distanceTraveled = 0.0f;
        bool spin = false;
        bool stickOnHit = false;
        bool stuck = false;
        float stuckElapsed = 0.0f;
        uint32_t targetInstanceId = 0;
        uint32_t targetAttachmentId = kAttachChestBloodFrontId;
        StuckArrowLocalPose stuckLocal;
        MissileTargetResolver targetResolver;
    };

    struct SpellMissile {
        uint32_t instanceId = 0;     // 0 until launched
        uint32_t modelId = 0;
        uint32_t visualId = 0;
        glm::vec3 start{0.0f};
        glm::vec3 end{0.0f};
        glm::vec3 pos{0.0f};
        glm::vec3 dir{1.0f, 0.0f, 0.0f};
        float elapsed = 0.0f;
        float launchAt = 0.0f;       // seconds after scheduling
        float duration = 0.0f;       // landing, seconds after scheduling
        float nominalSpeed = 0.0f;   // Spell.dbc Speed (yards/s)
        float animDurationSec = 0.0f;// M2 primary animation length in seconds
        float flightSpeed = 0.0f;    // pursuit speed after launchAt (0 = close-range 1-frame cross)
        float distanceTraveled = 0.0f;
        uint32_t targetAttachInstanceId = 0;
        bool impactAtEnd = false;
        float scale = 1.0f;          // SpellVisualEffectName.Scale of the missile model
        bool areaImpactAtEnd = false;
        bool resistEffectAtEnd = false;
        bool landed = false;         // drawn at its end; lands next frame
        std::function<void()> onArrive;
        uint32_t casterInstanceId = 0;
        SpellLaunchOrigin launchOrigin;
        uint32_t targetInstanceId = 0;
        uint32_t destinationAttachment = kAttachChestBloodFrontId;
        MissileTargetResolver targetResolver;
        uint32_t motionId = 0;       // SpellVisual.MissileMotion -> SpellMissileMotion.dbc
        uint32_t pathType = 0;       // SpellVisual.MissilePathType (2 = parabola fallback)
        uint32_t dbcDestinationAttachment = 0;  // SpellVisual.MissileDestinationAttachment (2 = base/ground)
        int missileIndex = 0;
        int missileCount = 1;
        float rand1 = 0.5f;
        float rand2 = 0.5f;
        float rand3 = 0.5f;
        float startDistance = 0.0f;
        float progress = 0.0f;
        glm::vec3 prevWorldPos{0.0f};
        bool hasPrevWorldPos = false;
    };

    void loadSpellVisualDbc();
    float effectScale(uint32_t effectNameId) const;
    uint32_t kitAnimation(const std::unordered_map<uint32_t, uint32_t>& kits, uint32_t visualId);
    // duration <= 0: the model's own animation length.
    // Returns the longest lifetime given to an effect (0 when none spawned).
    enum class KitStage : uint8_t { Precast, Cast, Other };
    // Retires an effect; a caster's own precast, cast or channel effect whose
    // particles are still out is remembered until they are gone, for
    // removeCasterStages.
    void retireEffect(const SpellVisualInstance& effect);
    struct DrainingEffect {
        uint32_t attachInstanceId = 0;
        uint32_t instanceId = 0;
    };
    std::vector<DrainingEffect> drainingCasterEffects_;
    float spawnKitEffects(uint32_t kitId, const glm::vec3& worldPosition, uint32_t attachInstanceId,
                          float duration, KitStage stage);
    uint32_t playKitSound(uint32_t kitId, const glm::vec3& worldPosition, uint32_t attachInstanceId,
                          bool stoppable);
    void playKitShake(uint32_t kitId, const glm::vec3& worldPosition);
    void loadCameraShakes();
    void stopPrecastSound(uint32_t attachInstanceId);
    void stopChannelSound(uint32_t attachInstanceId);
    std::unordered_map<uint32_t, uint32_t> kitAnimations_;
    std::unordered_map<uint32_t, uint32_t> kitSounds_;       // SpellVisualKit col 15 -> SoundEntries ID
    std::unordered_map<uint32_t, uint32_t> kitShakes_;       // SpellVisualKit col 16 -> SpellEffectCameraShakes ID
    std::unordered_map<uint32_t, uint32_t> kitFlags_;        // SpellVisualKit col 37 -> Flags
    CameraShakeTable cameraShakeTable_;
    std::unordered_map<uint32_t, uint32_t> activePrecastSounds_;  // attachInstanceId -> AudioEngine soundId
    std::unordered_map<uint32_t, uint32_t> activeChannelSounds_;  // attachInstanceId -> AudioEngine soundId
    std::unordered_map<uint64_t, uint32_t> activeAreaSounds_;     // persistentArea key -> AudioEngine soundId
    // scale: SpellVisualEffectName.Scale, the size the effect is drawn at.
    struct KitEffect { std::string path; uint32_t attachment; float scale = 1.0f; };
    std::unordered_map<uint32_t, std::vector<KitEffect>> kitEffects_;
    std::unordered_map<uint32_t, std::string> effectPaths_;  // SpellVisualEffectName ID -> .m2 path
    std::unordered_map<uint32_t, float> effectScales_;       // SpellVisualEffectName ID -> Scale
    std::unordered_map<uint32_t, uint32_t> channelKits_;
    std::unordered_map<uint32_t, uint32_t> precastKits_;
    std::unordered_map<uint32_t, uint32_t> stateKits_;
    std::unordered_map<uint32_t, uint32_t> castKits_;
    std::unordered_map<uint32_t, uint32_t> impactKits_;
    std::unordered_map<uint32_t, uint32_t> instantAreaKits_;
    std::unordered_map<uint32_t, uint32_t> impactAreaKits_;
    std::unordered_map<uint32_t, uint32_t> casterImpactKits_;
    std::unordered_map<uint32_t, uint32_t> targetImpactKits_;
    std::unordered_map<uint32_t, uint32_t> persistentAreaKits_;
    std::unordered_map<uint64_t, std::vector<uint32_t>> persistentAreas_;  // key -> M2 instances
    // Slots whose effect is not drawn on a model lacking their attachment:
    // weapon hands 1/2, spell hands 21/22, and breath 17 (others fall back to the unit position).
    static constexpr bool slotSkippedWhenAbsent(uint32_t attachment) {
        return attachment == kAttachRightWeapon || attachment == kAttachLeftWeapon ||
               attachment == kAttachSpellLeftHand || attachment == kAttachSpellRightHand ||
               attachment == kAttachBreath;
    }
    /// SpellVisualKit CharProc 0: CharParamZero is a SpellChainEffects.dbc id,
    /// the beam drawn while the kit plays. All 284 TBC kits with procedure 0
    /// and a parameter name an existing record; Green Beam's (33346) channel
    /// kit 6674 names 269 (SoulBeam.blp), Drain Life's kit 402 names 8.
    static constexpr uint32_t kCharProcChain = 0;
    /// SpellVisualKit CharProc 12 names a SpellChainEffects.dbc row the same
    /// way. All 34 TBC kits with it name existing rows, 33 of them cast kits:
    /// Chain Lightning 421 (kit 321 -> 1 Lightning.blp), Chain Heal 1064
    /// (3169 -> 2 HealBeam.blp), C'Thun's Eye Beam 26134 (6480 -> 18
    /// SoulBeam.blp). Native build 8606 draws Eye Beam's green wave from the
    /// Eye of C'Thun to the players it hits
    /// (_experiments/20261118T1800Z.cthun_beams in the workspace root).
    static constexpr uint32_t kCharProcCastChain = 12;
    static constexpr uint32_t kCharProcColor = 1;
    /// SpellVisualKit CharProc 11: freezes the unit's model animation clock in
    /// place while any active aura's StateKit carries it (10255 Stoned, 16245
    /// Freeze Anim, 27619/45438 Ice Block, 17624 Petrification, etc.).
    static constexpr uint32_t kCharProcFreezeAnim = 11;
    static constexpr uint32_t kCharProcOpacity = 14;
    /// SpellVisualKit CharProc 15: timed opacity fade (startAlpha, endAlpha, duration).
    static constexpr uint32_t kCharProcOpacityFade = 15;
    /// SpellVisualKit CharProc 17: temporary visual equipment override
    /// (CharParamZero = equip slot 0 Head, CharParamOne = Item.dbc entry ID).
    static constexpr uint32_t kCharProcEquipOverride = 17;
    /// SpellVisualKit CharProc 13: CharParamZero 0xRRGGBB tints the light on
    /// the unit for CharParamOne seconds (CharacterRenderer::startInstanceKitLight).
    static constexpr uint32_t kCharProcLight = 13;
    /// SpellVisualKit CharProc 8: colored melee weapon swing trail
    /// (CharParamZero=0xRRGGBB, CharParamOne=fadeOutRate, CharParamTwo=durationMs,
    /// CharParamThree=alpha 0..255).
    static constexpr uint32_t kCharProcWeaponTrail = 8;
    /// SpellVisualKit CharProc 9: impacts falling at random spots of the area
    /// while the kit plays - Blizzard's shards, Rain of Fire's fireballs, Cave
    /// In's rocks. CharParamZero picks the model (kAreaImpactModels),
    /// CharParamOne is impacts a second.
    static constexpr uint32_t kCharProcAreaImpacts = 9;
    struct AreaImpacts {
        std::string path;
        float perSecond = 0.0f;
    };
    std::unordered_map<uint32_t, AreaImpacts> kitAreaImpacts_;  // kit -> impacts
    struct ActiveAreaImpacts {
        AreaImpacts impacts;
        glm::vec3 center{0.0f};
        float radius = 0.0f;
        float due = 0.0f;  // impacts owed and not yet dropped
    };
    std::unordered_map<uint64_t, ActiveAreaImpacts> activeAreaImpacts_;  // persistent area key
    void dropAreaImpacts(float deltaTime);
    struct UnitAuraVisuals {
        std::vector<uint32_t> ids;
        bool observerConcealed = false;
        // The same visuals, oldest application first.
        std::vector<uint32_t> applied;
    };
    // CharProc 1 colour, CharProc 11 animation freeze, CharProc 14 opacity,
    // and CharProc 17 head item display override of a kit, where it has them.
    struct KitCharacterProcedures {
        std::optional<glm::vec3> color;
        std::optional<float> opacity;
        bool freezeAnimation = false;
        uint32_t headItemDisplayId = 0;
    };
    std::unordered_map<uint32_t, KitCharacterProcedures> kitCharacterProcedures_;
    std::unordered_map<uint32_t, KitOpacityFadeSpec> kitOpacityFades_;
    std::unordered_map<uint32_t, uint32_t> activeKitHeadOverride_; // attachInstanceId -> active ItemDisplayInfo ID
    uint32_t nextKitHeadOverrideModelId_ = 997000;
    struct KitLight {
        glm::vec3 color{1.0f};
        float seconds = 0.0f;
    };
    std::unordered_map<uint32_t, KitLight> kitLights_;
    std::unordered_map<uint32_t, WeaponTrailSpec> kitWeaponTrails_;
    std::vector<ActiveUnitWeaponTrail> activeWeaponTrails_;
    /// Start the kit's CharProc 13 light on the unit, where the kit has one.
    void startKitLight(uint32_t kitId, uint32_t attachInstanceId);
    /// Start the kit's CharProc 15 timed opacity fade on the unit, where the kit has one.
    void startKitOpacityFade(uint32_t kitId, uint32_t attachInstanceId);
    /// Start the kit's CharProc 8 weapon swing trail on the unit, where the kit has one.
    void startKitWeaponTrail(uint32_t kitId, uint32_t attachInstanceId);
    void updateWeaponTrails(float deltaTime);
    void applyCharacterProcedures(uint32_t attachInstanceId, bool immediateOpacity = false);
    std::unordered_map<uint32_t, UnitAuraVisuals> unitAuraVisuals_;
    /// Character instances that have completed at least one syncAuraVisuals()
    /// pass (or received transferred visuals). Used to distinguish initial
    /// appearance from runtime aura application/removal for CharProc 14 opacity.
    std::unordered_set<uint32_t> syncedAuraInstances_;
    struct ActiveChannelVisual {
        uint32_t visualId = 0;
        float remaining = 0.0f;  // seconds; inf for field-driven channels
    };
    std::unordered_map<uint32_t, ActiveChannelVisual> unitChannelVisuals_;
    /// SpellChainEffects.dbc, TBC 2.4.3 173-byte records: dwords 0-38, then
    /// bytes Alpha, Red, Green, Blue, BlendMode at +156, Combo string +161,
    /// RenderLayer +165, TextureLength float +169 (not read).
    struct ChainEffect {
        std::string texture;
        float avgSegLen = 0, width = 0, noiseScale = 0, texCoordScale = 0;
        uint32_t segDurationMs = 0, segDelayMs = 0;
        float minDurBetweenJoints = 0, maxDurBetweenJoints = 0, jointSmoothness = 0;
        uint32_t flags = 0;
        float pulseSpeed = 0, pulseOnLength = 0, pulseFadeLength = 0;
        ChainJoints joints;
        float jointMoveSpeed = 0;
        glm::vec2 flickerOn{0.0f}, flickerOff{0.0f};  // seconds, min..max
        float arcHeight = 0, minArcAngle = 0, maxArcAngle = 0, minArcSpin = 0, maxArcSpin = 0;
        std::vector<uint32_t> strands;  // Combo: further rows drawn between the same ends
        float waveHeight = 0, waveFreq = 0, waveSpeed = 0;
        float minWaveAngle = 0, maxWaveAngle = 0, minWaveSpin = 0, maxWaveSpin = 0;
        glm::vec3 color{1.0f};
        float alpha = 1.0f;
        uint8_t blendMode = 0;
    };
    struct KitChainEffect {
        uint32_t chainId = 0;
        float paramOne = 0.0f;  // SpellVisualKit CharParamOne for CharProc 0/12
    };
    std::unordered_map<uint32_t, ChainEffect> chainEffects_;
    std::unordered_map<uint32_t, KitChainEffect> kitChainEffects_;  // kit -> chain effect & CharParamOne
    void loadChainEffects();
    // One strand (the beam's row or one its Combo adds): wave and arc
    // planes, and interior vertex offsets in noise radii across the beam
    // (world-up-in-plane, side), stepped each frame toward `noiseTarget`
    // (chainNoiseStep).
    struct StrandState {
        uint32_t chainId = 0;
        float waveAngle = 0, waveSpin = 0, arcAngle = 0, arcSpin = 0, nextJoints = 0;
        std::vector<glm::vec2> noise, noiseTarget;
        // Joint offsets in yards (up, side) and where each is heading.
        std::vector<glm::vec2> joints, jointTargets;
        float jointsMoved = 0;  // beamClock_ of the last joint step
        bool flickerOn = true;
        float flickerUntil = 0;  // beamClock_ of the next flicker change; 0 before the first
    };
    struct BeamState {
        float age = 0;
        std::vector<StrandState> strands;
    };
    std::unordered_map<uint64_t, BeamState> beams_;           // caster guid
    std::unordered_map<uint64_t, std::vector<BeamState>> hopBeams_;  // caster guid, one per hop
    struct ChannelTargets {
        uint32_t spellId = 0;
        std::vector<uint64_t> targets;
    };
    std::unordered_map<uint64_t, ChannelTargets> channelTargets_;  // caster guid
    std::unordered_map<uint32_t, uint32_t> fieldChannels_;    // caster instance -> visual
    std::unordered_set<uint32_t> heldChannelKits_;            // instances whose kit we started
    std::unordered_set<uint64_t> endedChannels_;              // caster guids, see endChannel
    float beamClock_ = 0.0f;
    struct CastBeam {
        uint32_t chainId = 0;
        ChainEnd start;
        ChainEnd end;
        float startAt = 0.0f, lifetime = 0.0f;
        BeamState state;
    };
    std::vector<CastBeam> castBeams_;
    BeamState newBeamState(uint32_t chainId);
    glm::vec3 beamEnd(const ChainEnd& end);
    // The chain effect's camera-facing strips (the row and its Combo
    // strands) from start to end at age t; nothing for a degenerate beam or
    // a blend mode that is not drawn. Advances the state's vertex noise.
    void appendBeamStrip(std::vector<M2Renderer::ChainStrip>& strips, const glm::vec3& start,
                         const glm::vec3& end, float t, BeamState& state);
    // Model ID for a cast, impact or missile model, loading it on first use.
    // 0 when it cannot be loaded.
    uint32_t loadSpellModel(const std::string& modelPath);
    void placeSpellMissile(const SpellMissile& missile, float t);
    void placeSpellMissileAt(const SpellMissile& missile, const glm::vec3& pos,
                             const glm::vec3& rotation, float animProgress,
                             float scaleMult = 1.0f);
    bool evaluateMissileMotion(const SpellMissile& missile, float progress, float flightTime,
                               float totalDistance, MissileMotionOutput& out) const;

    M2Renderer* m2Renderer_ = nullptr;
    Renderer* renderer_ = nullptr;
    pipeline::AssetManager* cachedAssetManager_ = nullptr;

    std::vector<SpellVisualInstance> activeSpellVisuals_;
    void placeOnUnit(const SpellVisualInstance& visual, CharacterRenderer* characters,
                     const glm::vec3& position, float scale);
    std::vector<PhysicalProjectile> physicalProjectiles_;
    std::vector<SpellMissile> spellMissiles_;
    std::unordered_map<uint32_t, std::string> spellVisualMissilePath_; // visualId → missile M2 path
    std::unordered_map<uint32_t, float> spellVisualMissileScale_;
    std::unordered_map<uint32_t, uint32_t> spellVisualMissilePathType_;
    std::unordered_map<uint32_t, uint32_t> spellVisualMissileMotion_;
    SpellMissileMotionTable missileMotionTable_;
    std::unordered_map<uint32_t, MissileAttachments> missileAttachments_;
    std::unordered_map<uint32_t, int32_t> launchAttachments_;  // SpellVisual MissileAttachment, every visual
    std::unordered_map<std::string, uint32_t> spellVisualModelIds_;   // M2 path → M2Renderer modelId
    std::unordered_set<uint32_t> spellVisualFailedModels_;           // modelIds that failed to load (negative cache)
    uint32_t nextSpellVisualModelId_ = 999000; // Reserved range 999000-999799
    uint32_t nextProjectileModelId_ = 998000;  // Reserved range 998000-998999
    std::unordered_map<std::string, uint32_t> projectileModelIds_;
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, float>> recentUnitImpacts_;
    bool spellVisualDbcLoaded_ = false;
};

} // namespace rendering
} // namespace wowee
