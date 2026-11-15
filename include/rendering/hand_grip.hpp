#pragma once

#include "pipeline/m2_loader.hpp"
#include "rendering/animation/animation_ids.hpp"
#include <stdexcept>

namespace wowee::rendering {

struct HandGripPose {
    int sequence = -1;
    // Attachment 1 = right palm, 2 = left palm; zero leaves the bone alone.
    std::vector<uint8_t> boneHand;
};

inline HandGripPose makeHandGripPose(const pipeline::M2Model& model) {
    HandGripPose pose;
    for (size_t i = 0; i < model.sequences.size(); ++i) {
        if (model.sequences[i].id == anim::HANDS_CLOSED) {
            pose.sequence = static_cast<int>(i);
            break;
        }
    }
    pose.boneHand.resize(model.bones.size());
    for (size_t i = 0; i < model.bones.size(); ++i) {
        int boneIndex = static_cast<int>(i);
        size_t visited = 0;
        while (boneIndex >= 0) {
            if (static_cast<size_t>(boneIndex) >= model.bones.size() ||
                visited++ >= model.bones.size())
                throw std::logic_error("Invalid hand-grip bone hierarchy");
            const auto& bone = model.bones[boneIndex];
            if (bone.keyBoneId >= 8 && bone.keyBoneId <= 12) {
                pose.boneHand[i] = 1;
                break;
            }
            if (bone.keyBoneId >= 13 && bone.keyBoneId <= 17) {
                pose.boneHand[i] = 2;
                break;
            }
            boneIndex = bone.parentBone;
        }
    }
    return pose;
}

} // namespace wowee::rendering
