// Which attack animation to try, and what to fall back to.
//
// Not every model carries every attack, so each weapon situation names an
// ordered chain and the first sequence the model has is the one played. The
// chains were written out twice - in the controller that plays them and in the
// probe that answers what a model can do - under a comment in the probe saying
// its copies "match resolveMeleeAnimId", which nothing enforced. They did
// match, which is the state a pair of tables is in right before one is edited.
//
// A divergence reports nothing: the probe says a model can attack one-handed,
// the controller lands somewhere else in a different chain, and a rogue swings
// a dagger like a mace.
#include <catch_amalgamated.hpp>

#include <algorithm>
#include <set>
#include <vector>

#include "rendering/animation/melee_anim_chains.hpp"
#include "rendering/animation/weapon_spell_anim.hpp"

using namespace wowee::rendering::anim;

namespace {

std::vector<uint32_t> chain(MeleeChain kind) {
    const auto span = meleeAnimChain(kind);
    return std::vector<uint32_t>(span.begin(), span.end());
}

constexpr MeleeChain kAll[] = {
    MeleeChain::OneHand,   MeleeChain::TwoHand,       MeleeChain::TwoHandLoose,
    MeleeChain::Dagger,    MeleeChain::Fist,          MeleeChain::Unarmed,
    MeleeChain::OffHand,   MeleeChain::OffHandPierce, MeleeChain::OffHandFist,
    MeleeChain::OffHandUnarmed, MeleeChain::Generic,
};

}  // namespace

TEST_CASE("each chain asks first for what the weapon actually is", "[anim]") {
    // The head of the chain is the whole point; everything after it is a
    // compromise. Getting the head wrong means a model that has the right
    // animation still never plays it.
    CHECK(chain(MeleeChain::OneHand).front() == ATTACK_1H);
    CHECK(chain(MeleeChain::TwoHand).front() == ATTACK_2H);
    CHECK(chain(MeleeChain::TwoHandLoose).front() == ATTACK_2H_LOOSE_PIERCE);
    CHECK(chain(MeleeChain::Dagger).front() == ATTACK_1H_PIERCE);
    CHECK(chain(MeleeChain::Fist).front() == ATTACK_UNARMED);   // 3.3.5 has no fist animations
    CHECK(chain(MeleeChain::Unarmed).front() == ATTACK_UNARMED);
    CHECK(chain(MeleeChain::OffHand).front() == ATTACK_OFF);
    CHECK(chain(MeleeChain::OffHandPierce).front() == ATTACK_OFF_PIERCE);
    CHECK(chain(MeleeChain::OffHandFist).front() == ATTACK_UNARMED_OFF);
    CHECK(chain(MeleeChain::OffHandUnarmed).front() == ATTACK_UNARMED_OFF);
}

TEST_CASE("a dagger never falls back to a swing", "[anim]") {
    // The one distinction a player would notice immediately. A dagger thrusts;
    // ATTACK_2H in this chain would have a rogue swinging it overhead.
    const auto dagger = chain(MeleeChain::Dagger);
    CHECK(std::find(dagger.begin(), dagger.end(), ATTACK_2H) == dagger.end());
    CHECK(std::find(dagger.begin(), dagger.end(), ATTACK_2H_LOOSE) == dagger.end());
}

TEST_CASE("a polearm tries the thrust before the swing", "[anim]") {
    // Order, not membership: both are in the chain, and the pierce has to come
    // first or every polearm swings like a greatsword on a model that has both.
    const auto loose = chain(MeleeChain::TwoHandLoose);
    const auto pierceAt = std::find(loose.begin(), loose.end(), ATTACK_2H_LOOSE_PIERCE);
    const auto swingAt = std::find(loose.begin(), loose.end(), ATTACK_2H_LOOSE);
    REQUIRE(pierceAt != loose.end());
    REQUIRE(swingAt != loose.end());
    CHECK(pierceAt < swingAt);
}

TEST_CASE("every off-hand chain prefers an off-hand animation", "[anim]") {
    // Dual-wielding alternates hands, and an off-hand turn that plays the
    // main-hand animation makes both weapons swing on the same side.
    const MeleeChain offHands[] = {MeleeChain::OffHand, MeleeChain::OffHandPierce,
                                   MeleeChain::OffHandFist, MeleeChain::OffHandUnarmed};
    for (MeleeChain kind : offHands) {
        const auto ids = chain(kind);
        REQUIRE_FALSE(ids.empty());
        const bool offHandFirst = ids.front() == ATTACK_OFF ||
                                  ids.front() == ATTACK_OFF_PIERCE ||
                                  ids.front() == ATTACK_UNARMED_OFF;
        CHECK(offHandFirst);
    }
}

