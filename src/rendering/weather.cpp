#include "rendering/weather.hpp"

#include "core/logger.hpp"
#include "rendering/camera.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <cassert>
#include <cmath>
#include <cstring>
#include <random>

namespace wowee {
namespace rendering {

namespace {

// Measurements on 2.4.3 behind these numbers are in
// _experiments/20261119T0930.weather (workspace root): forced weather
// (`.wchange 1 1`, `.wchange 2 1`) in Stranglethorn, Westfall, Elwynn,
// Duskwood and Winterspring, native and WoWee side by side.

// ---- Rain drops: Shaders\Vertex\rain.bls (game data) ----
// Each drop is one triangle (Wow.exe: Packet<Rain::Drop,0x1800,3>, 6144 drops
// of three vertices). The program's constant defaults: corners 0 and 1 at the
// head, 0.05 yard either side across the view (vtxCoordsY), corner 2 two yards
// back along the drop's path (vtxCoordsZ); texture coordinates (0,1), (1,1),
// (0.5,0). The head stops where the drop lands; the tail keeps going for
// 2 / |vz| seconds and the drop is gone.
constexpr float kDropHalfWidth = 0.05f;
constexpr float kDropLength = 2.0f;
// Velocity. Streaks drift one way in the world whatever the camera's yaw:
// toward -x (south) by 0.30 of their fall in Stranglethorn, Grom'gol and
// Westfall (screen tilt +-16..17 degrees facing +-y, none facing +-x). Speed
// along the streak: its screen step per frame over its screen length is
// 0.81-0.97 of a two-yard drop per 37-42 ms frame, 42-47 yd/s over five
// views.
constexpr float kDropSpeed = 44.6f;
const glm::vec3 kDropDirection = glm::normalize(glm::vec3(-0.30f, 0.0f, -1.0f));
// Where drops are: a box around the camera, at any height of the camera above
// the ground, with drops born at its top. Rain that starts fills the box at
// once (streaks at every elevation of a 9-34 degree band appear within the
// same 0.4 s, 5-10 s after the packet), and stops at once. Density: streak
// counts per streak length (so per distance) on 2.4.3 against WoWee with a
// known density, same detector, same held camera, Westfall, pitches -22, 12
// and 45: native draws about 0.02 drops per cubic yard near the camera, with
// streaks out past 60 yards, and as many looking 45 degrees up as a box 50
// yards tall gives. 2.4.3 varies 2x from one capture to the next (mist);
// four packets in this box are within that at all three pitches.
constexpr float kDropsAtFull = 4.0f * 6144.0f;
constexpr float kRainBoxHalf = 80.0f;
constexpr float kRainSpawnAbove = 50.0f;
// None at all with the camera more than this far above the ground: flying up
// in Westfall's rain (GM fly), 500 streaks a frame at 69 yards above the
// terrain, 100 at 74, none from 79 up.
constexpr float kRainCeiling = 74.0f;
// Below this far under the camera a drop with no ground under it is dropped.
constexpr float kFallFloor = 100.0f;

// ---- Splashes: Shaders\Vertex\patter.bls (game data) ----
// One triangle per landed drop (Packet<Rain::Patter,0x1800,3>) facing the
// camera, base on the ground, RainDropSplash01.blp: a 4 x 4 atlas whose
// column is the frame, floor(age * 4 * 3.99), so 0.25 s of life, and whose
// row is the view, floor((1 - lookdown) * 3.99) with lookdown the downward
// component of the direction to it. Looked at from above the triangle moves
// down by half its height (onto the impact) and up 1/12 yard. Engine-set
// sizes, measured: full-grown splashes 4 yards away span 0.19-0.20 yard,
// 0.875 of their atlas cell, so the cell is 0.23 yard.
constexpr float kSplashLife = 0.25f;
constexpr float kSplashCell = 0.23f;

// ---- Mist: Mists::Mist puffs, SnowMist01.blp with rain and snow ----
// Same class as the sandstorm's (weather_mist.hpp), and the same slab: rain's
// haze is nothing nearer than 8-12 yards and builds evenly with distance out
// to about 110 (native terrain contrast against WoWee without mist, both
// against clear weather: Stranglethorn 4% at 22 yd, 12% at 50, 19% at 75-115).
// Optical depth per yard in that slab: Stranglethorn 0.0021, Westfall 0.0012,
// Elwynn 0.0014, Wetlands 0.0022 (four seconds each; a handful of puffs, so
// it varies), 0.0017 on average. Count = depth * slab volume / (quad area *
// mean texture alpha 46.7/255), as for the sandstorm.
constexpr float kMistSpeed = 12.0f;
constexpr float kMistNear = 8.0f, kMistFar = 110.0f, kMistSize = 36.0f;
constexpr float kMistHalfWidth = 60.0f, kMistBelow = 15.0f, kMistAbove = 30.0f;
constexpr float kRainMistDepthPerYard = 0.0017f;
constexpr float kMistPuffsPerDepth = (2.0f * kMistHalfWidth) * (kMistBelow + kMistAbove) * (kMistFar - kMistNear) /
                                     (kMistSize * kMistSize * (46.7f / 255.0f));
constexpr WeatherMist::Shape kRainMist{
    .speed = kMistSpeed, .nearDepth = kMistNear, .farDepth = kMistFar, .size = kMistSize,
    .halfWidth = kMistHalfWidth, .below = kMistBelow, .above = kMistAbove, .fade = 0.2f,
    .puffsAtFull = kRainMistDepthPerYard * kMistPuffsPerDepth};

// ---- Snow: Shaders\Vertex\snowpoint.bls (game data) ----
// Packet<Snow::Flake,0x1800,1>: points. Alpha rises over the first second of a
// flake's life and falls to zero over the quarter second after it lands.
// Velocity: flakes tracked until they stop on the ground in Winterspring, at
// the terrain distance of where they stopped (five views, four yaws, 36
// landings): 3.35 yd/s down, the drift 0.3 of that toward -x, as rain's.
// Snow that starts shows in the sky about 10 s after the packet: born ~34
// yards up. Count and box: transient blobs per frame by size on 2.4.3 against
// WoWee, same detector and held camera, Winterspring, pitch 8. Flakes of 10 px
// (the nearest) were as many as one packet in a 25-yard box gives (11.5 a
// frame against 12.2), 0.07 per cubic yard; smaller ones were 4-25 times as
// many as that box could hold, out to sizes of 1 px. The same density in a
// 50-yard box is four packets, as rain's.
// c_pointSize: landed flakes 6-15 yards away are all ~10.5 px (the star fills
// its sprite). Past that a flake loses about a pixel per 4 yards: the share of
// flakes at each size, against a uniform density, puts sizes 8, 6, 4, 2 and 1
// at 23, 31, 38, 49 and 61 yards.
constexpr float kFlakesAtFull = 4.0f * 6144.0f;
constexpr float kSnowBoxHalf = 50.0f;
constexpr float kSnowSpawnAbove = 34.0f;
constexpr float kFlakeSpeed = 3.35f * 1.044f;  // along kDropDirection
const glm::vec3 kFlakeVelocity = kDropDirection * kFlakeSpeed;
constexpr glm::vec3 kFlakeSize(-1.0f / 42.0f, 1.0f + 16.0f / 42.0f, 10.5f);
// Snow's mist in Winterspring, measured as rain's: 1-3% beyond 12 yards.
constexpr float kSnowMistDepthPerYard = 0.00026f;
constexpr WeatherMist::Shape kSnowMist{
    .speed = kMistSpeed, .nearDepth = kMistNear, .farDepth = kMistFar, .size = kMistSize,
    .halfWidth = kMistHalfWidth, .below = kMistBelow, .above = kMistAbove, .fade = 0.2f,
    .puffsAtFull = kSnowMistDepthPerYard * kMistPuffsPerDepth};

// Weather.dbc EffectColor of every rain and snow row.
constexpr glm::vec4 kEffectColor(1.0f, 1.0f, 1.0f, 1.0f);

constexpr size_t kMaxDrops = 2 * 4 * 6144;
constexpr size_t kMaxSplashes = 4 * 6144;
constexpr size_t kMaxFlakes = 2 * 4 * 6144;

std::mt19937& weatherRng() {
    static std::mt19937 gen(std::random_device{}());
    return gen;
}
float weatherRandFloat() {
    return std::uniform_real_distribution<float>(0.0f, 1.0f)(weatherRng());
}
float uniform(float lo, float hi) {
    return std::uniform_real_distribution<float>(lo, hi)(weatherRng());
}

// Keep a horizontal offset from the camera inside [-half, half).
float wrap(float offset, float half) {
    return offset - 2.0f * half * std::floor((offset + half) / (2.0f * half));
}

} // namespace

Weather::Weather() = default;

Weather::~Weather() {
    shutdown();
}

bool Weather::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout,
                         pipeline::AssetManager* assets) {
    assert(ctx && assets);
    if (vkCtx_) return true;
    vkCtx_ = ctx;
    perFrameLayout_ = perFrameLayout;
    // Rain's textures are Weather.dbc's EffectTexture and the names Wow.exe
    // keeps next to rain.bls and patter.bls; snow's next to snowpoint.bls.
    if (!dropTexture_.load(*ctx, *assets, "Textures\\Weather\\RainDrop01.blp") ||
        !splashTexture_.load(*ctx, *assets, "Textures\\Weather\\RainDropSplash01.blp") ||
        !flakeTexture_.load(*ctx, *assets, "Textures\\Weather\\SnowFlake01.blp") ||
        !mist_.initialize(ctx, perFrameLayout, assets, "Textures\\Weather\\SnowMist01.blp") ||
        !createPipelines()) {
        LOG_ERROR("Weather: initialization failed");
        shutdown();
        return false;
    }
    for (auto& f : frames_) {
        AllocatedBuffer s = createBuffer(ctx->getAllocator(),
                                         (kMaxDrops + kMaxSplashes) * 3 * sizeof(SpriteVertex),
                                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        AllocatedBuffer p = createBuffer(ctx->getAllocator(), kMaxFlakes * sizeof(FlakeVertex),
                                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        f.sprites = s.buffer;
        f.spritesAlloc = s.allocation;
        f.spritesMapped = s.info.pMappedData;
        f.flakes = p.buffer;
        f.flakesAlloc = p.allocation;
        f.flakesMapped = p.info.pMappedData;
        if (!f.sprites || !f.spritesMapped || !f.flakes || !f.flakesMapped) {
            LOG_ERROR("Weather: vertex buffer allocation failed");
            shutdown();
            return false;
        }
    }
    drops_.reserve(kMaxDrops);
    splashes_.reserve(kMaxSplashes);
    flakes_.reserve(kMaxFlakes);
    LOG_INFO("Weather: initialized (rain ", kDropsAtFull, " drops, snow ", kFlakesAtFull, " flakes at full)");
    return true;
}

bool Weather::createPipelines() {
    VkDevice device = vkCtx_->getDevice();
    auto spriteShaders = loadShaderPair(device, "assets/shaders/weather_mod2x.vert.spv",
                                        "assets/shaders/weather_mod2x.frag.spv", "weather mod2x");
    auto snowShaders = loadShaderPair(device, "assets/shaders/weather_snow.vert.spv",
                                      "assets/shaders/weather_snow.frag.spv", "weather snow");
    if (!spriteShaders || !snowShaders) return false;

    struct SpritePush { glm::vec4 color; };
    struct SnowPush { glm::vec4 color; glm::vec4 size; };
    if (!mod2xLayout_) {
        VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SpritePush)};
        mod2xLayout_ = createPipelineLayout(device, {perFrameLayout_, dropTexture_.layout()}, {push});
    }
    if (!snowLayout_) {
        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(SnowPush)};
        snowLayout_ = createPipelineLayout(device, {perFrameLayout_, flakeTexture_.layout()}, {push});
    }
    if (!mod2xLayout_ || !snowLayout_) return false;

