// TBC 2.4.3 ground-effect detail doodad material, UV clamping, mip-0 sampling,
// and two-pass alpha cutout + fringe blend parity tests.
#include <catch_amalgamated.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

#include "pipeline/blp_loader.hpp"
#include "pipeline/m2_loader.hpp"
#include "rendering/m2_ground_detail_material.hpp"

namespace {

std::vector<uint8_t> readBinaryFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const auto sz = in.tellg();
    if (sz <= 0) return {};
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(buf.data()), sz);
    return buf;
}

std::filesystem::path findTbcDetailAsset(const char* filename) {
    const std::filesystem::path roots[] = {
        "/workspace/azerothcore/client/wowee-data/expansions/tbc/world/nodxt/detail",
        "/workspace/azerothcore/client/wowee-data/world/nodxt/detail",
    };
    std::string target(filename);
    for (char& c : target) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    for (const auto& root : roots) {
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
            std::string name = entry.path().filename().string();
            for (char& c : name) {
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            }
            if (name == target) return entry.path();
        }
    }
    return {};
}

}  // namespace

TEST_CASE("ground detail UV wrap flags clamp multi-sprite atlas overhangs while keeping true tiling",
          "[m2][ground_detail]") {
    using wowee::rendering::m2GroundDetailEffectiveTexFlags;

    // BarGra05.m2: authored flags = 3 (WrapS|WrapT), u=[0.0158, 0.5940] (span 0.5782),
    // v=[-0.0100, 1.0012] (span 1.0112). Must clamp both axes so v=-0.0100 in mid-air
    // never wraps to v=0.9900 reed stems at the bottom of BarrensDetailDoodad_256.blp.
    CHECK(m2GroundDetailEffectiveTexFlags(0x3u, 0.5782f, 1.0112f) == 0x0u);

    // BarGra06.m2: authored flags = 3, u=[0.3080, 0.7244] (span 0.4164),
    // v=[0.4891, 1.3128] (span 0.8237). Must clamp both axes so v>1.0 never wraps
    // to top-of-atlas grass blades on sloped beaches.
    CHECK(m2GroundDetailEffectiveTexFlags(0x3u, 0.4164f, 0.8237f) == 0x0u);

    // DwmGra03.m2: authored flags = 3, u=[0.7058, 1.0227] (span 0.3169),
    // v=[0.1772, 0.4125] (span 0.2353). Clamping U prevents u>1.0 wrapping onto
    // the opaque rock slab at u=0.000..0.406 in DustwallowDetailDoodads_256.blp.
    CHECK(m2GroundDetailEffectiveTexFlags(0x3u, 0.3169f, 0.2353f) == 0x0u);

    // LchGra02.m2 (WetlandsTallGrass01.blp): horizontally tiling grass strip
    // u=[-0.4064, 2.2294] (span 2.6357), v=[0.0038, 0.9734] (span 0.9696).
    // Preserves WrapS (0x1) while clamping T.
    CHECK(m2GroundDetailEffectiveTexFlags(0x3u, 2.6357f, 0.9696f) == 0x1u);

    // LakGra02.m2 (LakeDetails.blp): vertically tiling underwater kelp stalk
    // u=[0.7512, 1.0000] (span 0.2488), v=[0.0070, 2.1038] (span 2.0968).
    // Clamps S (so u=1.000 never bleeds into u=0.000) while preserving WrapT (0x2).
    CHECK(m2GroundDetailEffectiveTexFlags(0x3u, 0.2488f, 2.0968f) == 0x2u);

    // Models authored with clamp (flags = 0) never gain wrap bits even if UV span > 1.1.
    CHECK(m2GroundDetailEffectiveTexFlags(0x0u, 2.5f, 2.5f) == 0x0u);

    // High non-wrap bits on authored flags are preserved untouched.
    CHECK(m2GroundDetailEffectiveTexFlags(0x8u | 0x3u, 0.5f, 0.5f) == 0x8u);
}

TEST_CASE("ground detail UV span ignores non-finite UVs and handles empty meshes",
          "[m2][ground_detail]") {
    using wowee::pipeline::M2Vertex;
    using wowee::rendering::m2GroundDetailTextureUVSpan;

    std::vector<M2Vertex> verts;
    CHECK(m2GroundDetailTextureUVSpan(verts, 0) == std::pair<float, float>{0.0f, 0.0f});

    M2Vertex a{};
    a.texCoords[0] = glm::vec2(0.0158f, -0.0100f);
    a.texCoords[1] = glm::vec2(0.10f, 0.20f);
    M2Vertex b{};
    b.texCoords[0] = glm::vec2(0.5940f, 1.0012f);
    b.texCoords[1] = glm::vec2(1.60f, 0.70f);
    M2Vertex nanV{};
    nanV.texCoords[0] = glm::vec2(std::numeric_limits<float>::quiet_NaN(), 50.0f);
    nanV.texCoords[1] = glm::vec2(-20.0f, std::numeric_limits<float>::infinity());
    verts = {a, b, nanV};

    const auto [u0, v0] = m2GroundDetailTextureUVSpan(verts, 0);
    CHECK(u0 == Catch::Approx(0.5940f - 0.0158f).epsilon(1e-5f));
    CHECK(v0 == Catch::Approx(1.0012f - (-0.0100f)).epsilon(1e-5f));

    const auto [u1, v1] = m2GroundDetailTextureUVSpan(verts, 1);
    CHECK(u1 == Catch::Approx(1.50f).epsilon(1e-5f));
    CHECK(v1 == Catch::Approx(0.50f).epsilon(1e-5f));
}

