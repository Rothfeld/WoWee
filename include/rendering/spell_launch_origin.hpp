#pragma once

#include "pipeline/m2_loader.hpp"
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
    };

    Source source = Source::FallbackPosition;
    uint32_t attachmentId = 0;
    uint16_t bone = 0;
    glm::vec3 localOffset{0.0f};
    float launchDelaySeconds = 0.0f;
};

inline constexpr uint32_t kAttachLaunchHandFallback = 2;   // M2 HandLeft
inline constexpr uint32_t kAttachSpellLeftHandId = 21;     // M2 SpellLeftHand
inline constexpr uint32_t kAttachSpellRightHandId = 22;    // M2 SpellRightHand
inline constexpr uint32_t kAttachChestBloodFrontId = 15;   // M2 ChestBloodFront (impact/reflect)
inline constexpr uint32_t kAttachChestId = 34;             // M2 Chest (beam fallback)
inline constexpr uint32_t kNoSpellAnimationId = 0xffffffffu;

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
// Chain Lightning), prefer the model's $CSL/$CSR/$CST event on the channel/cast
// animation (e.g. $CSL on Gnome/Human, $CSR on ScourgeMale/DwarfFemale/TrollMale),
// falling back to HandLeft (2). Non-(-1) MissileAttachment keeps Chest (34).
inline SpellLaunchOrigin resolveBeamStartOrigin(const pipeline::M2Model* model,
                                                uint32_t animationId,
                                                int32_t missileAttachment) {
    SpellLaunchOrigin out;
    if (missileAttachment != -1) {
        out.source = SpellLaunchOrigin::Source::Attachment;
        out.attachmentId = kAttachChestId;
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
    out.attachmentId = kAttachLaunchHandFallback;
    return out;
}

// Pure world-space transform math matching CharacterRenderer::getBonePointTransform
// and CharacterRenderer::getAttachmentTransform.
inline glm::mat4 computeBonePointWorldTransform(const glm::mat4& modelMatrix,
                                                const glm::mat4& boneMatrix,
                                                const glm::vec3& localOffset) {
    return modelMatrix * boneMatrix * glm::translate(glm::mat4(1.0f), localOffset);
}

}  // namespace rendering
}  // namespace wowee