    // Mod2x: destination * source * 2, destination alpha kept.
    VkPipelineColorBlendAttachmentState mod2x = PipelineBuilder::blendAlpha();
    mod2x.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
    mod2x.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
    mod2x.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    mod2x.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;

    VkVertexInputBindingDescription spriteBinding{0, sizeof(SpriteVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    std::vector<VkVertexInputAttributeDescription> spriteAttrs{
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SpriteVertex, position)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(SpriteVertex, uv)},
    };
    mod2xPipeline_ = PipelineBuilder()
        .setShaders(spriteShaders.vertStage, spriteShaders.fragStage)
        .setVertexInput({spriteBinding}, spriteAttrs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(mod2x)
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(mod2xLayout_)
        .setRenderPass(vkCtx_->getImGuiRenderPass())
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx_->getPipelineCache());

    VkVertexInputBindingDescription flakeBinding{0, sizeof(FlakeVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    std::vector<VkVertexInputAttributeDescription> flakeAttrs{
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(FlakeVertex, positionAlpha)},
    };
    snowPipeline_ = PipelineBuilder()
        .setShaders(snowShaders.vertStage, snowShaders.fragStage)
        .setVertexInput({flakeBinding}, flakeAttrs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_POINT_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(snowLayout_)
        .setRenderPass(vkCtx_->getImGuiRenderPass())
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx_->getPipelineCache());

    spriteShaders.vert.destroy();
    spriteShaders.frag.destroy();
    snowShaders.vert.destroy();
    snowShaders.frag.destroy();
    return mod2xPipeline_ && snowPipeline_;
}

void Weather::recreatePipelines() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    destroy(device, mod2xPipeline_);
    destroy(device, snowPipeline_);
    if (!createPipelines()) LOG_ERROR("Weather: pipeline recreation failed");
    mist_.recreatePipeline();
}

