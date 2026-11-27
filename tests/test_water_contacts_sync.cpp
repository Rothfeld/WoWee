// Per-frame UBO ring synchronization for WaterRenderer::updateContacts,
// updateWake, updateReflectionUBO, and drawn-vs-undrawn ticks (DrawSchedule).

#include <catch_amalgamated.hpp>

#include "core/draw_schedule.hpp"
#include "pipeline/adt_loader.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/water_renderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using wowee::core::DrawSchedule;
using wowee::pipeline::ADTTerrain;
using wowee::rendering::kMaxWakePoints;
using wowee::rendering::kMaxWaterContacts;
using wowee::rendering::MAX_FRAMES_IN_FLIGHT;
using wowee::rendering::WaterContactActor;
using wowee::rendering::WaterFrameUBOData;
using wowee::rendering::WaterRenderer;

ADTTerrain makeFlatWaterTerrain(float waterHeight, uint16_t liquidType = 1u) {
    ADTTerrain terrain;
    terrain.loaded = true;
    for (int cy = 0; cy < 16; ++cy) {
        for (int cx = 0; cx < 16; ++cx) {
            auto& chunk = terrain.getChunk(cx, cy);
            chunk.position[0] = -static_cast<float>(cy) * 33.33333f;
            chunk.position[1] = -static_cast<float>(cx) * 33.33333f;
            chunk.position[2] = 0.0f;
        }
    }
    for (int chunkIdx = 0; chunkIdx < 4; ++chunkIdx) {
        ADTTerrain::WaterLayer layer{};
        layer.liquidType = liquidType;
        layer.minHeight = waterHeight;
        layer.maxHeight = waterHeight;
        layer.x = 0;
        layer.y = 0;
        layer.width = 8;
        layer.height = 8;
        layer.heights.assign(81, waterHeight);
        layer.depths.assign(81, 128u);
        layer.mask.assign(8, 0xFFu);
        terrain.waterData[chunkIdx].layers.push_back(std::move(layer));
    }
    return terrain;
}

struct MappedFrameUBORing {
    static constexpr size_t kStride = static_cast<size_t>(WaterRenderer::frameUBOStride());
    static constexpr size_t kSlots = static_cast<size_t>(WaterRenderer::frameUBOSlotCount());
    alignas(256) std::array<uint8_t, kStride * kSlots> bytes{};

    uint8_t* slotBytes(uint32_t slot) {
        return bytes.data() + static_cast<size_t>(slot % kSlots) * kStride;
    }

    const uint8_t* slotBytes(uint32_t slot) const {
        return bytes.data() + static_cast<size_t>(slot % kSlots) * kStride;
    }

    WaterFrameUBOData readSlot(uint32_t slot) const {
        WaterFrameUBOData out{};
        std::memcpy(&out, slotBytes(slot), sizeof(WaterFrameUBOData));
        return out;
    }

    void fillTailCanary(uint32_t slot, uint8_t canary) {
        std::memset(slotBytes(slot) + sizeof(WaterFrameUBOData),
                    canary,
                    kStride - sizeof(WaterFrameUBOData));
    }

    bool tailMatchesCanary(uint32_t slot, uint8_t canary) const {
        const uint8_t* tail = slotBytes(slot) + sizeof(WaterFrameUBOData);
        for (size_t i = 0; i < kStride - sizeof(WaterFrameUBOData); ++i) {
            if (tail[i] != canary) return false;
        }
        return true;
    }
};

glm::mat4 makeDistinctMatrix(float tag) {
    glm::mat4 m(1.0f);
    m[0][0] = tag;
    m[1][1] = tag * 2.0f;
    m[2][2] = -tag;
    m[3][0] = tag + 10.0f;
    m[3][1] = tag + 20.0f;
    m[3][2] = tag + 30.0f;
    return m;
}

}  // namespace