TEST_CASE("ground detail mipmap suppression and disjoint two-pass alpha partition",
          "[m2][ground_detail]") {
    using wowee::rendering::kM2DrawModeGroundDetailCore;
    using wowee::rendering::kM2DrawModeGroundDetailFringe;
    using wowee::rendering::m2GroundDetailCorePassesAlpha;
    using wowee::rendering::m2GroundDetailDisableMipmaps;
    using wowee::rendering::m2GroundDetailFringePassesAlpha;

    // Only procedural ground-effect detail doodads disable mipmaps; ordinary placed
    // M2 foliage/trees keep their full mipchain.
    CHECK(m2GroundDetailDisableMipmaps(true));
    CHECK_FALSE(m2GroundDetailDisableMipmaps(false));

    // Core and fringe draw modes are distinct so m2.vert / m2.frag select Pass 1A vs Pass 1B.
    CHECK(kM2DrawModeGroundDetailCore == 2);
    CHECK(kM2DrawModeGroundDetailFringe == 3);

    // Alpha == 0 is discarded by both passes.
    CHECK_FALSE(m2GroundDetailCorePassesAlpha(0.0f));
    CHECK_FALSE(m2GroundDetailFringePassesAlpha(0.0f));

    // Every non-zero 8-bit alpha value in [1..255] belongs to EXACTLY ONE of
    // Pass 1A (depth-writing opaque core, >= 128) or Pass 1B (non-depth-writing
    // blended fringe, 1..127): never both, never neither.
    for (int a8 = 1; a8 <= 255; ++a8) {
        const float a = static_cast<float>(a8) / 255.0f;
        const bool inCore = m2GroundDetailCorePassesAlpha(a);
        const bool inFringe = m2GroundDetailFringePassesAlpha(a);
        INFO("a8=" << a8);
        CHECK(inCore != inFringe);
        if (a8 < 128) {
            CHECK(inFringe);
            CHECK_FALSE(inCore);
        } else {
            CHECK(inCore);
            CHECK_FALSE(inFringe);
        }
    }
}

TEST_CASE("shipped TBC Ratchet Barrens, Dustwallow, Wetlands and Lake detail assets obey parity invariants",
          "[m2][ground_detail][assets]") {
    using wowee::pipeline::BLPLoader;
    using wowee::pipeline::M2Loader;
    using wowee::rendering::m2GroundDetailDisableMipmaps;
    using wowee::rendering::m2GroundDetailEffectiveTexFlags;
    using wowee::rendering::m2GroundDetailTextureUVSpan;

    const auto barGra05Path = findTbcDetailAsset("bargra05.m2");
    if (barGra05Path.empty()) {
        return;  // TBC client data not mounted in minimal CI environment
    }

    auto checkModel = [](const char* file, uint32_t expectedEffectiveWrap) {
        const auto path = findTbcDetailAsset(file);
        REQUIRE_FALSE(path.empty());
        const auto bytes = readBinaryFile(path);
        REQUIRE_FALSE(bytes.empty());
        const auto m2 = M2Loader::load(bytes);
        REQUIRE(m2.isValid());
        REQUIRE_FALSE(m2.textures.empty());
        const auto [uSpan, vSpan] = m2GroundDetailTextureUVSpan(m2.vertices, 0);
        const uint32_t eff = m2GroundDetailEffectiveTexFlags(m2.textures[0].flags, uSpan, vSpan);
        INFO(file << " authoredFlags=" << m2.textures[0].flags
                  << " uSpan=" << uSpan << " vSpan=" << vSpan);
        CHECK(eff == expectedEffectiveWrap);
    };

    // Ratchet coastal reeds & Dustwallow grass clamp both S and T.
    checkModel("bargra05.m2", 0x0u);
    checkModel("bargra06.m2", 0x0u);
    checkModel("dwmgra01.m2", 0x0u);
    checkModel("dwmgra03.m2", 0x0u);

    // Wetlands tall grass repeats S only; Lake kelp repeats T only.
    checkModel("lchgra02.m2", 0x1u);
    checkModel("lakgra02.m2", 0x2u);

    // BarrensDetailDoodad_256.blp has soft 4-bit DXT3 alpha in mip 0 while its
    // authored mip 1 is binary; ground-detail mip suppression preserves mip 0.
    const auto atlasPath = findTbcDetailAsset("barrensdetaildoodad_256.blp");
    REQUIRE_FALSE(atlasPath.empty());
    const auto blpBytes = readBinaryFile(atlasPath);
    REQUIRE_FALSE(blpBytes.empty());
    const auto blp = BLPLoader::load(blpBytes, /*headerAndMipsOnly=*/false);
    REQUIRE(blp.isValid());
    CHECK(blp.width == 256);
    CHECK(blp.height == 256);
    CHECK(m2GroundDetailDisableMipmaps(true));

    size_t mip0SoftAlphaTexels = 0;
    for (size_t i = 3; i < blp.data.size(); i += 4) {
        const uint8_t a = blp.data[i];
        if (a > 0 && a < 255) ++mip0SoftAlphaTexels;
    }
    CHECK(mip0SoftAlphaTexels > 1000);
}