void Weather::shutdown() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    vkDeviceWaitIdle(device);
    for (auto& f : frames_) {
        destroy(vkCtx_->getAllocator(), f.sprites, f.spritesAlloc);
        destroy(vkCtx_->getAllocator(), f.flakes, f.flakesAlloc);
        f = FrameBuffers{};
    }
    dropTexture_.destroy(*vkCtx_);
    splashTexture_.destroy(*vkCtx_);
    flakeTexture_.destroy(*vkCtx_);
    mist_.shutdown();
    destroy(device, mod2xPipeline_);
    destroy(device, snowPipeline_);
    destroy(device, mod2xLayout_);
    destroy(device, snowLayout_);
    clearParticles();
    vkCtx_ = nullptr;
}

void Weather::clearParticles() {
    drops_.clear();
    splashes_.clear();
    flakes_.clear();
    dropVertices_.clear();
    splashVertices_.clear();
    flakeVertices_.clear();
    rainBudget_ = 0.0f;
    snowBudget_ = 0.0f;
    mist_.clear();
}

void Weather::setWeatherType(Type type) {
    if (type != weatherType) clearParticles();
    weatherType = type;
}

void Weather::setEnabled(bool on) {
    if (!on && enabled) clearParticles();
    enabled = on;
}

void Weather::setIntensity(float value) {
    intensity = glm::clamp(value, 0.0f, 1.0f);
    // Rain that stops is gone at once (2.4.3: within a second of the packet).
    if (intensity <= 0.0f) clearParticles();
}

