#pragma once

#include <glm/glm.hpp>
#include <algorithm>
#include <cstdint>
#include <unordered_map>

namespace wowee::pipeline { class AssetManager; }
namespace wowee::rendering {
class M2Renderer;

/// Base position and bounding height for a 3D quest marker over a GameObject's
/// world-space visual bounding box. Centered over the box in XY with height
/// equal to the box vertical span, so QuestMarkerRenderer::syncModels() hovers
/// the marker at worldMax.z + 0.3f regardless of where the model origin sits.
inline void gameObjectQuestMarkerAnchor(const glm::vec3& worldMin,
                                        const glm::vec3& worldMax,
                                        glm::vec3& outPosition,
                                        float& outBoundingHeight) {
    outPosition = glm::vec3(0.5f * (worldMin.x + worldMax.x),
                            0.5f * (worldMin.y + worldMax.y),
                            worldMin.z);
    outBoundingHeight = std::max(0.0f, worldMax.z - worldMin.z);
}

inline constexpr uint32_t kQuestMarkerAnimStand = 0;
inline constexpr uint32_t kQuestMarkerAnimStandHigh = 190;

/// Select the authored TalkToMe*.m2 sequence ID for a 3D world marker:
/// - Flight-point discovery markers (`markerType == 3`, TalkToMeGreen.m2)
///   already receive their +0.5 yd world-name lift at the caller's anchor,
///   so they stay on Stand (0) without double-lifting.
/// - Unit quest markers (`isUnit && markerType != 3`) play StandHigh (190,
///   +0.517 -> +0.427 yd bob) while the unit's plain world name is drawn
///   above its head (`unitWorldNameVisible`), and Stand (0, 0.000 -> -0.089 yd)
///   when no plain world name is drawn (e.g. UnitNameNPC=0 untargeted, or
///   covered by a health nameplate).
/// - GameObject quest markers (`!isUnit`) always play Stand (0).
inline constexpr uint32_t selectQuestMarkerAnimation(int markerType,
                                                     bool isUnit,
                                                     bool unitWorldNameVisible) {
    if (!isUnit || markerType == 3) return kQuestMarkerAnimStand;
    return unitWorldNameVisible ? kQuestMarkerAnimStandHigh : kQuestMarkerAnimStand;
}

// Owns the quest-marker placements; the shared M2 renderer draws their
// original geometry, materials and authored animation with the world.
class QuestMarkerRenderer {
public:
    void setMarker(uint64_t guid, const glm::vec3& position, int markerType,
                   float boundingHeight = 2.0f, float grayscale = 0.0f);
    void clear();
    void syncModels(M2Renderer& renderer, pipeline::AssetManager& assets, const glm::vec3& cameraPosition);

private:
    struct Marker {
        glm::vec3 position;
        int type;
        float boundingHeight;
        float grayscale;
    };
    struct Placement {
        uint32_t instanceId;
        uint32_t modelId;
        uint32_t animationId = kQuestMarkerAnimStand;
    };
    std::unordered_map<uint64_t, Marker> markers_;
    std::unordered_map<uint64_t, Placement> placements_;
};
} // namespace wowee::rendering
