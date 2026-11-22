#include <catch_amalgamated.hpp>
#include "game/shapeshift_auto_unshift.hpp"

using namespace wowee::game;

namespace {

// TBC 2.4.3 SpellShapeshiftForm.dbc Flags (verified from shipped spellshapeshiftform.dbc)
constexpr uint32_t kFlagsCat        = 0x0078u;
constexpr uint32_t kFlagsTreeOfLife = 0x0051u;
constexpr uint32_t kFlagsTravel     = 0x0058u;
constexpr uint32_t kFlagsBear       = 0x0058u;
constexpr uint32_t kFlagsGhostWolf  = 0x0058u;
constexpr uint32_t kFlagsWarStance  = 0x0007u;
constexpr uint32_t kFlagsFlight     = 0x0008u;
constexpr uint32_t kFlagsShadowform = 0x0009u;
constexpr uint32_t kFlagsStealth    = 0x0001u;
constexpr uint32_t kFlagsMoonkin    = 0x0041u;
constexpr uint32_t kFlagsSpiritRed  = 0x0100u;

constexpr uint8_t kFormCat        = 1;
constexpr uint8_t kFormTreeOfLife = 2;
constexpr uint8_t kFormTravel     = 3;
constexpr uint8_t kFormBear       = 5;
constexpr uint8_t kFormGhostWolf  = 16;
constexpr uint8_t kFormBattle     = 17;
constexpr uint8_t kFormShadow     = 28;
constexpr uint8_t kFormFlight     = 29;
constexpr uint8_t kFormStealth    = 30;
constexpr uint8_t kFormMoonkin    = 31;
constexpr uint8_t kFormSpiritRed  = 32;

} // namespace

TEST_CASE("Shadowform auto-unshifts Holy spells when autoUnshift=1 and blocks when autoUnshift=0", "[shapeshift][autounshift]") {
    // Flash Heal / Greater Heal / Renew / Gift of the Naaru:
    // attr=0x00010000, ex2=0x00080000, stances=0x80000000 (Spirit of Redemption), stancesNot=0x08000000 (Shadowform)
    ShapeshiftCastCheck flashHeal{
        .form = kFormShadow, .formFlags = kFlagsShadowform, .formAuraCancellable = true,
        .autoUnshiftEnabled = true,
        .spellAttr = 0x00010000u, .spellAttrEx2 = 0x00080000u,
        .spellStances = 0x80000000u, .spellStancesNot = 0x08000000u
    };
    REQUIRE(evaluateShapeshiftCast(flashHeal) == ShapeshiftCastResult::AutoUnshiftAndCast);

    flashHeal.autoUnshiftEnabled = false;
    REQUIRE(evaluateShapeshiftCast(flashHeal) == ShapeshiftCastResult::RefusedNotShapeshift);

    // Smite / Desperate Prayer: attr=0x00010000, ex2=0, stances=0, stancesNot=0x08000000
    ShapeshiftCastCheck smite{
        .form = kFormShadow, .formFlags = kFlagsShadowform, .formAuraCancellable = true,
        .autoUnshiftEnabled = true,
        .spellAttr = 0x00010000u, .spellAttrEx2 = 0u,
        .spellStances = 0u, .spellStancesNot = 0x08000000u
    };
    REQUIRE(evaluateShapeshiftCast(smite) == ShapeshiftCastResult::AutoUnshiftAndCast);

    smite.autoUnshiftEnabled = false;
    REQUIRE(evaluateShapeshiftCast(smite) == ShapeshiftCastResult::RefusedNotShapeshift);

    // Power Word: Shield (Discipline) & Mind Blast (Shadow): stances=0x08000000, stancesNot=0 -> AllowedInForm
    ShapeshiftCastCheck pws{
        .form = kFormShadow, .formFlags = kFlagsShadowform, .formAuraCancellable = true,
        .autoUnshiftEnabled = true,
        .spellAttr = 0x00050800u, .spellAttrEx2 = 0x00280000u,
        .spellStances = 0x08000000u, .spellStancesNot = 0u
    };
    REQUIRE(evaluateShapeshiftCast(pws) == ShapeshiftCastResult::AllowedInForm);

    // Hearthstone: NOT_SHAPESHIFT, stances=0, stancesNot=0 -> Shadowform has ActsAsNoForm -> AllowedInForm
    ShapeshiftCastCheck hearth{
        .form = kFormShadow, .formFlags = kFlagsShadowform, .formAuraCancellable = true,
        .autoUnshiftEnabled = true,
        .spellAttr = 0x08010000u, .spellAttrEx2 = 0u,
        .spellStances = 0u, .spellStancesNot = 0u
    };
    REQUIRE(evaluateShapeshiftCast(hearth) == ShapeshiftCastResult::AllowedInForm);
}

