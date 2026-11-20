#include <catch_amalgamated.hpp>
#include "game/spell_cooldown_state.hpp"

using wowee::game::SpellCooldownState;

TEST_CASE("SpellCooldownState: cross-rank category cooldown propagation", "[spell][cooldown]") {
    // TBC 2.4.3 Spell.dbc: Shadowburn R1 (17877) .. R8 (30546) all share Category 651,
    // RecoveryTime=0, CategoryRecoveryTime=15000.
    constexpr uint32_t kShadowburnR1 = 17877;
    constexpr uint32_t kShadowburnR8 = 30546;
    constexpr uint32_t kShadowburnCat = 651;
    constexpr uint32_t kShadowBoltR1 = 686;

    SpellCooldownState state;
    REQUIRE(state.seedCategory(kShadowburnCat, 15.0f));

    // Casting Rank 1 puts Rank 1 AND Rank 8 on 15s cooldown with matching total.
    const auto [r1Rem, r1Tot] = state.query(kShadowburnR1, kShadowburnCat);
    const auto [r8Rem, r8Tot] = state.query(kShadowburnR8, kShadowburnCat);
    CHECK(r1Rem == Catch::Approx(15.0f));
    CHECK(r1Tot == Catch::Approx(15.0f));
    CHECK(r8Rem == Catch::Approx(15.0f));
    CHECK(r8Tot == Catch::Approx(15.0f));

    // Unrelated spell (cat=0) is unaffected.
    const auto [sbRem, sbTot] = state.query(kShadowBoltR1, 0);
    CHECK(sbRem == 0.0f);
    CHECK(sbTot == 0.0f);

    // Ticking advances remaining while preserving total for sweep start-time wind-back.
    CHECK_FALSE(state.tick(5.0f));
    const auto [r8RemAfter, r8TotAfter] = state.query(kShadowburnR8, kShadowburnCat);
    CHECK(r8RemAfter == Catch::Approx(10.0f));
    CHECK(r8TotAfter == Catch::Approx(15.0f));

    // Expiring reports true and clears both ranks.
    CHECK(state.tick(10.5f));
    CHECK(state.query(kShadowburnR1, kShadowburnCat).first == 0.0f);
    CHECK(state.query(kShadowburnR8, kShadowburnCat).first == 0.0f);
}

TEST_CASE("SpellCooldownState: cross-spell shared category (Shaman Shocks)", "[spell][cooldown]") {
    // Earth Shock R1 (8042), Flame Shock R1 (8050), Frost Shock R1 (8056) all share Category 19,
    // CategoryRecoveryTime=6000.
    constexpr uint32_t kEarthShockR1 = 8042;
    constexpr uint32_t kFlameShockR1 = 8050;
    constexpr uint32_t kFrostShockR1 = 8056;
    constexpr uint32_t kShockCat = 19;

    SpellCooldownState state;
    REQUIRE(state.seedCategory(kShockCat, 6.0f));

    CHECK(state.query(kEarthShockR1, kShockCat).first == Catch::Approx(6.0f));
    CHECK(state.query(kFlameShockR1, kShockCat).first == Catch::Approx(6.0f));
    CHECK(state.query(kFrostShockR1, kShockCat).first == Catch::Approx(6.0f));
}

TEST_CASE("SpellCooldownState: combined spell + category cooldown picks longer remaining", "[spell][cooldown]") {
    // Earth Elemental Totem (2062) & Fire Elemental Totem (2894):
    // cat=23, RecoveryTime=1200000 (20m), CategoryRecoveryTime=120000 (2m).
    constexpr uint32_t kEarthElem = 2062;
    constexpr uint32_t kFireElem = 2894;
    constexpr uint32_t kElemCat = 23;

    SpellCooldownState state;
    REQUIRE(state.seedSpell(kEarthElem, 1200.0f));
    REQUIRE(state.seedCategory(kElemCat, 120.0f));

    // Earth Elemental reports its own 1200s cooldown; Fire Elemental reports the 120s category cooldown.
    const auto [earthRem, earthTot] = state.query(kEarthElem, kElemCat);
    const auto [fireRem, fireTot] = state.query(kFireElem, kElemCat);
    CHECK(earthRem == Catch::Approx(1200.0f));
    CHECK(earthTot == Catch::Approx(1200.0f));
    CHECK(fireRem == Catch::Approx(120.0f));
    CHECK(fireTot == Catch::Approx(120.0f));

    // After 130s, Fire Elemental is ready while Earth Elemental still has 1070s left.
    CHECK(state.tick(130.0f));
    CHECK(state.query(kFireElem, kElemCat).first == 0.0f);
    CHECK(state.query(kEarthElem, kElemCat).first == Catch::Approx(1070.0f));
    CHECK(state.query(kEarthElem, kElemCat).second == Catch::Approx(1200.0f));
}

TEST_CASE("SpellCooldownState: clear, modify, and seed guard", "[spell][cooldown]") {
    constexpr uint32_t kShadowfuryR1 = 30283;
    constexpr uint32_t kShadowfuryR3 = 30414;
    constexpr uint32_t kShadowfuryCat = 250;

    SpellCooldownState state;
    REQUIRE(state.seedCategory(kShadowfuryCat, 20.0f));
    // Re-seeding while > 0.5s remains does not restart the cooldown.
    CHECK_FALSE(state.tick(4.0f));
    CHECK_FALSE(state.seedCategory(kShadowfuryCat, 20.0f));
    CHECK(state.query(kShadowfuryR3, kShadowfuryCat).first == Catch::Approx(16.0f));

    // Modify reduces category cooldown for all ranks.
    REQUIRE(state.modify(kShadowfuryR1, kShadowfuryCat, -6.0f));
    CHECK(state.query(kShadowfuryR3, kShadowfuryCat).first == Catch::Approx(10.0f));

    // Clear removes category cooldown for all ranks.
    state.clear(kShadowfuryR1, kShadowfuryCat);
    CHECK(state.query(kShadowfuryR1, kShadowfuryCat).first == 0.0f);
    CHECK(state.query(kShadowfuryR3, kShadowfuryCat).first == 0.0f);
}

TEST_CASE("SpellCooldownState: player and pet states stay isolated across identical category IDs", "[spell][cooldown]") {
    constexpr uint32_t kFireBlastR1 = 2136;
    constexpr uint32_t kPetSpell = 27277;
    constexpr uint32_t kSharedCat = 19;

    SpellCooldownState playerState;
    SpellCooldownState petState;

    REQUIRE(petState.seedCategory(kSharedCat, 8.0f));
    CHECK(petState.query(kPetSpell, kSharedCat).first == Catch::Approx(8.0f));
    CHECK(playerState.query(kFireBlastR1, kSharedCat).first == 0.0f);
}
