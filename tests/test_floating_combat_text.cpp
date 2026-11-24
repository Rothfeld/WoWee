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

TEST_CASE("power gains, power drains, procs, honor, and self-landing heals/damage are not drawn as world text", "[fct]") {
    using wowee::game::CombatTextEntry;
    constexpr uint64_t kPlayer = 0x1001;
    constexpr uint64_t kPet    = 0x1002;
    constexpr uint64_t kAlly   = 0x2001;
    constexpr uint64_t kMob    = 0x3001;

    CHECK_FALSE(showsInWorld(CombatTextEntry::ENERGIZE));
    CHECK_FALSE(showsInWorld(CombatTextEntry::POWER_DRAIN));
    CHECK_FALSE(showsInWorld(CombatTextEntry::PROC_TRIGGER));
    CHECK_FALSE(showsInWorld(CombatTextEntry::HONOR_GAIN));
    CHECK(showsInWorld(CombatTextEntry::MELEE_DAMAGE));
    CHECK(showsInWorld(CombatTextEntry::SPELL_DAMAGE));
    CHECK(showsInWorld(CombatTextEntry::HEAL));
    CHECK(showsInWorld(CombatTextEntry::XP_GAIN));

    // Self-heal, self-crit-heal, self-HoT, and self-damage must NEVER show in 3D world space
    // (they belong exclusively to 2D Blizzard_CombatText and portrait CombatFeedback).
    for (auto selfType : {CombatTextEntry::HEAL, CombatTextEntry::CRIT_HEAL,
                          CombatTextEntry::PERIODIC_HEAL, CombatTextEntry::SPELL_DAMAGE}) {
        CombatTextEntry selfEntry{};
        selfEntry.type = selfType;
        selfEntry.amount = 100000;
        selfEntry.isPlayerSource = true;
        selfEntry.srcGuid = kPlayer;
        selfEntry.dstGuid = kPlayer;
        CHECK_FALSE(showsForUnit(selfEntry, kPlayer, kPet));
    }

    // Outgoing direct heal, crit heal, and HoT on another unit DO show in 3D world space
    // in TBC heal green (0.1, 1.0, 0.1) without exclamation marks.
    for (auto healType : {CombatTextEntry::HEAL, CombatTextEntry::CRIT_HEAL, CombatTextEntry::PERIODIC_HEAL}) {
        CombatTextEntry allyHeal{};
        allyHeal.type = healType;
        allyHeal.amount = 2140;
        allyHeal.spellId = 2060;
        allyHeal.isPlayerSource = true;
        allyHeal.srcGuid = kPlayer;
        allyHeal.dstGuid = kAlly;
        CHECK(showsForUnit(allyHeal, kPlayer, kPet));
        const auto p = presentationFor(allyHeal);
        CHECK(p.kind == Presentation::Number);
        CHECK(p.heal);
        CHECK(p.number == 2140);
        CHECK(p.critical == (healType == CombatTextEntry::CRIT_HEAL));
        const auto col = colorFor(p, false);
        CHECK(col.r == Catch::Approx(0.1f));
        CHECK(col.g == Catch::Approx(1.0f));
        CHECK(col.b == Catch::Approx(0.1f));
    }

    // XP_GAIN on the player still shows in world space, and pet hits on mobs show in world space.
    CombatTextEntry xp{};
    xp.type = CombatTextEntry::XP_GAIN;
    xp.amount = 450;
    xp.dstGuid = kPlayer;
    CHECK(showsForUnit(xp, kPlayer, kPet));

    CombatTextEntry petHit{};
    petHit.type = CombatTextEntry::MELEE_DAMAGE;
    petHit.amount = 88;
    petHit.srcGuid = kPet;
    petHit.dstGuid = kMob;
    CHECK(showsForUnit(petHit, kPlayer, kPet));
}

TEST_CASE("pet melee damage is orange (255,132,0) and pet spells are yellow (255,222,0)", "[fct]") {
    using wowee::game::CombatTextEntry;
    CombatTextEntry melee{};
    melee.type = CombatTextEntry::MELEE_DAMAGE;
    melee.amount = 104;
    const auto playerMeleeCol = colorFor(presentationFor(melee), false);
    CHECK(playerMeleeCol.r == Catch::Approx(1.0f));
    CHECK(playerMeleeCol.g == Catch::Approx(1.0f));
    CHECK(playerMeleeCol.b == Catch::Approx(1.0f));

    const auto petMeleeCol = colorFor(presentationFor(melee), true);
    CHECK(petMeleeCol.r == Catch::Approx(1.0f));
    CHECK(petMeleeCol.g == Catch::Approx(132.0f / 255.0f));
    CHECK(petMeleeCol.b == Catch::Approx(0.0f));

    CombatTextEntry petCrit = melee;
    petCrit.type = CombatTextEntry::CRIT_DAMAGE;
    petCrit.amount = 236;
    const auto petCritCol = colorFor(presentationFor(petCrit), true);
    CHECK(petCritCol.r == Catch::Approx(1.0f));
    CHECK(petCritCol.g == Catch::Approx(132.0f / 255.0f));
    CHECK(petCritCol.b == Catch::Approx(0.0f));

    CombatTextEntry petSpell{};
    petSpell.type = CombatTextEntry::SPELL_DAMAGE;
    petSpell.spellId = 3110; // Imp Firebolt
    petSpell.amount = 8;
    const auto petSpellCol = colorFor(presentationFor(petSpell), true);
    CHECK(petSpellCol.r == Catch::Approx(1.0f));
    CHECK(petSpellCol.g == Catch::Approx(222.0f / 255.0f));
    CHECK(petSpellCol.b == Catch::Approx(0.0f));
}

