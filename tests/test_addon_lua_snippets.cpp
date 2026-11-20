// The Lua this client injects into the interface, handed to Lua itself.
//
// These are C++ string literals that nothing compiles until the client runs.
// executeString answers false on a syntax error and the client carries on -
// the only sign is a warning in a log that is warning-only, and the panels
// simply are not there. Four hundred lines of Lua with no build step is four
// hundred lines where a stray `end` costs a release.
//
// This does not run them. Running needs the whole widget shim, the settings
// bridge and the interface loaded; that is what framexml_run is for. This asks
// the narrower question that has a cheap answer: does it parse.
#include <catch_amalgamated.hpp>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>

#include "addons/addon_lua_snippets.hpp"

namespace {

/// The Lua compiler's own verdict, and its message when it refuses.
std::string compileError(const char* chunk, const char* name) {
    lua_State* L = luaL_newstate();
    REQUIRE(L != nullptr);
    // No libraries opened: parsing needs none, and the vendored Lua leaves out
    // loadlib deliberately - a game client has no business dlopening things -
    // so luaL_openlibs would not even link here.
    std::string err;
    if (luaL_loadbuffer(L, chunk, std::strlen(chunk), name) != 0) {
        const char* message = lua_tostring(L, -1);
        err = message ? message : "(no message)";
    }
    lua_close(L);
    return err;
}

/// Run several chunks in one state, and answer with what the last one returned.
///
/// More than parsing, and still short of framexml_run: enough of Lua to run a
/// snippet against stand-ins for the frames it reaches for. A failure comes
/// back as the Lua error, so the message says which chunk and which line.
std::string runChunks(std::initializer_list<std::pair<const char*, const char*>> chunks) {
    lua_State* L = luaL_newstate();
    REQUIRE(L != nullptr);
    // Only what the snippets under test use: pairs, ipairs, type and rawget
    // from the base library, insert and remove from the table one.
    lua_pushcfunction(L, luaopen_base);
    lua_call(L, 0, 0);
    lua_pushcfunction(L, luaopen_table);
    lua_pushstring(L, LUA_TABLIBNAME);
    lua_call(L, 1, 0);
    lua_pushcfunction(L, luaopen_string);
    lua_pushstring(L, LUA_STRLIBNAME);
    lua_call(L, 1, 0);
    lua_pushcfunction(L, luaopen_math);
    lua_pushstring(L, LUA_MATHLIBNAME);
    lua_call(L, 1, 0);

    std::string result;
    for (const auto& [chunk, name] : chunks) {
        if (luaL_loadbuffer(L, chunk, std::strlen(chunk), name) != 0 ||
            lua_pcall(L, 0, 1, 0) != 0) {
            const char* message = lua_tostring(L, -1);
            lua_close(L);
            return std::string(name) + ": " + (message ? message : "(no message)");
        }
        const char* value = lua_tostring(L, -1);
        result = value ? value : "";
        lua_pop(L, 1);
    }
    lua_close(L);
    return result;
}

/// The options machinery the removal snippet walks, cut down to what it calls.
constexpr const char* kFakeOptionsPanelLua = R"LUA(
local function frame(name, parent)
    local f = {
        _name = name, _parent = parent, _hidden = false, _hooks = {},
        GetName = function(self) return self._name end,
        GetParent = function(self) return self._parent end,
        GetChildren = function(self) return unpack(self._children or {}) end,
        Hide = function(self) self._hidden = true end,
        HookScript = function(self, script, fn) self._hooks[script] = fn end,
    }
    _G[name] = f
    if parent then
        parent._children = parent._children or {}
        table.insert(parent._children, f)
    end
    return f
end

local panel = frame("VideoOptionsResolutionPanel")
-- The retired checkbox, and one control that stays.
local vsync = frame("VideoOptionsResolutionPanelVSync", panel)
local scale = frame("VideoOptionsResolutionPanelUIScaleSlider", panel)
panel.controls = { vsync, scale }

-- Both carry what BlizzardOptionsPanel_SetupControl would have left on them:
-- the cached value the Okay loops commit, and for the action bar boxes the
-- uvar naming a global FrameXML reads. A retired control has to lose the
-- first and keep the second.
vsync.value = "0"
vsync.uvar = "GX_VSYNC"
scale.value = "1"

local bars = frame("InterfaceOptionsActionBarsPanel")
local bottomLeft = frame("InterfaceOptionsActionBarsPanelBottomLeft", bars)
bottomLeft.value = "1"
bottomLeft.uvar = "SHOW_MULTI_ACTIONBAR_1"
bars.controls = { bottomLeft }
)LUA";

}  // namespace

