#pragma once

#include "pipeline/m2_loader.hpp"
#include "rendering/m2_track_sampler.hpp"
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace wowee {
namespace rendering {

// Resolved launch point on a caster for a spell missile or channel beam.
// Sampled live from CharacterRenderer at the release moment (t = launchDelaySeconds),
// not frozen at SMSG_SPELL_GO when the caster is still in Precast/Stand pose.
struct SpellLaunchOrigin {
    enum class Source : uint8_t {
        FallbackPosition,  // No model/bone/attachment; keep unit world position
        Attachment,        // CharacterRenderer::getAttachmentTransform(instance, attachmentId)
        EventBone,         // CharacterRenderer::getBonePointTransform(instance, bone, localOffset)
        RangedWeaponShot,  // CharacterRenderer::getRangedShotLaunchTransform(...)
    };

    Source source = Source::FallbackPosition;
    uint32_t attachmentId = 0;
    uint32_t shootAnimationId = 0;
    uint16_t bone = 0;
    bool preferWeaponMuzzle = false;
    glm::vec3 localOffset{0.0f};
    float launchDelaySeconds = 0.0f;
};

inline constexpr uint32_t kAttachLaunchHandFallback = 2;   // M2 HandLeft
inline constexpr uint32_t kAttachRangedMuzzleId = 36;      // M2 RangedWeaponMuzzle (rifles/crossbows)
inline constexpr uint32_t kAttachChestBloodFrontId = 15;   // M2 ChestBloodFront (impact/reflect)
inline constexpr uint32_t kAttachBreathId = 17;            // M2 Breath / Mouth
inline constexpr uint32_t kAttachHeadTopId = 18;           // M2 Name / Overhead (nameplate anchor)
inline constexpr uint32_t kAttachBaseId = 19;              // M2 Base
inline constexpr uint32_t kAttachHeadId = 20;              // M2 Head (eyes/head; parent=Head on Illidan.m2)
inline constexpr uint32_t kAttachSpellLeftHandId = 21;     // M2 SpellLeftHand
inline constexpr uint32_t kAttachSpellRightHandId = 22;    // M2 SpellRightHand
inline constexpr uint32_t kAttachSpecial1Id = 23;          // M2 Special1 (e.g. Beholder center eye)
inline constexpr uint32_t kAttachSpecial2Id = 24;          // M2 Special2 (e.g. Beholder left eye)
inline constexpr uint32_t kAttachSpecial3Id = 25;          // M2 Special3 (e.g. Beholder right eye)
inline constexpr uint32_t kAttachChestId = 34;             // M2 Chest (beam fallback)
inline constexpr uint32_t kNoSpellAnimationId = 0xffffffffu;

// Map SpellVisual.dbc MissileAttachment (TBC col 15, values 0..8) to M2 attachment ID.
// Follows the exact SpellVisualKit attachment slot order:
//   0=Head(20), 1=Chest(34), 2=Base(19), 3=LeftHand(21), 4=RightHand(22),
//   5=Breath(17), 6=Special1(23), 7=Special2(24), 8=Special3(25).
// Unknown/negative values fall back to Chest (34).
inline constexpr uint32_t missileAttachmentToM2Id(int32_t missileAttachment) {
    switch (missileAttachment) {
        case 0: return kAttachHeadId;
        case 1: return kAttachChestId;
        case 2: return kAttachBaseId;
        case 3: return kAttachSpellLeftHandId;
        case 4: return kAttachSpellRightHandId;
        case 5: return kAttachBreathId;
        case 6: return kAttachSpecial1Id;
        case 7: return kAttachSpecial2Id;
        case 8: return kAttachSpecial3Id;
        default: return kAttachChestId;
    }
}

// Map SpellVisual.dbc MissileDestinationAttachment (TBC col 9) to M2 attachment ID.
// Verified on Native 2.4.3 (build 8606):
//   1 = ChestBloodFront (15, default for bolts like Fireball 67 / Frostbolt 13),
//   2 = Base / feet (19, e.g. Incendiary Shot 7756, Incinerate 7675, Bomb 148/3819),
//   0 = Head (18), 3..8 = LeftHand/RightHand/Breath/Special1..3.
inline constexpr uint32_t missileDestinationAttachmentToM2Id(uint32_t destAttachment) {
    switch (destAttachment) {
        case 0: return kAttachHeadId;
        case 1: return kAttachChestBloodFrontId;
        case 2: return kAttachBaseId;
        case 3: return kAttachSpellLeftHandId;
        case 4: return kAttachSpellRightHandId;
        case 5: return kAttachBreathId;
        case 6: return kAttachSpecial1Id;
        case 7: return kAttachSpecial2Id;
        case 8: return kAttachSpecial3Id;
        default: return kAttachChestBloodFrontId;
    }
}

inline uint32_t fallbackAttachmentForReleaseSide(char side) {
    if (side == 'R') return kAttachSpellRightHandId;
    if (side == 'L') return kAttachSpellLeftHandId;
    return kAttachChestBloodFrontId;
}

inline const pipeline::M2Model::SpellReleaseEvent* findSequenceSpellReleaseEvent(
    const pipeline::M2Model* model, uint32_t animationId) {
    if (!model || animationId == kNoSpellAnimationId) return nullptr;
    for (size_t i = 0; i < model->sequences.size(); ++i) {
        if (model->sequences[i].id != animationId || model->sequences[i].variationIndex != 0) continue;
        if (i < model->spellReleaseEvents.size() && !model->spellReleaseEvents[i].empty()) {
            return &model->spellReleaseEvents[i].front();
        }
        break;
    }
    return nullptr;
}

// Resolve missile launch origin and release delay from the caster's M2 model
// and the spell's CastKit animation / SpellVisual.dbc MissileAttachment.
//
// In TBC 2.4.3 M2s, each race/gender/creature authors its casting release point
// as a $CSL (left hand), $CSR (right hand), or $CST (caster target/hand/eye)
// event on its cast sequence:
//   - GnomeMale / GnomeFemale / ScourgeMale SpellCastDirected (53) -> $CSR @ 200ms
//   - HumanMale / OrcMale / NightElfMale SpellCastDirected (53)    -> $CSL @ 200ms
//   - Imp SpellCastDirected (53)                                   -> $CST @ 766ms
inline SpellLaunchOrigin resolveSpellLaunchOrigin(const pipeline::M2Model* model,
                                                  uint32_t castAnimationId,
                                                  int32_t /*missileAttachment*/,
                                                  bool skipReleaseDelay) {
    SpellLaunchOrigin out;
    if (skipReleaseDelay) {
        out.source = SpellLaunchOrigin::Source::Attachment;
        out.attachmentId = kAttachChestBloodFrontId;
        out.launchDelaySeconds = 0.0f;
        return out;
    }

    if (const auto* ev = findSequenceSpellReleaseEvent(model, castAnimationId)) {
        out.launchDelaySeconds = static_cast<float>(ev->timeMs) / 1000.0f;
        if (ev->bone < model->bones.size()) {
            out.source = SpellLaunchOrigin::Source::EventBone;
            out.bone = ev->bone;
            out.localOffset = ev->position;
        } else {
            out.source = SpellLaunchOrigin::Source::Attachment;
            out.attachmentId = fallbackAttachmentForReleaseSide(ev->side);
        }
        return out;
    }

    out.source = SpellLaunchOrigin::Source::Attachment;
    out.attachmentId = kAttachLaunchHandFallback;
    out.launchDelaySeconds = 0.0f;
    return out;
}

// Resolve caster-side beam start origin for channeled/cast beams.
// When SpellVisual.dbc MissileAttachment == -1 (Drain Life, Drain Soul, Mind Flay,
// Chain Lightning, Holy Wrath), prefer the model's $CSL/$CSR/$CST event on the
// channel/cast animation (e.g. $CSL on Gnome/Human, $CSR on ScourgeMale/DwarfFemale/
// TrollMale). When the model has no release event for that animation (e.g.
// TitanFemale / Maiden of Virtue, which lacks SpellCastDirected 53), fall back to
// Chest (34) matching Native 2.4.3. Non-(-1) MissileAttachment maps via
// missileAttachmentToM2Id.
inline SpellLaunchOrigin resolveBeamStartOrigin(const pipeline::M2Model* model,
                                                uint32_t animationId,
                                                int32_t missileAttachment) {
    SpellLaunchOrigin out;
    if (missileAttachment != -1) {
        out.source = SpellLaunchOrigin::Source::Attachment;
        out.attachmentId = missileAttachmentToM2Id(missileAttachment);
        return out;
    }
    if (const auto* ev = findSequenceSpellReleaseEvent(model, animationId)) {
        if (ev->bone < model->bones.size()) {
            out.source = SpellLaunchOrigin::Source::EventBone;
            out.bone = ev->bone;
            out.localOffset = ev->position;
            return out;
        }
        out.source = SpellLaunchOrigin::Source::Attachment;
        out.attachmentId = fallbackAttachmentForReleaseSide(ev->side);
        return out;
    }
    out.source = SpellLaunchOrigin::Source::Attachment;
    out.attachmentId = kAttachChestId;
    return out;
}

// Pure world-space transform math matching CharacterRenderer::getBonePointTransform
// and CharacterRenderer::getAttachmentTransform.
inline glm::mat4 computeBonePointWorldTransform(const glm::mat4& modelMatrix,
                                                const glm::mat4& boneMatrix,
                                                const glm::vec3& localOffset) {
    return modelMatrix * boneMatrix * glm::translate(glm::mat4(1.0f), localOffset);
}

// Evaluate the model-space transform of `boneIndex` on `model` at
// `(animationId, sampleTimeMs)` without cross-fade blending from any prior pose
// (Stand, Run, ReadyBow, LoadBow). Walks only the ancestor chain of `boneIndex`.
inline bool evaluateModelBoneAtAnimation(const pipeline::M2Model& model,
                                         uint16_t boneIndex,
                                         uint32_t animationId,
                                         float sampleTimeMs,
                                         float globalSeqTimeMs,
                                         float torsoYawOverrideRad,
                                         glm::mat4& outBoneMatrix) {
    if (boneIndex >= model.bones.size()) return false;

    int seqIdx = -1;
    int firstMatch = -1;
    for (size_t i = 0; i < model.sequences.size(); ++i) {
        if (model.sequences[i].id != animationId || model.sequences[i].duration == 0) continue;
        if (firstMatch < 0) firstMatch = static_cast<int>(i);
        if (model.sequences[i].variationIndex == 0) {
            seqIdx = static_cast<int>(i);
            break;
        }
    }
    if (seqIdx < 0) seqIdx = firstMatch;
    if (seqIdx < 0) return false;

    uint16_t chain[64];
    size_t chainLen = 0;
    int cur = static_cast<int>(boneIndex);
    while (cur >= 0 && static_cast<size_t>(cur) < model.bones.size() && chainLen < 64) {
        chain[chainLen++] = static_cast<uint16_t>(cur);
        const int parent = model.bones[static_cast<size_t>(cur)].parentBone;
        if (parent >= cur) break;  // Guard against malformed cycles
        cur = parent;
    }

    constexpr int32_t kSpineLowKeyBone = 4;
    glm::mat4 accum(1.0f);
    for (size_t k = chainLen; k > 0; --k) {
        const auto& bone = model.bones[chain[k - 1]];
        const glm::vec3 trans = m2_track::sampleVec3(
            bone.translation, seqIdx, sampleTimeMs, globalSeqTimeMs,
            model.globalSequenceDurations, glm::vec3(0.0f));
        const glm::quat rot = m2_track::sampleQuat(
            bone.rotation, seqIdx, sampleTimeMs, globalSeqTimeMs,
            model.globalSequenceDurations);
        const glm::vec3 scl = m2_track::sampleVec3(
            bone.scale, seqIdx, sampleTimeMs, globalSeqTimeMs,
            model.globalSequenceDurations, glm::vec3(1.0f));

        const glm::mat3 R = glm::mat3_cast(rot);
        const glm::vec3 c0 = R[0] * scl.x;
        const glm::vec3 c1 = R[1] * scl.y;
        const glm::vec3 c2 = R[2] * scl.z;
        const glm::vec3 t = (bone.pivot + trans)
                          - (c0 * bone.pivot.x + c1 * bone.pivot.y + c2 * bone.pivot.z);
        glm::mat4 localTransform(1.0f);
        localTransform[0] = glm::vec4(c0, 0.0f);
        localTransform[1] = glm::vec4(c1, 0.0f);
        localTransform[2] = glm::vec4(c2, 0.0f);
        localTransform[3] = glm::vec4(t, 1.0f);

        if (bone.keyBoneId == kSpineLowKeyBone && torsoYawOverrideRad != 0.0f) {
            const glm::mat4 extraYaw =
                glm::translate(glm::mat4(1.0f), bone.pivot) *
                glm::rotate(glm::mat4(1.0f), torsoYawOverrideRad, glm::vec3(0.0f, 0.0f, 1.0f)) *
                glm::translate(glm::mat4(1.0f), -bone.pivot);
            localTransform = extraYaw * localTransform;
        }
        accum = accum * localTransform;
    }

    outBoneMatrix = accum;
    return true;
}

// Evaluate the model-space transform of M2 attachment `attachmentId` on `model`
// at `(animationId, sampleTimeMs)` without cross-fade blend contamination.
inline bool evaluateModelAttachmentAtAnimation(const pipeline::M2Model& model,
                                               uint32_t attachmentId,
                                               uint32_t animationId,
                                               float sampleTimeMs,
                                               float globalSeqTimeMs,
                                               float torsoYawOverrideRad,
                                               glm::mat4& outAttachmentMatrix) {
    uint16_t boneIndex = 0;
    glm::vec3 offset(0.0f);
    bool found = false;
    if (attachmentId < model.attachmentLookup.size()) {
        const uint16_t idx = model.attachmentLookup[attachmentId];
        if (idx != 0xFFFF && idx < model.attachments.size()) {
            boneIndex = model.attachments[idx].bone;
            offset = model.attachments[idx].position;
            found = true;
        }
    }
    if (!found) {
        for (const auto& att : model.attachments) {
            if (att.id == attachmentId) {
                boneIndex = att.bone;
                offset = att.position;
                found = true;
                break;
            }
        }
    }
    if (!found) return false;

    glm::mat4 boneMat(1.0f);
    if (!evaluateModelBoneAtAnimation(model, boneIndex, animationId, sampleTimeMs,
                                      globalSeqTimeMs, torsoYawOverrideRad, boneMat)) {
        return false;
    }
    outAttachmentMatrix = boneMat * glm::translate(glm::mat4(1.0f), offset);
    return true;
}

}  // namespace rendering
}  // namespace wowee
