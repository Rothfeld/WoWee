// The minimap's world<->pixel transform, which the client needs in both
// directions: markers project outward, clicks invert back.
//
// It was written out longhand three times in game_screen_minimap.cpp - once
// forward for markers, once inverted for the ctrl+click ping, once inverted
// again for the coordinate tooltip. Two copies of an inverse is two chances to
// get a sign wrong, and a wrong sign there pings the mirror image of where you
// clicked. FrameXML's Minimap:PingLocation would have been a fourth.
//
// The contract is not "north is up". It is that this projection is the exact
// inverse of what minimap_display.frag.glsl does, because that shader is what
// decides where a piece of the world is on screen - a marker is right when it
// lands on the ground it names, whatever direction that ground is drawn in.
// So the shader is reproduced here and the two are composed.
#include <cmath>

#include "catch_amalgamated.hpp"
#include "ui/minimap_projection.hpp"

using wowee::ui::MinimapView;
using wowee::ui::minimapOffsetToRenderDelta;
using wowee::ui::renderDeltaToMinimapOffset;

namespace {

/// minimap_display.frag.glsl, in C++. Takes an offset from the minimap centre
/// in pixels (+y down, as TexCoord grows) and answers the composite UV delta
/// it samples. Composite u grows east and v grows south - that is how
/// Minimap::compositePass lays the 3x3 tile grid out, dc along the east-west
/// tile axis into gridOffset.x and dr along north-south into gridOffset.y.
glm::vec2 shaderSampleUV(float px, float py, float mapRadius, float zoomK, float rotation) {
    // The shader works in TexCoord where the whole minimap spans 1.0, so an
    // offset in pixels is that offset over the minimap's width.
    const glm::vec2 center(px / (mapRadius * 2.0f), py / (mapRadius * 2.0f));
    const float cs = std::cos(rotation), sn = std::sin(rotation);
    const glm::vec2 mapCenter(-center.x, center.y);
    const glm::vec2 rotated(mapCenter.x * cs - mapCenter.y * sn,
                            mapCenter.x * sn + mapCenter.y * cs);
    return glm::vec2(rotated.y, -rotated.x) * zoomK * 2.0f;
}

}  // namespace

TEST_CASE("a marker lands on the ground the shader draws there") {
    // A non-rotating map, which is this client's default: the shader is handed
    // rotation 0 and the marker pass leaves the bearing at 0 to match.
    MinimapView v;
    v.viewRadius = 200.0f;
    v.mapRadius = 70.0f;

    // The shader's zoomRadius is the view radius in composite units, and the
    // composite spans three tiles. Any consistent value works here; this is
    // the one Minimap::render computes.
    constexpr float kTileSize = 533.33333f;
    const float zoomK = v.viewRadius / (kTileSize * 3.0f);
    // ...so one yard of world is this much UV.
    const float uvPerYard = 1.0f / (kTileSize * 3.0f);

    struct Case { float dx, dy; const char* what; };
    const Case cases[] = {
        {100.0f, 0.0f, "due west"},
        {0.0f, 100.0f, "due north"},
        {-60.0f, 0.0f, "due east"},
        {0.0f, -60.0f, "due south"},
        {45.0f, -80.0f, "off both axes"},
    };

    for (const auto& c : cases) {
        INFO(c.what);
        const glm::vec2 off = renderDeltaToMinimapOffset(c.dx, c.dy, v);
        const glm::vec2 uv = shaderSampleUV(off.x, off.y, v.mapRadius, zoomK, 0.0f);

        // Render +X is west and +Y is north, so the sample the shader takes
        // has to be that far east and that far south of the player.
        CHECK(uv.x == Catch::Approx(-c.dx * uvPerYard).margin(1e-6f));
        CHECK(uv.y == Catch::Approx(-c.dy * uvPerYard).margin(1e-6f));
    }
}

TEST_CASE("projecting out and inverting back returns the same world delta") {
    // At a bearing that mixes both axes, which is where a transposed or
    // negated term stops cancelling itself out. This is the property the
    // duplicated inverse existed to satisfy and nothing checked.
    for (int deg = 0; deg < 360; deg += 37) {
        MinimapView v;
        v.viewRadius = 460.0f;
        v.mapRadius = 64.0f;
        const float rad = static_cast<float>(deg) * 3.14159265f / 180.0f;
        v.cosBearing = std::cos(rad);
        v.sinBearing = std::sin(rad);

        const float dx = 137.0f, dy = -42.0f;
        const glm::vec2 off = renderDeltaToMinimapOffset(dx, dy, v);
        const glm::vec2 back = minimapOffsetToRenderDelta(off.x, off.y, v);
        INFO("bearing " << deg);
        CHECK(back.x == Catch::Approx(dx).margin(0.01f));
        CHECK(back.y == Catch::Approx(dy).margin(0.01f));
    }
}