TEST_CASE("a retired control commits nothing and keeps its place", "[addonlua]") {
    // Hiding a control is not retiring it. VideoOptionsPanel_Okay walks
    // panel.controls and writes every entry's cached value back to its cvar,
    // changed or not - so the hidden vertical-sync checkbox replayed the value
    // it read at load over the Display page's own row, and pressing Okay on the
    // video window turned vertical sync off again each time it was switched on.
    //
    // Both halves are asserted here because fixing the first by unregistering
    // the control broke the second, and shipped: BlizzardOptionsPanel_SetupControl
    // runs only over panel.controls, and it is what assigns the uvar globals, so
    // the action bar checkboxes leaving the list left SHOW_MULTI_ACTIONBAR_1 nil
    // and the player with no bottom action bar.
    const std::string result = runChunks({
        {kFakeOptionsPanelLua, "FakeOptionsPanel"},
        {wowee::addons::kRemovedControlsLua, "RemovedControls"},
        {R"LUA(
            local vsync = VideoOptionsResolutionPanelVSync
            local scale = VideoOptionsResolutionPanelUIScaleSlider
            local bar = InterfaceOptionsActionBarsPanelBottomLeft
            local listed = ""
            for _, c in ipairs(VideoOptionsResolutionPanel.controls) do
                listed = listed .. c:GetName() .. " "
            end
            return "listed=" .. listed
                .. "| vsync.value=" .. tostring(vsync.value)
                .. " scale.value=" .. tostring(scale.value)
                .. " bar.value=" .. tostring(bar.value)
                .. " bar.uvar=" .. tostring(bar.uvar)
                .. " hidden=" .. tostring(vsync._hidden)
        )LUA",
         "Check"},
    });
    // The retired boxes keep their place on the panel, so setup still reaches
    // them, and lose only the value the Okay loops would have written back.
    CHECK(result ==
          "listed=VideoOptionsResolutionPanelVSync VideoOptionsResolutionPanelUIScaleSlider "
          "| vsync.value=nil scale.value=1 bar.value=nil "
          "bar.uvar=SHOW_MULTI_ACTIONBAR_1 hidden=true");
}

TEST_CASE("the options panel script parses", "[addonlua]") {
    // Twelve categories of check button, slider and dropdown, built from the
    // settings schema. Added over several passes, none of which could have
    // been told by the compiler that it had broken the one before.
    const std::string err =
        compileError(wowee::addons::kWoweeOptionsPanelLua, "WoweeOptionsPanel");
    INFO(err);
    CHECK(err.empty());
}

TEST_CASE("the coin clearance script parses", "[addonlua]") {
    // Reaches into MoneyFrame_Update, which is Blizzard's, and hooks rather
    // than edits it - so a mistake here shows up as money that draws wrong
    // rather than as anything that says "script error".
    const std::string err =
        compileError(wowee::addons::kCoinAmountClearanceLua, "CoinAmountClearance");
    INFO(err);
    CHECK(err.empty());
}