TEST_CASE("WaterFrameUBOData std140 layout and per-frame ring constants match shader contract",
          "[water][contacts][sync]") {
    CHECK(WaterRenderer::frameUBOSlotCount() == MAX_FRAMES_IN_FLIGHT);
    CHECK(WaterRenderer::frameUBOStride() >= sizeof(WaterFrameUBOData));
    CHECK(WaterRenderer::frameUBOStride() % 256 == 0);

    CHECK(offsetof(WaterFrameUBOData, reflViewProj) == 0u);
    CHECK(offsetof(WaterFrameUBOData, wakeBounds) == 64u);
    CHECK(offsetof(WaterFrameUBOData, liquidParams) == 80u);
    CHECK(offsetof(WaterFrameUBOData, contactMeta) == 96u);
    CHECK(offsetof(WaterFrameUBOData, contactPositions) == 112u);
    CHECK(offsetof(WaterFrameUBOData, contactStyles) == 1136u);
    CHECK(offsetof(WaterFrameUBOData, wakePoints) == 2160u);
    CHECK(offsetof(WaterFrameUBOData, riverDeepColor) == 2672u);
    CHECK(offsetof(WaterFrameUBOData, riverShallowColor) == 2688u);
    CHECK(offsetof(WaterFrameUBOData, oceanShallowColor) == 2704u);
    CHECK(offsetof(WaterFrameUBOData, oceanDeepColor) == 2720u);
    CHECK(sizeof(WaterFrameUBOData) == 2736u);
    CHECK(WaterRenderer::frameUBOStride() == 2816u);
}

TEST_CASE("WaterRenderer::updateContacts and pre-fence mutators never overwrite mapped in-flight GPU UBO slots",
          "[water][contacts][sync]") {
    WaterRenderer water;
    water.loadFromTerrain(makeFlatWaterTerrain(/*waterHeight=*/10.0f), false, 32, 32, false);

    MappedFrameUBORing ring{};
    ring.bytes.fill(0xCDu);
    ring.fillTailCanary(0u, 0xA5u);
    ring.fillTailCanary(1u, 0x5Au);
    water.bindMappedFrameUBOBufferForTesting(ring.bytes.data());

    // Frame 0 (slot 0) fenced and uploaded with stationary actor at (-10, -10, 9.4)
    water.updateReflectionUBO(makeDistinctMatrix(1.0f));
    water.setRiverAlpha(glm::vec2(0.45f, 0.90f));
    const std::vector<WaterContactActor> frame0Actors{
        {.guid = 1001ULL, .position = glm::vec3(-10.0f, -10.0f, 9.4f), .radius = 0.5f},
    };
    water.updateContacts(0.05f, frame0Actors);
    water.uploadFrameUBO(0u);
    ring.fillTailCanary(0u, 0xA5u);

    // Frame 1 (slot 1) fenced and uploaded with moving actor at (-9.5, -10, 9.4)
    water.updateReflectionUBO(makeDistinctMatrix(2.0f));
    water.setRiverAlpha(glm::vec2(0.55f, 0.95f));
    const std::vector<WaterContactActor> frame1Actors{
        {.guid = 1001ULL, .position = glm::vec3(-9.5f, -10.0f, 9.4f), .radius = 0.5f},
    };
    water.updateContacts(0.10f, frame1Actors);
    water.uploadFrameUBO(1u);
    ring.fillTailCanary(1u, 0x5Au);

    // Both slot 0 and slot 1 are now in flight on the GPU. Snapshot all mapped bytes.
    const auto snapshotInFlight = ring.bytes;
    const WaterFrameUBOData slot0Before = ring.readSlot(0u);
    const WaterFrameUBOData slot1Before = ring.readSlot(1u);
    REQUIRE(slot0Before.reflViewProj[0][0] == Catch::Approx(1.0f));
    REQUIRE(slot1Before.reflViewProj[0][0] == Catch::Approx(2.0f));

    // Tick 2 runs Renderer::update BEFORE beginFrame / settleSubmittedFrames:
    // liquid colors, wake, contacts, and reflection matrix all mutate CPU state.
    water.setRiverAlpha(glm::vec2(0.25f, 0.80f));
    water.setRiverDeepColor(glm::vec3(0.05f, 0.15f, 0.35f));
    water.setRiverShallowColor(glm::vec3(0.15f, 0.40f, 0.65f));
    water.setOceanColors(glm::vec3(0.10f, 0.30f, 0.55f),
                         glm::vec3(0.02f, 0.08f, 0.25f),
                         glm::vec2(0.60f, 0.95f));
    water.updateWake(0.10f, glm::vec2(-8.8f, -10.0f), glm::vec2(1.0f, 0.0f), 0.9f, /*wading=*/true);
    const std::vector<WaterContactActor> tick2Actors{
        {.guid = 1001ULL, .position = glm::vec3(-8.8f, -10.0f, 9.4f), .radius = 0.5f},
        {.guid = 2002ULL, .position = glm::vec3(-12.0f, -14.0f, 9.5f), .radius = 0.75f},
    };
    water.updateContacts(0.10f, tick2Actors);
    water.updateReflectionUBO(makeDistinctMatrix(3.0f));

    // CPU staged UBO has the new data...
    CHECK(water.stagedFrameUBO().reflViewProj[0][0] == Catch::Approx(3.0f));
    CHECK(water.stagedFrameUBO().contactMeta.x >= 2.0f);
    CHECK(water.stagedFrameUBO().wakeBounds.w >= 1.0f);
    CHECK(water.stagedFrameUBO().riverDeepColor.z == Catch::Approx(0.35f));

    // ...while ZERO bytes of either mapped GPU slot were touched before fence wait!
    CHECK(ring.bytes == snapshotInFlight);
    CHECK(ring.tailMatchesCanary(0u, 0xA5u));
    CHECK(ring.tailMatchesCanary(1u, 0x5Au));

    // Once slot 0's fence settles and render() records frameIndex=2 (slot 0),
    // ONLY slot 0 updates; slot 1 (still in flight on GPU) stays byte-identical.
    water.uploadFrameUBO(2u);
    CHECK(ring.tailMatchesCanary(0u, 0xA5u));
    CHECK(ring.tailMatchesCanary(1u, 0x5Au));
    CHECK(std::memcmp(ring.slotBytes(1u),
                      snapshotInFlight.data() + MappedFrameUBORing::kStride,
                      MappedFrameUBORing::kStride) == 0);
    const WaterFrameUBOData slot0After = ring.readSlot(0u);
    CHECK(std::memcmp(&slot0After, &water.stagedFrameUBO(), sizeof(WaterFrameUBOData)) == 0);

    // Idempotency within the same drawn frame (e.g. render() then renderDepthOnly()):
    // poisoning a sentinel byte inside mapped slot 0 without mutating stagedFrameUBO()
    // confirms the second uploadFrameUBO(2u) does not re-memcpy unchanged data.
    ring.slotBytes(0u)[0] ^= 0xFFu;
    const uint8_t poisonedByte = ring.slotBytes(0u)[0];
    water.uploadFrameUBO(2u);
    CHECK(ring.slotBytes(0u)[0] == poisonedByte);

    // clear() during map unload also stages on CPU without racing in-flight GPU slots.
    const auto snapshotBeforeClear = ring.bytes;
    water.clear();
    CHECK(water.stagedFrameUBO().contactMeta.x == 0.0f);
    CHECK(water.stagedFrameUBO().wakeBounds.w == 0.0f);
    CHECK(ring.bytes == snapshotBeforeClear);

    // Next fenced frame on slot 1 (frameIndex=3) uploads cleared state to slot 1 only.
    water.uploadFrameUBO(3u);
    const WaterFrameUBOData slot1AfterClear = ring.readSlot(1u);
    CHECK(slot1AfterClear.contactMeta.x == 0.0f);
    CHECK(slot1AfterClear.wakeBounds.w == 0.0f);
    CHECK(std::memcmp(ring.slotBytes(0u), snapshotBeforeClear.data(), MappedFrameUBORing::kStride) == 0);
}

