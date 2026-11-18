// The local player's weapons that build 8606 stops drawing when they break,
// replayed from the original's observed sequences on pair 13 (rogue Edrikm,
// daggers in both hands, bow in the ranged slot):
// _experiments/20261118T124909.broken_items.
#include <catch_amalgamated.hpp>

#include "core/broken_weapon_visibility.hpp"

using wowee::core::BrokenWeaponVisibility;

namespace {

constexpr int kMain = 0, kOff = 1, kRanged = 2;
constexpr uint32_t kRightHand = 1, kLeftHand = 2;
constexpr uint8_t kUnarmed = 0, kMelee = 1, kRangedState = 2;
constexpr uint32_t kModel = 7;

struct Gear {
    BrokenWeaponVisibility::Snapshot slots{{
        {.itemGuid = 860, .broken = false, .heldPoint = kRightHand, .heldSheathState = kMelee},
        {.itemGuid = 879, .broken = false, .heldPoint = kLeftHand, .heldSheathState = kMelee},
        {.itemGuid = 859, .broken = false, .heldPoint = kLeftHand, .heldSheathState = kRangedState},
    }};
    BrokenWeaponVisibility::Snapshot& operator()(int slot, bool broken) {
        slots[slot].broken = broken;
        return slots;
    }
};

bool shown(const BrokenWeaponVisibility& v, int slot) { return !v.hidden(slot); }

}  // namespace

TEST_CASE("Broken weapon already broken at login is drawn", "[broken_weapon]") {
    // n02: the clone logged in with Worn Dagger at durability 0 on its hip.
    BrokenWeaponVisibility v;
    Gear gear;
    v.observe(gear(kMain, true), kModel, kRangedState);
    CHECK(shown(v, kMain));
    CHECK_FALSE(v.any());
}

TEST_CASE("A weapon breaking stops being drawn until it is repaired", "[broken_weapon]") {
    BrokenWeaponVisibility v;
    Gear gear;
    v.observe(gear.slots, kModel, kMelee);
    // n25 -> n26: main hand breaks in hand.
    v.observe(gear(kMain, true), kModel, kMelee);
    CHECK(v.hidden(kMain));
    CHECK(shown(v, kOff));
    // Unrelated refreshes (another slot, /reload, teleport) change nothing.
    v.observe(gear.slots, kModel, kMelee);
    CHECK(v.hidden(kMain));
    // n26 -> n27: repair brings it back.
    v.observe(gear(kMain, false), kModel, kMelee);
    CHECK(shown(v, kMain));
}

TEST_CASE("A sheath change draws a broken weapon again", "[broken_weapon]") {
    BrokenWeaponVisibility v;
    Gear gear;
    v.observe(gear.slots, kModel, kMelee);
    v.observe(gear(kMain, true), kModel, kMelee);  // n28
    REQUIRE(v.hidden(kMain));
    v.observe(gear.slots, kModel, kRangedState);   // n29: back on the hip
    CHECK(shown(v, kMain));
    CHECK_FALSE(v.any());
}

TEST_CASE("A new player model draws a broken weapon again", "[broken_weapon]") {
    BrokenWeaponVisibility v;
    Gear gear;
    v.observe(gear.slots, kModel, kMelee);
    v.observe(gear(kOff, true), kModel, kMelee);
    REQUIRE(v.hidden(kOff));
    // n60/n61: a map change rebuilds the player, broken weapon in hand.
    v.observe(gear.slots, kModel + 1, kMelee);
    CHECK(shown(v, kOff));
}

TEST_CASE("Equipping into the slot draws it; equipping elsewhere does not", "[broken_weapon]") {
    BrokenWeaponVisibility v;
    Gear gear;
    v.observe(gear.slots, kModel, kRangedState);
    v.observe(gear(kMain, true), kModel, kRangedState);
    REQUIRE(v.hidden(kMain));
    // n47/n48: a new off hand dagger leaves the hidden main hand hidden.
    gear.slots[kOff].itemGuid = 902;
    v.observe(gear.slots, kModel, kRangedState);
    CHECK(v.hidden(kMain));
    CHECK(shown(v, kOff));
    // Taking the main hand off and putting the same item back draws it.
    const uint64_t mainGuid = gear.slots[kMain].itemGuid;
    gear.slots[kMain] = {};
    v.observe(gear.slots, kModel, kRangedState);
    gear.slots[kMain] = {.itemGuid = mainGuid, .broken = true,
                         .heldPoint = kRightHand, .heldSheathState = kMelee};
    v.observe(gear.slots, kModel, kRangedState);
    CHECK(shown(v, kMain));
}

TEST_CASE("Off hand breaking in the ranged state takes the bow out of the hand", "[broken_weapon]") {
    BrokenWeaponVisibility v;
    Gear gear;
    v.observe(gear.slots, kModel, kRangedState);
    v.observe(gear(kOff, true), kModel, kRangedState);  // n31 -> n32
    CHECK(v.hidden(kOff));
    CHECK(v.hidden(kRanged));
    CHECK(shown(v, kMain));
}

TEST_CASE("Bow breaking takes the off hand only when it is in hand", "[broken_weapon]") {
    SECTION("ranged state: the off hand is on the hip and stays (n48 -> n49)") {
        BrokenWeaponVisibility v;
        Gear gear;
        v.observe(gear.slots, kModel, kRangedState);
        v.observe(gear(kRanged, true), kModel, kRangedState);
        CHECK(v.hidden(kRanged));
        CHECK(shown(v, kOff));
    }
    SECTION("melee state: the off hand is in the left hand and goes (n52 -> n53)") {
        BrokenWeaponVisibility v;
        Gear gear;
        v.observe(gear.slots, kModel, kMelee);
        v.observe(gear(kRanged, true), kModel, kMelee);
        CHECK(v.hidden(kRanged));
        CHECK(v.hidden(kOff));
        CHECK(shown(v, kMain));
        // n65 -> n66: repairing the bow does not bring the off hand back.
        v.observe(gear(kRanged, false), kModel, kMelee);
        CHECK(shown(v, kRanged));
        CHECK(v.hidden(kOff));
    }
    SECTION("unarmed state: nothing is in hand to take") {
        BrokenWeaponVisibility v;
        Gear gear;
        v.observe(gear.slots, kModel, kUnarmed);
        v.observe(gear(kRanged, true), kModel, kUnarmed);
        CHECK(v.hidden(kRanged));
        CHECK(shown(v, kOff));
    }
}
