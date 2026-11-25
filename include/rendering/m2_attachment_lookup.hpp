#pragma once

#include "pipeline/m2_loader.hpp"

#include <cstdint>
#include <glm/glm.hpp>

namespace wowee::rendering {

/// Resolve an M2 attachment ID to its parent bone index and bind-pose offset.
///
/// Only authored M2 attachments (via `attachmentLookup` or `attachments[i].id`)
/// are accepted for weapon/equipment points (such as HandRight=1, HandLeft=2,
/// SheathMainHand=30). Models without a hand attachment (e.g. Gargoyle.m2,
/// Harpy.m2, Pterrordax.m2) must return false so Creature virtual weapons are
/// skipped rather than glued to keyBoneId 26 ($ROOT / bone 0).
inline bool resolveModelAttachmentBone(const pipeline::M2Model& model,
                                       uint32_t attachmentId,
                                       uint16_t& outBoneIndex,
                                       glm::vec3& outOffset) {
    outBoneIndex = 0;
    outOffset = glm::vec3(0.0f);
    bool found = false;

    if (attachmentId < model.attachmentLookup.size()) {
        const uint16_t attIdx = model.attachmentLookup[attachmentId];
        if (attIdx < model.attachments.size()) {
            outBoneIndex = model.attachments[attIdx].bone;
            outOffset = model.attachments[attIdx].position;
            found = true;
        }
    }

    if (!found) {
        for (const auto& att : model.attachments) {
            if (att.id == attachmentId) {
                outBoneIndex = att.bone;
                outOffset = att.position;
                found = true;
                break;
            }
        }
    }

    // Fallback for head attachment (ID 11): use bone 0 if attachment not defined.
    if (!found && attachmentId == 11 && !model.bones.empty()) {
        outBoneIndex = 0;
        found = true;
    }

    if (found && outBoneIndex >= model.bones.size()) {
        found = false;
    }

    return found;
}

} // namespace wowee::rendering