TEST_CASE("DrawSchedule(4) undrawn ticks advance water contacts and wakes without touching mapped GPU slots",
          "[water][contacts][sync][draw-schedule]") {
    WaterRenderer water;
    water.loadFromTerrain(makeFlatWaterTerrain(/*waterHeight=*/5.0f), false, 32, 32, false);

    MappedFrameUBORing ring{};
    ring.bytes.fill(0x11u);
    water.bindMappedFrameUBOBufferForTesting(ring.bytes.data());

    DrawSchedule schedule(4);
    uint32_t currentFrameSlot = 0u;
    constexpr float kTickDt = 0.05f;

    int drawnCount = 0;
    int undrawnCount = 0;
    for (int tick = 0; tick < 9; ++tick) {
        const bool drawThisTick = schedule.beginTick(/*inWorld=*/true);
        const float actorX = -20.0f + static_cast<float>(tick) * 0.25f;

        const auto mappedBeforeUpdate = ring.bytes;

        // Simulation phase (runs on BOTH drawn and undrawn ticks before beginFrame)
        water.updateWake(kTickDt, glm::vec2(actorX, -10.0f), glm::vec2(1.0f, 0.0f),
                         0.85f, /*wading=*/false);
        const std::vector<WaterContactActor> actors{
            {.guid = 77ULL, .position = glm::vec3(actorX, -10.0f, 4.3f), .radius = 0.6f},
        };
        water.updateContacts(kTickDt, actors);

        // Must NEVER touch mapped GPU memory during update!
        REQUIRE(ring.bytes == mappedBeforeUpdate);

        if (drawThisTick) {
            ++drawnCount;
            const uint32_t otherSlot = (currentFrameSlot + 1u) % WaterRenderer::frameUBOSlotCount();
            std::array<uint8_t, MappedFrameUBORing::kStride> otherSlotSnapshot{};
            std::memcpy(otherSlotSnapshot.data(), ring.slotBytes(otherSlot), MappedFrameUBORing::kStride);

            // Reflection pre-pass (after beginFrame fence wait, before water draw)
            water.updateReflectionUBO(makeDistinctMatrix(static_cast<float>(tick + 1)));
            REQUIRE(ring.bytes == mappedBeforeUpdate);

            // Water draw + water depth-only pass in the same command buffer
            water.uploadFrameUBO(currentFrameSlot);
            water.uploadFrameUBO(currentFrameSlot);

            const WaterFrameUBOData uploaded = ring.readSlot(currentFrameSlot);
            CHECK(std::memcmp(&uploaded, &water.stagedFrameUBO(), sizeof(WaterFrameUBOData)) == 0);
            CHECK(std::memcmp(ring.slotBytes(otherSlot),
                              otherSlotSnapshot.data(),
                              MappedFrameUBORing::kStride) == 0);

            currentFrameSlot = otherSlot;
        } else {
            ++undrawnCount;
            CHECK(ring.bytes == mappedBeforeUpdate);
        }
    }

    CHECK(drawnCount == 3);
    CHECK(undrawnCount == 6);
    CHECK(water.stagedFrameUBO().contactMeta.x > 0.0f);
    CHECK(water.stagedFrameUBO().wakeBounds.w > 0.0f);
}