TEST_CASE("normal ranged attacks (Auto Shot, Shoot, Throw) use white combat text while special shots stay yellow",
          "[fct][ranged]") {
    using wowee::game::CombatTextEntry;
    using wowee::game::isNormalRangedAttackSpell;
    using wowee::game::kSpellAttrEx3NormalRangedAttack;

    // Spell.dbc AttributesEx3 bit 15 (SPELL_ATTR_EX3_NORMAL_RANGED_ATTACK = 0x8000):
    // Auto Shot (75), Throw (2764), Shoot (3018), Wand Shoot (5019).
    CHECK(isNormalRangedAttackSpell(kSpellAttrEx3NormalRangedAttack));
    CHECK(isNormalRangedAttackSpell(0x00408000u));  // Wand Shoot 5019
    CHECK_FALSE(isNormalRangedAttackSpell(0x00000000u));  // Arcane Shot 3044, Aimed Shot 19434, Steady Shot 34120
    CHECK_FALSE(isNormalRangedAttackSpell(0x00000400u));  // Sinister Strike 1752

    // Normal Auto Shot hit -> white (255, 255, 255), not spell yellow.
    CombatTextEntry autoShot{};
    autoShot.type = CombatTextEntry::SPELL_DAMAGE;
    autoShot.spellId = 75;
    autoShot.amount = 187;
    autoShot.normalRangedAttack = true;
    const auto autoShotPres = presentationFor(autoShot);
    CHECK_FALSE(autoShotPres.spell);
    const auto autoShotCol = colorFor(autoShotPres, false);
    CHECK(autoShotCol.r == Catch::Approx(1.0f));
    CHECK(autoShotCol.g == Catch::Approx(1.0f));
    CHECK(autoShotCol.b == Catch::Approx(1.0f));

    // Critical Auto Shot hit -> white (255, 255, 255) and critical.
    CombatTextEntry autoShotCrit = autoShot;
    autoShotCrit.type = CombatTextEntry::CRIT_DAMAGE;
    const auto critPres = presentationFor(autoShotCrit);
    CHECK(critPres.critical);
    CHECK_FALSE(critPres.spell);
    const auto critCol = colorFor(critPres, false);
    CHECK(critCol.r == Catch::Approx(1.0f));
    CHECK(critCol.g == Catch::Approx(1.0f));
    CHECK(critCol.b == Catch::Approx(1.0f));

    // Special bow ability (Arcane Shot 3044) -> yellow (255, 222, 0).
    CombatTextEntry arcaneShot{};
    arcaneShot.type = CombatTextEntry::SPELL_DAMAGE;
    arcaneShot.spellId = 3044;
    arcaneShot.amount = 240;
    arcaneShot.normalRangedAttack = false;
    const auto arcanePres = presentationFor(arcaneShot);
    CHECK(arcanePres.spell);
    const auto arcaneCol = colorFor(arcanePres, false);
    CHECK(arcaneCol.r == Catch::Approx(1.0f));
    CHECK(arcaneCol.g == Catch::Approx(222.0f / 255.0f));
    CHECK(arcaneCol.b == Catch::Approx(0.0f));
}

