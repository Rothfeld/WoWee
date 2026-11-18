// expansion_api_surface.hpp - global Lua functions an expansion's own client
// does not define.
//
// The Lua API is registered once for every expansion, so a WotLK function such
// as VehicleAimUpStart exists on a TBC login too. The 2.4.3 client has no such
// global: `type(VehicleAimUpStart)` is "nil" there, a call raises "attempt to
// call a global 'VehicleAimUpStart' (a nil value)", and `if SortBGList then`
// takes the absent branch. A stand-in that answers nothing hides all three.
#pragma once

#include <span>
#include <string_view>

struct lua_State;

namespace wowee::addons {

/// Names the expansion's own client leaves undefined. Empty for an expansion
/// that has not been measured.
std::span<const char* const> apiAbsentFrom(std::string_view expansionId);

/// Set every name in apiAbsentFrom(expansionId) to nil. Run before FrameXML
/// loads, so its OnLoad feature checks see the expansion's real surface.
void removeApiAbsentFrom(lua_State* L, std::string_view expansionId);

} // namespace wowee::addons
