#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class M2Renderer;

/// The glitter over a corpse that has loot for the player, and over a game
/// object a quest needs, as the original client draws both: Particles\LootFX.m2
/// ("HARDCODED Loot Art", SpellVisualEffectName 14) set on the body for as
/// long as the server marks it lootable (UNIT_DYNFLAG_LOOTABLE, which it only
/// sends to a player allowed to loot it), or on the object for as long as its
/// GAMEOBJECT_DYN_FLAGS carry SPARKLE, and taken off once that ends.
class LootSparkles {
public:
    /// LootFX scale over a game object: the height of its model's collision
    /// box (M2 header bounding box, times the object's scale), never below 1.
    /// Fitted to build 8606 captures in Hellfire Peninsula: RuinedSteamTankGear01
    /// (box 0.55) and Bush_Sansam (no box) at 1, GnomeBigGear 3.78 and
    /// AllianceVeteranBanner02 5.79 at about their box height
    /// (_experiments/20261117T2300Z.go_sparkle).
    static constexpr float objectScale(float collisionBoxHeight) {
        return collisionBoxHeight > 1.0f ? collisionBoxHeight : 1.0f;
    }

    /// Something that sparkles, where in render coordinates and how large.
    struct Spot {
        uint64_t guid = 0;
        glm::vec3 position{0.0f};
        float scale = 1.0f;
    };

    /// Once a frame with everything that sparkles now. Starts the sparkle on
    /// new ones, keeps each in place, and stops it on the rest.
    void update(M2Renderer* m2, pipeline::AssetManager* assets,
                const std::vector<Spot>& spots);

private:
    /// Clear of the ranges the level-up effect (999900) and spell visuals
    /// (below 999800) take.
    static constexpr uint32_t MODEL_ID = 999901;

    bool ensureModel(M2Renderer* m2, pipeline::AssetManager* assets);

    M2Renderer* m2_ = nullptr;
    bool failed_ = false;
    struct Placed {
        uint32_t id = 0;
        float scale = 1.0f;
    };
    std::unordered_map<uint64_t, Placed> instances_;  // guid -> sparkle instance
};

} // namespace rendering
} // namespace wowee
