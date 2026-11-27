// The order two light volumes are blended in, when they weigh the same.
//
// Light.dbc's volumes overlap and the branch for "inside the inner radius"
// assigns the constant 1.0, so equal weights are the ordinary case rather than
// a coincidence: 166 points on the shipped maps sit inside two such volumes
// and 23 inside three, while only the top two are blended.
//
// Weight alone therefore leaves the order undecided, and std::sort is unstable
// and decides it differently for different sequences. Walking kept changing
// the sequence - distant volumes crossing their outer radius enter and leave
// the list - so the chosen pair flipped from frame to frame. The blended
// colours are smoothed over time and survived it; the skybox is whichever
// volume comes first and names one, and that is swapped outright. The sky
// flickered while moving and stood still when the player did.
//
// What is asserted here is that the order is total: any two volumes that are
// not the same volume compare one way round and stay that way, whatever else
// is in the list.
#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "rendering/light_volume_order.hpp"

using wowee::rendering::lightVolumeOrderedBefore;

namespace {

struct Volume {
    float weight;
    float outerRadius;
    uint32_t id;
};

bool before(const Volume& a, const Volume& b) {
    return lightVolumeOrderedBefore(a.weight, a.outerRadius, a.id,
                                    b.weight, b.outerRadius, b.id);
}

}  // namespace

TEST_CASE("a heavier volume comes first", "[lighting]") {
    const Volume heavy{1.0f, 500.0f, 7};
    const Volume light{0.25f, 100.0f, 3};
    CHECK(before(heavy, light));
    CHECK_FALSE(before(light, heavy));
}

TEST_CASE("equal weights are decided by the tighter volume", "[lighting]") {
    // Both contain the player fully. The smaller one describes somewhere more
    // specific - a city inside a continent - so it is the one that wins.
    const Volume city{1.0f, 300.0f, 90};
    const Volume continent{1.0f, 20000.0f, 2};
    CHECK(before(city, continent));
    CHECK_FALSE(before(continent, city));
}

TEST_CASE("two volumes of the same extent still cannot swap", "[lighting]") {
    const Volume lower{1.0f, 300.0f, 11};
    const Volume higher{1.0f, 300.0f, 12};
    CHECK(before(lower, higher));
    CHECK_FALSE(before(higher, lower));
}

TEST_CASE("a volume never comes before itself", "[lighting]") {
    const Volume one{1.0f, 300.0f, 11};
    CHECK_FALSE(before(one, one));
}

TEST_CASE("the chosen pair does not depend on what else is in the list",
          "[lighting]") {
    // The bug, as the player experienced it. Three volumes tie at 1.0 and only
    // two are kept; around them, volumes far enough away to weigh almost
    // nothing come and go as the player walks. Under an order that decides
    // ties, the two kept are the same two every time.
    const std::vector<Volume> tied = {
        {1.0f, 20000.0f, 2},   // the continent
        {1.0f, 900.0f, 47},    // a zone
        {1.0f, 240.0f, 618},   // a courtyard
    };

    std::vector<Volume> expected = tied;
    std::sort(expected.begin(), expected.end(), before);
    expected.resize(2);

    std::mt19937 rng(20260813);
    for (int round = 0; round < 200; ++round) {
        std::vector<Volume> list = tied;
        // Whatever else happens to be in range this frame, in whatever order
        // the volume table happens to hold it.
        const int extras = static_cast<int>(rng() % 6);
        for (int i = 0; i < extras; ++i) {
            list.push_back({static_cast<float>(rng() % 100) / 1000.0f,
                            static_cast<float>(500 + rng() % 4000),
                            1000 + static_cast<uint32_t>(rng() % 500)});
        }
        std::shuffle(list.begin(), list.end(), rng);

        std::partial_sort(list.begin(), list.begin() + 2, list.end(), before);
        INFO("round " << round << " with " << extras << " extra volumes");
        CHECK(list[0].id == expected[0].id);
        CHECK(list[1].id == expected[1].id);
    }
}