TEST_CASE("rapid .damage 6-digit stream uses below and left flanks instead of rightward train", "[fct][layout]") {
    // Simulates spamming `.damage 300000` (~104 px wide at 720p) every ~180 ms
    // (~12 px natural rise per arrival) so 6 numbers are simultaneously active:
    // newestFirst = [220, 208, 196, 184, 172, 158].
    std::vector<Box> boxes{
        text(220.0f, 104.0f),
        text(208.0f, 104.0f),
        text(196.0f, 104.0f),
        text(184.0f, 104.0f),
        text(172.0f, 104.0f),
        text(158.0f, 104.0f),
    };
    const Viewport vp{1280.0f, 720.0f, 720.0f / 768.0f};
    layout(boxes, vp);

    // Every pair must be non-overlapping and stay within [left1, center, right1].
    for (size_t i = 0; i < boxes.size(); ++i) {
        CHECK(fitsHorizontally(boxes[i], vp));
        CHECK(std::abs(boxes[i].x - 640.0f) <= 130.0f);
        for (size_t j = i + 1; j < boxes.size(); ++j) {
            CHECK_FALSE(overlaps(boxes[i], boxes[j]));
        }
    }
    // Center column has newest (220), lifted (164), dropped below newest (276), and upper lifted (108);
    // flanks occupy both right (>640) and left (<640).
    CHECK(boxes[0].x == Catch::Approx(640.0f));
    CHECK(boxes[0].top == Catch::Approx(220.0f));
    CHECK(boxes[1].x == Catch::Approx(640.0f));
    CHECK(boxes[1].top == Catch::Approx(164.0f));
    CHECK(boxes[2].x > 640.0f + 100.0f);
    CHECK(boxes[3].x == Catch::Approx(640.0f));
    CHECK(boxes[3].top == Catch::Approx(276.0f));
    CHECK(boxes[4].x < 640.0f - 100.0f);
    CHECK(boxes[5].x == Catch::Approx(640.0f));
    CHECK(boxes[5].top == Catch::Approx(108.0f));

    // Also verify 7 simultaneous same-frame hits expand symmetrically left and right
    // rather than forming a lopsided rightward horizontal train.
    std::vector<Box> burst(7, text(300.0f, 100.0f));
    layout(burst, vp);
    int leftCount = 0;
    int rightCount = 0;
    for (size_t i = 0; i < burst.size(); ++i) {
        CHECK(fitsHorizontally(burst[i], vp));
        if (burst[i].x < 639.0f) ++leftCount;
        if (burst[i].x > 641.0f) ++rightCount;
        for (size_t j = i + 1; j < burst.size(); ++j) {
            CHECK_FALSE(overlaps(burst[i], burst[j]));
        }
    }
    CHECK(leftCount == 2);
    CHECK(rightCount == 2);
}

TEST_CASE("top-of-screen and bottom-of-screen clamping participate in layout without overlap", "[fct][layout]") {
    const Viewport vp{1280.0f, 720.0f, 720.0f / 768.0f};
    const float minTop = kTopBaseline * vp.uiScale - 28.0f;
    const float maxTop = vp.height - kBottomBaselineFromBottom * vp.uiScale - 28.0f;

    // Top-of-screen stream (e.g. Al'ar / mirage_B_top_spam where naturalTop ~ 95..65):
    // lifting above 95 clamps to minTop (~51), which still overlaps 95, so the 4 numbers
    // form (center, right, below, left) with zero post-clamp overlap.
    std::vector<Box> topBoxes{
        text(95.0f, 99.0f),
        text(85.0f, 99.0f),
        text(75.0f, 99.0f),
        text(65.0f, 99.0f),
    };
    layout(topBoxes, vp);
    for (size_t i = 0; i < topBoxes.size(); ++i) {
        CHECK(topBoxes[i].top >= minTop - 0.01f);
        for (size_t j = i + 1; j < topBoxes.size(); ++j) {
            CHECK_FALSE(overlaps(topBoxes[i], topBoxes[j]));
        }
    }
    CHECK(topBoxes[0].x == Catch::Approx(640.0f));
    CHECK(topBoxes[1].x > 640.0f + 90.0f);
    CHECK(topBoxes[2].x == Catch::Approx(640.0f));
    CHECK(topBoxes[2].top == Catch::Approx(95.0f + 56.0f));
    CHECK(topBoxes[3].x < 640.0f - 90.0f);

    // Bottom-of-screen stream (hostile_A_spam_20yd where naturalTop >= maxTop):
    // dropping below maxTop clamps to maxTop and is rejected, so the 4 numbers
    // form (center bottom, center lifted, right, left) above MainMenuExpBar.
    std::vector<Box> bottomBoxes{
        text(660.0f, 99.0f),
        text(650.0f, 99.0f),
        text(640.0f, 99.0f),
        text(635.0f, 99.0f),
    };
    layout(bottomBoxes, vp);
    for (size_t i = 0; i < bottomBoxes.size(); ++i) {
        CHECK(bottomBoxes[i].top <= maxTop + 0.01f);
        for (size_t j = i + 1; j < bottomBoxes.size(); ++j) {
            CHECK_FALSE(overlaps(bottomBoxes[i], bottomBoxes[j]));
        }
    }
    CHECK(bottomBoxes[0].top == Catch::Approx(maxTop));
    CHECK(bottomBoxes[1].x == Catch::Approx(640.0f));
    CHECK(bottomBoxes[1].top == Catch::Approx(maxTop - 56.0f));
    CHECK(bottomBoxes[2].x > 640.0f + 90.0f);
    CHECK(bottomBoxes[3].x < 640.0f - 90.0f);
}