TEST_CASE("distance from the centre is the distance from the player, to scale") {
    // What the marker pass actually gates on: anything further out than the
    // map radius is off the edge and not drawn. That only holds if the
    // transform is a rotation and a uniform scale, with no shear.
    MinimapView v;
    v.viewRadius = 300.0f;
    v.mapRadius = 75.0f;
    v.cosBearing = std::cos(1.1f);
    v.sinBearing = std::sin(1.1f);

    const float dx = 90.0f, dy = 120.0f;  // 150 yards out
    const glm::vec2 off = renderDeltaToMinimapOffset(dx, dy, v);
    CHECK(std::sqrt(off.x * off.x + off.y * off.y)
          == Catch::Approx(150.0f * v.mapRadius / v.viewRadius).margin(0.01f));
}

TEST_CASE("zooming in pushes a fixed world point further from the centre") {
    MinimapView wide, close;
    wide.mapRadius = close.mapRadius = 70.0f;
    wide.viewRadius = 800.0f;
    close.viewRadius = 200.0f;

    const glm::vec2 a = renderDeltaToMinimapOffset(0.0f, 100.0f, wide);
    const glm::vec2 b = renderDeltaToMinimapOffset(0.0f, 100.0f, close);
    CHECK(std::hypot(b.x, b.y) > std::hypot(a.x, a.y));
}

TEST_CASE("a map with no rect yet answers zero rather than dividing by it") {
    // The map has no screen rect before the first frame it is placed on, and
    // the interface can still ask - Minimap_OnClick runs off a real click.
    MinimapView zero;
    zero.viewRadius = 0.0f;
    zero.mapRadius = 0.0f;

    CHECK(renderDeltaToMinimapOffset(10.0f, 10.0f, zero).x == Catch::Approx(0.0f));
    CHECK(minimapOffsetToRenderDelta(10.0f, 10.0f, zero).x == Catch::Approx(0.0f));
}

TEST_CASE("off-minimap guard POI clamps to a directional rim arrow (north-up and rotateMinimap)",
          "[minimap][poi]") {
    using wowee::ui::kMinimapRimArrowInset;
    using wowee::ui::placeMinimapPoi;

    MinimapView northUp;
    northUp.viewRadius = 100.0f;
    northUp.mapRadius = 70.0f;
    const float cx = 1200.0f, cy = 92.0f;

    // Stormwind Cathedral Square (-8677.5, 637.5) -> Stormwind Gryphon Master (-8839, 487.546)
    // in render coords (renderX = serverX north, renderY = serverY west):
    // dx = -161.5 (south -> +screenY), dy = -149.954 (east -> +screenX) => SE rim arrow.
    const glm::vec2 swDelta(-161.5f, -149.954f);
    const auto farNorthUp = placeMinimapPoi(swDelta, cx, cy, northUp, 1.0f);
    REQUIRE_FALSE(farNorthUp.onMap);
    CHECK(glm::length(farNorthUp.direction) == Catch::Approx(1.0f).margin(1e-5f));
    CHECK(farNorthUp.direction.x > 0.0f);  // east (+screenX)
    CHECK(farNorthUp.direction.y > 0.0f);  // south (+screenY)
    const float rimDist = std::hypot(farNorthUp.pos.x - cx, farNorthUp.pos.y - cy);
    CHECK(rimDist == Catch::Approx(northUp.mapRadius - kMinimapRimArrowInset).margin(1e-4f));

    // With rotateMinimap=1 and bearing = +pi/2 (cos=0, sin=1), the minimap
    // rotates clockwise by 90 deg in screen space: (x, y) -> (-y, x).
    MinimapView rotated = northUp;
    rotated.cosBearing = 0.0f;
    rotated.sinBearing = 1.0f;
    const auto farRotated = placeMinimapPoi(swDelta, cx, cy, rotated, 1.0f);
    REQUIRE_FALSE(farRotated.onMap);
    CHECK(glm::length(farRotated.direction) == Catch::Approx(1.0f).margin(1e-5f));
    CHECK(farRotated.direction.x == Catch::Approx(-farNorthUp.direction.y).margin(1e-5f));
    CHECK(farRotated.direction.y == Catch::Approx(farNorthUp.direction.x).margin(1e-5f));

    // Walking inside the minimap zoom radius transitions from rim arrow to in-map icon.
    const glm::vec2 closeDelta(-30.0f, -20.0f);  // ~36 yd < 100 yd viewRadius
    const auto inside = placeMinimapPoi(closeDelta, cx, cy, northUp, 1.0f);
    REQUIRE(inside.onMap);
    const glm::vec2 expectedOff = renderDeltaToMinimapOffset(closeDelta.x, closeDelta.y, northUp);
    CHECK(inside.pos.x == Catch::Approx(cx + expectedOff.x).margin(1e-4f));
    CHECK(inside.pos.y == Catch::Approx(cy + expectedOff.y).margin(1e-4f));
}