TEST_CASE("the chat box visibility script parses", "[addonlua]") {
    // The one that proves the point of this file. It lived in the settings
    // panel as a run of adjacent C++ string literals with no separator, so the
    // `end` closing its loop ran into the `end` closing its `if` and then into
    // the `if` after that. Every click of Chat Style printed a script error and
    // changed nothing, and no build ever said so.
    const std::string err =
        compileError(wowee::addons::kChatBoxVisibilityLua, "ChatBoxVisibility");
    INFO(err);
    CHECK(err.empty());
}

TEST_CASE("the chat background swatch script parses", "[addonlua]") {
    const std::string err = compileError(wowee::addons::kChatBackgroundSwatchLua,
                                         "ChatBackgroundSwatch");
    INFO(err);
    CHECK(err.empty());
}

TEST_CASE("the UI scale confirmation and TBC options integration scripts parse", "[addonlua]") {
    const std::string scaleErr =
        compileError(wowee::addons::kUiScaleConfirmLua, "UiScaleConfirm");
    INFO(scaleErr);
    CHECK(scaleErr.empty());

    const std::string tbcErr =
        compileError(wowee::addons::kTbcOptionsIntegrationLua, "TbcOptionsIntegration");
    INFO(tbcErr);
    CHECK(tbcErr.empty());

    const std::string minimapErr =
        compileError(wowee::addons::kMinimapShapeLua, "MinimapShape");
    INFO(minimapErr);
    CHECK(minimapErr.empty());

    const std::string hudErr =
        compileError(wowee::addons::kHudLayoutSettingsLua, "HudLayoutSettings");
    INFO(hudErr);
    CHECK(hudErr.empty());

    const std::string chatVisErr =
        compileError(wowee::addons::kChatBoxVisibilityLua, "ChatBoxVisibility");
    INFO(chatVisErr);
    CHECK(chatVisErr.empty());
}

