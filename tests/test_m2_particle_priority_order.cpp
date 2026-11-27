#include <catch_amalgamated.hpp>

#include "pipeline/m2_loader.hpp"
#include "rendering/particle_emitter_order.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

using wowee::pipeline::M2Loader;
using wowee::pipeline::M2Model;
using wowee::pipeline::M2ParticleEmitter;
using wowee::rendering::SceneParticleEmitterDrawKey;
using wowee::rendering::particleEmitterInPhase;
using wowee::rendering::sceneParticleEmitterDrawsBefore;
using wowee::rendering::sortedPhaseParticleEmitterIndices;

namespace {

M2ParticleEmitter makeEmitterWithPriority(int16_t priorityPlane, uint8_t blendType = 4) {
    M2ParticleEmitter em{};
    em.priorityPlane = priorityPlane;
    em.blendingType = blendType;
    return em;
}

std::vector<uint8_t> readBinaryFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }
    const auto size = file.tellg();
    if (size <= 0) {
        return {};
    }
    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(buffer.data()), size);
    return buffer;
}

} // namespace

TEST_CASE("Non-negative particle emitters sort by ascending priorityPlane then emitterIndex",
          "[m2][particles][priority_plane]") {
    // Mirrors Spells\Creature_SpellPortalLarge_Purple.m2 active Hold (seq 1) emitters:
    //   [0] -> PE[15] prio 4 Alpha center smoke
    //   [1] -> PE[19] prio 0 Add   center glow
    //   [2] -> PE[22] prio 2 Add   left fire core
    //   [3] -> PE[23] prio 1 Alpha left fire body
    //   [4] -> PE[24] prio 2 Add   right fire core
    //   [5] -> PE[25] prio 1 Alpha right fire body
    std::vector<M2ParticleEmitter> emitters = {
        makeEmitterWithPriority(4, 2),
        makeEmitterWithPriority(0, 4),
        makeEmitterWithPriority(2, 4),
        makeEmitterWithPriority(1, 2),
        makeEmitterWithPriority(2, 4),
        makeEmitterWithPriority(1, 2),
    };

    std::vector<size_t> ordered;
    sortedPhaseParticleEmitterIndices(emitters, /*beforeTransparent=*/false, ordered);

    // Expected back-to-front draw order:
    // prio 0 ([1]) -> prio 1 ([3], [5]) -> prio 2 ([2], [4]) -> prio 4 ([0]).
    // This ensures PE[22]/PE[24] (prio 2 Additive) add incandescent cores ON TOP OF
    // PE[23]/PE[25] (prio 1 Alpha), and PE[15] (prio 4 Alpha) veils PE[19] (prio 0 Add).
    REQUIRE(ordered == std::vector<size_t>{1, 3, 5, 2, 4, 0});
}

TEST_CASE("Negative priorityPlane emitters partition to BeforeTransparent and sort ascending",
          "[m2][particles][priority_plane]") {
    std::vector<M2ParticleEmitter> emitters = {
        makeEmitterWithPriority(1, 4),
        makeEmitterWithPriority(-1, 2),
        makeEmitterWithPriority(-100, 2),
        makeEmitterWithPriority(-3, 4),
        makeEmitterWithPriority(-1, 4),
        makeEmitterWithPriority(0, 2),
    };

    std::vector<size_t> beforeOrder;
    sortedPhaseParticleEmitterIndices(emitters, /*beforeTransparent=*/true, beforeOrder);
    REQUIRE(beforeOrder == std::vector<size_t>{2, 3, 1, 4});
    for (size_t idx : beforeOrder) {
        REQUIRE(particleEmitterInPhase(emitters[idx].priorityPlane, true));
    }

    std::vector<size_t> afterOrder;
    sortedPhaseParticleEmitterIndices(emitters, /*beforeTransparent=*/false, afterOrder);
    REQUIRE(afterOrder == std::vector<size_t>{5, 0});
    for (size_t idx : afterOrder) {
        REQUIRE(particleEmitterInPhase(emitters[idx].priorityPlane, false));
    }
}

TEST_CASE("Equal priorityPlane emitters preserve authored M2 emitter order",
          "[m2][particles][priority_plane]") {
    // StratholmeFireSmokeEmberM and standard campfires author Alpha smoke at #0
    // and Additive flame at #2 on priorityPlane 0.
    std::vector<M2ParticleEmitter> emitters = {
        makeEmitterWithPriority(0, 2),
        makeEmitterWithPriority(0, 2),
        makeEmitterWithPriority(0, 4),
    };

    std::vector<size_t> ordered;
    sortedPhaseParticleEmitterIndices(emitters, /*beforeTransparent=*/false, ordered);
    REQUIRE(ordered == std::vector<size_t>{0, 1, 2});
}