void Weather::setDensityLevel(int level) {
    assert(level >= 0 && level <= 3);
    densityScale_ = (2.0f * static_cast<float>(level) + 1.0f) / 5.0f;
}

int Weather::getParticleCount() const {
    return static_cast<int>(drops_.size() + splashes_.size() + flakes_.size());
}

float Weather::groundAt(float x, float y) {
    // Twenty thousand drops a second land somewhere; the ground under them is
    // looked up once per two-yard cell, searching down from the top of the
    // box, and kept while the camera stays within a few yards of the height
    // it was looked up from (update).
    if (groundCache_.size() > 200000) groundCache_.clear();
    const int64_t cx = static_cast<int64_t>(std::floor(x * 0.5f));
    const int64_t cy = static_cast<int64_t>(std::floor(y * 0.5f));
    const int64_t key = (cx << 32) ^ (cy & 0xffffffff);
    if (auto it = groundCache_.find(key); it != groundCache_.end()) return it->second;
    const std::optional<float> h = groundQuery_ ? groundQuery_(cx * 2.0f + 1.0f, cy * 2.0f + 1.0f, groundCacheZ_)
                                                : std::nullopt;
    // Nothing under it yet (terrain still streaming in) is asked again later.
    if (!h) return groundCacheZ_ - kRainSpawnAbove - kFallFloor;
    groundCache_.emplace(key, *h);
    return *h;
}

void Weather::spawnDrop(const glm::vec3& camera, bool anyHeight) {
    glm::vec3 head(camera.x + uniform(-kRainBoxHalf, kRainBoxHalf),
                   camera.y + uniform(-kRainBoxHalf, kRainBoxHalf),
                   camera.z + kRainSpawnAbove);
    const glm::vec3 v = kDropDirection * kDropSpeed;
    // Where it comes down: the ground under the point the wind carries it to.
    float ground = groundAt(head.x, head.y);
    for (int i = 0; i < 2; ++i) {
        const float t = (head.z - ground) / -v.z;
        ground = groundAt(head.x + v.x * t, head.y + v.y * t);
    }
    ground = std::max(ground, camera.z - kFallFloor);
    if (anyHeight) {
        // Rain that starts fills its box at once: anywhere along the path.
        if (ground >= head.z) return;
        head += v * uniform(0.0f, (head.z - ground) / -v.z);
    }
    drops_.push_back({head, ground, -1.0f});
}

void Weather::spawnFlake(const glm::vec3& camera, bool anyHeight) {
    glm::vec3 p(camera.x + uniform(-kSnowBoxHalf, kSnowBoxHalf),
                camera.y + uniform(-kSnowBoxHalf, kSnowBoxHalf),
                camera.z + kSnowSpawnAbove);
    float ground = groundAt(p.x, p.y);
    for (int i = 0; i < 2; ++i) {
        const float t = (p.z - ground) / -kFlakeVelocity.z;
        ground = groundAt(p.x + kFlakeVelocity.x * t, p.y + kFlakeVelocity.y * t);
    }
    ground = std::max(ground, camera.z - kFallFloor);
    float age = 0.0f;
    if (anyHeight) {
        if (ground >= p.z) return;
        p += kFlakeVelocity * uniform(0.0f, (p.z - ground) / -kFlakeVelocity.z);
        age = 1.0f;  // past the fade-in
    }
    flakes_.push_back({p, ground, age, -1.0f});
}

