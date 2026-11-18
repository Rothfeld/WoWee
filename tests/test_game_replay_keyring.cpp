// HasKey() against the player's slot fields, through GameHandler's own
// dispatch. See game_replay.hpp.
//
// mainmenubar.xml sets the per-character SHOW_KEYRING to 1 the first time
// HasKey() answers true on PLAYER_ENTERING_WORLD, VARIABLES_LOADED or
// BAG_UPDATE, and nothing sets it back. A HasKey that answers true for one
// update without a key in the ring shows the keyring for good. On 2.4.3 the
// answer follows the keyring slots alone: a key added with `.additem 8444`
// answers 1, and nil again once it is removed (SHOW_KEYRING stays 1).
// Evidence: _experiments/20261118T194107.keyring_show/ in the workspace root.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"
#include "addons/lua_api_registrations.hpp"

#include <cstdint>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

using replay::Session;
using replay::UpdateObject;
using wowee::game::UF;

namespace {

// An item guid as CMaNGOS TBC writes it into a slot field: low word, then
// HIGHGUID_ITEM (0x4000) in the top half of the high word.
constexpr uint32_t kItemLow = 0x47C;
constexpr uint32_t kItemHigh = 0x40000000;

bool hasKey(Session& s) {
    lua_State* L = luaL_newstate();
    // The libraries LuaEngine opens; the registrar builds on them.
    luaopen_base(L);
    luaopen_table(L);
    luaopen_string(L);
    luaopen_math(L);
    lua_settop(L, 0);
    lua_pushlightuserdata(L, &s.handler());
    lua_setfield(L, LUA_REGISTRYINDEX, "wowee_game_handler");
    wowee::addons::registerSystemLuaAPI(L);
    REQUIRE(luaL_dostring(L, "return HasKey()") == 0);
    const bool result = lua_toboolean(L, -1) != 0;
    lua_close(L);
    return result;
}

}  // namespace

TEST_CASE("HasKey answers from the keyring slots alone", "[replay][keyring]") {
    Session s("tbc");
    // Backpack, bank and bank bag slots all holding items at login.
    s.receive(UpdateObject().tbc().create(Session::kPlayerGuid, wowee::game::ObjectType::PLAYER,
        {{UF::PLAYER_FIELD_PACK_SLOT_1, kItemLow},
         {UF::PLAYER_FIELD_BANK_SLOT_1, kItemLow + 1},
         {UF::PLAYER_FIELD_BANKBAG_SLOT_1, kItemLow + 2}}, true));
    CHECK_FALSE(hasKey(s));

    // CMaNGOS TBC UpdateFields.h: PLAYER_FIELD_VENDORBUYBACK_SLOT_1 is 24
    // fields ending right before PLAYER_FIELD_KEYRING_SLOT_1 (UNIT_END +
    // 0x234 + 24 = 0x24C), and PLAYER_FIELD_VANITYPET_SLOT_1 starts right
    // after the keyring's 64 (0x24C + 64 = 0x28C). The neighbours on both
    // sides of the ring hold items; the ring does not.
    const auto keyring = wowee::game::fieldIndex(UF::PLAYER_FIELD_KEYRING_SLOT_1);
    REQUIRE(keyring != 0xFFFF);
    const auto lastBuyback = static_cast<uint16_t>(keyring - 2);
    const auto firstVanityPet = static_cast<uint16_t>(keyring + 64);
    s.receive(UpdateObject().tbc().valuesAt(Session::kPlayerGuid,
        {{lastBuyback, kItemLow + 3}, {static_cast<uint16_t>(lastBuyback + 1), kItemHigh},
         {firstVanityPet, kItemLow + 4}, {static_cast<uint16_t>(firstVanityPet + 1), kItemHigh}}));
    CHECK_FALSE(hasKey(s));

    // A key looted into the first keyring slot (bag 0 slot 86), then used up.
    s.receive(UpdateObject().tbc().values(Session::kPlayerGuid,
        {{UF::PLAYER_FIELD_KEYRING_SLOT_1, kItemLow + 5}}));
    s.receive(UpdateObject().tbc().valuesAt(Session::kPlayerGuid,
        {{static_cast<uint16_t>(keyring + 1), kItemHigh}}));
    CHECK(hasKey(s));
    s.receive(UpdateObject().tbc().valuesAt(Session::kPlayerGuid,
        {{keyring, 0}, {static_cast<uint16_t>(keyring + 1), 0}}));
    CHECK_FALSE(hasKey(s));

    // The last of the 32 keyring slots counts too.
    const auto lastKeyring = static_cast<uint16_t>(keyring + 62);
    s.receive(UpdateObject().tbc().valuesAt(Session::kPlayerGuid,
        {{lastKeyring, kItemLow + 6}, {static_cast<uint16_t>(lastKeyring + 1), kItemHigh}}));
    CHECK(hasKey(s));
}
