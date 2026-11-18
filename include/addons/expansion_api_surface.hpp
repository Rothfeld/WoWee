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

/// The expansion's measured answer conventions, applied by wrapping globals
/// and widget methods: 1/nil where this client answers true/false, and the
/// expansion's result counts. Run after registration, before FrameXML loads.
/// Measured only for TBC (build 8606); other expansions are left as they are.
void applyApiConventions(lua_State* L, std::string_view expansionId);

} // namespace wowee::addons