void Weather::update(const Camera& camera, float deltaTime) {
    if (!vkCtx_) return;
    const bool raining = weatherType == Type::RAIN || weatherType == Type::STORM;
    float amount = (enabled && weatherType != Type::NONE) ? intensity * densityScale_ : 0.0f;
    // Ground lookups search down from the top of the box.
    const float top = camera.getPosition().z + std::max(kRainSpawnAbove, kSnowSpawnAbove);
    if (std::abs(top - groundCacheZ_) > 5.0f) {
        groundCache_.clear();
        groundCacheZ_ = top;
    }
    // No weather at all with the camera high above the ground (kRainCeiling).
    if (amount > 0.0f) {
        const glm::vec3 cam = camera.getPosition();
        if (cam.z - groundAt(cam.x, cam.y) > kRainCeiling) {
            amount = 0.0f;
            clearParticles();
        }
    }
    updateRain(camera, deltaTime, raining ? amount : 0.0f);
    updateSnow(camera, deltaTime, weatherType == Type::SNOW ? amount : 0.0f);
    mist_.update(camera, deltaTime, amount, weatherType == Type::SNOW ? kSnowMist : kRainMist);

    // What is in the air, every ten seconds while anything is.
    logTimer_ += deltaTime;
    if (logTimer_ >= 10.0f && (getParticleCount() > 0 || mist_.puffCount() > 0)) {
        logTimer_ = 0.0f;
        LOG_INFO("Weather: amount ", amount, " drops ", drops_.size(), " splashes ", splashes_.size(),
                 " flakes ", flakes_.size(), " mist puffs ", mist_.puffCount());
    }
}

void Weather::updateRain(const Camera& camera, float deltaTime, float amount) {
    const glm::vec3 cam = camera.getPosition();
    const glm::vec3 v = kDropDirection * kDropSpeed;
    const float tailTime = kDropLength / -v.z;

    // Born at the top of the box at the rate that keeps amount * the full
    // count in the air above flat ground at the camera.
    if (drops_.empty() && amount > 0.0f) {
        for (int i = 0; i < static_cast<int>(amount * kDropsAtFull); ++i) spawnDrop(cam, true);
    }
    rainBudget_ += amount * kDropsAtFull * -v.z / kRainSpawnAbove * deltaTime;
    while (rainBudget_ >= 1.0f && drops_.size() < kMaxDrops) {
        spawnDrop(cam, false);
        rainBudget_ -= 1.0f;
    }
    rainBudget_ = std::min(rainBudget_, 1.0f);

    for (Splash& s : splashes_) s.age += deltaTime;
    std::erase_if(splashes_, [](const Splash& s) { return s.age >= kSplashLife; });

    for (Drop& d : drops_) {
        if (d.landedAge >= 0.0f) {
            d.landedAge += deltaTime;
            continue;
        }
        d.head += v * deltaTime;
        // The box moves with the camera: a drop that leaves it on one side
        // comes in on the other, over different ground.
        const float wx = cam.x + wrap(d.head.x - cam.x, kRainBoxHalf);
        const float wy = cam.y + wrap(d.head.y - cam.y, kRainBoxHalf);
        if (wx != d.head.x || wy != d.head.y) {
            d.head.x = wx;
            d.head.y = wy;
            const float t = (d.head.z - d.groundZ) / -v.z;
            d.groundZ = std::max(groundAt(wx + v.x * std::max(t, 0.0f), wy + v.y * std::max(t, 0.0f)),
                                 cam.z - kFallFloor);
        }
        if (d.head.z <= d.groundZ) {
            d.head.z = d.groundZ;
            d.landedAge = 0.0f;
            if (splashes_.size() < kMaxSplashes) splashes_.push_back({d.head, 0.0f});
        }
    }
    std::erase_if(drops_, [tailTime](const Drop& d) { return d.landedAge >= tailTime; });

    // Corners, camera-relative. rain.bls: side = normalize(toCamera x back),
    // back = -velocity direction; the tail corner of a landed drop keeps
    // moving with the drop's velocity.
    const glm::vec3 back = -kDropDirection;
    dropVertices_.clear();
    for (const Drop& d : drops_) {
        const glm::vec3 head = d.head - cam;
        const glm::vec3 tailBase = head + (d.landedAge > 0.0f ? v * d.landedAge : glm::vec3(0.0f));
        glm::vec3 side = glm::cross(-head, back);
        const float len = glm::length(side);
        if (len < 1e-4f) continue;
        side *= kDropHalfWidth / len;
        dropVertices_.push_back({glm::vec4(head - side, 0.0f), {0.0f, 1.0f}});
        dropVertices_.push_back({glm::vec4(head + side, 0.0f), {1.0f, 1.0f}});
        dropVertices_.push_back({glm::vec4(tailBase + back * kDropLength, 0.0f), {0.5f, 0.0f}});
    }

    // Splashes: patter.bls. Base corners kSplashCell apart across the view,
    // apex kSplashCell up; texture triangle (0,1/4) (1/8,0) (1/4,1/4) of the
    // frame's cell.
    const glm::vec3 right = camera.getRight();
    const glm::vec3 up = camera.getUp();
    const glm::vec3 offsets[3] = {-right * (0.5f * kSplashCell), up * kSplashCell, right * (0.5f * kSplashCell)};
    const glm::vec2 cellUv[3] = {{0.0f, 0.25f}, {0.125f, 0.0f}, {0.25f, 0.25f}};
    splashVertices_.clear();
    for (const Splash& s : splashes_) {
        const glm::vec3 rel = s.position - cam;
        const float dist = glm::length(rel);
        if (dist < 1e-3f) continue;
        const float lookdown = std::max(-rel.z / dist, 0.0f);
        const float column = std::floor(s.age * 4.0f * 3.99f);
        const float row = std::floor((1.0f - lookdown) * 3.99f);
        const glm::vec3 shift = -0.5f * lookdown * offsets[1] + glm::vec3(0.0f, 0.0f, lookdown / 12.0f);
        for (int i = 0; i < 3; ++i) {
            splashVertices_.push_back({glm::vec4(rel + offsets[i] + shift, 0.0f),
                                       cellUv[i] + 0.25f * glm::vec2(column, row)});
        }
    }
}