TEST_CASE("Light rows lie over the wider rows under them", "[light-blend]") {
    using wowee::rendering::layerLightWeights;
    // One light at 0.3 in its falloff: the default row takes 0.7.
    std::vector<float> one{0.3f};
    CHECK(layerLightWeights(one, true) == Catch::Approx(0.7f));
    CHECK(one[0] == Catch::Approx(0.3f));
    // Inside a light's inner radius: the default takes nothing (Dun Morogh,
    // Light 22 at 348 yd of its 737 yd inner radius).
    std::vector<float> full{1.0f};
    CHECK(layerLightWeights(full, true) == Catch::Approx(0.0f));
    // Forge Camp: Mageddon, inside Light 653 (inner 81 yd) and Light 520
    // (Hellfire, inner 1779 yd): 653 alone, as in the original.
    std::vector<float> camp{1.0f, 1.0f};
    CHECK(layerLightWeights(camp, true) == Catch::Approx(0.0f));
    CHECK(camp[0] == Catch::Approx(1.0f));
    CHECK(camp[1] == Catch::Approx(0.0f));
    // Light 653 at 0.4 in its falloff: Light 520 takes the other 0.6.
    std::vector<float> edge{0.4f, 1.0f};
    CHECK(layerLightWeights(edge, true) == Catch::Approx(0.0f));
    CHECK(edge[0] == Catch::Approx(0.4f));
    CHECK(edge[1] == Catch::Approx(0.6f));
    // Two falloffs and the default row share what each leaves.
    std::vector<float> two{0.9f, 0.9f};
    CHECK(layerLightWeights(two, true) == Catch::Approx(0.01f));
    CHECK(two[0] == Catch::Approx(0.9f));
    CHECK(two[1] == Catch::Approx(0.09f));
    // Nothing positioned: all default.
    std::vector<float> none;
    CHECK(layerLightWeights(none, true) == Catch::Approx(1.0f));
    // A map without a default row keeps its lights normalized to 1.
    std::vector<float> noDefault{0.3f};
    CHECK(layerLightWeights(noDefault, false) == Catch::Approx(0.0f));
    CHECK(noDefault[0] == Catch::Approx(1.0f));
}

TEST_CASE("Light row weight falls linearly across its falloff", "[light-blend]") {
    using wowee::rendering::lightVolumeFalloff;
    // Light 653: inner 81.4, outer 127.9 yd.
    CHECK(lightVolumeFalloff(60.0f, 81.4f, 127.9f) == 1.0f);
    CHECK(lightVolumeFalloff(95.1f, 81.4f, 127.9f) == Catch::Approx(0.705f).margin(0.005f));
    CHECK(lightVolumeFalloff(118.3f, 81.4f, 127.9f) == Catch::Approx(0.206f).margin(0.005f));
    CHECK(lightVolumeFalloff(140.0f, 81.4f, 127.9f) == 0.0f);
}

