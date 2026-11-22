#pragma once

#include <glm/glm.hpp>
#include <algorithm>
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
    /// LootFX scale over a game object: never below 1, covering both the
    /// header collision box height and wide chests/props (e.g. G_ChestTitan.m2
    /// at 3.14 x 4.11 x 2.34, boundRadius 2.84) so LootFX particles clear the
    /// solid mesh walls instead of spawning occluded inside the chest.
    /// Fitted to build 8606 captures: RuinedSteamTankGear01 (box 0.55, r 0.11)
    /// and Bush_Sansam (no box) at 1, GnomeBigGear (3.78), AllianceVeteranBanner02
    /// (5.79), and Gordok Tribute / G_ChestTitan (2.34 h, 2.84 r -> 4.26).
    static constexpr float objectScale(float collisionBoxHeight,
                                       float boundingRadius = 0.0f) {
        return std::max({1.0f, collisionBoxHeight, 1.5f * boundingRadius});
    }

    /// LootFX scale over a lootable unit corpse: never below 1, growing with
    /// the corpse's selection radius so large boss corpses (e.g. Entropius at
    /// scale 6, selection radius 5.03 yd) sparkle across their full extent
    /// instead of burying a 1x sparkle inside the body or smoke.
    static constexpr float corpseScale(float selectionRadius) {
        return selectionRadius > 1.0f ? selectionRadius : 1.0f;
    }

    /// Something that sparkles, where in render coordinates and how large.
    struct Spot {
        uint64_t guid = 0;
        glm::vec3 position{0.0f};
        float scale = 1.0f;
        /// The game object's M2 instance: the sparkle is drawn with its
        /// room. 0 for a corpse.
        uint32_t roomSource = 0;
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