void Weather::updateSnow(const Camera& camera, float deltaTime, float amount) {
    const glm::vec3 cam = camera.getPosition();
    if (flakes_.empty() && amount > 0.0f) {
        for (int i = 0; i < static_cast<int>(amount * kFlakesAtFull); ++i) spawnFlake(cam, true);
    }
    snowBudget_ += amount * kFlakesAtFull * -kFlakeVelocity.z / kSnowSpawnAbove * deltaTime;
    while (amount > 0.0f && snowBudget_ >= 1.0f && flakes_.size() < kMaxFlakes) {
        spawnFlake(cam, false);
        snowBudget_ -= 1.0f;
    }
    if (amount > 0.0f) snowBudget_ = std::min(snowBudget_, 1.0f);

    for (Flake& f : flakes_) {
        f.age += deltaTime;
        if (f.landedAge >= 0.0f) {
            f.landedAge += deltaTime;
            continue;
        }
        f.position += kFlakeVelocity * deltaTime;
        const float wx = cam.x + wrap(f.position.x - cam.x, kSnowBoxHalf);
        const float wy = cam.y + wrap(f.position.y - cam.y, kSnowBoxHalf);
        if (wx != f.position.x || wy != f.position.y) {
            f.position.x = wx;
            f.position.y = wy;
            f.groundZ = std::max(groundAt(wx, wy), cam.z - kFallFloor);
        }
        if (f.position.z <= f.groundZ) {
            f.position.z = f.groundZ;
            f.landedAge = 0.0f;
        }
    }
    // snowpoint.bls: alpha 1 - 4 * (time since landing), gone at zero.
    std::erase_if(flakes_, [](const Flake& f) { return f.landedAge >= 0.25f; });

    flakeVertices_.clear();
    for (const Flake& f : flakes_) {
        const float alpha = f.landedAge >= 0.0f ? 1.0f - 4.0f * f.landedAge : std::min(f.age, 1.0f);
        flakeVertices_.push_back({glm::vec4(f.position - cam, alpha)});
    }
}