TEST_CASE("no chain is empty and none repeats itself", "[anim]") {
    // A repeat is a wasted probe and a sign the chain was edited by hand in
    // one of the two places it used to live.
    for (MeleeChain kind : kAll) {
        const auto ids = chain(kind);
        INFO("chain " << static_cast<int>(kind));
        REQUIRE_FALSE(ids.empty());
        const std::set<uint32_t> unique(ids.begin(), ids.end());
        CHECK(unique.size() == ids.size());
    }
}

TEST_CASE("every chain ends somewhere a model with nothing can still go",
          "[anim]") {
    // The last entry is what plays when a model carries none of the rest. It
    // has to be one of the two every rig has - an unarmed swing, or a parry,
    // which at least moves the weapon arm.
    for (MeleeChain kind : kAll) {
        const auto ids = chain(kind);
        INFO("chain " << static_cast<int>(kind));
        const uint32_t last = ids.back();
        CHECK((last == ATTACK_UNARMED || last == ATTACK_1H ||
               last == PARRY_1H || last == PARRY_UNARMED));
    }
}

TEST_CASE("the generic chain has no parry in it", "[anim]") {
    // It is what a creature swings when nothing knows its weapon, and it ends
    // at an unarmed attack rather than falling through to a parry: a parry in
    // place of an attack reads as the model flinching.
    const auto ids = chain(MeleeChain::Generic);
    CHECK(std::find(ids.begin(), ids.end(), PARRY_1H) == ids.end());
    CHECK(std::find(ids.begin(), ids.end(), PARRY_UNARMED) == ids.end());
    CHECK(ids.back() == ATTACK_UNARMED);
}

namespace {

wowee::rendering::WeaponLoadout makeLoadout(uint32_t invType, bool is2HLoose = false,
                                            bool isFist = false, bool isDagger = false,
                                            bool hasOffHand = false, bool hasShield = false) {
    wowee::rendering::WeaponLoadout wl{};
    wl.inventoryType = invType;
    wl.is2HLoose = is2HLoose;
    wl.isFist = isFist;
    wl.isDagger = isDagger;
    wl.hasOffHand = hasOffHand;
    wl.hasShield = hasShield;
    return wl;
}

}  // namespace