TEST_CASE("Scene-wide particle emitters sort by priorityPlane then far-to-near emitter distance",
          "[m2][particles][priority_plane]") {
    // Mirrors Shadowmoon Valley Boss Portal: Purple (PE[19] prio 0, PE[25] prio 1,
    // PE[24] prio 2, PE[15] prio 4) overlapping Shadowhoof Summoner's Arcane
    // Channeling precast emitters (Magic_PreCast_Hand prio 0 at 28.20 yd while
    // Boss Portal root is at 29.89 yd and PE[24]/PE[25] world origin is at 27.89 yd).
    std::vector<SceneParticleEmitterDrawKey> keys = {
        {.visibleSlot = 0, .emitterIndex = 15, .priorityPlane = 4, .emitterDistanceSq = 893.6f, .registrationOrder = 0},
        {.visibleSlot = 0, .emitterIndex = 24, .priorityPlane = 2, .emitterDistanceSq = 777.8f, .registrationOrder = 0},
        {.visibleSlot = 0, .emitterIndex = 25, .priorityPlane = 1, .emitterDistanceSq = 777.8f, .registrationOrder = 0},
        {.visibleSlot = 1, .emitterIndex = 0,  .priorityPlane = 0, .emitterDistanceSq = 795.4f, .registrationOrder = 1},
        {.visibleSlot = 0, .emitterIndex = 19, .priorityPlane = 0, .emitterDistanceSq = 893.6f, .registrationOrder = 0},
        {.visibleSlot = 1, .emitterIndex = 1,  .priorityPlane = 0, .emitterDistanceSq = 795.4f, .registrationOrder = 1},
    };

    std::sort(keys.begin(), keys.end(), sceneParticleEmitterDrawsBefore);

    // Plane 0 draws first, far-to-near (893.6 -> 795.4), preserving emitterIndex on ties;
    // then Plane 1 (PE[25] Alpha fire veils prio 0 sparkles), Plane 2 (PE[24] Add core), Plane 4 (PE[15] smoke).
    REQUIRE(keys[0].priorityPlane == 0);
    REQUIRE(keys[0].emitterIndex == 19);
    REQUIRE(keys[1].priorityPlane == 0);
    REQUIRE(keys[1].emitterIndex == 0);
    REQUIRE(keys[2].priorityPlane == 0);
    REQUIRE(keys[2].emitterIndex == 1);
    REQUIRE(keys[3].priorityPlane == 1);
    REQUIRE(keys[3].emitterIndex == 25);
    REQUIRE(keys[4].priorityPlane == 2);
    REQUIRE(keys[4].emitterIndex == 24);
    REQUIRE(keys[5].priorityPlane == 4);
    REQUIRE(keys[5].emitterIndex == 15);
}

TEST_CASE("Real TBC Creature_SpellPortalLarge_Purple.m2 Hold emitters order prio 0 -> 1 -> 2 -> 4",
          "[m2][particles][priority_plane]") {
    const std::filesystem::path m2Path =
        "/workspace/azerothcore/client/wowee-data/expansions/tbc/spells/creature_spellportallarge_purple.m2";
    if (!std::filesystem::exists(m2Path)) {
        SKIP("Extracted TBC Creature_SpellPortalLarge_Purple.m2 not present");
    }

    const auto bytes = readBinaryFile(m2Path);
    REQUIRE_FALSE(bytes.empty());

    M2Model model = M2Loader::load(bytes);
    REQUIRE(model.particleEmitters.size() == 26);

    REQUIRE(model.particleEmitters[15].priorityPlane == 4);
    REQUIRE(model.particleEmitters[15].blendingType == 2);
    REQUIRE(model.particleEmitters[19].priorityPlane == 0);
    REQUIRE(model.particleEmitters[19].blendingType == 4);
    REQUIRE(model.particleEmitters[22].priorityPlane == 2);
    REQUIRE(model.particleEmitters[22].blendingType == 4);
    REQUIRE(model.particleEmitters[23].priorityPlane == 1);
    REQUIRE(model.particleEmitters[23].blendingType == 2);
    REQUIRE(model.particleEmitters[24].priorityPlane == 2);
    REQUIRE(model.particleEmitters[24].blendingType == 4);
    REQUIRE(model.particleEmitters[25].priorityPlane == 1);
    REQUIRE(model.particleEmitters[25].blendingType == 2);

    std::vector<size_t> ordered;
    sortedPhaseParticleEmitterIndices(model.particleEmitters, /*beforeTransparent=*/false, ordered);
    REQUIRE(ordered.size() == 26);

    auto posOf = [&ordered](size_t emitterIdx) {
        auto it = std::find(ordered.begin(), ordered.end(), emitterIdx);
        REQUIRE(it != ordered.end());
        return static_cast<size_t>(std::distance(ordered.begin(), it));
    };

    // Center additive glow (19, prio 0) draws before side alpha fire bodies (23/25, prio 1),
    // which draw before side additive fire cores (22/24, prio 2), which draw before center
    // alpha smoke veil (15, prio 4).
    REQUIRE(posOf(19) < posOf(23));
    REQUIRE(posOf(23) < posOf(25));
    REQUIRE(posOf(25) < posOf(22));
    REQUIRE(posOf(22) < posOf(24));
    REQUIRE(posOf(24) < posOf(15));
}