TEST_CASE("Inner LightParams with lightSkyboxId == 0 inherit enclosing zone skybox",
          "[light-blend][skybox]") {
    using wowee::rendering::layerLightSkyboxWeights;
    using wowee::rendering::SkyVolumeCandidate;

    // The Stormspire Eco-Dome: Light 669 (LP 552, sb=0) inside Light 666
    // (LP 552, sb=0) inside Netherstorm Light 502 (LP 508, sb=10).
    {
        const std::vector<SkyVolumeCandidate> stormspire = {
            {.falloff = 1.0f, .clearSkyboxId = 0, .stormSkyboxId = 0, .clearParamsId = 552, .stormParamsId = 552},
            {.falloff = 1.0f, .clearSkyboxId = 0, .stormSkyboxId = 0, .clearParamsId = 552, .stormParamsId = 552},
            {.falloff = 1.0f, .clearSkyboxId = 10, .stormSkyboxId = 10, .clearParamsId = 508, .stormParamsId = 508},
        };
        std::vector<wowee::rendering::SkyParamsIdShare> paramShares;
        const auto shares = layerLightSkyboxWeights(
            stormspire, 0.0f, 0, 0, true, 379, 379, &paramShares);
        REQUIRE(shares.size() == 1);
        CHECK(shares[0].skyboxId == 10);
        CHECK(shares[0].weight == Catch::Approx(1.0f));
        REQUIRE(paramShares.size() == 1);
        CHECK(paramShares[0].paramsId == 508);
        CHECK(paramShares[0].weight == Catch::Approx(1.0f));
    }

    // Walking across Light 669's falloff ring (falloff = 0.45): Netherstorm's
    // skybox stays steady at 1.0 without dipping.
    {
        const std::vector<SkyVolumeCandidate> bridge = {
            {.falloff = 0.45f, .clearSkyboxId = 0, .stormSkyboxId = 0},
            {.falloff = 1.0f, .clearSkyboxId = 10, .stormSkyboxId = 10},
        };
        const auto shares = layerLightSkyboxWeights(bridge, 0.0f, 0, 0, true);
        REQUIRE(shares.size() == 1);
        CHECK(shares[0].skyboxId == 10);
        CHECK(shares[0].weight == Catch::Approx(1.0f));
    }

    // Inner volume with its own skybox (Blade's Edge Forest sb=14 inside
    // Blade's Edge sb=11) crossfades smoothly across its falloff.
    {
        const std::vector<SkyVolumeCandidate> forestEdge = {
            {.falloff = 0.4f, .clearSkyboxId = 14, .stormSkyboxId = 14},
            {.falloff = 1.0f, .clearSkyboxId = 11, .stormSkyboxId = 11},
        };
        const auto shares = layerLightSkyboxWeights(forestEdge, 0.0f, 0, 0, true);
        REQUIRE(shares.size() == 2);
        CHECK(shares[0].skyboxId == 14);
        CHECK(shares[0].weight == Catch::Approx(0.4f));
        CHECK(shares[1].skyboxId == 11);
        CHECK(shares[1].weight == Catch::Approx(0.6f));
    }

    // Zangarmarsh (Light 523, sb=0, falloff=1.0) overlapping the outer 8 yd of
    // Hellfire Peninsula (Light 377, sb=7, falloff=0.043) over map 530 default
    // (Terokkar Light 374, sb=0): Hellfire stays at 0.043 rather than 1.0.
    {
        const std::vector<SkyVolumeCandidate> zangarBorder = {
            {.falloff = 1.0f, .clearSkyboxId = 0, .stormSkyboxId = 0},
            {.falloff = 0.043f, .clearSkyboxId = 7, .stormSkyboxId = 7},
        };
        const auto shares = layerLightSkyboxWeights(zangarBorder, 0.0f, 0, 0, true);
        REQUIRE(shares.size() == 1);
        CHECK(shares[0].skyboxId == 7);
        CHECK(shares[0].weight == Catch::Approx(0.043f));
    }

    // Netherweb Ridge (Light 602: clear LP 565 sb=0, storm LP 564 sb=15)
    // nested inside Bone Wastes (Light 742: LP 500 sb=8).
    {
        const std::vector<SkyVolumeCandidate> netherweb = {
            {.falloff = 1.0f, .clearSkyboxId = 0, .stormSkyboxId = 15},
            {.falloff = 1.0f, .clearSkyboxId = 8, .stormSkyboxId = 8},
        };
        const auto clearShares = layerLightSkyboxWeights(netherweb, 0.0f, 0, 0, true);
        REQUIRE(clearShares.size() == 1);
        CHECK(clearShares[0].skyboxId == 8);
        CHECK(clearShares[0].weight == Catch::Approx(1.0f));

        const auto stormShares = layerLightSkyboxWeights(netherweb, 1.0f, 0, 0, true);
        REQUIRE(stormShares.size() == 1);
        CHECK(stormShares[0].skyboxId == 15);
        CHECK(stormShares[0].weight == Catch::Approx(1.0f));

        const auto midShares = layerLightSkyboxWeights(netherweb, 0.35f, 0, 0, true);
        REQUIRE(midShares.size() == 2);
        CHECK(midShares[0].skyboxId == 15);
        CHECK(midShares[0].weight == Catch::Approx(0.35f));
        CHECK(midShares[1].skyboxId == 8);
        CHECK(midShares[1].weight == Catch::Approx(0.65f));
    }

    // Map default with a skybox (e.g. Tempest Keep / Black Temple) shines
    // through an inner positioned volume with lightSkyboxId == 0.
    {
        const std::vector<SkyVolumeCandidate> innerNoSky = {
            {.falloff = 1.0f, .clearSkyboxId = 0, .stormSkyboxId = 0},
        };
        const auto shares = layerLightSkyboxWeights(innerNoSky, 0.0f, 18, 18, true);
        REQUIRE(shares.size() == 1);
        CHECK(shares[0].skyboxId == 18);
        CHECK(shares[0].weight == Catch::Approx(1.0f));
    }
}