TEST_CASE("TBC options keep stock Blizzard panels and add OptionsFrame Video/Renderer tabs", "[addonlua]") {
    const std::string result = runChunks({
        {R"LUA(
            __WoweeInterfaceVersion = 20400
            local settings = {
                upscaling = "0", fsrquality = "3", fsrsharpness = "1.6", framecap = "0",
                fxaa = "0", sunshafts = "1", lensflare = "1",
                raytracedlighting = "0", lightshafts = "2", parallaxquality = "1",
                normalmapping = "1", parallax = "1", normalmapstrength = "0.8",
                shadowdistance = "300", grassenabled = "0", sharpstars = "1",
                fogstrength = "0.4", grassdensity = "100", grassdistance = "150",
            }
            WoweeGetSetting = function(k) return settings[k] end
            WoweeSetSetting = function(k, v) settings[k] = tostring(v) end
            WoweeSettingList = function()
                return { { key = "fxaa", label = "FXAA", category = "Graphics", kind = "bool" } }
            end
            GetCVarMax = function(name)
                if name == "uiscale" then return 1.2 end
                return nil
            end
            GetCVar = function(name)
                if name == "gxRefresh" then return "60" end
                return "0"
            end
            GetRefreshRates = function() return 0 end
            HERTZ = " Hz"

            local function newWidget(name, parent, template)
                local w = {
                    _name = name, _parent = parent, _template = template,
                    _shown = true, _text = "", _checked = nil, _val = 0,
                    _hooks = {}, _scripts = {},
                    GetName = function(self) return self._name end,
                    GetParent = function(self) return self._parent end,
                    Show = function(self) self._shown = true end,
                    Hide = function(self)
                        self._shown = false
                        if self._hooks["OnHide"] then self._hooks["OnHide"](self) end
                    end,
                    IsShown = function(self) return self._shown end,
                    SetWidth = function(self, v) self._w = v end,
                    SetHeight = function(self, v) self._h = v end,
                    SetPoint = function(self) end,
                    ClearAllPoints = function(self) end,
                    SetBackdropBorderColor = function() end,
                    SetBackdropColor = function() end,
                    SetID = function(self, id) self._id = id end,
                    GetID = function(self) return self._id or 1 end,
                    SetText = function(self, t) self._text = tostring(t or "") end,
                    GetText = function(self) return self._text end,
                    SetChecked = function(self, c) self._checked = (c and c ~= 0) and 1 or nil end,
                    GetChecked = function(self) return self._checked end,
                    SetMinMaxValues = function(self, lo, hi) self._lo = lo; self._hi = hi end,
                    SetValueStep = function(self, s) self._step = s end,
                    SetValue = function(self, v)
                        self._val = v
                        if self._scripts["OnValueChanged"] then
                            local prevThis, prevArg1 = this, arg1
                            this, arg1 = self, v
                            self._scripts["OnValueChanged"](self, v)
                            this, arg1 = prevThis, prevArg1
                        end
                    end,
                    GetValue = function(self) return self._val end,
                    Enable = function() end,
                    Disable = function() end,
                    SetVertexColor = function() end,
                    SetScript = function(self, k, fn) self._scripts[k] = fn end,
                    GetScript = function(self, k) return self._scripts[k] end,
                    HookScript = function(self, k, fn) self._hooks[k] = fn end,
                    CreateFontString = function(self, fsName)
                        local fs = { _text = "", SetPoint = function() end, SetVertexColor = function() end,
                                     SetText = function(s, t) s._text = tostring(t or "") end,
                                     GetText = function(s) return s._text end }
                        if fsName then _G[fsName] = fs end
                        return fs
                    end,
                }
                if name then
                    _G[name] = w
                    if template == "OptionFrameBoxTemplate" then
                        _G[name .. "Title"] = w:CreateFontString(name .. "Title")
                    elseif template == "OptionsCheckButtonTemplate" then
                        _G[name .. "Text"] = w:CreateFontString(name .. "Text")
                    elseif template == "OptionsSliderTemplate" then
                        _G[name .. "Text"] = w:CreateFontString(name .. "Text")
                        _G[name .. "Low"] = w:CreateFontString(name .. "Low")
                        _G[name .. "High"] = w:CreateFontString(name .. "High")
                    end
                end
                return w
            end
            CreateFrame = function(_, name, parent, template) return newWidget(name, parent, template) end

            UIDropDownMenu_SetWidth = function(width, frame)
                assert(type(width) == "number" and type(frame) == "table", "TBC SetWidth expects (width, frame)")
                frame._ddWidth = width
            end
            UIDropDownMenu_SetText = function(text, frame)
                assert(type(text) == "string" and type(frame) == "table", "TBC SetText expects (text, frame)")
                frame._ddText = text
            end
            UIDropDownMenu_Initialize = function(frame, fn) frame._initFn = fn end
            UIDropDownMenu_SetSelectedID = function(frame, id) frame._selId = id end
            UIDropDownMenu_SetSelectedValue = function(frame, v) frame._selVal = v end
            UIDropDownMenu_EnableDropDown = function() end
            UIDropDownMenu_DisableDropDown = function() end
            PanelTemplates_SetNumTabs = function(f, n) f.numTabs = n end
            PanelTemplates_SetTab = function(f, id) f.activeTab = id end

            GameMenuFrame = newWidget("GameMenuFrame")
            KeyBindingFrame = newWidget("KeyBindingFrame")
            KeyBindingFrame_OnKeyDown = function() end
            KeyBindingFrameCharacterButton = newWidget("KeyBindingFrameCharacterButton", KeyBindingFrame)

            OptionsFrame = newWidget("OptionsFrame")
            OptionsFrameDisplay = newWidget("OptionsFrameDisplay", OptionsFrame, "OptionFrameBoxTemplate")
            OptionsFrameBrightness = newWidget("OptionsFrameBrightness", OptionsFrame, "OptionFrameBoxTemplate")
            OptionsFrameWorldAppearance = newWidget("OptionsFrameWorldAppearance", OptionsFrame, "OptionFrameBoxTemplate")
            OptionsFramePixelShaders = newWidget("OptionsFramePixelShaders", OptionsFrame, "OptionFrameBoxTemplate")
            OptionsFrameMiscellaneous = newWidget("OptionsFrameMiscellaneous", OptionsFrame, "OptionFrameBoxTemplate")
            OptionsFrameRefreshDropDown = newWidget("OptionsFrameRefreshDropDown", OptionsFrameDisplay, "UIDropDownMenuTemplate")
            OptionsFrameLoadCalled = 0
            OptionsFrameSaveCalled = 0
            OptionsFrameCancelCalled = 0
            OptionsFrameDefaultsCalled = 0
            OptionsFrame_Load = function() OptionsFrameLoadCalled = OptionsFrameLoadCalled + 1 end
            OptionsFrame_Save = function() OptionsFrameSaveCalled = OptionsFrameSaveCalled + 1 end
            OptionsFrame_Cancel = function() OptionsFrameCancelCalled = OptionsFrameCancelCalled + 1 end
            OptionsFrame_SetDefaults = function() OptionsFrameDefaultsCalled = OptionsFrameDefaultsCalled + 1 end

            OPTIONS_FARCLIP_MIN = 177
            OPTIONS_FARCLIP_MAX = 777
            OptionsFrameSliders = {
                { func = "uiscale", minValue = 0.64, maxValue = 1.0, valueStep = 0.01 },
                { func = "Gamma", minValue = -0.5, maxValue = 0.5, valueStep = 0.1 },
                { func = "farclip", minValue = 177, maxValue = 777, valueStep = 60 },
                { func = "TerrainMip", minValue = 0, maxValue = 1, valueStep = 1, restartClient = 1 },
                { func = "spellEffectLevel", minValue = 0, maxValue = 9, valueStep = 1 },
                { func = "WorldDetail", minValue = 0, maxValue = 2, valueStep = 1 },
                { func = "groundEffectDensity", minValue = 16, maxValue = 64, valueStep = 8 },
                { func = "groundEffectDist", minValue = 70, maxValue = 140, valueStep = 10 },
                { func = "BaseMip", minValue = 0, maxValue = 1, valueStep = 1 },
                { func = "textureFilteringMode", minValue = 0, maxValue = 5, valueStep = 1, restartClient = 1 },
                { func = "weatherDensity", minValue = 0, maxValue = 3, valueStep = 1 },
            }
            OptionsFrameCheckButtons = {
                TRIPLE_BUFFER = { index = 13, cvar = "gxTripleBuffer" },
                CHARACTER_SHADOWS = { index = 19, cvar = "shadowLOD" },
            }
            SoundOptionsFrameCheckButtons = {
                ENABLE_REVERB = { index = 10, cvar = "Sound_EnableReverb" },
                ENABLE_HARDWARE = { index = 11, cvar = "Sound_EnableHardware" },
            }
            InterfaceOptionsQuestsPanel = newWidget("InterfaceOptionsQuestsPanel")
            InterfaceOptionsPartyRaidPanel = newWidget("InterfaceOptionsPartyRaidPanel")
            InterfaceOptionsActionBarsPanel = newWidget("InterfaceOptionsActionBarsPanel")
        )LUA",
         "TbcSetup"},
        {wowee::addons::kWoweeOptionsPanelLua, "WoweeOptionsPanel"},
        {wowee::addons::kAboutMenuLua, "AboutMenu"},
        {wowee::addons::kBindingDeviceEditor, "BindingDeviceEditor"},
        {wowee::addons::kRemovedControlsLua, "RemovedControls"},
        {wowee::addons::kMinimapShapeLua, "MinimapShape"},
        {wowee::addons::kHudLayoutSettingsLua, "HudLayoutSettings"},
        {wowee::addons::kTbcOptionsIntegrationLua, "TbcOptionsIntegration"},
        {R"LUA(
            -- Verify zero WoWee category pollution and stock controls preserved
            assert(rawget(_G, "WoweeOptionsRoot") == nil, "WoweeOptionsRoot must not exist on TBC")
            assert(rawget(_G, "GameMenuButtonWoweeAbout") == nil, "About button must not inject on TBC")
            assert(rawget(_G, "WoweeBindingDeviceSelector") == nil, "Device selector must not inject on TBC")
            assert(rawget(_G, "WoweeVideoMoreButton") == nil, "WoWee Graphics button must not exist")
            assert(rawget(_G, "WoweeAudioMoreButton") == nil, "WoWee Sound button must not exist")
            assert(KeyBindingFrameCharacterButton:IsShown(), "Character Specific Key Bindings stays visible")
            assert(not InterfaceOptionsQuestsPanel.hidden, "Quests panel stays visible")
            assert(not InterfaceOptionsPartyRaidPanel.hidden, "PartyRaid panel stays visible")
            assert(not InterfaceOptionsActionBarsPanel.hidden, "ActionBars panel stays visible")
            assert(OptionsFrameSliders[4] ~= nil and OptionsFrameSliders[9] ~= nil, "TerrainMip and BaseMip kept")
            assert(OptionsFrameCheckButtons.TRIPLE_BUFFER ~= nil, "Triple Buffer kept")
            assert(SoundOptionsFrameCheckButtons.ENABLE_REVERB ~= nil, "Enable Reverb kept")

            -- Verify Video/Renderer tabs and dropdown texts
            assert(OptionsFrame.numTabs == 2, "OptionsFrame has 2 bottom tabs")
            assert(OptionsFrameTab1:GetText() == "Video" and OptionsFrameTab2:GetText() == "Renderer")
            assert(OptionsFrameRefreshDropDown._ddText == "60 Hz", "Refresh rate dropdown shows 60 Hz")
            assert(OptionsFrameRendererUpscalingDropDown._ddText == "Off")
            assert(OptionsFrameRendererFsrQualityDropDown._ddText == "Native (100%)")
            assert(OptionsFrameRendererLightShaftsDropDown._ddText == "Medium")

            -- Switch to Renderer tab, toggle FXAA, then Cancel -> rolls back to 0
            OptionsFrame_Load()
            OptionsFrameTab2:GetScript("OnClick")()
            assert(OptionsFrameRendererPanel:IsShown() and not OptionsFrameDisplay:IsShown())
            OptionsFrameRendererFxaaCheck:SetChecked(1)
            this = OptionsFrameRendererFxaaCheck
            OptionsFrameRendererFxaaCheck:GetScript("OnClick")()
            assert(WoweeGetSetting("fxaa") == "1")
            OptionsFrame_Cancel()
            assert(WoweeGetSetting("fxaa") == "0", "Cancel rolls back Tab 2 changes")

            -- Open again, set FXAA + Shadow Distance, Save -> commits; Defaults -> resets
            OptionsFrame_Load()
            OptionsFrameRendererFxaaCheck:SetChecked(1)
            this = OptionsFrameRendererFxaaCheck
            OptionsFrameRendererFxaaCheck:GetScript("OnClick")()
            OptionsFrameRendererShadowDistSlider:SetValue(420)
            OptionsFrame_Save()
            OptionsFrame:Hide()
            assert(WoweeGetSetting("fxaa") == "1" and WoweeGetSetting("shadowdistance") == "420")
            OptionsFrame_SetDefaults()
            assert(WoweeGetSetting("fxaa") == "0" and WoweeGetSetting("shadowdistance") == "300")

            return "ok farMax=" .. tostring(OptionsFrameSliders[3].maxValue)
                .. " anisoMax=" .. tostring(OptionsFrameSliders[10].maxValue)
                .. " missing=" .. tostring(#__WoweeRemovedControlsMissing)
        )LUA",
         "Verify"},
    });
    CHECK(result == "ok farMax=2400 anisoMax=4 missing=0");
}