TEST_CASE("WaterRenderer::updateContacts clamps at kMaxWaterContacts and expires trails cleanly",
          "[water][contacts][sync]") {
    WaterRenderer water;
    water.loadFromTerrain(makeFlatWaterTerrain(/*waterHeight=*/12.0f), false, 32, 32, false);

    MappedFrameUBORing ring{};
    ring.bytes.fill(0x7Eu);
    water.bindMappedFrameUBOBufferForTesting(ring.bytes.data());
    const auto untouchedRing = ring.bytes;

    std::vector<WaterContactActor> actors(24);
    for (size_t i = 0; i < actors.size(); ++i) {
        actors[i] = WaterContactActor{
            .guid = 5000ULL + i,
            .position = glm::vec3(-30.0f, -0.5f - static_cast<float>(i) * 1.2f, 11.4f),
            .radius = 0.5f,
        };
    }

    // Seed emitters, then run several fast steps so total emitted contacts exceed 64.
    water.updateContacts(0.05f, actors);
    for (int step = 0; step < 6; ++step) {
        for (auto& actor : actors) {
            actor.position.x += 0.9f;
        }
        water.updateContacts(0.08f, actors);
    }

    CHECK(water.stagedFrameUBO().contactMeta.x == Catch::Approx(static_cast<float>(kMaxWaterContacts)));
    for (int i = 0; i < kMaxWaterContacts; ++i) {
        const glm::vec4 pos = water.stagedFrameUBO().contactPositions[i];
        const glm::vec4 style = water.stagedFrameUBO().contactStyles[i];
        CHECK(std::isfinite(pos.x));
        CHECK(std::isfinite(pos.y));
        CHECK(pos.z == Catch::Approx(12.0f));
        CHECK(pos.w > 0.0f);
        CHECK(style.x >= 0.0f);
    }
    CHECK(ring.bytes == untouchedRing);

    // Advance past 0.65s contact lifetime with no actors -> all contacts expire.
    water.updateContacts(0.70f, {});
    CHECK(water.stagedFrameUBO().contactMeta.x == 0.0f);
    CHECK(ring.bytes == untouchedRing);

    // Upload once fenced -> GPU slot receives expired count = 0.
    water.uploadFrameUBO(0u);
    CHECK(ring.readSlot(0u).contactMeta.x == 0.0f);
    CHECK(std::memcmp(ring.slotBytes(1u),
                      untouchedRing.data() + MappedFrameUBORing::kStride,
                      MappedFrameUBORing::kStride) == 0);
}