TEST_CASE("SpellVisualKit special-attack animations adapt to equipped weapon loadout (TBC 2.4.3 parity)",
          "[anim][weapon-spell]") {
    namespace InvType = wowee::game::InvType;
    using wowee::rendering::resolveWeaponSpellAnimation;

    const auto oneHandShield = makeLoadout(InvType::ONE_HAND, false, false, false, false, true);
    const auto twoHandSword  = makeLoadout(InvType::TWO_HAND);
    const auto twoHandStaff  = makeLoadout(InvType::TWO_HAND, /*is2HLoose=*/true);
    const auto dualDaggers   = makeLoadout(InvType::ONE_HAND, false, false, /*isDagger=*/true, /*hasOffHand=*/true);
    const auto fistWeapon    = makeLoadout(InvType::ONE_HAND, false, /*isFist=*/true);
    const auto unarmed       = makeLoadout(InvType::NON_EQUIP);

    // Mortal Strike / Heroic Strike / Overpower / Rend / Cleave / Execute / Devastate / Revenge (kit anim = Special1H 57):
    // 1H+Shield MUST play Special1H (57), NEVER ShieldBash (59).
    CHECK(resolveWeaponSpellAnimation(SPECIAL_1H, oneHandShield) == SPECIAL_1H);
    CHECK(resolveWeaponSpellAnimation(SPECIAL_1H, twoHandSword)  == SPECIAL_2H);
    CHECK(resolveWeaponSpellAnimation(SPECIAL_1H, twoHandStaff)  == SPECIAL_2H);
    CHECK(resolveWeaponSpellAnimation(SPECIAL_1H, dualDaggers)   == SPECIAL_1H);
    CHECK(resolveWeaponSpellAnimation(SPECIAL_1H, fistWeapon)    == SPECIAL_1H);
    CHECK(resolveWeaponSpellAnimation(SPECIAL_1H, unarmed)       == SPECIAL_UNARMED);

    // Slam / Concussion Blow (kit anim = Special2H 58): adapts down to Special1H with 1H+Shield.
    CHECK(resolveWeaponSpellAnimation(SPECIAL_2H, oneHandShield) == SPECIAL_1H);
    CHECK(resolveWeaponSpellAnimation(SPECIAL_2H, twoHandSword)  == SPECIAL_2H);
    CHECK(resolveWeaponSpellAnimation(SPECIAL_2H, unarmed)       == SPECIAL_UNARMED);

    // Sinister Strike / Cheap Shot / Sap (kit anim = Attack1H 17): adapts across all weapon classes.
    CHECK(resolveWeaponSpellAnimation(ATTACK_1H, oneHandShield) == ATTACK_1H);
    CHECK(resolveWeaponSpellAnimation(ATTACK_1H, twoHandSword)  == ATTACK_2H);
    CHECK(resolveWeaponSpellAnimation(ATTACK_1H, twoHandStaff)  == ATTACK_2H_LOOSE);
    CHECK(resolveWeaponSpellAnimation(ATTACK_1H, dualDaggers)   == ATTACK_1H_PIERCE);
    CHECK(resolveWeaponSpellAnimation(ATTACK_1H, fistWeapon)    == ATTACK_UNARMED);
    CHECK(resolveWeaponSpellAnimation(ATTACK_1H, unarmed)       == ATTACK_UNARMED);

    // Explicit non-generic animations play as authored regardless of equipped weapon.
    for (const auto& wl : {oneHandShield, twoHandSword, twoHandStaff, dualDaggers, fistWeapon, unarmed}) {
        CHECK(resolveWeaponSpellAnimation(SHIELD_BASH, wl)       == SHIELD_BASH);       // Shield Bash (72), Shield Slam (23922)
        CHECK(resolveWeaponSpellAnimation(WHIRLWIND, wl)         == WHIRLWIND);         // Whirlwind (1680)
        CHECK(resolveWeaponSpellAnimation(KICK, wl)              == KICK);              // Kick (1766)
        CHECK(resolveWeaponSpellAnimation(MUTILATE, wl)          == MUTILATE);          // Mutilate (1329)
        CHECK(resolveWeaponSpellAnimation(SPECIAL_UNARMED, wl)   == SPECIAL_UNARMED);   // Pummel (6552), Hammer of Justice (853)
        CHECK(resolveWeaponSpellAnimation(ATTACK_1H_PIERCE, wl)  == ATTACK_1H_PIERCE);  // Backstab (53), Ambush (8676)
        CHECK(resolveWeaponSpellAnimation(ATTACK_OFF_PIERCE, wl) == ATTACK_OFF_PIERCE); // Shiv (5938)
        CHECK(resolveWeaponSpellAnimation(DRAGON_STOMP, wl)      == DRAGON_STOMP);      // Thunder Clap (6343)
        CHECK(resolveWeaponSpellAnimation(DRUID_CAT_CLAW, wl)    == DRUID_CAT_CLAW);    // Claw / Shred
        CHECK(resolveWeaponSpellAnimation(DRUID_BEAR_MAUL, wl)   == DRUID_BEAR_MAUL);   // Maul / Mangle (Bear)
        CHECK(resolveWeaponSpellAnimation(BATTLE_ROAR, wl)       == BATTLE_ROAR);       // Taunt / Battle Shout
        CHECK(resolveWeaponSpellAnimation(SPELL_CAST_DIRECTED, wl) == SPELL_CAST_DIRECTED);
        CHECK(resolveWeaponSpellAnimation(SPELL_CAST_OMNI, wl)   == SPELL_CAST_OMNI);
    }
}

TEST_CASE("isMeleeWeaponSpellAnimation identifies melee weapon swings for unsheathing",
          "[anim][weapon-spell]") {
    using wowee::rendering::isMeleeWeaponSpellAnimation;

    CHECK(isMeleeWeaponSpellAnimation(SPECIAL_1H));
    CHECK(isMeleeWeaponSpellAnimation(SPECIAL_2H));
    CHECK(isMeleeWeaponSpellAnimation(SHIELD_BASH));
    CHECK(isMeleeWeaponSpellAnimation(WHIRLWIND));
    CHECK(isMeleeWeaponSpellAnimation(MUTILATE));
    CHECK(isMeleeWeaponSpellAnimation(ATTACK_1H));
    CHECK(isMeleeWeaponSpellAnimation(ATTACK_2H));
    CHECK(isMeleeWeaponSpellAnimation(ATTACK_2H_LOOSE));
    CHECK(isMeleeWeaponSpellAnimation(ATTACK_1H_PIERCE));
    CHECK(isMeleeWeaponSpellAnimation(ATTACK_OFF_PIERCE));
    CHECK(isMeleeWeaponSpellAnimation(SPECIAL_UNARMED));

    CHECK_FALSE(isMeleeWeaponSpellAnimation(SPELL_CAST_DIRECTED));
    CHECK_FALSE(isMeleeWeaponSpellAnimation(SPELL_CAST_OMNI));
    CHECK_FALSE(isMeleeWeaponSpellAnimation(BATTLE_ROAR));
    CHECK_FALSE(isMeleeWeaponSpellAnimation(STAND));
}
