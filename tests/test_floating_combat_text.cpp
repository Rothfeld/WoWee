// Build 8606 floating damage text over one unit, against the cases measured
// in _experiments/20261117T211838Z.fct_layout (28 px text at 720 lines).
#include <catch_amalgamated.hpp>
#include "ui/floating_combat_text.hpp"

#include <string>

using namespace wowee::ui::fct;

namespace {
Box text(float top, float width, float fontPx = 28.0f) {
    Box b;
    b.naturalX = 640.0f;
    b.naturalTop = top;
    b.width = width;
    b.fontPx = fontPx;
    return b;
}
}  // namespace

TEST_CASE("two simultaneous numbers stack one row apart", "[fct]") {
    std::vector<Box> boxes{text(300, 31), text(300, 6)};
    layout(boxes);
    CHECK(boxes[0].top == Catch::Approx(300));
    CHECK(boxes[1].top == Catch::Approx(244));
    CHECK(boxes[1].x == Catch::Approx(640));
}

TEST_CASE("of three simultaneous numbers the oldest goes beside the newest", "[fct]") {
    std::vector<Box> boxes{text(300, 50), text(300, 31), text(300, 6)};
    layout(boxes);
    CHECK(boxes[1].top == Catch::Approx(244));
    CHECK(boxes[2].top == Catch::Approx(300));
    CHECK(boxes[2].x > 640 + 25);
}

TEST_CASE("a 0.3 s stream forms one column", "[fct]") {
    // Natural rise between arrivals is 37 px, less than the 56 px row.
    std::vector<Box> boxes{text(300, 10), text(263, 10), text(226, 10)};
    layout(boxes);
    CHECK(boxes[1].top == Catch::Approx(244));
    CHECK(boxes[2].top == Catch::Approx(188));
    for (const auto& b : boxes) CHECK(b.x == Catch::Approx(640));
}

TEST_CASE("separated numbers keep their own places", "[fct]") {
    std::vector<Box> boxes{text(300, 10), text(200, 10)};
    layout(boxes);
    CHECK(boxes[1].top == Catch::Approx(200));
}

TEST_CASE("fade and critical pulse follow the measured curve", "[fct]") {
    CHECK(alpha(0.075f, false) == Catch::Approx(0.05f));
    CHECK(alpha(0.5f, false) == Catch::Approx(1.0f));
    CHECK(alpha(1.13f, false) == Catch::Approx(0.5f));
    CHECK(alpha(0.9f, true) == Catch::Approx(1.0f));
    CHECK(alpha(1.25f, true) == Catch::Approx(0.5f));
    CHECK(critPulse(0.15f) == Catch::Approx(2.0f));
    CHECK(critPulse(0.3f) == Catch::Approx(1.0f));
}

TEST_CASE("of four simultaneous numbers the oldest drops a row below", "[fct]") {
    // st-original frames 520-545: 4 stays, 333 rises, 22 beside 4, 1 below.
    std::vector<Box> boxes{text(300, 14), text(300, 49), text(300, 31), text(300, 6)};
    layout(boxes);
    CHECK(boxes[0].top == Catch::Approx(300));
    CHECK(boxes[1].top == Catch::Approx(244));
    CHECK(boxes[2].top == Catch::Approx(300));
    CHECK(boxes[2].x > 640 + 14);
    CHECK(boxes[3].top == Catch::Approx(356));
    CHECK(boxes[3].x == Catch::Approx(640));
}

TEST_CASE("a hit shows its damage, a stopped hit names what stopped it", "[fct]") {
    // _experiments/20261119T122954Z.combat_text: abs_original, imm_original, melee3.
    using wowee::game::CombatTextEntry;
    CombatTextEntry hit{};
    hit.type = CombatTextEntry::MELEE_DAMAGE;
    hit.amount = 4;
    auto shown = presentationFor(hit);
    CHECK(shown.kind == Presentation::Number);
    CHECK(shown.number == 4);
    CHECK_FALSE(shown.spell);

    CombatTextEntry absorbedPart = hit;
    absorbedPart.type = CombatTextEntry::ABSORB;
    absorbedPart.amount = 36;
    absorbedPart.reduction = true;
    CHECK(presentationFor(absorbedPart).kind == Presentation::NotWorldText);

    CombatTextEntry absorbed = absorbedPart;
    absorbed.reduction = false;
    shown = presentationFor(absorbed);
    CHECK(shown.kind == Presentation::Word);
    CHECK(std::string(shown.word) == "ABSORB");

    CombatTextEntry nothing = hit;
    nothing.amount = 0;
    CHECK(std::string(presentationFor(nothing).word) == "MISS");

    CombatTextEntry immune{};
    immune.type = CombatTextEntry::IMMUNE;
    immune.spellId = 686;
    shown = presentationFor(immune);
    CHECK(std::string(shown.word) == "IMMUNE");
    CHECK(shown.spell);

    CombatTextEntry tick{};
    tick.type = CombatTextEntry::PERIODIC_DAMAGE;
    tick.amount = 150;
    tick.spellId = 172;
    shown = presentationFor(tick);
    CHECK(shown.kind == Presentation::Number);
    CHECK(shown.number == 150);
    CHECK(shown.spell);
    CHECK_FALSE(shown.critical);
}

TEST_CASE("outcome words stay opaque as long as a critical", "[fct]") {
    CHECK(alpha(0.9f, true) == Catch::Approx(1.0f));
    CHECK(alpha(0.9f, false) < 1.0f);
}