TEST_CASE("Druid forms and Ghost Wolf auto-unshift incompatible spells including AllowWhileNotShifted spells", "[shapeshift][autounshift]") {
    for (auto [form, flags] : {std::pair{kFormCat, kFlagsCat},
                               std::pair{kFormBear, kFlagsBear},
                               std::pair{kFormTravel, kFlagsTravel},
                               std::pair{kFormFlight, kFlagsFlight},
                               std::pair{kFormGhostWolf, kFlagsGhostWolf}}) {
        // Healing Touch: attr=0x10000, stances=0, stancesNot=0x40000002
        ShapeshiftCastCheck ht{
            .form = form, .formFlags = flags, .formAuraCancellable = true,
            .autoUnshiftEnabled = true,
            .spellAttr = 0x00010000u, .spellAttrEx2 = 0u,
            .spellStances = 0u, .spellStancesNot = 0x40000002u
        };
        REQUIRE(evaluateShapeshiftCast(ht) == ShapeshiftCastResult::AutoUnshiftAndCast);
        ht.autoUnshiftEnabled = false;
        REQUIRE(evaluateShapeshiftCast(ht) == ShapeshiftCastResult::RefusedNotShapeshift);

        // Wrath: attr=0x10000, ex2=0x80000 (AllowWhileNotShifted), stances=0x40000000 (Moonkin), stancesNot=0x2
        ShapeshiftCastCheck wrath{
            .form = form, .formFlags = flags, .formAuraCancellable = true,
            .autoUnshiftEnabled = true,
            .spellAttr = 0x00010000u, .spellAttrEx2 = 0x00080000u,
            .spellStances = 0x40000000u, .spellStancesNot = 0x00000002u
        };
        REQUIRE(evaluateShapeshiftCast(wrath) == ShapeshiftCastResult::AutoUnshiftAndCast);
        wrath.autoUnshiftEnabled = false;
        REQUIRE(evaluateShapeshiftCast(wrath) == ShapeshiftCastResult::RefusedNotShapeshift);
    }

    // Moonkin Form: Wrath allowed in form, Rejuvenation/Healing Touch auto-unshift
    ShapeshiftCastCheck moonkinWrath{
        .form = kFormMoonkin, .formFlags = kFlagsMoonkin, .formAuraCancellable = true,
        .autoUnshiftEnabled = true,
        .spellAttr = 0x00010000u, .spellAttrEx2 = 0x00080000u,
        .spellStances = 0x40000000u, .spellStancesNot = 0x00000002u
    };
    REQUIRE(evaluateShapeshiftCast(moonkinWrath) == ShapeshiftCastResult::AllowedInForm);

    ShapeshiftCastCheck moonkinRejuv{
        .form = kFormMoonkin, .formFlags = kFlagsMoonkin, .formAuraCancellable = true,
        .autoUnshiftEnabled = true,
        .spellAttr = 0x00010000u, .spellAttrEx2 = 0x00080000u,
        .spellStances = 0x00000002u, .spellStancesNot = 0x40000000u
    };
    REQUIRE(evaluateShapeshiftCast(moonkinRejuv) == ShapeshiftCastResult::AutoUnshiftAndCast);

    // Tree of Life: Rejuvenation allowed in form, Wrath/Healing Touch auto-unshift
    ShapeshiftCastCheck treeRejuv = moonkinRejuv;
    treeRejuv.form = kFormTreeOfLife;
    treeRejuv.formFlags = kFlagsTreeOfLife;
    REQUIRE(evaluateShapeshiftCast(treeRejuv) == ShapeshiftCastResult::AllowedInForm);

    ShapeshiftCastCheck treeWrath = moonkinWrath;
    treeWrath.form = kFormTreeOfLife;
    treeWrath.formFlags = kFlagsTreeOfLife;
    REQUIRE(evaluateShapeshiftCast(treeWrath) == ShapeshiftCastResult::AutoUnshiftAndCast);
}