void Weather::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    if (!vkCtx_) return;
    mist_.render(cmd, perFrameSet);
    FrameBuffers& f = frames_[vkCtx_->getCurrentFrame()];
    VkDeviceSize offset = 0;
    if (!dropVertices_.empty() || !splashVertices_.empty()) {
        auto* out = static_cast<SpriteVertex*>(f.spritesMapped);
        std::memcpy(out, dropVertices_.data(), dropVertices_.size() * sizeof(SpriteVertex));
        std::memcpy(out + dropVertices_.size(), splashVertices_.data(), splashVertices_.size() * sizeof(SpriteVertex));
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mod2xPipeline_);
        vkCmdBindVertexBuffers(cmd, 0, 1, &f.sprites, &offset);
        vkCmdPushConstants(cmd, mod2xLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(glm::vec4), &kEffectColor);
        VkDescriptorSet sets[] = {perFrameSet, dropTexture_.set()};
        if (!dropVertices_.empty()) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mod2xLayout_, 0, 2, sets, 0, nullptr);
            vkCmdDraw(cmd, static_cast<uint32_t>(dropVertices_.size()), 1, 0, 0);
        }
        if (!splashVertices_.empty()) {
            sets[1] = splashTexture_.set();
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mod2xLayout_, 0, 2, sets, 0, nullptr);
            vkCmdDraw(cmd, static_cast<uint32_t>(splashVertices_.size()), 1,
                      static_cast<uint32_t>(dropVertices_.size()), 0);
        }
    }
    if (!flakeVertices_.empty()) {
        std::memcpy(f.flakesMapped, flakeVertices_.data(), flakeVertices_.size() * sizeof(FlakeVertex));
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, snowPipeline_);
        const VkDescriptorSet sets[] = {perFrameSet, flakeTexture_.set()};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, snowLayout_, 0, 2, sets, 0, nullptr);
        struct { glm::vec4 color; glm::vec4 size; } push{kEffectColor, glm::vec4(kFlakeSize, 0.0f)};
        vkCmdPushConstants(cmd, snowLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        vkCmdBindVertexBuffers(cmd, 0, 1, &f.flakes, &offset);
        vkCmdDraw(cmd, static_cast<uint32_t>(flakeVertices_.size()), 1, 0, 0);
    }
}

// ---------------------------------------------------------------------------
// Zone-based weather configuration
// ---------------------------------------------------------------------------

void Weather::setZoneWeather(uint32_t zoneId, Type type, float minIntensity, float maxIntensity, float probability) {
    zoneWeatherTable_[zoneId] = {.type = type, .minIntensity = minIntensity, .maxIntensity = maxIntensity, .probability = probability};
}

void Weather::initializeZoneWeatherDefaults() {
    if (zoneWeatherInitialized_) return;
    zoneWeatherInitialized_ = true;

    // Eastern Kingdoms zones
    setZoneWeather(11,   Type::RAIN, 0.1f, 0.4f, 0.15f);  // Wetlands - moderate rain
    setZoneWeather(8,    Type::RAIN, 0.1f, 0.5f, 0.2f);   // Swamp of Sorrows
    setZoneWeather(33,   Type::RAIN, 0.2f, 0.7f, 0.25f);  // Stranglethorn Vale
    setZoneWeather(44,   Type::RAIN, 0.1f, 0.3f, 0.1f);   // Redridge Mountains - light rain
    setZoneWeather(36,   Type::RAIN, 0.1f, 0.4f, 0.15f);  // Alterac Mountains
    setZoneWeather(45,   Type::RAIN, 0.1f, 0.3f, 0.1f);   // Arathi Highlands
    setZoneWeather(267,  Type::RAIN, 0.2f, 0.5f, 0.2f);   // Hillsbrad Foothills
    setZoneWeather(28,   Type::RAIN, 0.1f, 0.3f, 0.1f);   // Western Plaguelands - occasional rain
    setZoneWeather(139,  Type::RAIN, 0.1f, 0.3f, 0.1f);   // Eastern Plaguelands

    // Snowy zones
    setZoneWeather(1,    Type::SNOW, 0.2f, 0.6f, 0.3f);   // Dun Morogh
    setZoneWeather(51,   Type::SNOW, 0.1f, 0.5f, 0.2f);   // Searing Gorge (occasional)
    setZoneWeather(41,   Type::SNOW, 0.1f, 0.4f, 0.15f);  // Deadwind Pass
    setZoneWeather(2817, Type::SNOW, 0.3f, 0.7f, 0.4f);   // Crystalsong Forest
    setZoneWeather(67,   Type::SNOW, 0.2f, 0.6f, 0.35f);  // Storm Peaks
    setZoneWeather(65,   Type::SNOW, 0.2f, 0.5f, 0.3f);   // Dragonblight
    setZoneWeather(394,  Type::SNOW, 0.1f, 0.4f, 0.2f);   // Grizzly Hills
    setZoneWeather(495,  Type::SNOW, 0.3f, 0.8f, 0.5f);   // Howling Fjord
    setZoneWeather(210,  Type::SNOW, 0.2f, 0.5f, 0.25f);  // Icecrown
    setZoneWeather(3537, Type::SNOW, 0.2f, 0.6f, 0.3f);   // Borean Tundra
    setZoneWeather(4742, Type::SNOW, 0.2f, 0.5f, 0.3f);   // Hrothgar's Landing

    // Kalimdor zones
    setZoneWeather(15,   Type::RAIN, 0.1f, 0.4f, 0.15f);  // Dustwallow Marsh
    setZoneWeather(16,   Type::RAIN, 0.1f, 0.3f, 0.1f);   // Azshara
    setZoneWeather(148,  Type::RAIN, 0.1f, 0.4f, 0.15f);  // Darkshore
    setZoneWeather(331,  Type::RAIN, 0.1f, 0.3f, 0.1f);   // Ashenvale
    setZoneWeather(405,  Type::RAIN, 0.1f, 0.3f, 0.1f);   // Desolace
    setZoneWeather(490,  Type::RAIN, 0.1f, 0.4f, 0.15f);  // Un'Goro Crater
    setZoneWeather(493,  Type::RAIN, 0.1f, 0.3f, 0.1f);   // Moonglade

    // Winterspring is snowy
    setZoneWeather(618,  Type::SNOW, 0.2f, 0.6f, 0.3f);   // Winterspring

    // Outland
    setZoneWeather(3483, Type::RAIN, 0.1f, 0.3f, 0.1f);   // Hellfire Peninsula (occasional)
    setZoneWeather(3521, Type::RAIN, 0.1f, 0.4f, 0.15f);  // Zangarmarsh
    setZoneWeather(3519, Type::RAIN, 0.1f, 0.3f, 0.1f);   // Terokkar Forest
}

