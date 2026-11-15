#include <catch_amalgamated.hpp>
#include "rendering/hand_grip.hpp"

using namespace wowee;

TEST_CASE("Grip selects finger descendants without wrist or arm bones") {
    pipeline::M2Model model;
    model.sequences.resize(2);
    model.sequences[0].id = rendering::anim::STAND;
    model.sequences[1].id = rendering::anim::HANDS_CLOSED;
    model.bones.resize(6);
    const int keys[] = {3, -1, 8, -1, 13, -1};
    const int parents[] = {-1, 0, 1, 2, 1, 4};
    for (size_t i = 0; i < model.bones.size(); ++i) {
        model.bones[i].keyBoneId = keys[i];
        model.bones[i].parentBone = parents[i];
    }
    const auto grip = rendering::makeHandGripPose(model);
    CHECK(grip.sequence == 1);
    CHECK(grip.boneHand == std::vector<uint8_t>{0, 0, 1, 1, 2, 2});
}

TEST_CASE("Models without HandsClosed do not invent a grip sequence") {
    pipeline::M2Model model;
    model.sequences.resize(1);
    model.sequences[0].id = rendering::anim::STAND;
    CHECK(rendering::makeHandGripPose(model).sequence == -1);
}

TEST_CASE("Grip hierarchy accepts forward parents and rejects cycles") {
    pipeline::M2Model model;
    model.bones.resize(2);
    model.bones[0].keyBoneId = -1;
    model.bones[0].parentBone = 1;
    model.bones[1].keyBoneId = 17;
    model.bones[1].parentBone = -1;
    CHECK(rendering::makeHandGripPose(model).boneHand == std::vector<uint8_t>{2, 2});
    model.bones[1].keyBoneId = -1;
    model.bones[1].parentBone = 0;
    CHECK_THROWS_AS(rendering::makeHandGripPose(model), std::logic_error);
}
