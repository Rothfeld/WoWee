#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <unordered_map>

namespace wowee::pipeline { class AssetManager; }
namespace wowee::rendering {
class M2Renderer;

// Owns the quest-marker placements; the shared M2 renderer draws their
// original geometry, materials and authored animation with the world.
class QuestMarkerRenderer {
public:
    void setMarker(uint64_t guid, const glm::vec3& position, int markerType,
                   float boundingHeight = 2.0f, float grayscale = 0.0f);
    void clear();
    void syncModels(M2Renderer& renderer, pipeline::AssetManager& assets);

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
    };
    std::unordered_map<uint64_t, Marker> markers_;
    std::unordered_map<uint64_t, Placement> placements_;
};
} // namespace wowee::rendering