TEST_CASE("Warrior stances, Stealth, Spirit of Redemption, and non-cancellable forms never auto-unshift", "[shapeshift][autounshift]") {
    // Battle Stance + Intercept (requires Berserker Stance, ex2=0) -> RefusedOnlyShapeshift
    ShapeshiftCastCheck intercept{
        .form = kFormBattle, .formFlags = kFlagsWarStance, .formAuraCancellable = false,
        .autoUnshiftEnabled = true,
        .spellAttr = 0x20050010u, .spellAttrEx2 = 0u,
        .spellStances = 0x00040000u, .spellStancesNot = 0u
    };
    REQUIRE(evaluateShapeshiftCast(intercept) == ShapeshiftCastResult::RefusedOnlyShapeshift);

    // Battle Stance + hypothetical StancesNot spell -> NOT_TOGGLEABLE prevents auto-unshift
    ShapeshiftCastCheck stanceBlocked{
        .form = kFormBattle, .formFlags = kFlagsWarStance, .formAuraCancellable = true,
        .autoUnshiftEnabled = true,
        .spellAttr = 0x00010000u, .spellAttrEx2 = 0u,
        .spellStances = 0u, .spellStancesNot = (1u << (kFormBattle - 1))
    };
    REQUIRE(evaluateShapeshiftCast(stanceBlocked) == ShapeshiftCastResult::RefusedNotShapeshift);

    // Stealth + Vanish (NOT_SHAPESHIFT, stances=0) -> AllowedInForm (Stealth has ActsAsNoForm)
    ShapeshiftCastCheck vanish{
        .form = kFormStealth, .formFlags = kFlagsStealth, .formAuraCancellable = true,
        .autoUnshiftEnabled = true,
        .spellAttr = 0x08150010u, .spellAttrEx2 = 0x00000002u,
        .spellStances = 0u, .spellStancesNot = 0u
    };
    REQUIRE(evaluateShapeshiftCast(vanish) == ShapeshiftCastResult::AllowedInForm);

    // Spirit of Redemption (flags=0x100 NoAutoUnshift) + Smite -> RefusedNotShapeshift
    ShapeshiftCastCheck spiritSmite{
        .form = kFormSpiritRed, .formFlags = kFlagsSpiritRed, .formAuraCancellable = true,
        .autoUnshiftEnabled = true,
        .spellAttr = 0x00010000u, .spellAttrEx2 = 0u,
        .spellStances = 0u, .spellStancesNot = 0u
    };
    REQUIRE(evaluateShapeshiftCast(spiritSmite) == ShapeshiftCastResult::RefusedNotShapeshift);

    // Harmful / non-cancellable polymorph debuff -> formAuraCancellable=false -> RefusedNotShapeshift
    ShapeshiftCastCheck polymorphed{
        .form = kFormCat, .formFlags = kFlagsCat, .formAuraCancellable = false,
        .autoUnshiftEnabled = true,
        .spellAttr = 0x00010000u, .spellAttrEx2 = 0u,
        .spellStances = 0u, .spellStancesNot = 0u
    };
    REQUIRE(evaluateShapeshiftCast(polymorphed) == ShapeshiftCastResult::RefusedNotShapeshift);
}