void Weather::updateZoneWeather(uint32_t zoneId, float deltaTime) {
    if (!zoneWeatherInitialized_) {
        initializeZoneWeatherDefaults();
    }

    // Zone changed - reset weather cycle
    if (zoneId != currentWeatherZone_) {
        currentWeatherZone_ = zoneId;
        zoneWeatherTimer_ = 0.0f;

        auto it = zoneWeatherTable_.find(zoneId);
        if (it == zoneWeatherTable_.end()) {
            // Zone has no configured weather - clear gradually
            targetIntensity_ = 0.0f;
        } else {
            // Roll whether weather is active based on probability
            float roll = weatherRandFloat();
            zoneWeatherActive_ = (roll < it->second.probability);

            if (zoneWeatherActive_) {
                weatherType = it->second.type;
                // Random intensity within configured range
                float t = weatherRandFloat();
                targetIntensity_ = glm::mix(it->second.minIntensity, it->second.maxIntensity, t);
                // Random cycle duration: 3-8 minutes
                zoneWeatherCycleDuration_ = 180.0f + weatherRandFloat() * 300.0f;
            } else {
                targetIntensity_ = 0.0f;
                zoneWeatherCycleDuration_ = 120.0f + weatherRandFloat() * 180.0f;
            }
        }
    }

    // Smooth intensity transitions
    float transitionSpeed = 0.15f * deltaTime; // ~7 seconds to full transition
    if (intensity < targetIntensity_) {
        intensity = std::min(intensity + transitionSpeed, targetIntensity_);
    } else if (intensity > targetIntensity_) {
        intensity = std::max(intensity - transitionSpeed, targetIntensity_);
    }

    // If intensity reached zero and target is zero, clear weather type
    if (intensity <= 0.01f && targetIntensity_ <= 0.01f) {
        if (weatherType != Type::NONE) {
            weatherType = Type::NONE;
            clearParticles();
        }
    }

    // Weather cycling - periodically re-roll weather
    zoneWeatherTimer_ += deltaTime;
    if (zoneWeatherTimer_ >= zoneWeatherCycleDuration_ && zoneWeatherCycleDuration_ > 0.0f) {
        zoneWeatherTimer_ = 0.0f;

        auto it = zoneWeatherTable_.find(zoneId);
        if (it != zoneWeatherTable_.end()) {
            float roll = weatherRandFloat();
            zoneWeatherActive_ = (roll < it->second.probability);

            if (zoneWeatherActive_) {
                weatherType = it->second.type;
                float t = weatherRandFloat();
                targetIntensity_ = glm::mix(it->second.minIntensity, it->second.maxIntensity, t);
            } else {
                targetIntensity_ = 0.0f;
            }

            // New cycle duration
            zoneWeatherCycleDuration_ = 180.0f + weatherRandFloat() * 300.0f;
        }
    }
}

} // namespace rendering
} // namespace wowee
