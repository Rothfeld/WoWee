#include <sstream>
#include <fstream>
#include <cstdlib>
#include "core/npc_interaction_facing.hpp"
#include "core/npc_panel_emotes.hpp"
#include "core/coordinates.hpp"
#include "input_test_support.hpp"
#include "game_replay.hpp"
#include "game/packet_parsers.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/asset_manager.hpp"
#include "game/transport_manager.hpp"
#include "ui/game_screen.hpp"
#include "ui/framexml_takeover.hpp"
#include "addons/lua_engine.hpp"
#include "addons/lua_api_helpers.hpp"
#include "addons/addon_lua_snippets.hpp"
#include "addons/addon_manager.hpp"
#include "addons/lua_api_registrations.hpp"
#include "core/gamepad.hpp"
#include "core/input.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/locomotion_fsm.hpp"
#include "ui/gamepad_controls.hpp"
#include "ui/framexml_emitter.hpp"
#include "ui/xml_parser.hpp"
#include <array>
#include <cmath>
#include <memory>
#include <set>
#include <zlib.h>

using namespace input_test;

TEST_CASE("TBC transport creation retains server clock including wrap values", "[transport]") {
    wowee::game::TbcPacketParsers parser;
    for (uint32_t clock : {0u, 0xfedcba98u, 0xffffffffu}) {
        wowee::network::Packet packet(0);
        packet.writeUInt8(0x02);
        packet.writeUInt32(clock);
        wowee::game::UpdateBlock block{};
        REQUIRE(parser.parseMovementBlock(packet, block));
        CHECK(block.hasTransportClock);
        CHECK(block.transportClockMs == clock);
        CHECK(packet.getRemainingSize() == 0);
    }
}
TEST_CASE("TBC boarding and passenger creation use raw transport GUIDs", "[transport]") {
    using namespace wowee::game;
    TbcPacketParsers parser;
    MovementInfo movement{};
    movement.flags = 0x200;
    movement.time = 123456;
    movement.transportGuid = 0x01fc00000002b074ULL;
    movement.transportX = 1.25f;
    movement.transportY = -2.5f;
    movement.transportZ = 7.0f;
    movement.transportO = 0.75f;
    movement.transportTime = 987654;
    movement.fallTime = 4321;
    wowee::network::Packet payload(0);
    parser.writeMovementPayload(payload, movement);
    REQUIRE(payload.getSize() == 57);
    CHECK(payload.readUInt32() == 0x200);
    CHECK(payload.readUInt8() == 0);
    CHECK(payload.readUInt32() == movement.time);
    for (int i=0; i<4; ++i) CHECK(payload.readFloat() == 0);
    CHECK(payload.readUInt64() == movement.transportGuid);
    CHECK(payload.readFloat() == movement.transportX);
    CHECK(payload.readFloat() == movement.transportY);
    CHECK(payload.readFloat() == movement.transportZ);
    CHECK(payload.readFloat() == movement.transportO);
    CHECK(payload.readUInt32() == movement.transportTime);
    CHECK(payload.readUInt32() == movement.fallTime);
    CHECK(payload.getRemainingSize() == 0);

    wowee::network::Packet create(0);
    create.writeUInt8(0x20);
    parser.writeMovementPayload(create, movement);
    for (float speed : {2.5f,7.f,4.5f,4.7f,2.5f,7.f,4.5f,3.14f}) create.writeFloat(speed);
    UpdateBlock block{};
    REQUIRE(parser.parseMovementBlock(create, block));
    CHECK(block.onTransport);
    CHECK(block.transportGuid == movement.transportGuid);
    CHECK(block.transportX == movement.transportX);
    CHECK(block.transportY == movement.transportY);
    CHECK(block.transportZ == movement.transportZ);
    CHECK(block.transportO == movement.transportO);
    CHECK(block.runSpeed == 7.f);
    CHECK(create.getRemainingSize() == 0);
}

namespace wowee::ui {
// Plates are drawn by renderNameplates, which needs a renderer; the click
// routing under test reads only the boxes it leaves behind.
struct GameScreenTestAccess {
    static void setPlateHits(GameScreen& screen, std::vector<PlateHit> hits) {
        screen.plateHits_ = std::move(hits);
    }
};
}  // namespace wowee::ui

namespace ui = wowee::ui;
namespace addons = wowee::addons;
namespace core = wowee::core;
namespace rendering = wowee::rendering;

TEST_CASE("TBC game object creation and updates deliver authored state", "[game-object-pose]") {
    using namespace wowee::game;
    for (const uint32_t initialState : {0u, 1u}) {
        replay::Session session("tbc");
        constexpr uint64_t guid = 0xf11002ae7200c228ull;
        std::vector<uint8_t> states;
        session.handler().setGameObjectStateCallback([&](uint64_t id, uint8_t state) {
            CHECK(id == guid);
            states.push_back(state);
        });
        const auto send = [&](bool create, uint32_t state) {
            wowee::network::Packet packet(wireOpcode(Opcode::SMSG_UPDATE_OBJECT));
            packet.writeUInt32(1); packet.writeUInt8(0);
            packet.writeUInt8(static_cast<uint8_t>(create ? UpdateType::CREATE_OBJECT2 : UpdateType::VALUES));
            packet.writePackedGuid(guid);
            if (create) {
                packet.writeUInt8(static_cast<uint8_t>(ObjectType::GAMEOBJECT));
                packet.writeUInt8(0); // No movement payload.
            }
            // Independent wire oracle: STATE=14. CREATE omits zero-valued fields.
            const bool present = !create || state != 0;
            packet.writeUInt8(1);
            packet.writeUInt32(present ? 1u << 14 : 0u);
            if (present) packet.writeUInt32(state);
            session.receive(packet);
        };
        send(true, initialState);
        REQUIRE(states.size() == 1);
        CHECK(states.back() == initialState);
        send(false, 1u - initialState);
        REQUIRE(states.size() == 2);
        CHECK(states.back() == 1u - initialState);
        session.handler().setReadingGameObject(guid);
        REQUIRE(states.size() == 3);
        CHECK(states.back() == 0);
        session.handler().clearBook();
        REQUIRE(states.size() == 4);
        CHECK(states.back() == 1u - initialState);
    }
}

namespace {
struct Integration : Fixture {
    addons::LuaEngine lua;
    Integration() {
        REQUIRE(lua.initialize());
        // Missing APIs must fail, not turn a broken test into a no-op success.
        run("setmetatable(_G, nil); __WoweeBindingScripts = {}; __WoweeBindingRunOnUp = {}");
        ui::setTypedInputProbe([this] { return lua.editBoxHasFocus(); });
    }
    ~Integration() {
        lua.releaseInput();
        ui::setTypedInputProbe({});
        core::Input::getInstance().clearVirtualKeys();
        core::Input::getInstance().update();
    }
    void run(const std::string& code) {
        const bool ok = lua.executeString(code);
        INFO(lua.lastError());
        REQUIRE(ok);
    }
    void truth(const std::string& expression) {
        INFO(expression);
        REQUIRE(lua.evaluateBoolean(expression));
    }
    void layout() { lua.widgets().layout(800, 600); }
};

// Feed SDL's real joystick state, not synthetic button events: Gamepad polls
// SDL_GetGamepadButton/Axis. The tests fail if virtual-device setup is broken.
struct VirtualPad {
    SDL_JoystickID id = 0;
    SDL_Joystick* joystick = nullptr;
    std::optional<std::string> oldBackground;
    float previousDeadzone = core::gamepad().stickDeadzone();
    bool initialized = false;
    VirtualPad() {
        if (const char* old = SDL_GetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS)) oldBackground = old;
    }
    // Initialize after construction so failed REQUIREs still unwind this
    // member and restore SDL resources/hints before Catch runs another case.
    void initialize() {
        REQUIRE(SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1"));
        initialized = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
        REQUIRE(initialized);
        SDL_VirtualJoystickDesc desc{};
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.name = "WoWee integration virtual controller";
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
        desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
        id = SDL_AttachVirtualJoystick(&desc);
        INFO(SDL_GetError());
        REQUIRE(id != 0);
        REQUIRE(SDL_IsGamepad(id));
        joystick = SDL_OpenJoystick(id);
        REQUIRE(joystick != nullptr);
        // Select this id explicitly, even when a physical pad is plugged in.
        core::gamepad().shutdown();
        SDL_Event added{}; added.type = SDL_EVENT_GAMEPAD_ADDED; added.cdevice.which = id;
        core::gamepad().handleEvent(added);
        REQUIRE(core::gamepad().isConnected());
        REQUIRE(core::gamepad().name() == desc.name);
        core::gamepad().setStickDeadzone(0.0f);
        SDL_UpdateJoysticks();
        core::gamepad().update();
    }
    ~VirtualPad() {
        core::gamepad().shutdown();
        core::gamepad().setStickDeadzone(previousDeadzone);
        if (joystick) SDL_CloseJoystick(joystick);
        if (id) SDL_DetachVirtualJoystick(id);
        if (initialized) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        if (oldBackground) SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, oldBackground->c_str());
        else SDL_ResetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS);
    }
    void button(SDL_GamepadButton button, bool down) {
        INFO(SDL_GetError());
        REQUIRE(SDL_SetJoystickVirtualButton(joystick, button, down));
    }
    void axis(SDL_GamepadAxis axis, float value) {
        REQUIRE(SDL_SetJoystickVirtualAxis(joystick, axis, static_cast<Sint16>(value * 32767)));
    }
    void sample() { SDL_UpdateJoysticks(); core::gamepad().update(); }
};
struct PadIntegration : Integration {
    VirtualPad pad;
    ui::GamepadControls controls;
    PadIntegration() {
        pad.initialize();
        controls.setInWorld(true);
        controls.setKeyRouter([this](const char* key, bool down) {
            const auto outcome = lua.dispatchPadKey(key, down);
            ui::PadKeyAnswer answer;
            answer.command = outcome.command;
            answer.kind = !outcome.command.empty() ? ui::PadKeyAnswer::Kind::Command :
                outcome.taken ? ui::PadKeyAnswer::Kind::Taken : ui::PadKeyAnswer::Kind::Unbound;
            return answer;
        });
    }
    ~PadIntegration() { controls.reset(); }
    void pump() { manager.beginInputFrame(); pad.sample(); controls.update(1.0f / 60); }
    void press(SDL_GamepadButton button, bool down = true) { pad.button(button, down); pump(); }
};
} // namespace

TEST_CASE_METHOD(Integration, "Real Lua binding scripts latch their command and release function", "[input][lua][O4]") {
    REQUIRE(manager.setBinding("BUTTON2", "TEST_RIGHT"));
    REQUIRE(manager.setBinding("BUTTON3", "TEST_MIDDLE"));
    run(R"lua(
        calls = {}
        __WoweeBindingScripts.TEST_RIGHT = function(down) table.insert(calls, down == 'down' and 'right-down' or 'right-up') end
        __WoweeBindingScripts.TEST_MIDDLE = function(down) table.insert(calls, down == 'down' and 'middle-down' or 'middle-up') end
        __WoweeBindingRunOnUp.TEST_RIGHT = true
        __WoweeBindingRunOnUp.TEST_MIDDLE = true
    )lua");
    REQUIRE(lua.dispatchBindingInput("BUTTON2", false, false, false, true));
    REQUIRE(lua.dispatchBindingInput("BUTTON2", false, false, false, true));
    REQUIRE(manager.setBinding("BUTTON2", "TEST_MIDDLE"));
    run("__WoweeBindingScripts.TEST_RIGHT = function() error('new function must not receive old release') end");
    REQUIRE(lua.dispatchBindingInput("BUTTON2", true, true, false, false));
    truth("#calls == 2 and calls[1] == 'right-down' and calls[2] == 'right-up'");
    REQUIRE(lua.dispatchBindingInput("BUTTON3", false, false, false, true));
    lua.releaseInput();
    truth("#calls == 4 and calls[3] == 'middle-down' and calls[4] == 'middle-up'");
    CHECK_FALSE(lua.dispatchBindingInput("BUTTON3", false, false, false, false));
}

TEST_CASE_METHOD(Integration, "Keyboard frames use layout keys while binding capture uses physical keys", "[input][lua][O7]") {
    run(R"lua(
        dialog = CreateFrame('Frame', 'LayoutDialog', UIParent)
        dialog:SetSize(300, 200); dialog:SetPoint('CENTER')
        dialog:EnableKeyboard(true)
        dialog:SetScript('OnKeyDown', function(self, key) lastKey = key end)
        dialog:Show()
    )lua");
    layout();
    REQUIRE(lua.keyboardFrameBlocksInput());
    REQUIRE(lua.dispatchFrameKey(SDLK_Z, true, SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_W)));
    truth("lastKey == 'Z'");
    lua.dispatchFrameKey(SDLK_Z, false, SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_W));
    run("dialog:SetPropagateKeyboardInput(true)");
    CHECK_FALSE(lua.keyboardFrameBlocksInput());
    CHECK_FALSE(lua.dispatchFrameKey(SDLK_Y, true));
    truth("lastKey == 'Y'");
    lua.dispatchFrameKey(SDLK_Y, false);
    run("dialog:Hide()"); layout();
    CHECK_FALSE(lua.dispatchFrameKey(SDLK_C, true));
    run(R"lua(
        capture = CreateFrame('Frame', 'KeyBindingFrame', UIParent)
        capture:SetSize(300, 200); capture:SetPoint('CENTER'); capture:EnableKeyboard(true)
        capture:SetScript('OnKeyDown', function(self, key) lastKey = key end)
        capture:Show()
    )lua");
    layout();
    REQUIRE(lua.dispatchFrameKey(SDLK_Z, true, SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_W)));
    truth("lastKey == 'W'");
    lua.dispatchFrameKey(SDLK_Z, false, SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_W));
    run("capture:EnableKeyboard(false)"); layout();
    CHECK_FALSE(lua.keyboardFrameBlocksInput());
    CHECK_FALSE(lua.dispatchFrameKey(SDLK_C, true));
}

TEST_CASE_METHOD(Integration, "Real edit boxes accept layout text and Return or keypad Enter", "[input][lua][O7][O9]") {
    run(R"lua(
        edit = CreateFrame('EditBox', 'IntegrationChat', UIParent)
        edit:SetAutoFocus(false); edit:SetSize(300, 30); edit:SetPoint('CENTER'); edit:Show()
        sent = 0
        edit:SetScript('OnEnterPressed', function(self) sent = sent + 1; message = self:GetText(); self:ClearFocus() end)
    )lua");
    layout();
    for (auto code : {SDLK_RETURN, SDLK_KP_ENTER}) {
        run("edit:SetText(''); edit:SetFocus()");
        REQUIRE(lua.editBoxHasFocus());
        lua.dispatchText("/say azerty qwertz");
        lua.dispatchKey(code, false);
        CHECK_FALSE(lua.editBoxHasFocus());
        truth("message == '/say azerty qwertz'");
    }
    truth("sent == 2");
}

TEST_CASE_METHOD(Integration, "Real wheel handlers own fractional UI travel and camera keeps fractional zoom", "[input][lua][camera][O8]") {
    run(R"lua(
        scroll = CreateFrame('Frame', 'IntegrationWheel', UIParent)
        scroll:SetSize(400, 300); scroll:SetPoint('CENTER'); scroll:EnableMouseWheel(true); scroll:Show()
        wheel = 0; scroll:SetScript('OnMouseWheel', function(self, delta) wheel = wheel + delta end)
    )lua");
    layout();
    for (int i = 0; i < 8; ++i) REQUIRE(lua.dispatchMouseWheel(400, 300, 0.125f));
    truth("wheel == 1");
    REQUIRE(lua.dispatchMouseWheel(400, 300, -0.125f));
    truth("wheel == 1");
    run("scroll:Hide()"); layout();
    CHECK_FALSE(lua.dispatchMouseWheel(400, 300, 0.125f));
    rendering::Camera camera;
    rendering::CameraController controller(&camera);
    glm::vec3 target(0);
    controller.setFollowTarget(&target);
    controller.setIdleOrbitEnabled(false);
    controller.setGravityDisabled(true);
    controller.update(0.05f);
    const auto before = camera.getPosition();
    controller.processMouseWheel(0.125f);
    controller.update(0.05f);
    // This observes the actual camera, not a duplicated zoom formula.
    CHECK(glm::length(camera.getPosition() - before) > 0.0001f);
}

// Build 8606 measurements: _experiments/20261118T140302.014047Z.faux_scroll_api
// (probe*.txt typed into the 2.4.3 client's chat, probe*_original.png).
TEST_CASE_METHOD(Integration, "Sliders, status bars and scroll frames hold values as 2.4.3 does",
                 "[lua][scroll][faux]") {
    run(R"lua(
        s = CreateFrame('Slider', nil, UIParent)
        changed = 0
        s:SetScript('OnValueChanged', function() changed = changed + 1 end)
        function settle(lo, hi, list)
            s:SetMinMaxValues(lo, hi)
            local out = {}
            for _, v in ipairs(list) do s:SetValue(v); out[#out + 1] = s:GetValue() end
            return table.concat(out, ' ')
        end
    )lua");
    // A new slider spans 0..0.
    truth("select(2, s:GetMinMaxValues()) == 0");
    // Clamped, without a step; one handler run per real change.
    truth("settle(0, 32, {50}) == '32' and changed == 1");
    run("s:SetValue(50)");
    truth("changed == 1");
    truth("settle(0, 32, {-5}) == '0' and changed == 2");
    // On a step counted from the minimum; the last whole step is the maximum.
    run("s:SetValueStep(16)");
    truth("settle(0, 50, {41, 43, 30, 8, 7, 24, 25, 49}) == '50 50 32 16 0 32 32 50'");
    truth("settle(3, 50, {41, 43, 30, 8, 7, 24, 25, 49}) == '50 50 50 3 3 19 19 50'");
    truth("settle(0, 10, {10, 9.9}) == '0 0'");
    run("s:SetValueStep(4)");
    truth("settle(0, 10, {0, 1, 2, 3, 5, 6, 7, 10}) == '0 0 4 4 4 10 10 10'");
    // A new range or step moves the value onto it, and says so.
    run("s:SetValueStep(16); s:SetMinMaxValues(0, 100); s:SetValue(32); changed = 0; s:SetMinMaxValues(0, 40)");
    truth("s:GetValue() == 40 and changed == 1");
    run("s:SetValueStep(0); s:SetMinMaxValues(0, 100); s:SetValue(33); changed = 0; s:SetValueStep(16)");
    truth("s:GetValue() == 32 and changed == 1");

    // Until SetValue first moves it, a new range or step leaves the value
    // alone and says nothing. BlizzardOptionsPanel_OnLoad gives Mouse Look
    // Speed 90..270 before SetValue(GetCVar(...)); its OnValueChanged writes
    // the CVar, so a clamp here rewrote the saved 180 as 90 at every start.
    run(R"lua(
        look = CreateFrame('Slider', nil, UIParent)
        written = {}
        look:SetScript('OnValueChanged', function(self, value) written[#written + 1] = value end)
        look:SetValueStep(10); look:SetMinMaxValues(90, 270)
        fresh = look:GetValue()
        look:SetValue(180)
        back = CreateFrame('Slider', nil, UIParent)
        back:SetMinMaxValues(0, 10); back:SetValue(5); back:SetValue(0); back:SetMinMaxValues(90, 270)
        same = CreateFrame('Slider', nil, UIParent)
        same:SetValue(0); same:SetMinMaxValues(90, 270)
    )lua");
    truth("fresh == 0 and #written == 1 and written[1] == 180 and look:GetValue() == 180");
    truth("back:GetValue() == 90 and same:GetValue() == 0");

    // A status bar is clamped too, on SetValue and on a new range.
    run(R"lua(
        bar = CreateFrame('StatusBar', nil, UIParent)
        bar:SetMinMaxValues(0, 10); bar:SetValue(20)
        high = bar:GetValue(); bar:SetValue(-3); low = bar:GetValue()
        bar:SetValue(7); barChanged = 0
        bar:SetScript('OnValueChanged', function() barChanged = barChanged + 1 end)
        bar:SetMinMaxValues(0, 5)
    )lua");
    run("fresh = CreateFrame('StatusBar', nil, UIParent); fresh:SetMinMaxValues(90, 270)");
    truth("fresh:GetValue() == 0");
    truth("high == 10 and low == 0 and bar:GetValue() == 5 and barChanged == 1");

    // A scroll frame keeps the offset it is given, past its range either way.
    run(R"lua(
        sf = CreateFrame('ScrollFrame', nil, UIParent)
        sf:SetWidth(100); sf:SetHeight(100); sf:SetPoint('CENTER')
        child = CreateFrame('Frame', nil, sf); child:SetWidth(100); child:SetHeight(50)
        sf:SetScrollChild(child)
        scrolled = 0
        sf:SetScript('OnVerticalScroll', function(self, value) scrolled = scrolled + 1; last = value end)
        sf:SetVerticalScroll(32); sf:SetVerticalScroll(32)
    )lua");
    truth("sf:GetVerticalScrollRange() == 0 and sf:GetVerticalScroll() == 32 and scrolled == 1 and last == 32");
    run("sf:SetVerticalScroll(-7)");
    truth("sf:GetVerticalScroll() == -7 and scrolled == 2");

    // FauxScrollFrame's arithmetic on MangAdmin's teleport list: 20 rows of
    // 16 in a frame 225 tall, 12 shown. The bar runs to 128 and the real
    // range is 95; the end of the bar has to reach row 8, the last offset.
    run(R"lua(
        list = CreateFrame('ScrollFrame', nil, UIParent)
        list:SetWidth(224); list:SetHeight(225); list:SetPoint('CENTER')
        rows = CreateFrame('Frame', nil, list); rows:SetWidth(224); rows:SetHeight(20 * 16)
        list:SetScrollChild(rows)
        list.offset = 0
        listBar = CreateFrame('Slider', nil, list)
        listBar:SetScript('OnValueChanged', function(self, value) list:SetVerticalScroll(value) end)
        list:SetScript('OnVerticalScroll', function(self, value)
            self.offset = math.floor(value / 16 + 0.5)
        end)
        listBar:SetMinMaxValues(0, (20 - 12) * 16); listBar:SetValueStep(16)
        listBar:SetValue(0 + 193 / 2)   -- ScrollFrameTemplate_OnMouseWheel: half the bar
    )lua");
    truth("listBar:GetValue() == 96 and list.offset == 6");
    run("listBar:SetValue(listBar:GetValue() + 193 / 2)");
    truth("listBar:GetValue() == 128 and list.offset == 8");

    // The wheel over a row laid on the list - a sibling button taking the
    // mouse and not the wheel - reaches the scroll frame below it.
    run(R"lua(
        wheelFrame = CreateFrame('Frame', nil, UIParent)
        wheelFrame:SetWidth(300); wheelFrame:SetHeight(300); wheelFrame:SetPoint('CENTER')
        wheelFrame:EnableMouseWheel(true)
        wheeled = 0; wheelFrame:SetScript('OnMouseWheel', function() wheeled = wheeled + 1 end)
        row = CreateFrame('Button', nil, UIParent)
        row:SetWidth(300); row:SetHeight(300); row:SetPoint('CENTER'); row:SetFrameStrata('HIGH')
        row:EnableMouse(true)
    )lua");
    layout();
    REQUIRE(lua.dispatchMouseWheel(400, 300, 1.0f));
    truth("wheeled == 1");
}

TEST_CASE_METHOD(Integration, "$parent in an anchor deep inside unnamed frames names the template's frame",
                 "[lua][framexml][anchor]") {
    // PartyMemberFrameTemplate's shape: the dispellable-debuff ring sits in a
    // Layer two unnamed frames deep and anchors CENTER to $parentPortrait,
    // which on 2.4.3 is centred on the portrait.
    ui::XmlNode root;
    std::string error;
    REQUIRE(ui::parseXml(R"xml(<Ui>
        <Button name="RingTemplate" virtual="true">
          <Size><AbsDimension x="128" y="53"/></Size>
          <Layers><Layer level="BACKGROUND">
            <Texture name="$parentPortrait">
              <Size><AbsDimension x="37" y="37"/></Size>
              <Anchors><Anchor point="TOPLEFT"><Offset><AbsDimension x="7" y="-6"/></Offset></Anchor></Anchors>
            </Texture>
          </Layer></Layers>
          <Frames><Frame setAllPoints="true"><Frames><Frame setAllPoints="true"><Frames>
            <Frame name="$parentBadge"><Size><AbsDimension x="8" y="8"/></Size>
              <Anchors><Anchor point="TOPLEFT" relativeTo="$parentPortrait" relativePoint="BOTTOMRIGHT"/></Anchors>
            </Frame>
          </Frames><Layers><Layer level="ARTWORK">
            <Texture name="$parentStatus">
              <Size><AbsDimension x="36" y="36"/></Size>
              <Anchors><Anchor point="CENTER" relativeTo="$parentPortrait"/></Anchors>
            </Texture>
          </Layer></Layers></Frame></Frames></Frame></Frames>
        </Button>
        <Button name="RingFrame1" inherits="RingTemplate" parent="UIParent">
          <Anchors><Anchor point="TOPLEFT"><Offset><AbsDimension x="100" y="-100"/></Offset></Anchor></Anchors>
        </Button>
    </Ui>)xml", root, error));
    run(ui::emitFrameXml(root).lua);
    layout();
    truth("select(2, RingFrame1Status:GetPoint(1)) == RingFrame1Portrait");
    truth("math.abs(RingFrame1Status:GetLeft() - (RingFrame1Portrait:GetLeft() + 0.5)) < 0.01");
    truth("math.abs(RingFrame1Status:GetTop() - (RingFrame1Portrait:GetTop() - 0.5)) < 0.01");
    // A named frame in there gets its name, and its anchors, from the same frame.
    truth("RingFrame1Badge ~= nil and select(2, RingFrame1Badge:GetPoint(1)) == RingFrame1Portrait");
}

TEST_CASE_METHOD(Integration, "SetPoint with a nil relative frame measures from the screen",
                 "[lua][anchor]") {
    // Read off 2.4.3 (workspace _experiments/20261119T0935Z.raidinfo_clip, NP()):
    // a 10x10 child of a 50x50 frame at (300, 200).
    run(R"lua(
        local p = CreateFrame("Frame", "NilAnchorParent", UIParent)
        p:SetPoint("BOTTOMLEFT", UIParent, "BOTTOMLEFT", 300, 200); p:SetWidth(50); p:SetHeight(50)
        local c = CreateFrame("Frame", "NilAnchorChild", p)
        c:SetWidth(10); c:SetHeight(10)
    )lua");
    run("NilAnchorChild:ClearAllPoints() NilAnchorChild:SetPoint('CENTER', nil, 'BOTTOMLEFT', 100, 100)");
    layout();
    truth("select(1, NilAnchorChild:GetCenter()) == 100 and select(2, NilAnchorChild:GetCenter()) == 100");
    truth("select(2, NilAnchorChild:GetPoint(1)) == nil");
    // Left out altogether it is the parent.
    run("NilAnchorChild:ClearAllPoints() NilAnchorChild:SetPoint('BOTTOMLEFT', 100, 100)");
    layout();
    truth("select(1, NilAnchorChild:GetCenter()) == 405 and select(2, NilAnchorChild:GetCenter()) == 305");
    run("NilAnchorChild:ClearAllPoints() NilAnchorChild:SetPoint('CENTER')");
    layout();
    truth("select(1, NilAnchorChild:GetCenter()) == 325 and select(2, NilAnchorChild:GetCenter()) == 225");
}

TEST_CASE_METHOD(Integration, "ClearAllPoints leaves a frame where it was until it is anchored again",
                 "[lua][anchor]") {
    // 2.4.3: a 20x20 frame at (100, 100) answers GetCenter 110, 110 both
    // before and after ClearAllPoints (workspace _experiments/20261119T0935Z.raidinfo_clip).
    run(R"lua(
        local f = CreateFrame("Frame", "HeldRectFrame", UIParent)
        f:SetWidth(20); f:SetHeight(20)
        f:SetPoint("BOTTOMLEFT", UIParent, "BOTTOMLEFT", 100, 100)
    )lua");
    layout();
    run("HeldRectFrame:ClearAllPoints()");
    truth("select(1, HeldRectFrame:GetCenter()) == 110 and select(2, HeldRectFrame:GetCenter()) == 110");
    truth("HeldRectFrame:GetNumPoints() == 0");
    layout();
    truth("select(1, HeldRectFrame:GetCenter()) == 110 and HeldRectFrame:GetTop() == 120");
    // RaidPulloutStopMoving's sequence: re-anchored where it was held.
    run("local x = HeldRectFrame:GetCenter() HeldRectFrame:SetPoint('TOP', nil, 'BOTTOMLEFT', x, HeldRectFrame:GetTop())");
    layout();
    truth("select(1, HeldRectFrame:GetCenter()) == 110 and HeldRectFrame:GetTop() == 120");
}

// Build 8606 measurements: _experiments/20261117T202346.492175Z.camera_max_distance.
TEST_CASE_METHOD(Integration, "Camera zoom follows the original's distance CVars", "[camera][zoom]") {
    rendering::Camera camera;
    rendering::CameraController controller(&camera);
    glm::vec3 target(0);
    controller.setFollowTarget(&target);
    controller.setIdleOrbitEnabled(false);
    controller.setGravityDisabled(true);
    const auto settle = [&] { for (int i = 0; i < 2000; ++i) controller.update(0.01f); };
    const auto zoomOutFully = [&] { controller.processMouseWheel(-100.0f); settle(); };

    zoomOutFully();
    CHECK(controller.zoomDistance() == Catch::Approx(15.0f));
    controller.setDistanceMaxFactor(2.0f);
    zoomOutFully();
    CHECK(controller.zoomDistance() == Catch::Approx(30.0f));
    controller.setDistanceMax(30.0f);
    zoomOutFully();
    CHECK(controller.zoomDistance() == Catch::Approx(50.0f));
    // Lowering the limit leaves the camera until the next zoom.
    controller.setDistanceMax(15.0f);
    controller.setDistanceMaxFactor(1.0f);
    settle();
    CHECK(controller.zoomDistance() == Catch::Approx(50.0f));
    controller.processMouseWheel(-1.0f);
    controller.update(0.01f);
    CHECK(controller.zoomDistance() == Catch::Approx(15.0f));

    // One notch is one yard at cameraDistanceMoveSpeed, 8.33 yards a second.
    controller.processMouseWheel(1.0f);
    controller.update(0.01f);
    CHECK(controller.zoomDistance() == Catch::Approx(15.0f - 0.0833f));
    settle();
    CHECK(controller.zoomDistance() == Catch::Approx(14.0f).margin(0.0833f));
    // Notches the same way add up; one the other way replaces them.
    const float before = controller.zoomDistance();
    controller.processMouseWheel(-1.0f);
    controller.processMouseWheel(-1.0f);
    controller.processMouseWheel(1.0f);
    settle();
    CHECK(controller.zoomDistance() == Catch::Approx(before - 1.0f).margin(0.0833f));
}

// Build 8606 (pair 4, Shattrath): SetView 1-5 = 0/0, 5.55/-10, 5.55/-20,
// 13.88/-30, 13.88/-10; End/Home step without wrapping; SaveView keeps the
// current distance and pitch; ResetView of the current view applies it.
TEST_CASE_METHOD(Integration, "Camera views follow the original's SetView, SaveView and ResetView", "[camera][views]") {
    rendering::Camera camera;
    rendering::CameraController controller(&camera);
    glm::vec3 target(0);
    controller.setFollowTarget(&target);
    controller.setIdleOrbitEnabled(false);
    controller.setGravityDisabled(true);
    const auto settle = [&] { for (int i = 0; i < 2000; ++i) controller.update(0.01f); };
    const float expected[][2] = {{0, 0}, {0.0f, 0.0f}, {5.55f, -10.0f}, {5.55f, -20.0f}, {13.88f, -30.0f}, {13.88f, -10.0f}};
    for (int view = 1; view <= 5; ++view) {
        controller.setView(view);
        settle();
        CHECK(controller.zoomDistance() == Catch::Approx(expected[view][0]).margin(0.01f));
        CHECK(controller.getPitch() == Catch::Approx(expected[view][1]));
    }
    controller.nextView();
    CHECK(controller.currentView() == 5);
    for (int i = 0; i < 6; ++i) controller.prevView();
    CHECK(controller.currentView() == 1);
    settle();
    CHECK(controller.zoomDistance() == Catch::Approx(0.0f).margin(0.01f));
    controller.setView(2);
    controller.processMouseWheel(3.0f);
    settle();
    const float zoomed = controller.zoomDistance();
    CHECK(zoomed < 5.0f);
    controller.saveView(2);
    controller.setView(3);
    controller.setView(2);
    settle();
    CHECK(controller.zoomDistance() == Catch::Approx(zoomed).margin(0.01f));
    controller.resetView(2);
    settle();
    CHECK(controller.zoomDistance() == Catch::Approx(5.55f).margin(0.01f));
}

TEST_CASE_METHOD(Integration, "Camera collision pulls in at once and eases back over two seconds", "[camera][collision]") {
    rendering::Camera camera;
    rendering::CameraController controller(&camera);
    glm::vec3 target(0);
    controller.setFollowTarget(&target);
    controller.setIdleOrbitEnabled(false);
    controller.setGravityDisabled(true);
    controller.processMouseWheel(-100.0f);
    for (int i = 0; i < 2000; ++i) controller.update(0.01f);
    REQUIRE(controller.zoomDistance() == Catch::Approx(15.0f));

    CHECK(controller.easedCollisionLimit(5.0f, 0.01f) == 5.0f);
    // Half way through, half a cosine is half way back.
    for (int i = 0; i < 99; ++i) controller.easedCollisionLimit(std::nullopt, 0.01f);
    CHECK(controller.easedCollisionLimit(std::nullopt, 0.01f) == Catch::Approx(10.0f).margin(0.01f));
    // Geometry still in the way caps the ease without restarting it.
    CHECK(controller.easedCollisionLimit(12.0f, 0.01f) == Catch::Approx(10.0785f).margin(0.001f));
    for (int i = 0; i < 98; ++i) controller.easedCollisionLimit(std::nullopt, 0.01f);
    CHECK(controller.easedCollisionLimit(std::nullopt, 0.01f) >= 14.99f);
    CHECK(controller.easedCollisionLimit(std::nullopt, 0.01f) > 1000.0f);
    // Held against a wall the ease waits; it starts when the wall goes.
    for (int i = 0; i < 300; ++i) controller.easedCollisionLimit(4.0f, 0.01f);
    CHECK(controller.easedCollisionLimit(std::nullopt, 0.01f) < 4.01f);
    // Moving back into a wall pulls in again on that frame.
    CHECK(controller.easedCollisionLimit(3.0f, 0.01f) == 3.0f);
}

TEST_CASE_METHOD(Integration, "Real camera applies temporary Ctrl walking and a rebindable toggle", "[input][camera][O6]") {
    rendering::Camera camera;
    rendering::CameraController controller(&camera);
    controller.setIdleOrbitEnabled(false);
    controller.setFollowTarget(nullptr);
    controller.setUseWoWSpeed(true);
    camera.setPosition({0, 0, 0}); // Measure speed without large-world float quantization.
    auto& input = core::Input::getInstance();
    const auto speed = [&] {
        const auto before = camera.getPosition();
        input.update(); controller.update(0.01f);
        const auto change = camera.getPosition() - before;
        return glm::length(glm::vec2(change.x, change.y)) / 0.01f;
    };
    event(SDL_SCANCODE_W);
    CHECK(speed() == Catch::Approx(7.0f).margin(0.01));
    for (auto ctrl : {SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL}) {
        input.setVirtualKey(ctrl, true);
        CHECK(speed() == Catch::Approx(2.5f).margin(0.01));
        input.setVirtualKey(ctrl, false);
        CHECK(speed() == Catch::Approx(7.0f).margin(0.01));
    }
    event(SDL_SCANCODE_KP_DIVIDE);
    CHECK(speed() == Catch::Approx(2.5f).margin(0.01));
    manager.beginInputFrame(); event(SDL_SCANCODE_KP_DIVIDE, true, SDL_KMOD_NONE, true);
    CHECK(speed() == Catch::Approx(2.5f).margin(0.01));
    event(SDL_SCANCODE_KP_DIVIDE, false);
    manager.beginInputFrame(); event(SDL_SCANCODE_KP_DIVIDE);
    CHECK(speed() == Catch::Approx(7.0f).margin(0.01));
    manager.beginInputFrame(); event(SDL_SCANCODE_KP_DIVIDE, false);
    controller.setRunSpeedOverride(10); controller.setWalkSpeedOverride(3);
    CHECK(speed() == Catch::Approx(10.0f).margin(0.01));
    input.setVirtualKey(SDL_SCANCODE_LCTRL, true);
    CHECK(speed() == Catch::Approx(3.0f).margin(0.01));
    input.setVirtualKey(SDL_SCANCODE_LCTRL, false);
    REQUIRE(manager.setBinding("CTRL-W", "ADDON_CUSTOM"));
    event(SDL_SCANCODE_W, false); event(SDL_SCANCODE_W, true, SDL_KMOD_CTRL);
    CHECK(speed() == Catch::Approx(0).margin(0.01));
    event(SDL_SCANCODE_W, false);
    REQUIRE(manager.setBinding("CTRL-F8", "TOGGLERUN"));
    manager.beginInputFrame(); event(SDL_SCANCODE_F8, true, SDL_KMOD_CTRL); event(SDL_SCANCODE_F8, false);
    controller.update(0);
    manager.beginInputFrame(); event(SDL_SCANCODE_W);
    CHECK(speed() == Catch::Approx(3.0f).margin(0.01));
    event(SDL_SCANCODE_W, false); event(SDL_SCANCODE_S);
    controller.setRunBackSpeedOverride(4);
    CHECK(speed() == Catch::Approx(4.0f).margin(0.01));
    event(SDL_SCANCODE_S, false); event(SDL_SCANCODE_W);
    manager.beginInputFrame();
    controller.setUseWoWSpeed(false);
    CHECK(speed() == Catch::Approx(50.0f).margin(0.01));
    input.setVirtualKey(SDL_SCANCODE_LCTRL, true);
    CHECK(speed() == Catch::Approx(15.0f).margin(0.01));
    input.setVirtualKey(SDL_SCANCODE_LCTRL, false);
    input.setVirtualKey(SDL_SCANCODE_LSHIFT, true);
    CHECK(speed() == Catch::Approx(150.0f).margin(0.01));
    input.setVirtualKey(SDL_SCANCODE_LCTRL, true);
    CHECK(speed() == Catch::Approx(45.0f).margin(0.01));
}

TEST_CASE_METHOD(Integration, "Local stop transitions to Stand on release frame and direction switches stay continuous", "[input][camera][locomotion]") {
    rendering::Camera camera;
    rendering::CameraController controller(&camera);
    controller.setIdleOrbitEnabled(false);
    controller.setFollowTarget(nullptr);
    controller.setUseWoWSpeed(true);
    camera.setPosition({0, 0, 0});

    rendering::LocomotionFSM fsm;
    rendering::AnimCapabilitySet caps;
    caps.resolvedStand = rendering::anim::STAND;
    caps.resolvedWalk = rendering::anim::WALK;
    caps.resolvedRun = rendering::anim::RUN;
    caps.resolvedWalkBackwards = rendering::anim::WALK_BACKWARDS;

    auto& input = core::Input::getInstance();
    constexpr float dt60 = 1.0f / 60.0f;
    const auto step = [&]() -> rendering::AnimOutput {
        input.update();
        controller.update(dt60);
        rendering::LocomotionFSM::Input in;
        in.moving = controller.isMoving();
        in.sprinting = controller.isSprinting();
        in.movingForward = controller.isMovingForward();
        in.movingBackward = controller.isMovingBackward();
        in.strafeLeft = controller.isStrafingLeft();
        in.strafeRight = controller.isStrafingRight();
        in.grounded = true;
        in.deltaTime = dt60;
        return fsm.resolve(in, caps);
    };

    // W -> Stop transitions Run -> Stand on the exact release frame
    event(SDL_SCANCODE_W, true);
    CHECK(step().animId == rendering::anim::RUN);
    event(SDL_SCANCODE_W, false);
    CHECK(step().animId == rendering::anim::STAND);
    CHECK(fsm.getState() == rendering::LocomotionFSM::State::IDLE);

    // W -> S reversal transitions Run -> WalkBackwards without any Stand flash
    event(SDL_SCANCODE_W, true);
    CHECK(step().animId == rendering::anim::RUN);
    event(SDL_SCANCODE_W, false);
    event(SDL_SCANCODE_S, true);
    CHECK(step().animId == rendering::anim::WALK_BACKWARDS);
    CHECK(fsm.getState() == rendering::LocomotionFSM::State::WALK);
    event(SDL_SCANCODE_S, false);
    CHECK(step().animId == rendering::anim::STAND);

    // Q -> E strafe switch stays Run, then E release transitions to Stand immediately
    event(SDL_SCANCODE_Q, true);
    CHECK(step().animId == rendering::anim::RUN);
    event(SDL_SCANCODE_Q, false);
    event(SDL_SCANCODE_E, true);
    CHECK(step().animId == rendering::anim::RUN);
    event(SDL_SCANCODE_E, false);
    CHECK(step().animId == rendering::anim::STAND);
}

TEST_CASE_METHOD(Integration, "Real camera sitting preserves queued taps and wheel command edges", "[input][camera][sit]") {
    rendering::Camera camera;
    rendering::CameraController controller(&camera);
    glm::vec3 target(0);
    controller.setIdleOrbitEnabled(false);
    controller.setFollowTarget(&target);
    controller.setGravityDisabled(true);
    CHECK_FALSE(controller.isSitting());
    // Both transitions precede the consumer: held state is already false.
    event(SDL_SCANCODE_X); event(SDL_SCANCODE_X, false);
    REQUIRE(manager.isCommandPressed("SITORSTAND"));
    REQUIRE_FALSE(manager.isCommandDown("SITORSTAND"));
    controller.update(0);
    CHECK(controller.isSitting());
    manager.beginInputFrame(); controller.update(0);
    CHECK(controller.isSitting());
    event(SDL_SCANCODE_X); controller.update(0);
    CHECK_FALSE(controller.isSitting());
    manager.beginInputFrame(); controller.update(0);
    CHECK_FALSE(controller.isSitting()); // Holding is not another toggle.
    event(SDL_SCANCODE_X, false);
    REQUIRE(manager.setBinding("MOUSEWHEELUP", "SITORSTAND"));
    for (bool expected : {true, false, true}) {
        manager.beginInputFrame();
        // Application's wheel route pulses this real input source in one pump.
        const auto command = manager.resolveCommand("MOUSEWHEELUP");
        manager.setVirtualCommand(command, "mouse-wheel", true);
        manager.setVirtualCommand(command, "mouse-wheel", false);
        REQUIRE(manager.isCommandPressed("SITORSTAND"));
        REQUIRE_FALSE(manager.isCommandDown("SITORSTAND"));
        controller.update(0);
        CHECK(controller.isSitting() == expected);
        manager.beginInputFrame(); controller.update(0);
        CHECK(controller.isSitting() == expected);
    }
}

TEST_CASE("Right button turns the character only once the mouse turns the view", "[input][camera][facing]") {
    using rendering::CameraController;
    // A right-click with no drag - targeting a unit beside or behind the
    // player - leaves the facing alone (build 8606 syncs it in the mouse-move
    // handler), so the server keeps judging melee by the old heading.
    CHECK_FALSE(CameraController::cameraDrivesFacing(true, false, false, false, false));
    // Dragging with the right button held turns the character with the view.
    CHECK(CameraController::cameraDrivesFacing(true, true, false, false, false));
    // Both buttons held (mouse-run) turn it on the press itself.
    CHECK(CameraController::cameraDrivesFacing(true, false, true, false, false));
    // A left-button orbit never turns it; a steering finger always does.
    CHECK_FALSE(CameraController::cameraDrivesFacing(false, true, false, false, false));
    CHECK(CameraController::cameraDrivesFacing(false, false, false, true, false));
    // A chair owns the heading.
    CHECK_FALSE(CameraController::cameraDrivesFacing(true, true, true, true, true));
}

TEST_CASE_METHOD(Integration, "R resets only real camera angles and respects capture and rebindings", "[input][camera][O6]") {
    rendering::Camera camera;
    rendering::CameraController controller(&camera);
    controller.setIdleOrbitEnabled(false);
    controller.setDefaultSpawn(glm::vec3(99), 37, -12);
    controller.setFollowTarget(nullptr);
    controller.resetAngles();
    camera.setPosition(glm::vec3(10, 20, 30));
    controller.applyLookDelta(50, 30);
    event(SDL_SCANCODE_GRAVE); controller.update(0);
    event(SDL_SCANCODE_GRAVE, false); manager.beginInputFrame();
    const auto position = camera.getPosition();
    const bool grounded = controller.isGrounded(), sitting = controller.isSitting(), autorun = controller.isAutoRunning();
    const bool firstPerson = controller.isFirstPersonView();
    event(SDL_SCANCODE_R); controller.update(0);
    CHECK(controller.getYaw() == Catch::Approx(37));
    CHECK(controller.getFacingYaw() == Catch::Approx(37));
    CHECK(controller.getPitch() == Catch::Approx(-12));
    CHECK(camera.getPosition() == position);
    CHECK(controller.isGrounded() == grounded);
    CHECK(controller.isSitting() == sitting);
    CHECK(controller.isAutoRunning() == autorun);
    CHECK(controller.isFirstPersonView() == firstPerson);
    controller.applyLookDelta(50, 30);
    const auto yaw = controller.getYaw();
    manager.beginInputFrame(); event(SDL_SCANCODE_R, true, SDL_KMOD_NONE, true); controller.update(0);
    CHECK(controller.getYaw() == yaw);
    event(SDL_SCANCODE_R, false);
    ImGui::GetIO().WantTextInput = true;
    manager.beginInputFrame(); event(SDL_SCANCODE_R); controller.update(0);
    CHECK(controller.getYaw() == yaw);
    ImGui::GetIO().WantTextInput = false; controller.update(0);
    CHECK(controller.getYaw() == yaw);
    event(SDL_SCANCODE_R, false);
    REQUIRE(manager.setBinding("CTRL-R", "ADDON_CUSTOM"));
    manager.beginInputFrame(); event(SDL_SCANCODE_R, true, SDL_KMOD_CTRL); controller.update(0);
    CHECK(controller.getYaw() == yaw);
    event(SDL_SCANCODE_R, false);
    REQUIRE(manager.setBinding("CTRL-F8", "WOWEERESETCAMERA"));
    REQUIRE(manager.saveBindings(path("reset-camera.cfg").string()));
    manager.resetToDefaults(); REQUIRE(manager.loadBindings(path("reset-camera.cfg").string()));
    manager.beginInputFrame(); event(SDL_SCANCODE_F8, true, SDL_KMOD_CTRL); controller.update(0);
    CHECK(controller.getYaw() == Catch::Approx(37));
}

TEST_CASE_METHOD(PadIntegration, "Real SDL pad defaults work immediately and LB fallback survives keyboard focus", "[input][pad][R1][O1][O2][O5]") {
    run(R"lua(
        popup = CreateFrame('Frame', 'IntegrationPopup', UIParent)
        popup:SetSize(300, 200); popup:SetPoint('CENTER'); popup:EnableKeyboard(true); popup:Show()
    )lua");
    layout();
    REQUIRE(lua.keyboardFrameBlocksInput());
    press(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    for (auto button : {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
                       SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK}) {
        const auto command = manager.commandForKey(ui::padKeyName(button));
        REQUIRE_FALSE(command.empty());
        press(button);
        CHECK(manager.isCommandPressed(command));
        CHECK(manager.isCommandDown(command));
        press(button, false);
        CHECK_FALSE(manager.isCommandDown(command));
    }
    for (const auto& row : ui::padBindings()) {
        const std::string command = row.command ? row.command : "";
        if (!command.starts_with("ACTIONBUTTON")) continue;
        press(row.button);
        CHECK(manager.isCommandPressed("MULTIACTIONBAR1BUTTON" + command.substr(12)));
        press(row.button, false);
    }
    press(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, false);
    // Explicit pad chords take precedence over the modifier fallback.
    REQUIRE(manager.setBinding("SHIFT-PAD1", "ACTIONBUTTON12"));
    press(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER); press(SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(manager.isCommandPressed("ACTIONBUTTON12"));
    press(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, false);
    CHECK(manager.isCommandDown("ACTIONBUTTON12"));
    CHECK_FALSE(manager.isCommandDown("JUMP"));
    press(SDL_GAMEPAD_BUTTON_SOUTH, false);
    CHECK_FALSE(manager.isCommandDown("ACTIONBUTTON12"));
    REQUIRE(manager.setBinding("PAD1"));
    press(SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK_FALSE(manager.isCommandDown("JUMP"));
    press(SDL_GAMEPAD_BUTTON_SOUTH, false);
    // Run the press through the actual GameScreen escape consumer. With
    // nothing open there is nothing for it to close; what it must do is take
    // the press without falling over. The Blizzard map/popup cascade still
    // requires extracted FrameXML.
    replay::Session session;
    ui::GameScreen screen;
    press(SDL_GAMEPAD_BUTTON_EAST);
    REQUIRE(manager.isCommandPressed("TOGGLEGAMEMENU"));
    ImGui::NewFrame();
    screen.render(session.handler());
    ImGui::EndFrame();
}

TEST_CASE_METHOD(PadIntegration, "Pad scripts release original bindings and reset leaves other sources alone", "[input][pad][lua]") {
    REQUIRE(manager.setBinding("PAD1", "TEST_PAD"));
    run(R"lua(
        downs = 0; ups = 0
        __WoweeBindingScripts.TEST_PAD = function(down) if down == 'down' then downs = downs + 1 else ups = ups + 1 end end
        __WoweeBindingRunOnUp.TEST_PAD = true
    )lua");
    press(SDL_GAMEPAD_BUTTON_SOUTH);
    truth("downs == 1 and ups == 0");
    pump(); truth("downs == 1");
    REQUIRE(manager.setBinding("PAD1", "JUMP"));
    press(SDL_GAMEPAD_BUTTON_SOUTH, false);
    truth("ups == 1");
    REQUIRE(manager.setBinding("PAD1", "TEST_PAD"));
    press(SDL_GAMEPAD_BUTTON_SOUTH);
    manager.setVirtualCommand("MOVEFORWARD", "touch", true);
    controls.reset();
    truth("downs == 2 and ups == 2");
    CHECK(manager.isCommandDown("MOVEFORWARD"));
    manager.setVirtualCommand("MOVEFORWARD", "touch", false);
    press(SDL_GAMEPAD_BUTTON_SOUTH, false);
    pad.axis(SDL_GAMEPAD_AXIS_LEFTY, -0.31f); pump();
    CHECK(manager.isCommandDown("MOVEFORWARD"));
    pad.axis(SDL_GAMEPAD_AXIS_LEFTY, -0.25f); pump();
    CHECK(manager.isCommandDown("MOVEFORWARD"));
    pad.axis(SDL_GAMEPAD_AXIS_LEFTY, -0.19f); pump();
    CHECK_FALSE(manager.isCommandDown("MOVEFORWARD"));
    pad.axis(SDL_GAMEPAD_AXIS_LEFTY, 0);
    pad.axis(SDL_GAMEPAD_AXIS_LEFTX, 0.46f); pump();
    CHECK(manager.isCommandDown("STRAFERIGHT"));
    pad.axis(SDL_GAMEPAD_AXIS_LEFTX, 0.34f); pump();
    CHECK_FALSE(manager.isCommandDown("STRAFERIGHT"));
    event(SDL_SCANCODE_W);
    controls.reset();
    CHECK(manager.isCommandDown("MOVEFORWARD"));
}

TEST_CASE_METHOD(Integration, "The real GameScreen idle and action path stays live across rebinds and page changes", "[input][screen][O10]") {
    replay::Session session;
    session.spawnPlayer({{wowee::game::UF::UNIT_FIELD_HEALTH, 100}});
    // No extracted DBCs are needed to send a spell; keep metadata loading out
    // of this input test, exactly as the packet-replay suite does.
    session.handler().spellNameCacheLoadedRef() = true;
    ui::GameScreen screen;
    const auto render = [&] {
        ImGui::NewFrame();
        screen.render(session.handler());
        ImGui::EndFrame();
    };
    const auto casts = [&] { return session.sent().of(wowee::game::Opcode::CMSG_CAST_SPELL); };
    render(); // Warm the real UI/static command names, not a copied poll loop.
    const auto initial = casts().size();
    for (int i = 0; i < 8; ++i) { manager.beginInputFrame(); render(); }
    CHECK(casts().size() == initial);
    for (int page : {1, 2, 6}) {
        screen.actionBarPanel().setMainActionBarPage(page);
        for (int i = 0; i < wowee::game::GameHandler::SLOTS_PER_BAR; ++i) {
            const auto command = (page == 6 ? "MULTIACTIONBAR1BUTTON" : "ACTIONBUTTON") + std::to_string(i + 1);
            const uint32_t spell = 10000 + page * 100 + i;
            const auto slot = ui::ActionBarPanel::actionSlotForPage(page, i);
            session.handler().setActionBarSlot(slot, wowee::game::ActionBarSlot::SPELL, spell);
            // A Spell.dbc row: castSpell refuses an id with none
            // (SpellPrerequisite::Unknown), and this fixture loads no DBCs.
            session.handler().spellNameCacheRef()[spell].name = "Integration spell";
            REQUIRE(manager.setBinding("F10", command));
            manager.beginInputFrame(); event(SDL_SCANCODE_F10); event(SDL_SCANCODE_F10, false);
            const auto before = casts().size();
            render();
            const auto sent = casts();
            REQUIRE(sent.size() == before + 1);
            // WotLK CMSG_CAST_SPELL starts with castCount, then uint32 spell.
            const auto& bytes = sent.back().getData();
            REQUIRE(bytes.size() >= 5);
            const uint32_t actual = uint32_t(bytes[1]) | uint32_t(bytes[2]) << 8 |
                uint32_t(bytes[3]) << 16 | uint32_t(bytes[4]) << 24;
            CHECK(actual == spell);
            manager.beginInputFrame(); render();
            CHECK(casts().size() == before + 1);
        }
    }
    // Main and bottom commands at the same index are independent presses.
    screen.actionBarPanel().setMainActionBarPage(2);
    session.handler().setActionBarSlot(ui::ActionBarPanel::actionSlotForPage(2, 0), wowee::game::ActionBarSlot::SPELL, 22001);
    session.handler().setActionBarSlot(ui::ActionBarPanel::actionSlotForPage(6, 0), wowee::game::ActionBarSlot::SPELL, 26001);
    session.handler().spellNameCacheRef()[22001].name = "Integration spell";
    session.handler().spellNameCacheRef()[26001].name = "Integration spell";
    REQUIRE(manager.setBinding("F9", "ACTIONBUTTON1"));
    REQUIRE(manager.setBinding("F10", "MULTIACTIONBAR1BUTTON1"));
    manager.beginInputFrame();
    event(SDL_SCANCODE_F9); event(SDL_SCANCODE_F9, false);
    event(SDL_SCANCODE_F10); event(SDL_SCANCODE_F10, false);
    const auto simultaneousBefore = casts().size();
    render();
    const auto simultaneous = casts();
    REQUIRE(simultaneous.size() == simultaneousBefore + 2);
    std::set<uint32_t> simultaneousSpells;
    for (size_t i = simultaneousBefore; i < simultaneous.size(); ++i) {
        const auto& bytes = simultaneous[i].getData();
        REQUIRE(bytes.size() >= 5);
        simultaneousSpells.insert(uint32_t(bytes[1]) | uint32_t(bytes[2]) << 8 |
            uint32_t(bytes[3]) << 16 | uint32_t(bytes[4]) << 24);
    }
    CHECK((simultaneousSpells == std::set<uint32_t>{22001, 26001}));
    manager.beginInputFrame(); render();
    CHECK(casts().size() == simultaneousBefore + 2);
    const auto before = casts().size();
    run("edit = CreateFrame('EditBox', 'ActionCapture', UIParent); edit:SetSize(200,30); edit:SetPoint('CENTER'); edit:Show(); edit:SetFocus()");
    layout();
    REQUIRE(lua.editBoxHasFocus());
    manager.beginInputFrame(); event(SDL_SCANCODE_F10); event(SDL_SCANCODE_F10, false); render();
    CHECK(casts().size() == before);
}

TEST_CASE_METHOD(Integration, "Emitted custom native commands have real labels and do not execute Lua actions", "[input][lua][bindings]") {
    ui::XmlNode root;
    std::string error;
    REQUIRE(ui::parseXml("<Bindings><Binding name='CAMERAZOOMOUT'>CameraZoomOut();</Binding><Binding name='SCREENSHOT' header='MISC'>Screenshot();</Binding></Bindings>", root, error));
    const auto emitted = ui::emitFrameXml(root);
    REQUIRE(emitted.warnings.empty());
    run(emitted.lua);
    truth("BINDING_NAME_WOWEERESETCAMERA == 'Reset Camera Angles'");
    truth("BINDING_NAME_WOWEETOGGLERECORDING == 'Start/Stop Video Recording'");
    CHECK(addons::clientActsOnBinding("WOWEERESETCAMERA"));
    CHECK(addons::clientActsOnBinding("WOWEETOGGLERECORDING"));
    run("__WoweeBindingScripts.WOWEERESETCAMERA('down'); __WoweeBindingScripts.WOWEETOGGLERECORDING('down')");
    manager.beginInputFrame(); event(SDL_SCANCODE_PRINTSCREEN, true, SDL_KMOD_SHIFT); event(SDL_SCANCODE_PRINTSCREEN, false);
    CHECK(manager.isCommandPressed("WOWEETOGGLERECORDING"));
    CHECK_FALSE(manager.isCommandPressed("SCREENSHOT"));
    manager.beginInputFrame(); event(SDL_SCANCODE_PRINTSCREEN); event(SDL_SCANCODE_PRINTSCREEN, false);
    CHECK(manager.isCommandPressed("SCREENSHOT"));
    CHECK_FALSE(manager.isCommandPressed("WOWEETOGGLERECORDING"));
}

TEST_CASE_METHOD(Integration, "Controller editor queries never replace addon keyboard queries", "[input][lua][bindings][editor]") {
    ui::XmlNode root;
    std::string error;
    REQUIRE(ui::parseXml("<Bindings><Binding name='TOGGLEAUTORUN'>ToggleAutoRun();</Binding><Binding name='JUMP'>JumpOrAscendStart();</Binding></Bindings>", root, error));
    run(ui::emitFrameXml(root).lua);
    truth("GetBindingKey('TOGGLEAUTORUN') == '`' and select(2, GetBindingKey('TOGGLEAUTORUN')) == 'NUMLOCK'");
    truth("WoweeGetControllerBindingKey('TOGGLEAUTORUN') == 'PADLSTICK'");
    run(R"lua(
        for i = 1, GetNumBindings() do
            local command, key1, key2 = GetBinding(i)
            if command == 'TOGGLEAUTORUN' then
                assert(key1 == '`' and key2 == 'NUMLOCK')
                local padCommand, pad1 = WoweeGetControllerBinding(i)
                assert(padCommand == command and pad1 == 'PADLSTICK')
                foundAutorun = true
            end
        end
        assert(foundAutorun)
        assert(SetBinding('F9', 'TOGGLEAUTORUN'))
        assert(SetBinding('PADLSTICK', 'JUMP'))
    )lua");
    const auto keyboard = manager.bindings();
    run("WoweeResetControllerBindings()");
    CHECK(manager.bindings() == keyboard);
    truth("WoweeGetControllerBindingKey('TOGGLEAUTORUN') == 'PADLSTICK'");
    REQUIRE(manager.setBinding("PAD1", "ACTIONBUTTON12"));
    const auto controller = manager.controllerBindings();
    run("WoweeResetKeyboardBindings()");
    CHECK(manager.controllerBindings() == controller);
    truth("GetBindingKey('TOGGLEAUTORUN') == '`' and select(2, GetBindingKey('TOGGLEAUTORUN')) == 'NUMLOCK'");
    truth("GetBindingAction('`') == 'TOGGLEAUTORUN'");
    run("assert(SaveBindings(1))");
    const auto savedKeyboard = manager.bindings(), savedController = manager.controllerBindings();
    manager.resetToDefaults();
    REQUIRE(manager.loadBindings(path("bindings.cfg").string()));
    CHECK(manager.bindings() == savedKeyboard);
    CHECK(manager.controllerBindings() == savedController);
}

TEST_CASE_METHOD(Integration, "Existing binding editor isolates device capture, reset, Save and Cancel", "[input][lua][editor]") {
    run(R"lua(
        KeyBindingFrame = CreateFrame('Button', 'KeyBindingFrame', UIParent)
        KeyBindingFrame:SetSize(600, 500); KeyBindingFrame:SetPoint('CENTER')
        KeyBindingFrame:EnableKeyboard(true)
        KeyBindingFrameOutputText = KeyBindingFrame:CreateFontString(nil)
        KeyBindingFrameUnbindButton = CreateFrame('Button', nil, KeyBindingFrame)
        function KeyBindingFrame_SetSelected(command) KeyBindingFrame.selected = command end
        function KeyBindingFrame_Update()
            shownKey1, shownKey2 = GetBindingKey('TOGGLEAUTORUN')
        end
        function KeyBindingFrame_OnKeyDown(self, key)
            if key == 'ESCAPE' then LoadBindings(1); self:Hide() end
        end
        KEY_BOUND = 'Bound'
        assert(SaveBindings(1))
    )lua");
    run(addons::kBindingDeviceEditor);
    // Reinstalling after an addon load is harmless, never makes two selectors.
    run(addons::kBindingDeviceEditor);
    run(R"lua(
        KeyBindingFrame_Update()
        assert(shownKey1 == '`' and shownKey2 == 'NUMLOCK')
        WoweeBindingDeviceSelector:GetScript('OnClick')(WoweeBindingDeviceSelector)
        assert(shownKey1 == 'PADLSTICK' and shownKey2 == nil)
        assert(GetBindingKey('TOGGLEAUTORUN') == '`')
        KeyBindingFrame.selected = 'TOGGLEAUTORUN'; KeyBindingFrame.keyID = 2
        KeyBindingFrame_OnKeyDown(KeyBindingFrame, 'F8') -- wrong device
        assert(KeyBindingFrame.selected == 'TOGGLEAUTORUN')
        KeyBindingFrame_OnKeyDown(KeyBindingFrame, 'PAD4')
        assert(shownKey1 == 'PADLSTICK' and shownKey2 == 'PAD4')
        assert(GetBindingKey('TOGGLEAUTORUN') == '`')
        KeyBindingFrame.selected = 'TOGGLEAUTORUN'; KeyBindingFrame.keyID = 1
        KeyBindingFrameUnbindButton:GetScript('OnClick')()
        assert(shownKey1 == nil and shownKey2 == 'PAD4')
        assert(SaveBindings(1))
        getfenv(KeyBindingFrame_Update).LoadBindings(0) -- reset selected device
        KeyBindingFrame_Update()
        assert(shownKey1 == 'PADLSTICK' and shownKey2 == nil)
        assert(GetBindingKey('TOGGLEAUTORUN') == '`')
        KeyBindingFrame_OnKeyDown(KeyBindingFrame, 'ESCAPE') -- Cancel restores both groups
        KeyBindingFrame_Update()
        assert(shownKey1 == nil and shownKey2 == 'PAD4')
        WoweeBindingDeviceSelector:GetScript('OnClick')(WoweeBindingDeviceSelector)
        KeyBindingFrame.selected = 'TOGGLEAUTORUN'; KeyBindingFrame.keyID = 2
        KeyBindingFrame_OnKeyDown(KeyBindingFrame, 'F8')
        assert(shownKey1 == '`' and shownKey2 == 'F8')
        assert(select(2, WoweeGetControllerBindingKey('TOGGLEAUTORUN')) == 'PAD4')
        KeyBindingFrame_OnKeyDown(KeyBindingFrame, 'ESCAPE')
        KeyBindingFrame_Update()
        assert(shownKey1 == '`' and shownKey2 == 'NUMLOCK')
        assert(not WoweeSetBindingSlot('JUMP', 3, 'SPACE', false))
        assert(not WoweeSetBindingSlot('JUMP', 1, 'PAD1', false))
        assert(not WoweeSetBindingSlot('bad\nrow', 1, nil, false))
    )lua");

    run("KeyBindingFrame:Show(); KeyBindingFrame.selected = 'TOGGLEAUTORUN'; KeyBindingFrame.keyID = 2");
    layout();
    // SDL has already pumped the release when the queued F8 press is dispatched.
    SDL_SetModState(SDL_KMOD_NONE);
    SDL_FlushEvents(SDL_EVENT_KEY_DOWN, SDL_EVENT_KEY_UP);
    for (auto queued : {key(SDL_SCANCODE_LSHIFT, true, SDL_KMOD_SHIFT),
                        key(SDL_SCANCODE_F8, true, SDL_KMOD_SHIFT),
                        key(SDL_SCANCODE_LSHIFT, false), key(SDL_SCANCODE_F8, false)}) {
        REQUIRE(SDL_PushEvent(&queued));
    }
    SDL_Event queued{};
    int dispatched = 0;
    while (SDL_PollEvent(&queued)) {
        if (queued.type != SDL_EVENT_KEY_DOWN && queued.type != SDL_EVENT_KEY_UP) continue;
        REQUIRE(SDL_GetModState() == SDL_KMOD_NONE);
        REQUIRE(lua.dispatchFrameKey(queued.key.key, queued.type == SDL_EVENT_KEY_DOWN,
            SDL_SCANCODE_TO_KEYCODE(queued.key.scancode), queued.key.mod));
        ++dispatched;
    }
    REQUIRE(dispatched == 4);
    truth("select(2, GetBindingKey('TOGGLEAUTORUN')) == 'SHIFT-F8'");
    truth("GetBindingAction('SHIFT-F8') == 'TOGGLEAUTORUN' and KeyBindingFrame.selected == nil");
    // Event context must not leak to a later direct call or change addon queries.
    truth("not IsShiftKeyDown() and not select(1, __WoweeBindingCaptureModifiers())");
    run(R"lua(
        KeyBindingFrame:EnableMouseWheel(true)
        KeyBindingFrame:SetScript('OnMouseWheel', function(self, delta)
            KeyBindingFrame_OnKeyDown(self, delta > 0 and 'MOUSEWHEELUP' or 'MOUSEWHEELDOWN')
        end)
        KeyBindingFrame.selected = 'TOGGLEAUTORUN'; KeyBindingFrame.keyID = 2
    )lua");
    layout();
    REQUIRE(lua.dispatchMouseWheel(400, 300, 1, SDL_KMOD_CTRL | SDL_KMOD_ALT));
    truth("select(2, GetBindingKey('TOGGLEAUTORUN')) == 'ALT-CTRL-MOUSEWHEELUP'");
    truth("not IsControlKeyDown() and not IsAltKeyDown()");
    run(R"lua(
        KeyBindingFrame:RegisterForClicks('MiddleButtonUp')
        KeyBindingFrame.selected = 'TOGGLEAUTORUN'; KeyBindingFrame.keyID = 2
    )lua");
    addons::LuaEngine::MouseButtons buttons;
    buttons.middle = true;
    lua.noteMouseModifiers(SDL_BUTTON_MIDDLE, SDL_KMOD_SHIFT);
    lua.dispatchMouse(400, 300, 600, buttons);
    // Shift was released later in the pump, before the sampled click callback.
    SDL_SetModState(SDL_KMOD_NONE);
    lua.noteMouseModifiers(SDL_BUTTON_MIDDLE, SDL_KMOD_SHIFT);
    lua.dispatchMouse(400, 300, 600, {});
    truth("select(2, GetBindingKey('TOGGLEAUTORUN')) == 'SHIFT-BUTTON3'");
    truth("not select(1, __WoweeBindingCaptureModifiers())");
    run(R"lua(
        WoweeBindingDeviceSelector:GetScript('OnClick')(WoweeBindingDeviceSelector)
        KeyBindingFrame.selected = 'TOGGLEAUTORUN'; KeyBindingFrame.keyID = 2
    )lua");
    // Pad tokens already contain their own modifiers; keyboard state adds none.
    SDL_SetModState(SDL_KMOD_CTRL | SDL_KMOD_ALT);
    REQUIRE(lua.dispatchPadKey("SHIFT-PAD4").taken);
    lua.dispatchPadKey("SHIFT-PAD4", false);
    truth("select(2, WoweeGetControllerBindingKey('TOGGLEAUTORUN')) == 'SHIFT-PAD4'");
}

TEST_CASE_METHOD(Integration, "Mouse clicks survive a complete press and release between frames", "[input][mouse-edges]") {
    auto& input = core::Input::getInstance();
    for (const auto button : {SDL_BUTTON_LEFT, SDL_BUTTON_RIGHT}) {
        SDL_Event event{};
        event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        event.button.button = button;
        event.button.x = 620;
        event.button.y = 310;
        input.processEvent(event);
        event.type = SDL_EVENT_MOUSE_BUTTON_UP;
        event.button.x = 622;
        input.processEvent(event);
        input.update();
        CHECK(input.isMouseButtonJustPressed(button));
        CHECK(input.isMouseButtonJustReleased(button));
        CHECK(input.getMousePressPosition(button) == glm::vec2(620, 310));
        CHECK(input.getMouseReleasePosition(button) == glm::vec2(622, 310));
        input.update();
        CHECK_FALSE(input.isMouseButtonJustPressed(button));
        CHECK_FALSE(input.isMouseButtonJustReleased(button));
    }
}

TEST_CASE_METHOD(Integration, "Virtual pointer edges retain held state across frames", "[input][mouse-edges]") {
    auto& input = core::Input::getInstance();
    input.setVirtualMouseButton(SDL_BUTTON_RIGHT, true);
    input.update();
    CHECK(input.isMouseButtonPressed(SDL_BUTTON_RIGHT));
    CHECK(input.isMouseButtonJustPressed(SDL_BUTTON_RIGHT));
    CHECK_FALSE(input.isMouseButtonJustReleased(SDL_BUTTON_RIGHT));
    input.update();
    CHECK(input.isMouseButtonPressed(SDL_BUTTON_RIGHT));
    CHECK_FALSE(input.isMouseButtonJustPressed(SDL_BUTTON_RIGHT));
    input.setVirtualMouseButton(SDL_BUTTON_RIGHT, false);
    input.update();
    CHECK_FALSE(input.isMouseButtonPressed(SDL_BUTTON_RIGHT));
    CHECK(input.isMouseButtonJustReleased(SDL_BUTTON_RIGHT));
    input.update();
    CHECK_FALSE(input.isMouseButtonJustReleased(SDL_BUTTON_RIGHT));
}

TEST_CASE_METHOD(Integration, "TBC target frame checks run through for a player target", "[target-frame]") {
    using namespace wowee::game;
    replay::Session session("tbc");
    auto& handler = session.handler();
    lua.setGameHandler(&handler);
    session.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_LEVEL, 70}});
    // Another player, PvP flagged (UNIT_FLAG_PVP) and in a sanctuary
    // (PLAYER_FLAGS_SANCTUARY, CMaNGOS TBC Unit::SetPvPSanctuary).
    constexpr uint64_t kOther = 3;
    session.receive(replay::UpdateObject().create(kOther, ObjectType::PLAYER,
        {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_LEVEL, 70},
         {UF::UNIT_FIELD_FLAGS, 0x00001000}, {UF::PLAYER_FLAGS, 0x00010000}}));
    handler.setTarget(kOther);
    truth("UnitIsPVPSanctuary('target') == true");
    truth("UnitIsPVPSanctuary('player') == false");
    // TargetFrame_CheckFaction's ally branch, verbatim from the 2.4.3
    // targetframe.lua: it raised while UnitIsPVPSanctuary was missing and
    // stopped TargetFrame_Update before TargetFrame_CheckClassification, so a
    // player target kept the previous target's elite border.
    truth("(UnitIsPVP('target') and not UnitIsPVPSanctuary('target') and not UnitIsPVPSanctuary('player')) == false");
    truth("UnitClassification('target') == 'normal'");
    session.receive(replay::UpdateObject().values(kOther, {{UF::PLAYER_FLAGS, 0}}));
    truth("UnitIsPVPSanctuary('target') == false");
    lua.setGameHandler(nullptr);
}

TEST_CASE_METHOD(Integration, "Merchant usability follows equipment requirements", "[merchant]") {
    replay::Session session("tbc");
    auto& handler = session.handler();
    lua.setGameHandler(&handler);
    handler.serverPlayerLevelRef() = 2;
    auto& info = handler.itemInfoCacheRef()[100];
    info.valid = true;
    info.name = "Test armor";
    info.itemClass = 4;
    info.subClass = 3; // Mail requires a proficiency this rogue does not have.
    info.requiredLevel = 1;
    wowee::network::Packet proficiency(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_SET_PROFICIENCY));
    proficiency.writeUInt8(4);
    proficiency.writeUInt32((1u << 1) | (1u << 2));
    session.receive(proficiency);
    wowee::network::Packet inventory(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_LIST_INVENTORY));
    inventory.writeUInt64(123);
    inventory.writeUInt8(1);
    for (uint32_t value : {1u, 100u, 0u, 0xffffffffu, 10000u, 0u, 1u, 0u}) inventory.writeUInt32(value);
    session.receive(inventory);
    truth("select(6, GetMerchantItemInfo(1)) == false");
    info.subClass = 2; // Leather usable even when unaffordable.
    truth("select(6, GetMerchantItemInfo(1)) == true");
    info.requiredLevel = 3;
    truth("select(6, GetMerchantItemInfo(1)) == false");
    info.requiredLevel = 1;
    info.requiredSkill = 164;
    info.requiredSkillRank = 90;
    truth("select(6, GetMerchantItemInfo(1)) == false");
    handler.playerSkillsRef()[164].value = 100;
    truth("select(6, GetMerchantItemInfo(1)) == true");
    info.itemClass = 2;
    info.subClass = 14; // Profession tools require no weapon proficiency.
    info.requiredSkill = 0;
    truth("select(6, GetMerchantItemInfo(1)) == true");
    info.subClass = 0; // Axes still require their proficiency.
    truth("select(6, GetMerchantItemInfo(1)) == false");
    lua.setGameHandler(nullptr);
}

TEST_CASE_METHOD(Integration, "Merchant recognizes learned proficiencies without a proficiency packet", "[merchant]") {
    using namespace wowee::game;
    replay::Session session("tbc");
    auto& handler = session.handler();
    lua.setGameHandler(&handler);
    handler.spellNameCacheLoadedRef() = true;
    auto& leather = handler.spellNameCacheRef()[9077];
    leather.equippedItemClass = 4;
    leather.equippedItemSubClassMask = 1u << 2;
    leather.effectIds[0] = 60;
    auto& mailAbility = handler.spellNameCacheRef()[10001];
    mailAbility.equippedItemClass = 4;
    mailAbility.equippedItemSubClassMask = 1u << 3;
    mailAbility.effectIds[0] = 6; // An equipment requirement does not grant proficiency.
    wowee::network::Packet initial(wireOpcode(Opcode::SMSG_INITIAL_SPELLS));
    initial.writeUInt8(0);
    initial.writeUInt16(2);
    for (const auto spell : {9077u, 10001u}) {
        initial.writeUInt16(spell);
        initial.writeUInt16(0);
    }
    initial.writeUInt16(0);
    session.receive(initial);
    auto& info = handler.itemInfoCacheRef()[100];
    info.valid = true;
    info.name = "Leather armor";
    info.itemClass = 4;
    info.subClass = 2;
    wowee::network::Packet inventory(wireOpcode(Opcode::SMSG_LIST_INVENTORY));
    inventory.writeUInt64(123);
    inventory.writeUInt8(1);
    for (uint32_t value : {1u, 100u, 0u, 0xffffffffu, 10000u, 0u, 1u, 0u}) inventory.writeUInt32(value);
    session.receive(inventory);
    truth("select(6, GetMerchantItemInfo(1)) == true");
    info.subClass = 3;
    truth("select(6, GetMerchantItemInfo(1)) == false");
    info.subClass = 2;
    wowee::network::Packet removed(wireOpcode(Opcode::SMSG_REMOVED_SPELL));
    removed.writeUInt32(9077);
    session.receive(removed);
    truth("select(6, GetMerchantItemInfo(1)) == false");
    wowee::network::Packet learned(wireOpcode(Opcode::SMSG_LEARNED_SPELL));
    learned.writeUInt32(9077);
    session.receive(learned);
    truth("select(6, GetMerchantItemInfo(1)) == true");
    CHECK_FALSE(handler.canUseArmorSubclass(32));
    lua.setGameHandler(nullptr);
}

static void announceTbcBoat(replay::Session& session, uint64_t guid) {
    using namespace wowee::game;
    wowee::network::Packet create(wireOpcode(Opcode::SMSG_UPDATE_OBJECT));
    create.writeUInt32(1);
    create.writeUInt8(0);
    create.writeUInt8(static_cast<uint8_t>(UpdateType::CREATE_OBJECT2));
    create.writePackedGuid(guid);
    create.writeUInt8(static_cast<uint8_t>(ObjectType::GAMEOBJECT));
    create.writeUInt8(0x02); // UPDATEFLAG_TRANSPORT
    create.writeUInt32(1000);
    create.writeUInt8(1);
    create.writeUInt32(1u << fieldIndex(UF::OBJECT_FIELD_ENTRY));
    create.writeUInt32(176244);
    session.receive(create);
    REQUIRE(session.handler().isTransportGuid(guid));
}

TEST_CASE_METHOD(Integration, "Transport clocks include socket and decompression queue delay", "[transport]") {
    using namespace wowee::game;
    for (bool compressed : {false, true}) {
        replay::Session session("tbc");
        constexpr uint64_t boat = 0x01fc00000002b074ULL;
        wowee::network::Packet create(wireOpcode(Opcode::SMSG_UPDATE_OBJECT));
        create.writeUInt32(1);
        create.writeUInt8(0);
        create.writeUInt8(static_cast<uint8_t>(UpdateType::CREATE_OBJECT2));
        create.writePackedGuid(boat);
        create.writeUInt8(static_cast<uint8_t>(ObjectType::GAMEOBJECT));
        create.writeUInt8(0x02);
        create.writeUInt32(1000);
        create.writeUInt8(1);
        create.writeUInt32(1u << fieldIndex(UF::OBJECT_FIELD_ENTRY));
        create.writeUInt32(176244);
        const auto received = std::chrono::steady_clock::now()-std::chrono::seconds(2);
        if (compressed) {
            std::vector<uint8_t> bytes(compressBound(create.getSize()));
            uLongf length = bytes.size();
            REQUIRE(compress(bytes.data(), &length, create.getData().data(), create.getSize()) == Z_OK);
            wowee::network::Packet wrapper(wireOpcode(Opcode::SMSG_COMPRESSED_UPDATE_OBJECT));
            wrapper.writeUInt32(create.getSize());
            wrapper.writeBytes(bytes.data(), length);
            wrapper.setReceivedAt(received);
            session.receive(wrapper);
        } else {
            create.setReceivedAt(received);
            session.receive(create);
        }
        REQUIRE(session.handler().isTransportGuid(boat));
        const auto now = session.handler().getTransportManager()->currentTransportTime(boat);
        CHECK(now >= 3000);
        CHECK(now < 3500);
    }
}

namespace {
struct SentMovement {
    uint32_t flags = 0;
    uint32_t time = 0;
    glm::vec3 server{0.0f};
    uint32_t fallTime = 0;
};

SentMovement readSentMovement(wowee::network::Packet packet) {
    SentMovement out;
    out.flags = packet.readUInt32();
    packet.readUInt8();
    out.time = packet.readUInt32();
    out.server.x = packet.readFloat();
    out.server.y = packet.readFloat();
    out.server.z = packet.readFloat();
    packet.readFloat();
    out.fallTime = packet.readUInt32();
    return out;
}
}  // namespace

// Native 2.4.3 (pair 4, 2026-11-18): heartbeats exactly 500 ms after the
// previous movement packet, 3.50 yards apart at run speed 7, none while
// standing or only turning; STOP in the air keeps FORWARD with PENDING_STOP
// (0x5001), FALL_LAND carries the whole fall and clears it (0x0, 825 ms).
TEST_CASE_METHOD(Integration, "TBC movement packets carry the instant of their physics position", "[movement]") {
    using namespace wowee::game;
    replay::Session session("tbc");
    auto& handler = session.handler();
    auto* movement = handler.getMovementHandler();
    REQUIRE(movement);
    const auto canonicalAt = [](float serverX) {
        return wowee::core::coords::serverToCanonical(glm::vec3(serverX, 100.0f, 50.0f));
    };
    const auto sample = [&](uint32_t timeMs, float serverX) {
        movement->beginFrame();
        const glm::vec3 c = canonicalAt(serverX);
        handler.setPosition(c.x, c.y, c.z);
        movement->recordPhysicsSample(timeMs);
        movement->sendDueHeartbeats();
    };
    // Physics times far ahead of the real movement clock of this test.
    constexpr uint32_t t0 = 1000000;
    sample(t0, 0.0f);
    handler.sendMovement(Opcode::MSG_MOVE_START_FORWARD);
    sample(t0 + 450, 3.15f);
    CHECK(session.sent().of(Opcode::MSG_MOVE_HEARTBEAT).empty());
    sample(t0 + 600, 4.2f);
    sample(t0 + 1040, 7.28f);
    auto start = session.sent().of(Opcode::MSG_MOVE_START_FORWARD);
    REQUIRE(start.size() == 1);
    CHECK(readSentMovement(start.front()).time == t0);
    auto beats = session.sent().of(Opcode::MSG_MOVE_HEARTBEAT);
    REQUIRE(beats.size() == 2);
    const auto first = readSentMovement(beats[0]);
    const auto second = readSentMovement(beats[1]);
    CHECK(first.time == t0 + 500);
    CHECK(second.time == t0 + 1000);
    CHECK(first.server.x == Catch::Approx(3.5f).margin(1e-3));
    CHECK(second.server.x == Catch::Approx(7.0f).margin(1e-3));
    CHECK(first.flags == 0x1);

    handler.sendMovement(Opcode::MSG_MOVE_STOP);
    for (uint32_t t = t0 + 1100; t < t0 + 3000; t += 100) sample(t, 7.28f);
    CHECK(session.sent().of(Opcode::MSG_MOVE_HEARTBEAT).size() == 2);

    // Running jump, W released in the air, landing.
    sample(t0 + 4000, 7.28f);
    handler.sendMovement(Opcode::MSG_MOVE_START_FORWARD);
    sample(t0 + 4100, 7.98f);
    handler.sendMovement(Opcode::MSG_MOVE_JUMP);
    sample(t0 + 4500, 10.78f);
    sample(t0 + 4700, 12.18f);
    handler.sendMovement(Opcode::MSG_MOVE_STOP);
    sample(t0 + 4925, 13.755f);
    handler.sendMovement(Opcode::MSG_MOVE_FALL_LAND);
    const auto jump = readSentMovement(session.sent().of(Opcode::MSG_MOVE_JUMP).front());
    CHECK(jump.time == t0 + 4100);
    CHECK(jump.fallTime == 0);
    const auto airStop = readSentMovement(session.sent().of(Opcode::MSG_MOVE_STOP).back());
    CHECK(airStop.flags == 0x5001);
    CHECK(airStop.time == t0 + 4700);
    CHECK(airStop.fallTime == 600);
    const auto airBeat = readSentMovement(session.sent().of(Opcode::MSG_MOVE_HEARTBEAT).back());
    CHECK(airBeat.time == t0 + 4600);
    CHECK(airBeat.fallTime == 500);
    CHECK(airBeat.server.x == Catch::Approx(11.48f).margin(1e-3));
    const auto land = readSentMovement(session.sent().of(Opcode::MSG_MOVE_FALL_LAND).front());
    CHECK(land.flags == 0x0);
    CHECK(land.fallTime == 825);
    sample(t0 + 6000, 13.755f);
    handler.sendMovement(Opcode::MSG_MOVE_START_BACKWARD);
    CHECK(readSentMovement(session.sent().of(Opcode::MSG_MOVE_START_BACKWARD).front()).fallTime == 825);
    handler.sendMovement(Opcode::MSG_MOVE_STOP);

    // Standing jump: horizontal speed 0, then the first direction in the
    // air drifts at 2.5 yards/s; a later direction leaves it (native).
    const auto jumpSpeed = [](wowee::network::Packet packet) {
        packet.readUInt32(); packet.readUInt8(); packet.readUInt32();
        for (int i = 0; i < 4; ++i) packet.readFloat();
        packet.readUInt32();
        packet.readFloat();
        const float c = packet.readFloat();
        const float sn = packet.readFloat();
        return glm::vec3(c, sn, packet.readFloat());
    };
    sample(t0 + 7000, 13.755f);
    handler.sendMovement(Opcode::MSG_MOVE_JUMP);
    CHECK(jumpSpeed(session.sent().of(Opcode::MSG_MOVE_JUMP).back()).z == 0.0f);
    sample(t0 + 7300, 13.755f);
    handler.sendMovement(Opcode::MSG_MOVE_START_STRAFE_RIGHT);
    const glm::vec3 drift = jumpSpeed(session.sent().of(Opcode::MSG_MOVE_START_STRAFE_RIGHT).back());
    CHECK(drift.z == 2.5f);
    handler.sendMovement(Opcode::MSG_MOVE_START_FORWARD);
    const glm::vec3 later = jumpSpeed(session.sent().of(Opcode::MSG_MOVE_START_FORWARD).back());
    CHECK(later == drift);
}

// Kalecgos's Spectral Realm: Sathrovarr's template 1959 (faction 960) is
// friendly to players; aura 44852 makes the server send forced reactions
// 960 -> 1 (hostile) and 14 -> 4 (friendly), logged live on pair 4.
TEST_CASE_METHOD(Integration, "TBC forced reactions override the faction template", "[faction]") {
    using namespace wowee::game;
    replay::Session session("tbc");
    auto& handler = session.handler();
    handler.setFactionTemplateFactions({{1959, 960}, {16, 14}});
    handler.setFactionFriendlyMap({{1959, true}, {16, false}});
    handler.setFactionHostileMap({{1959, false}, {16, true}});
    CHECK_FALSE(handler.isHostileFaction(1959));
    wowee::network::Packet forced(wireOpcode(Opcode::SMSG_SET_FORCED_REACTIONS));
    forced.writeUInt32(2);
    forced.writeUInt32(14); forced.writeUInt32(4);
    forced.writeUInt32(960); forced.writeUInt32(1);
    session.receive(forced);
    CHECK(handler.isHostileFaction(1959));
    CHECK_FALSE(handler.isFriendlyFaction(1959));
    CHECK(handler.isFriendlyFaction(16));
    CHECK_FALSE(handler.isHostileFaction(16));
    wowee::network::Packet cleared(wireOpcode(Opcode::SMSG_SET_FORCED_REACTIONS));
    cleared.writeUInt32(0);
    session.receive(cleared);
    CHECK_FALSE(handler.isHostileFaction(1959));
}

// World packets are applied as they arrive, so at login the party member
// standing beside the player (Graulberto, dwarf, template 3) is created before
// the world loader builds the faction maps. Judged then, an unknown template
// counts as hostile and his plate drew red where native shows a friendly blue.
// Building the maps re-judges every unit already there, against the player's
// live template (35 for a game master) rather than the race's.
TEST_CASE_METHOD(Integration, "Faction maps built after login re-judge the units already created", "[faction]") {
    using namespace wowee::game;
    replay::Session session("tbc");
    auto& handler = session.handler();
    auto self = std::make_shared<Player>(handler.getPlayerGuid());
    auto partyMember = std::make_shared<Player>(3);
    const auto factionField = fieldIndex(UF::UNIT_FIELD_FACTIONTEMPLATE);
    self->setField(factionField, 35);
    partyMember->setField(factionField, 3);
    partyMember->setFactionTemplate(3);
    handler.getEntityManager().addEntity(self->getGuid(), self);
    handler.getEntityManager().addEntity(3, partyMember);
    handler.notePlayerFactionTemplate(35);  // no builder yet: logged, nothing built
    handler.refreshUnitHostility();         // what player creation does
    CHECK(partyMember->isHostile());        // no maps: unknown template
    std::vector<uint32_t> built;
    handler.registerFactionMapBuilder([&](uint32_t playerTemplate) {
        built.push_back(playerTemplate);
        handler.setFactionHostileMap({{3, false}, {35, false}});
        handler.setFactionFriendlyMap({{3, true}, {35, true}});
        handler.setAppliedPlayerFactionTemplate(playerTemplate);
    }, 3);
    CHECK(built == std::vector<uint32_t>{35});
    CHECK_FALSE(partyMember->isHostile());
    CHECK(handler.unitReactionToPlayer(*partyMember) == 5);
}

// Spectral Blast 46648 is aimed at a place; its SpellVisual ImpactAreaKit
// (kit 9254, the pink beams and shards native draws) plays there.
TEST_CASE("TBC SMSG_SPELL_GO keeps the destination of a ground-aimed spell", "[spell]") {
    using namespace wowee::game;
    TbcPacketParsers parsers;
    wowee::network::Packet packet(wireOpcode(Opcode::SMSG_SPELL_GO));
    packet.writePackedGuid(0x17);
    packet.writePackedGuid(0x17);
    packet.writeUInt32(46648);
    packet.writeUInt16(0x100);
    packet.writeUInt32(12345);
    packet.writeUInt8(0);    // hits
    packet.writeUInt8(0);    // misses
    packet.writeUInt32(0x40); // TARGET_FLAG_DEST_LOCATION
    for (float v : {-9617.06f, -288.949f, 57.3053f}) packet.writeFloat(v);
    SpellGoData data;
    REQUIRE(parsers.parseSpellGo(packet, data));
    REQUIRE(data.destination.has_value());
    CHECK(data.destination->x == Catch::Approx(-9617.06f));
    CHECK(data.destination->y == Catch::Approx(-288.949f));
    CHECK(data.destination->z == Catch::Approx(57.3053f));
}

TEST_CASE_METHOD(Integration, "TBC outgoing boat offsets preserve model-local axes", "[transport]") {
    using namespace wowee::game;
    replay::Session session("tbc");
    auto& handler = session.handler();
    auto* manager = handler.getTransportManager();
    REQUIRE(manager);
    constexpr uint64_t boat = 0x01fc00000002b074ULL;
    announceTbcBoat(session, boat);
    manager->loadPathFromNodes(176244, {{0,0,0}});
    manager->registerTransport(boat, 0, 176244, {1000,8500,0}, 176244, 7087, false, 1.4f);
    handler.setPlayerOnTransport(boat, {4,-6,8});
    handler.movementInfoRef().orientation = wowee::core::coords::serverToCanonicalYaw(2.1f);
    handler.sendMovement(Opcode::MSG_MOVE_START_FORWARD);
    auto packets = session.sent().of(Opcode::MSG_MOVE_START_FORWARD);
    REQUIRE(packets.size() == 1);
    auto packet = packets.front();
    CHECK((packet.readUInt32() & 0x200) != 0);
    packet.readUInt8();
    packet.readUInt32();
    const float serverX = packet.readFloat();
    const float serverY = packet.readFloat();
    CHECK(packet.readFloat() == 8);
    CHECK(packet.readFloat() == Catch::Approx(2.1f));
    CHECK(packet.readUInt64() == boat);
    const float localX = packet.readFloat();
    const float localY = packet.readFloat();
    CHECK(localX == 4);
    CHECK(localY == -6);
    CHECK(packet.readFloat() == 8);
    CHECK(packet.readFloat() == Catch::Approx(0.7f));
    // CMaNGOS CalculatePassengerPosition must reconstruct the same world point.
    CHECK(serverX == Catch::Approx(8500+localX*std::cos(1.4f)-localY*std::sin(1.4f)).margin(.002));
    CHECK(serverY == Catch::Approx(1000+localY*std::cos(1.4f)+localX*std::sin(1.4f)).margin(.002));
    CHECK(manager->transportLocalToServer(boat, manager->serverToTransportLocal(boat, {4,-6,8})) == glm::vec3(4,-6,8));
}

TEST_CASE_METHOD(Integration,
                 "TBC boat passengers stay silent when stationary and trigger zone discovery on real movement or facing input",
                 "[transport][explore][boatcam]") {
    using namespace wowee::game;
    replay::Session session("tbc");
    auto& handler = session.handler();
    auto* movement = handler.getMovementHandler();
    auto* manager = handler.getTransportManager();
    REQUIRE(movement);
    REQUIRE(manager);
    GameHandlerTestAccess::setAreaName(handler, 35, "Booty Bay");

    std::vector<std::string> uiInfo;
    handler.setAddonEventCallback([&](const std::string& e, const std::vector<std::string>& a) {
        if (e == "UI_INFO_MESSAGE" && !a.empty()) uiInfo.push_back(a.front());
    });

    constexpr uint64_t boat = 0x01fc000000005148ULL; // The Maiden's Fancy
    announceTbcBoat(session, boat);
    manager->loadPathFromNodes(176231, {{0, 0, 0}});
    manager->registerTransport(boat, 0, 176231,
                               wowee::core::coords::serverToCanonical(glm::vec3(-14350.0f, 720.0f, 0.0f)),
                               176231, 3015, false, 0.0f);
    handler.setPlayerOnTransport(boat, {0.0f, 0.0f, 6.2f});

    constexpr uint32_t t0 = 2000000;
    const auto step = [&](uint32_t timeMs) {
        movement->beginFrame();
        const glm::vec3 worldPos = manager->getPlayerWorldPosition(boat, handler.playerTransportOffsetRef());
        handler.setPosition(worldPos.x, worldPos.y, worldPos.z);
        movement->recordPhysicsSample(timeMs);
        movement->sendDueHeartbeats();
    };

    step(t0);
    handler.sendMovement(Opcode::MSG_MOVE_STOP);
    CHECK(session.sent().of(Opcode::MSG_MOVE_HEARTBEAT).empty());

    // 1. Boat sails into Booty Bay and rotates while passenger stands still on deck:
    // Native TBC 2.4.3 emits ZERO MSG_MOVE_HEARTBEAT packets across stationary travel.
    manager->updateServerTransport(
        boat, wowee::core::coords::serverToCanonical(glm::vec3(-14270.2f, 580.86f, 0.0f)), 0.45f);
    handler.setOrientation(wowee::core::coords::normalizeAngleRad(
        manager->getTransport(boat)->canonicalYawRad() + 1.36f));
    for (uint32_t dt = 100; dt <= 6000; dt += 250) {
        step(t0 + dt);
    }
    CHECK(session.sent().of(Opcode::MSG_MOVE_HEARTBEAT).empty());

    // 2. Deliberate RMB character turn on deck (as at 23:50:08.358 in Booty Bay and
    // on pair 16 in Menethil Harbor) sends MSG_MOVE_SET_FACING with composed Booty Bay
    // world coordinates + ONTRANSPORT, triggering server CheckAreaExploreAndOutdoor ->
    // SMSG_EXPLORATION_EXPERIENCE(35, 0) -> "Discovered: Booty Bay".
    handler.setOrientation(wowee::core::coords::normalizeAngleRad(
        manager->getTransport(boat)->canonicalYawRad() + 1.81f));
    handler.sendMovement(Opcode::MSG_MOVE_SET_FACING);
    const auto facings = session.sent().of(Opcode::MSG_MOVE_SET_FACING);
    REQUIRE(facings.size() == 1);
    const auto facingMove = readSentMovement(facings[0]);
    CHECK((facingMove.flags & static_cast<uint32_t>(MovementFlags::ONTRANSPORT)) != 0);
    CHECK(facingMove.server.x == Catch::Approx(-14270.2f).margin(1e-2));
    CHECK(facingMove.server.y == Catch::Approx(580.86f).margin(1e-2));

    wowee::network::Packet explore(wireOpcode(Opcode::SMSG_EXPLORATION_EXPERIENCE));
    explore.writeUInt32(35);
    explore.writeUInt32(0);
    session.receive(explore);
    REQUIRE(uiInfo.size() == 1);
    CHECK(uiInfo.back() == "Discovered: Booty Bay");

    // 3. Active walking on deck still emits standard 500 ms MSG_MOVE_HEARTBEAT packets,
    // and stopping returns to silence.
    step(t0 + 6100);
    handler.sendMovement(Opcode::MSG_MOVE_START_FORWARD);
    step(t0 + 6650);
    const auto beats = session.sent().of(Opcode::MSG_MOVE_HEARTBEAT);
    REQUIRE(beats.size() == 1);
    CHECK((readSentMovement(beats[0]).flags & static_cast<uint32_t>(MovementFlags::ONTRANSPORT)) != 0);
    handler.sendMovement(Opcode::MSG_MOVE_STOP);
    for (uint32_t dt = 6800; dt <= 8000; dt += 200) {
        step(t0 + dt);
    }
    CHECK(session.sent().of(Opcode::MSG_MOVE_HEARTBEAT).size() == 1);
}

TEST_CASE_METHOD(Integration, "Only announced boat transfers preserve passenger state across maps", "[transport]") {
    using namespace wowee::game;
    for (bool aboardTransfer : {false, true}) {
        replay::Session session("tbc");
        auto& handler = session.handler();
        auto* manager = handler.getTransportManager();
        constexpr uint64_t boat = 0x01fc00000002b074ULL;
        announceTbcBoat(session, boat);
        manager->loadPathFromNodes(176244, {{0,0,0}});
        manager->registerTransport(boat, 0, 176244, {1000,8500,0}, 176244, 7087, false, 1.4f);
        handler.setPlayerOnTransport(boat, {4,-6,8});
        wowee::network::Packet pending(wireOpcode(Opcode::SMSG_TRANSFER_PENDING));
        pending.writeUInt32(530);
        if (aboardTransfer) {
            pending.writeUInt32(176244);
            pending.writeUInt32(handler.getCurrentMapId());
        }
        session.receive(pending);
        wowee::network::Packet destination(wireOpcode(Opcode::SMSG_NEW_WORLD));
        destination.writeUInt32(530);
        for (float value : {4.f,-6.f,8.f,1.f}) destination.writeFloat(value);
        session.receive(destination);
        CHECK(handler.hasPendingPlayerTransportWorldTransfer() == aboardTransfer);
        if (!aboardTransfer) {
            CHECK_FALSE(handler.isOnTransport());
            handler.sendMovement(Opcode::MSG_MOVE_START_FORWARD);
            REQUIRE(session.sent().of(Opcode::MSG_MOVE_START_FORWARD).size() == 1);
        } else {
            glm::vec3 restored;
            REQUIRE(handler.completePlayerTransportWorldTransfer(boat, restored));
            CHECK(handler.playerTransportOffsetRef() == glm::vec3(4,-6,8));
            CHECK_FALSE(handler.hasPendingPlayerTransportWorldTransfer());
        }
    }
}

TEST_CASE_METHOD(Integration, "TBC remote boat passengers use a raw transport GUID", "[transport]") {
    replay::Session session("tbc");
    auto& handler = session.handler();
    constexpr uint64_t rider = 3;
    constexpr uint64_t boat = 0x000000000002B074ull;
    handler.getEntityManager().addEntity(rider, std::make_shared<wowee::game::Player>(rider));
    wowee::network::Packet packet(wowee::game::wireOpcode(wowee::game::Opcode::MSG_MOVE_HEARTBEAT));
    packet.writePackedGuid(rider);
    packet.writeUInt32(0x200); // TBC ONTRANSPORT
    packet.writeUInt8(0);
    packet.writeUInt32(12345);
    for (float value : {8500.f, 600.f, 8.f, 1.f}) packet.writeFloat(value);
    packet.writeUInt64(boat);
    for (float value : {4.f, -6.f, 8.f, 2.5f}) packet.writeFloat(value);
    packet.writeUInt32(12340);
    packet.writeUInt32(0);
    session.receive(packet);
    const auto& attachments = handler.transportAttachmentsRef();
    REQUIRE(attachments.contains(rider));
    const auto& attachment = attachments.at(rider);
    CHECK(attachment.transportGuid == boat);
    CHECK(attachment.localOffset == wowee::core::coords::serverToCanonical(glm::vec3(4.f, -6.f, 8.f)));
    CHECK(attachment.localOrientation == Catch::Approx(wowee::core::coords::serverToCanonicalYaw(2.5f)));
    CHECK(std::abs(wowee::core::coords::normalizeAngleRad(1e30f)) <= wowee::core::coords::PI);
    CHECK_THROWS_AS(wowee::core::coords::normalizeAngleRad(INFINITY), std::invalid_argument);
}

TEST_CASE_METHOD(Integration, "TBC battlemaster offerings preserve GUID and newest selection", "[battlemaster]") {
    replay::Session session("tbc");
    auto& handler = session.handler();
    std::vector<std::string> events;
    handler.setAddonEventCallback([&](const std::string& event, const auto&) { events.push_back(event); });
    constexpr uint64_t guid = 0xF130004E94000001ull;
    for (const uint32_t type : {2u, 3u, 2u}) {
        wowee::network::Packet packet(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_BATTLEFIELD_LIST));
        packet.writeUInt64(guid);
        packet.writeUInt32(type);
        packet.writeUInt8(0);
        packet.writeUInt32(2);
        packet.writeUInt32(17);
        packet.writeUInt32(23);
        session.receive(packet);
        REQUIRE_FALSE(handler.getAvailableBgs().empty());
        const auto& offering = handler.getAvailableBgs().back();
        CHECK(offering.bgTypeId == type);
        CHECK(offering.battlemasterGuid == guid);
        CHECK(offering.instanceIds == std::vector<uint32_t>{17, 23});
        CHECK(events.back() == "BATTLEFIELDS_SHOW");
    }
    CHECK(handler.getAvailableBgs().size() == 2);
}

TEST_CASE_METHOD(Integration, "Charter query, rename and close share petition state", "[petition]") {
    replay::Session session("tbc");
    auto& handler = session.handler();
    using wowee::game::Opcode;
    using wowee::game::wireOpcode;
    constexpr uint64_t guid = 0x4000000000000148ull;
    wowee::network::Packet signatures(wireOpcode(Opcode::SMSG_PETITION_SHOW_SIGNATURES));
    signatures.writeUInt64(guid);
    signatures.writeUInt64(replay::Session::kPlayerGuid);
    signatures.writeUInt32(328);
    signatures.writeUInt8(0);
    session.receive(signatures);
    REQUIRE(handler.hasPetitionSignaturesUI());
    wowee::network::Packet query(wireOpcode(Opcode::SMSG_PETITION_QUERY_RESPONSE));
    query.writeUInt32(328);
    query.writeUInt64(replay::Session::kPlayerGuid);
    query.writeString("Test Guild");
    query.writeString("");
    query.writeUInt32(9);
    query.writeUInt32(9);
    query.writeUInt32(0);
    session.receive(query);
    REQUIRE(handler.queryPetitionItem(guid));
    CHECK(handler.queryPetitionItem(guid)->ownerGuid == replay::Session::kPlayerGuid);
    CHECK(handler.getPetitionInfo().guildName == "Test Guild");
    wowee::network::Packet rename(wireOpcode(Opcode::MSG_PETITION_RENAME));
    rename.writeUInt64(guid);
    rename.writeString("Renamed Guild");
    session.receive(rename);
    CHECK(handler.queryPetitionItem(guid)->guildName == "Renamed Guild");
    CHECK(handler.getPetitionInfo().guildName == "Renamed Guild");
    handler.clearPetitionSignaturesUI();
    CHECK_FALSE(handler.hasPetitionSignaturesUI());
}

namespace {
// One SMSG_LIST_INVENTORY row in the TBC layout (CMaNGOS ItemHandler.cpp
// SendListInventory): slot, item, display, count, price, durability, stack,
// extended cost.
void writeVendorRow(wowee::network::Packet& packet, uint32_t slot, uint32_t item, uint32_t extendedCost) {
    for (uint32_t value : {slot, item, 0u, 0xffffffffu, 0u, 0u, 1u, extendedCost}) packet.writeUInt32(value);
}
}  // namespace

TEST_CASE("TBC ItemExtendedCost columns come from the expansion's layout", "[merchant]") {
    // Two rows of build 8606's ItemExtendedCost.dbc (14 fields, CMaNGOS fmt
    // "niiiiiiiiiiiii"): Lieutenant Tristia's Guardian's bracers (2401) and the
    // Vindicator's bands (129). Read with 3.3.5's offsets they lost the
    // Warsong Gulch and Alterac Valley marks and gave a count as an item id.
    const std::vector<std::array<uint32_t, 14>> rows = {
        {2401, 11794, 0, 20558, 0, 0, 0, 0, 20, 0, 0, 0, 0, 1575},
        {129, 11934, 0, 20560, 0, 0, 0, 0, 10, 0, 0, 0, 0, 0},
    };
    std::vector<uint8_t> bytes = {'W', 'D', 'B', 'C'};
    auto put = [&bytes](uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>(v >> (8 * i)));
    };
    put(static_cast<uint32_t>(rows.size())); put(14); put(14 * 4); put(1);
    for (const auto& row : rows) for (uint32_t v : row) put(v);
    bytes.push_back(0);
    wowee::pipeline::DBCFile dbc;
    REQUIRE(dbc.load(bytes));
    wowee::pipeline::DBCLayout layouts;
    REQUIRE(layouts.loadFromJson(std::string(WOWEE_SOURCE_DIR) + "/Data/expansions/tbc/dbc_layouts.json"));
    const auto* layout = layouts.getLayout("ItemExtendedCost");
    REQUIRE(layout);
    const auto costs = wowee::game::GameHandler::readExtendedCosts(dbc, *layout);
    REQUIRE(costs.size() == 2);
    const auto& bracers = costs.at(2401);
    CHECK(bracers.honorPoints == 11794);
    CHECK(bracers.arenaPoints == 0);
    CHECK(bracers.itemId[0] == 20558);
    CHECK(bracers.itemCount[0] == 20);
    CHECK(bracers.itemId[4] == 0);
    CHECK(bracers.requiredArenaRating == 1575);
    const auto& band = costs.at(129);
    CHECK(band.itemId[0] == 20560);
    CHECK(band.itemCount[0] == 10);
    CHECK(band.requiredArenaRating == 0);
}

TEST_CASE_METHOD(Integration, "Merchant rows are red for a vendor arena rating the player lacks", "[merchant]") {
    using namespace wowee::game;
    replay::Session session("tbc");
    auto& handler = session.handler();
    lua.setGameHandler(&handler);
    session.receive(replay::UpdateObject().tbc().create(replay::Session::kPlayerGuid, ObjectType::PLAYER,
        {{UF::UNIT_FIELD_LEVEL, 70}}, true));
    handler.serverPlayerLevelRef() = 70;
    // Big Zokk Torquewrench's costs: 2342 is 1125 arena points with no rating,
    // 2359 is 1500 points at 2200. Tristia's 2401 is honor and marks at 1575.
    handler.extendedCostCacheLoadedRef() = true;
    auto& costs = handler.extendedCostCacheRef();
    costs[2342].arenaPoints = 1125;
    costs[2359].arenaPoints = 1500;
    costs[2359].requiredArenaRating = 2200;
    costs[2401].honorPoints = 11794;
    costs[2401].itemId[0] = 20558;
    costs[2401].itemCount[0] = 20;
    costs[2401].requiredArenaRating = 1575;
    for (uint32_t id : {35032u, 35035u, 35171u}) {
        auto& info = handler.itemInfoCacheRef()[id];
        info.valid = true;
        info.name = "Item " + std::to_string(id);
        info.itemClass = 15;
        info.requiredLevel = 70;
    }
    wowee::network::Packet inventory(wireOpcode(Opcode::SMSG_LIST_INVENTORY));
    inventory.writeUInt64(123);
    inventory.writeUInt8(5);
    writeVendorRow(inventory, 1, 35032, 2342);
    writeVendorRow(inventory, 2, 35035, 2359);
    writeVendorRow(inventory, 3, 35171, 2401);
    // Items whose template has not arrived: the rating alone decides.
    writeVendorRow(inventory, 4, 35999, 2342);
    writeVendorRow(inventory, 5, 35998, 2359);
    session.receive(inventory);

    // No team: no rating. Points, honor and marks the player lacks do not
    // make a row red, the rating does.
    CHECK(handler.getArenaRatingForPurchases() == 0);
    truth("select(6, GetMerchantItemInfo(1)) == true");
    truth("select(6, GetMerchantItemInfo(2)) == false");
    truth("select(6, GetMerchantItemInfo(3)) == false");
    truth("select(6, GetMerchantItemInfo(4)) == true");
    truth("select(6, GetMerchantItemInfo(5)) == false");
    truth("select(7, GetMerchantItemInfo(3)) == 2401");
    truth("select(1, GetMerchantItemCostInfo(3)) == 11794 and select(3, GetMerchantItemCostInfo(3)) == 1");
    truth("select(2, GetMerchantItemCostItem(3, 1)) == 20");
    truth("select(1, _GetMerchantItemArenaRating(2)) == 2200 and select(2, _GetMerchantItemArenaRating(2)) == false");
    truth("_GetMerchantItemArenaRating(1) == nil");

    // A 2v2 team (slot 0): id 77, personal rating 2300, team rating 2250 -
    // the lower of the two counts.
    const auto player = handler.getEntityManager().getEntity(replay::Session::kPlayerGuid);
    REQUIRE(player);
    const uint16_t teamInfo = fieldIndex(UF::PLAYER_FIELD_ARENA_TEAM_INFO_1_1);
    REQUIRE(teamInfo == 1544);
    player->setField(teamInfo, 77);
    player->setField(teamInfo + 5, 2300);
    CHECK(handler.getArenaRatingForPurchases() == 0);  // the team's own rating is not known yet
    auto teamStats = [&](uint32_t rating) {
        wowee::network::Packet stats(wireOpcode(Opcode::SMSG_ARENA_TEAM_STATS));
        for (uint32_t v : {77u, rating, 0u, 0u, 0u, 0u, 0u}) stats.writeUInt32(v);
        session.receive(stats);
    };
    teamStats(2250);
    CHECK(handler.getArenaRatingForPurchases() == 2250);
    truth("select(6, GetMerchantItemInfo(2)) == true");
    truth("select(2, _GetMerchantItemArenaRating(2)) == true");
    teamStats(2100);
    CHECK(handler.getArenaRatingForPurchases() == 2100);
    truth("select(6, GetMerchantItemInfo(2)) == false");
    truth("select(6, GetMerchantItemInfo(3)) == true");

    // The tooltip line, from GlobalStrings.lua, which is extracted client data
    // outside this repository, e.g.
    //   WOWEE_TBC_GLOBALSTRINGS=/workspace/azerothcore/client/wowee-data/expansions/tbc/interface/framexml/globalstrings.lua
    const char* globalStrings = std::getenv("WOWEE_TBC_GLOBALSTRINGS");
    if (!globalStrings) SKIP("WOWEE_TBC_GLOBALSTRINGS not set");
    std::ifstream globalStringsFile(globalStrings);
    REQUIRE(globalStringsFile.good());
    std::stringstream globalStringsText;
    globalStringsText << globalStringsFile.rdbuf();
    run(globalStringsText.str());
    run("merchantTip = CreateFrame('GameTooltip', 'MerchantTip', UIParent); assert(merchantTip:SetMerchantItem(2))");
    const auto* tip = lua.widgets().findByName("MerchantTip");
    REQUIRE(tip);
    auto ratingLine = [&]() -> const wowee::ui::Widget::TooltipLine* {
        for (const auto& line : tip->tooltipLines)
            if (line.left == "Requires personal and team arena rating of 2200") return &line;
        return nullptr;
    };
    REQUIRE(ratingLine());
    CHECK(ratingLine()->lc[0] == 1.0f);   // RED_FONT_COLOR (1, 0.125, 0.125)
    CHECK(ratingLine()->lc[1] < 0.2f);
    // The same item from a link: the rating is the vendor's, not the item's.
    run("assert(merchantTip:SetHyperlink(GetMerchantItemLink(2)))");
    CHECK(ratingLine() == nullptr);
    run("assert(merchantTip:SetMerchantItem(1))");
    for (const auto& line : tip->tooltipLines) CHECK(line.left.find("arena rating") == std::string::npos);
    lua.setGameHandler(nullptr);
}

TEST_CASE_METHOD(Integration, "Merchant rows are red for a required spell the player lacks", "[merchant]") {
    using namespace wowee::game;
    replay::Session session("tbc");
    auto& handler = session.handler();
    lua.setGameHandler(&handler);
    handler.serverPlayerLevelRef() = 70;
    // Spellfire Robe (21848): cloth, level 70, Spellfire Tailoring (26797).
    auto& robe = handler.itemInfoCacheRef()[21848];
    robe.valid = true;
    robe.name = "Spellfire Robe";
    robe.itemClass = 4;
    robe.subClass = 1;
    robe.requiredLevel = 70;
    robe.requiredSpell = 26797;
    wowee::network::Packet proficiency(wireOpcode(Opcode::SMSG_SET_PROFICIENCY));
    proficiency.writeUInt8(4);
    proficiency.writeUInt32(1u << 1);
    session.receive(proficiency);
    wowee::network::Packet inventory(wireOpcode(Opcode::SMSG_LIST_INVENTORY));
    inventory.writeUInt64(123);
    inventory.writeUInt8(1);
    writeVendorRow(inventory, 1, 21848, 0);
    session.receive(inventory);
    truth("select(6, GetMerchantItemInfo(1)) == false");
    wowee::network::Packet learned(wireOpcode(Opcode::SMSG_LEARNED_SPELL));
    learned.writeUInt32(26797);
    session.receive(learned);
    REQUIRE(handler.getKnownSpells().count(26797));
    truth("select(6, GetMerchantItemInfo(1)) == true");
    lua.setGameHandler(nullptr);
}

TEST_CASE_METHOD(Integration, "TBC purchases encode quantity without vendor slot", "[merchant]") {
    replay::Session session("tbc");
    for (const uint32_t slot : {11u, 13u}) {
        auto packet = wowee::game::BuyItemPacket::build(123, 10648, slot, 1);
        CHECK(packet.readUInt64() == 123);
        CHECK(packet.readUInt32() == 10648);
        CHECK(packet.readUInt8() == 1);
        CHECK(packet.readUInt8() == 1);
        CHECK(packet.getRemainingSize() == 0);
    }
}

TEST_CASE_METHOD(Integration, "Repeated vendor gossip refreshes without closing the shop", "[merchant]") {
    replay::Session session("tbc");
    auto& handler = session.handler();
    std::vector<std::string> events;
    handler.setAddonEventCallback([&](const std::string& name, const std::vector<std::string>&) {
        events.push_back(name);
        if (name == "GOSSIP_SHOW") handler.selectGossipOption(0);
    });
    wowee::network::Packet inventory(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_LIST_INVENTORY));
    inventory.writeUInt64(123);
    inventory.writeUInt8(0);
    session.receive(inventory);
    REQUIRE(handler.isVendorWindowOpen());
    CHECK(handler.getInteractNpcGuid() == 123);
    CHECK(std::count(events.begin(), events.end(), "MERCHANT_SHOW") == 1);
    events.clear();
    wowee::network::Packet gossip(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_GOSSIP_MESSAGE));
    gossip.writeUInt64(123);
    gossip.writeUInt32(0); // menu
    gossip.writeUInt32(0); // NPC text
    gossip.writeUInt32(1); // options
    gossip.writeUInt32(0); // option id
    gossip.writeUInt8(1);  // vendor icon
    gossip.writeUInt8(0);  // not coded
    gossip.writeUInt32(0); // no money prompt
    gossip.writeString("I want to browse your goods.");
    gossip.writeString("");
    gossip.writeUInt32(0); // quests
    session.receive(gossip);
    REQUIRE(handler.isVendorWindowOpen());
    handler.selectGossipOption(0); // A repeated selection while response pending is ignored.
    CHECK(session.sent().of(wowee::game::Opcode::CMSG_GOSSIP_SELECT_OPTION).size() == 1);
    CHECK(session.sent().of(wowee::game::Opcode::CMSG_LIST_INVENTORY).empty());
    session.receive(inventory);
    CHECK(handler.isVendorWindowOpen());
    CHECK(std::count(events.begin(), events.end(), "MERCHANT_UPDATE") == 1);
    CHECK(std::count(events.begin(), events.end(), "MERCHANT_SHOW") == 0);
    CHECK(std::count(events.begin(), events.end(), "MERCHANT_CLOSED") == 0);
    CHECK(handler.getInteractNpcGuid() == 123);
    handler.closeVendor();
    CHECK(handler.getInteractNpcGuid() == 0);
}

TEST_CASE("Conversation facing settles in native 50 ms interval", "[npc-facing]") {
    using wowee::core::NpcInteractionFacing;
    using wowee::core::coords::normalizeAngleRad;
    NpcInteractionFacing facing(3.0f);
    const float target = -3.0f;
    CHECK(normalizeAngleRad(facing.advance(target, 0.01f) - 3.0f) > 0.0f);
    CHECK(facing.remaining > 0.039f);
    // Repeated target requests keep the original deadline.
    for (int frame = 0; frame < 3; ++frame) facing.advance(target, 0.01f);
    CHECK(facing.remaining < 0.011f);
    CHECK(std::abs(normalizeAngleRad(facing.current - target)) > 0.01f);
    CHECK(std::abs(normalizeAngleRad(facing.advance(target, 0.011f) - target)) < 0.0001f);
    CHECK(facing.remaining == 0.0f);
    CHECK(facing.advance(target, 0.01f) == facing.current);
    CHECK(facing.advance(0.0f, 0.0f) == target);
    CHECK(std::abs(facing.advance(0.0f, 0.05f)) < 0.0001f);
    for (float dt : {1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 144.0f}) {
        NpcInteractionFacing turn(0.0f);
        float elapsed = 0.0f;
        while (elapsed < 0.05f) {
            turn.advance(2.0f, dt);
            elapsed += dt;
        }
        CHECK(std::abs(turn.current - 2.0f) < 0.0001f);
    }
}

TEST_CASE("Conversation shuffle starts only on directional heading changes", "[npc-facing]") {
    wowee::core::NpcInteractionFacing facing(3.0f);
    facing.advance(-3.0f, 0.01f);
    CHECK(facing.turnDirection == 1); // shortest arc crosses +pi
    facing.advance(-3.0f, 0.01f);
    CHECK(facing.turnDirection == 0); // repeated gossip cannot restart feet
    facing.advance(-3.0f, 0.05f);
    CHECK(facing.turnDirection == 0); // settling does not restart feet
    facing.advance(3.0f, 0.01f);
    CHECK(facing.turnDirection == -1); // closing turns the other way
    facing.advance(3.0f, 0.05f);
    facing.advance(3.001f, 0.01f);
    CHECK(facing.turnDirection == 0); // tiny facing corrections need no step
}

TEST_CASE("Only standing, unstunned conversation partners turn", "[npc-facing]") {
    using wowee::core::npcTurnsToConversationPartner;
    // Native 2.4.3 on GM Island: Marshal Dughan (240) and Vaelastrasz (13020).
    CHECK(npcTurnsToConversationPartner(0, 0x1200));
    CHECK(npcTurnsToConversationPartner(0, 0x8040)); // Vaelastrasz unit flags
    for (uint8_t state = 1; state <= 9; ++state)
        CHECK_FALSE(npcTurnsToConversationPartner(state, 0x8040)); // sit/chairs/sleep/dead/kneel/submerged
    CHECK_FALSE(npcTurnsToConversationPartner(0, 0x00040000 | 0x1200)); // stunned
    CHECK(npcTurnsToConversationPartner(0, 0x00020000 | 0x00080000 | 0x00400000 | 0x00800000 |
                                           0x01000000 | 0x00000004 | 0x1200)); // pacified, combat, confused, fleeing, possessed, control lost
}

TEST_CASE_METHOD(Integration, "WMO building names and room names have distinct zone APIs", "[zone-text]") {
    std::string building = "Anvilmar";
    std::string room;
    addons::LuaServices services;
    services.getInteriorZoneName = [&] { return building; };
    services.getInteriorAreaName = [&] { return room; };
    lua.setLuaServices(services);
    truth("GetZoneText() == 'Anvilmar'");
    truth("GetSubZoneText() == ''");
    truth("GetMinimapZoneText() == 'Anvilmar'");
    // Named rooms remain subzones rather than replacing their building name.
    building = "Ironforge";
    room = "Hall of Explorers";
    truth("GetZoneText() == 'Ironforge'");
    truth("GetSubZoneText() == 'Hall of Explorers'");
    truth("GetMinimapZoneText() == 'Hall of Explorers'");
    building.clear();
    room.clear();
    truth("GetZoneText() == '' and GetSubZoneText() == ''");
    lua.setLuaServices({});
}

TEST_CASE("FontString alpha gradient reports quest reveal completion", "[quest-text]") {
    Integration f;
    f.run(R"lua(
        revealHost = CreateFrame("Frame", "RevealHost", UIParent)
        reveal = revealHost:CreateFontString("RevealText", "ARTWORK")
        reveal:SetText("|cffffd200Hello|r world")
        assert(reveal:SetAlphaGradient(0, 5) == 1)
        assert(reveal:SetAlphaGradient(10, 5) == 1)
        assert(reveal:SetAlphaGradient(11, 5) == nil)
        assert(reveal:SetAlphaGradient(1024, 5) == nil)
        reveal:SetText("")
        assert(reveal:SetAlphaGradient(0, 5) == nil)
    )lua");
    const auto* widget = f.lua.widgets().findByName("RevealText");
    REQUIRE(widget);
    CHECK(widget->alphaGradientStart == 0.0f);
    CHECK(widget->alphaGradientLength == 5.0f);
}

TEST_CASE("Empty FontString and Button text reads back as nil", "[fontstring-text]") {
    // Measured on 2.4.3: a new FontString, and one after SetText(""), (nil)
    // or (), answer nil; so does a button's label. An EditBox answers "".
    Integration f;
    f.run(R"lua(
        local host = CreateFrame("Frame", "EmptyTextHost", UIParent)
        local label = host:CreateFontString(nil, "ARTWORK", "GameFontNormal")
        assert(label:GetText() == nil)
        label:SetText("") assert(label:GetText() == nil)
        label:SetText("a") assert(label:GetText() == "a")
        label:SetText(nil) assert(label:GetText() == nil)
        label:SetText("a") label:SetText() assert(label:GetText() == nil)
        local button = CreateFrame("Button", "EmptyTextButton", UIParent, "UIPanelButtonTemplate")
        assert(button:GetText() == nil)
        button:SetText("") assert(button:GetText() == nil)
        local edit = CreateFrame("EditBox", "EmptyTextEdit", UIParent)
        assert(edit:GetText() == "")
    )lua");
}

TEST_CASE("Tooltip SetText keeps its wrap flag through the frame method", "[fontstring-text]") {
    // LFMFrame: GameTooltip:SetText(AUTO_ADD_TOOLTIP, nil, nil, nil, nil, 1)
    // draws three wrapped rows on 2.4.3; the flag was dropped on the way in.
    Integration f;
    f.run(R"lua(
        wrapTip = CreateFrame('GameTooltip', 'WrapTip', UIParent)
        wrapTip:SetText('Automatically add members to your group.', nil, nil, nil, nil, 1)
        flatTip = CreateFrame('GameTooltip', 'FlatTip', UIParent)
        flatTip:SetText('One line', 1, 1, 1)
    )lua");
    const auto* wrap = f.lua.widgets().findByName("WrapTip");
    const auto* flat = f.lua.widgets().findByName("FlatTip");
    REQUIRE(wrap); REQUIRE(flat);
    REQUIRE(wrap->tooltipLines.size() == 1);
    CHECK(wrap->tooltipLines[0].wrap);
    CHECK(wrap->tooltipLines[0].lc[1] == Catch::Approx(210.0f / 255.0f));
    REQUIRE(flat->tooltipLines.size() == 1);
    CHECK_FALSE(flat->tooltipLines[0].wrap);
}

TEST_CASE_METHOD(Integration, "Named follow skips non-units and preserves selection", "[follow]") {
    replay::Session session("tbc");
    auto& handler = session.handler();
    lua.setGameHandler(&handler);
    auto player = std::make_shared<wowee::game::Player>(3);
    player->setName("Graulberto");
    handler.getEntityManager().addEntity(3, player);
    handler.getEntityManager().addEntity(4, std::make_shared<wowee::game::Entity>(4));
    handler.getEntityManager().addEntity(5, std::make_shared<wowee::game::GameObject>(5));
    run("FollowUnit('gRaUlBeRtO')");
    CHECK(handler.followTargetGuidRef() == 3);
    CHECK(handler.getTargetGuid() == 0);
    handler.cancelFollow();
    run("FollowUnit('missing player')");
    CHECK(handler.followTargetGuidRef() == 0);
    lua.setGameHandler(nullptr);
}


TEST_CASE_METHOD(Integration, "TBC duel menu sends expansion-correct spell packet", "[duel]") {
    replay::Session session("tbc");
    auto& handler = session.handler();
    lua.setGameHandler(&handler);
    auto player = std::make_shared<wowee::game::Player>(3);
    player->setName("Graulberto");
    handler.getEntityManager().addEntity(3, player);
    handler.setTarget(3);
    run("StartDuel('target', 1)");
    auto packets = session.sent().of(wowee::game::Opcode::CMSG_CAST_SPELL);
    REQUIRE(packets.size() == 1);
    auto& packet = packets.front();
    CHECK(packet.readUInt32() == 7266);
    CHECK(packet.readUInt8() == 1);
    CHECK(packet.readUInt32() == 2);
    CHECK(packet.readPackedGuid() == 3);
    CHECK(packet.getSize() == 11);
    lua.setGameHandler(nullptr);
}


TEST_CASE_METHOD(Integration, "Duel teams override friendly faction only for matching arbiter", "[duel]") {
    replay::Session session("tbc");
    auto& handler = session.handler();
    auto self = std::make_shared<wowee::game::Player>(handler.getPlayerGuid());
    auto opponent = std::make_shared<wowee::game::Player>(3);
    handler.getEntityManager().addEntity(self->getGuid(), self);
    handler.getEntityManager().addEntity(3, opponent);
    handler.setFactionHostileMap({{0, false}});
    handler.setFactionFriendlyMap({{0, true}});
    const auto team = wowee::game::fieldIndex(wowee::game::UF::PLAYER_DUEL_TEAM);
    const auto flag = wowee::game::fieldIndex(wowee::game::UF::PLAYER_DUEL_ARBITER);
    self->setField(flag, 123); opponent->setField(flag, 123);
    CHECK_FALSE(handler.isDuelOpponent(*opponent));
    self->setField(team, 1); opponent->setField(team, 2);
    handler.refreshUnitHostility();
    CHECK(handler.isDuelOpponent(*opponent));
    CHECK(handler.unitReactionToPlayer(*opponent) == 2);
    CHECK(opponent->isHostile());
    lua.setGameHandler(&handler);
    handler.setTarget(opponent->getGuid());
    // Cached presentation state must not override authoritative duel fields.
    opponent->setHostile(false);
    truth("UnitCanAttack('player', 'target') and not UnitIsFriend('player', 'target')");
    lua.setGameHandler(nullptr);
    handler.refreshUnitHostility();
    opponent->setField(flag, 124);
    CHECK_FALSE(handler.isDuelOpponent(*opponent));
    opponent->setField(flag, 123);
    self->setField(team, 0);
    handler.refreshUnitHostility();
    CHECK_FALSE(handler.isDuelOpponent(*opponent));
    CHECK_FALSE(opponent->isHostile());
}

TEST_CASE_METHOD(Integration, "Duel countdown prints localized chat once per second and cancels", "[duel]") {
    run(R"(
        duelClock = 0
        GetTime = function() return duelClock end
        DUEL_COUNTDOWN = "Duel starting: %d"
        duelLines = {}
        DEFAULT_CHAT_FRAME = {AddMessage = function(self, text) table.insert(duelLines, text) end}
    )");
    lua.fireEvent("WOWEE_DUEL_COUNTDOWN", {"3000"});
    truth("#duelLines == 1 and duelLines[1] == 'Duel starting: 3'");
    run("duelClock = 0.5; __WoweeDuelCountdown:GetScript('OnUpdate')(__WoweeDuelCountdown)");
    truth("#duelLines == 1");
    run("duelClock = 1.1; __WoweeDuelCountdown:GetScript('OnUpdate')(__WoweeDuelCountdown)");
    truth("#duelLines == 2 and duelLines[2] == 'Duel starting: 2'");
    run("duelClock = 2.1; __WoweeDuelCountdown:GetScript('OnUpdate')(__WoweeDuelCountdown)");
    truth("#duelLines == 3 and duelLines[3] == 'Duel starting: 1'");
    run("duelClock = 3.1; __WoweeDuelCountdown:GetScript('OnUpdate')(__WoweeDuelCountdown)");
    truth("#duelLines == 3 and not __WoweeDuelCountdown:IsShown()");
    lua.fireEvent("WOWEE_DUEL_COUNTDOWN", {"3000"});
    truth("__WoweeDuelCountdown:IsShown()");
    lua.fireEvent("DUEL_FINISHED");
    truth("not __WoweeDuelCountdown:IsShown()");
}

TEST_CASE_METHOD(Integration, "TBC mailbox default stationery uses the legacy index argument", "[mail-stationery]") {
    replay::Session session("tbc");
    auto& handler = session.handler();
    handler.setInterfaceCommandCallback([this](const std::string& command) { run(command); });
    run(R"(
        StationeryPopupFrame = {}
        StationeryPopupButton_OnClick = function(index)
            assert(index == 1)
            StationeryPopupFrame.selectedIndex = index
        end
    )");
    handler.openMailbox(123);
    truth("StationeryPopupFrame.selectedIndex == 1");
    handler.setInterfaceCommandCallback({});
}

TEST_CASE_METHOD(Integration, "Coin icons match native denomination and ten-coin boundaries", "[mail-coin]") {
    run(R"(
        local cases = {{0,5},{1,5},{9,5},{10,6},{99,6},{100,3},{999,3},
                       {1000,4},{9999,4},{10000,1},{99999,1},{100000,2}}
        for _, case in ipairs(cases) do
            assert(GetCoinIcon(case[1]) == string.format("Interface\\Icons\\INV_Misc_Coin_%02d", case[2]), case[1])
        end
    )");
}

TEST_CASE_METHOD(Integration, "Guard destinations send only gossip selection", "[guard-gossip]") {
    replay::Session session("tbc");
    auto& handler = session.handler();
    for (const std::string text : {"Auction House", "Stable Master", "Bank"}) {
        wowee::network::Packet gossip(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_GOSSIP_MESSAGE));
        gossip.writeUInt64(123);
        gossip.writeUInt32(2121);
        gossip.writeUInt32(0);
        gossip.writeUInt32(1);
        gossip.writeUInt32(0);
        gossip.writeUInt8(0);
        gossip.writeUInt8(0);
        gossip.writeUInt32(0);
        gossip.writeString(text);
        gossip.writeString("");
        gossip.writeUInt32(0);
        session.receive(gossip);
        handler.selectGossipOption(0);
    }
    CHECK(session.sent().of(wowee::game::Opcode::CMSG_GOSSIP_SELECT_OPTION).size() == 3);
    CHECK(session.sent().of(wowee::game::Opcode::MSG_AUCTION_HELLO).empty());
    CHECK(session.sent().of(wowee::game::Opcode::CMSG_BANKER_ACTIVATE).empty());
    CHECK(session.sent().of(wowee::game::Opcode::MSG_LIST_STABLED_PETS).empty());
}

TEST_CASE_METHOD(Integration, "Gossip icon types match authored TBC texture names", "[gossip-icons]") {
    replay::Session session("tbc");
    lua.setGameHandler(&session.handler());
    wowee::network::Packet gossip(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_GOSSIP_MESSAGE));
    gossip.writeUInt64(123);
    gossip.writeUInt32(2121);
    gossip.writeUInt32(0);
    gossip.writeUInt32(3);
    for (uint8_t icon : {4, 5, 10}) {
        gossip.writeUInt32(icon);
        gossip.writeUInt8(icon);
        gossip.writeUInt8(0);
        gossip.writeUInt32(0);
        gossip.writeString("Service");
        gossip.writeString("");
    }
    gossip.writeUInt32(0);
    session.receive(gossip);
    truth("select(2, GetGossipOptions()) == 'healer'");
    truth("select(4, GetGossipOptions()) == 'binder'");
    truth("select(6, GetGossipOptions()) == 'vendor'");
    lua.setGameHandler(nullptr);
}

TEST_CASE_METHOD(Integration, "Attack permission respects immunity and peaceful reputation", "[npc-attack]") {
    replay::Session session("tbc");
    auto& handler = session.handler();
    wowee::game::Unit unit(123);
    unit.setFactionTemplate(1217);
    CHECK(handler.canPlayerAttackUnit(unit));
    unit.setUnitFlags(0x100); // Stormpike Emissary: immune to player attacks.
    CHECK_FALSE(handler.canPlayerAttackUnit(unit));
    unit.setUnitFlags(2);
    CHECK_FALSE(handler.canPlayerAttackUnit(unit));
    unit.setUnitFlags(0);
    handler.setFactionFriendlyMap({{1217, true}});
    CHECK_FALSE(handler.canPlayerAttackUnit(unit));
}

TEST_CASE_METHOD(Integration, "Guard directions replace destination and refresh world map", "[guard-gossip]") {
    replay::Session session("tbc");
    auto& handler = session.handler();
    int updates = 0;
    handler.setAddonEventCallback([&](const std::string& event, const std::vector<std::string>&) {
        if (event == "WORLD_MAP_UPDATE") ++updates;
    });
    for (const std::string name : {"Ironforge Auction House", "Ironforge Bank"}) {
        wowee::network::Packet poi(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_GOSSIP_POI));
        poi.writeUInt32(99);
        poi.writeFloat(-4957.0f);
        poi.writeFloat(-911.604f);
        poi.writeUInt32(6);
        poi.writeUInt32(0);
        poi.writeString(name);
        session.receive(poi);
    }
    REQUIRE(handler.getGossipPois().size() == 1);
    const auto& stored = handler.getGossipPois().front();
    CHECK(stored.name == "Ironforge Bank");
    CHECK(stored.flags == 99);
    CHECK(stored.icon == 6);
    CHECK(stored.questObjectiveIndex == -2);
    // SMSG_GOSSIP_POI wire coords are server (X, Y); stored canonically as (Y, X).
    CHECK(stored.x == Catch::Approx(-911.604f));
    CHECK(stored.y == Catch::Approx(-4957.0f));
    CHECK(updates == 2);

    // Arrival auto-clear: stays present at >= 10 yd, clears at < 10 yd and notifies map.
    auto& move = handler.movementInfoRef();
    move.x = stored.x + 10.0f;
    move.y = stored.y;
    CHECK_FALSE(handler.clearArrivedGossipPois());
    CHECK(handler.getGossipPois().size() == 1);
    CHECK(updates == 2);

    move.x = stored.x + 9.5f;
    move.y = stored.y;
    CHECK(handler.clearArrivedGossipPois());
    CHECK(handler.getGossipPois().empty());
    CHECK(updates == 3);

    // Re-add a guard POI and verify logout/disconnect (clearWorld) clears it.
    {
        wowee::network::Packet poi(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_GOSSIP_POI));
        poi.writeUInt32(99);
        poi.writeFloat(-8839.0f);
        poi.writeFloat(487.546f);
        poi.writeUInt32(6);
        poi.writeUInt32(0);
        poi.writeString("Stormwind Gryphon Master");
        session.receive(poi);
    }
    REQUIRE(handler.getGossipPois().size() == 1);
    handler.disconnect();
    CHECK(handler.getGossipPois().empty());
}

TEST_CASE_METHOD(Integration, "GM stationery identifies support mail without a player sender", "[gm-mail]") {
    replay::Session session("tbc");
    auto& handler = session.handler();
    lua.setGameHandler(&handler);
    wowee::game::MailMessage mail;
    mail.stationeryId = 61;
    mail.money = 1000000;
    handler.mailInboxRef().push_back(mail);
    CHECK(handler.getMailSenderName(mail) == "Customer Support");
    run("header = {GetInboxHeaderInfo(1)}");
    truth("header[3] == 'Customer Support' and header[5] == 1000000 and header[12] == false and header[13] == true");
    mail.senderGuid = 123;
    mail.senderName = "Player";
    CHECK(handler.getMailSenderName(mail) == "Customer Support");
    mail.stationeryId = 41;
    handler.mailInboxRef()[0] = mail;
    run("header = {GetInboxHeaderInfo(1)}");
    truth("header[3] == 'Player' and header[12] == true and header[13] == false");
    lua.setGameHandler(nullptr);
}

TEST_CASE_METHOD(Integration, "TBC joining patrol restores route and elapsed time", "[tbc-patrol]") {
    replay::Session session("tbc");
    using namespace wowee::game;
    constexpr uint64_t guid = 0xf130002a7d0007e0ull;
    wowee::network::Packet packet(wireOpcode(Opcode::SMSG_UPDATE_OBJECT));
    packet.writeUInt32(1); packet.writeUInt8(0);
    packet.writeUInt8(static_cast<uint8_t>(UpdateType::CREATE_OBJECT2));
    packet.writePackedGuid(guid); packet.writeUInt8(static_cast<uint8_t>(ObjectType::UNIT));
    packet.writeUInt8(0x20); packet.writeUInt32(0x08000101); packet.writeUInt8(0);
    packet.writeUInt32(0);
    packet.writeFloat(10); packet.writeFloat(5); packet.writeFloat(501); packet.writeFloat(0);
    packet.writeUInt32(0);
    for (int i = 0; i < 8; ++i) packet.writeFloat(2.5f);
    packet.writeUInt32(0); // linear spline flags
    packet.writeUInt32(6000); packet.writeUInt32(8000); packet.writeUInt32(42);
    packet.writeUInt32(5); // control, start, corner, end, control
    for (const auto& point : std::vector<std::array<float, 3>>{
            {-10,0,501}, {0,0,501}, {10,0,501}, {10,10,501}, {10,10,501}})
        for (float coordinate : point) packet.writeFloat(coordinate);
    packet.writeFloat(10); packet.writeFloat(10); packet.writeFloat(501);
    packet.writeUInt8(0); // no fields
    session.receive(packet);
    auto entity = session.handler().getEntityManager().getEntity(guid);
    REQUIRE(entity);
    CHECK(entity->isActivelyMoving());
    CHECK(entity->getX() == Catch::Approx(5));
    CHECK(entity->getY() == Catch::Approx(10));
    entity->updateMovement(1);
    CHECK(entity->getX() == Catch::Approx(7.5));
    CHECK(entity->getY() == Catch::Approx(10));
    CHECK(entity->getZ() == Catch::Approx(501));

    // Next segment must use the wire start, even if the cached position lags.
    wowee::network::Packet move(wireOpcode(Opcode::SMSG_MONSTER_MOVE));
    move.writePackedGuid(guid);
    move.writeFloat(10); move.writeFloat(10); move.writeFloat(501);
    move.writeUInt32(43); move.writeUInt8(0); move.writeUInt32(0x100);
    move.writeUInt32(4000); move.writeUInt32(1);
    move.writeFloat(20); move.writeFloat(10); move.writeFloat(501);
    session.receive(move);
    entity->updateMovement(2);
    CHECK(entity->getX() == Catch::Approx(10));
    CHECK(entity->getY() == Catch::Approx(15));
}

TEST_CASE_METHOD(Integration, "Learning a spell refreshes stance bar", "[learned-forms]") {
    replay::Session session("tbc");
    std::vector<std::string> events;
    session.handler().setAddonEventCallback([&](const std::string& name, const std::vector<std::string>&) {
        events.push_back(name);
    });
    // Stealth: Spell.dbc EffectApplyAuraName 36 (shapeshift), 16, 33.
    session.handler().spellNameCacheLoadedRef() = true;
    auto& stealth = session.handler().spellNameCacheRef()[1784];
    stealth.name = "Stealth";
    stealth.effectAuraIds[0] = 36;
    session.handler().spellNameCacheRef()[703].name = "Garrote";
    wowee::network::Packet garrote(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_LEARNED_SPELL));
    garrote.writeUInt32(703);
    session.receive(garrote);
    CHECK(std::count(events.begin(), events.end(), "UPDATE_SHAPESHIFT_FORMS") == 0);
    wowee::network::Packet packet(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_LEARNED_SPELL));
    packet.writeUInt32(1784);
    session.receive(packet);
    CHECK(session.handler().getKnownSpells().count(1784) == 1);
    CHECK(std::count(events.begin(), events.end(), "UPDATE_SHAPESHIFT_FORMS") == 1);
    session.handler().setAddonEventCallback({});
}

TEST_CASE("Observed TBC melee dispatches visuals without local combat UI", "[remote-combat]") {
    replay::Session session("tbc");
    auto& handler = session.handler();
    using HR = wowee::game::GameHandler::HitReaction;
    std::vector<uint64_t> swings;
    std::vector<std::pair<uint64_t, HR>> reactions;
    handler.setNpcSwingCallback([&](uint64_t guid, bool) { swings.push_back(guid); });
    handler.setHitReactionCallback([&](uint64_t, uint64_t guid, HR reaction) { reactions.emplace_back(guid, reaction); });
    // CMaNGOS TBC SendAttackStateUpdate: packed participants, one damage
    // component, victim state, unknown, spell ID, blocked amount.
    for (uint32_t state : {1u, 2u, 3u, 5u, 6u, 7u, 8u}) {
        wowee::network::Packet packet(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_ATTACKERSTATEUPDATE));
        packet.writeUInt32(state == 6 ? 0x10 : 0);
        packet.writePackedGuid(3); packet.writePackedGuid(4);
        packet.writeUInt32(state == 1 ? 7 : 0); packet.writeUInt8(1);
        packet.writeUInt32(1); packet.writeFloat(7); packet.writeUInt32(7);
        packet.writeUInt32(0); packet.writeUInt32(0);
        packet.writeUInt32(state); packet.writeUInt32(0); packet.writeUInt32(0); packet.writeUInt32(0);
        session.receive(packet);
    }
    CHECK(swings == std::vector<uint64_t>(7, 3));
    REQUIRE(reactions.size() == 4);
    CHECK(reactions[0] == std::pair<uint64_t, HR>{4, HR::WOUND});
    CHECK(reactions[1] == std::pair<uint64_t, HR>{4, HR::DODGE});
    CHECK(reactions[2] == std::pair<uint64_t, HR>{4, HR::PARRY});
    CHECK(reactions[3] == std::pair<uint64_t, HR>{4, HR::BLOCK});
    CHECK(handler.getCombatText().empty());
    CHECK(handler.getTargetGuid() == 0);
}

TEST_CASE("A creature swing holds its melee stance without SMSG_ATTACKSTART", "[remote-combat]") {
    // CMaNGOS sends ATTACKSTART once, when the attack begins. A creature
    // already fighting when it came into view has only its swings; build 8606
    // holds its ready stance from the first one. The off-hand swing is flagged.
    replay::Session session("tbc");
    auto& handler = session.handler();
    auto creature = std::make_shared<wowee::game::Unit>(3);
    handler.getEntityManager().addEntity(3, creature);
    std::vector<bool> offHands;
    handler.setNpcSwingCallback([&](uint64_t, bool offHand) { offHands.push_back(offHand); });
    REQUIRE(creature->getMeleeVictimGuid() == 0);
    for (uint32_t hitInfo : {0x2u, 0x4u}) {
        wowee::network::Packet packet(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_ATTACKERSTATEUPDATE));
        packet.writeUInt32(hitInfo);
        packet.writePackedGuid(3); packet.writePackedGuid(4);
        packet.writeUInt32(7); packet.writeUInt8(1);
        packet.writeUInt32(1); packet.writeFloat(7); packet.writeUInt32(7);
        packet.writeUInt32(0); packet.writeUInt32(0);
        packet.writeUInt32(1); packet.writeUInt32(0); packet.writeUInt32(0); packet.writeUInt32(0);
        session.receive(packet);
    }
    CHECK(creature->getMeleeVictimGuid() == 4);
    CHECK(offHands == std::vector<bool>{false, true});
}

TEST_CASE("TBC spell go carries a creature's ammo display", "[remote-combat]") {
    // CMaNGOS Spell::SendSpellGo/WriteAmmoToPacket: CAST_FLAG_AMMO (0x20)
    // appends the ammo display (5996 arrow for a creature's crossbow) and its
    // inventory type (24) after the cast targets.
    auto parsers = wowee::game::createPacketParsers("tbc");
    wowee::network::Packet go(0);
    go.writePackedGuid(3); go.writePackedGuid(3);
    go.writeUInt32(29575); go.writeUInt16(0x120); go.writeUInt32(1000);
    go.writeUInt8(1); go.writeUInt64(4); go.writeUInt8(0);
    go.writeUInt32(0x2); go.writePackedGuid(4);  // TARGET_FLAG_UNIT
    go.writeUInt32(5996); go.writeUInt32(24);
    wowee::game::SpellGoData data;
    REQUIRE(parsers->parseSpellGo(go, data));
    CHECK(data.spellId == 29575);
    CHECK(data.hitTargets == std::vector<uint64_t>{4});
    CHECK(data.ammoDisplayId == 5996);
    CHECK(data.ammoInventoryType == 24);
}

TEST_CASE("Attack start preserves both participants movement facing", "[remote-facing]") {
    replay::Session session("tbc");
    auto& handler = session.handler();
    auto wolf = std::make_shared<wowee::game::Unit>(3);
    auto player = std::make_shared<wowee::game::Player>(4);
    wolf->setPosition(10, 5, 0, .4f);
    player->setPosition(0, 0, 0, 1.7f);
    handler.getEntityManager().addEntity(3, wolf);
    handler.getEntityManager().addEntity(4, player);
    wowee::network::Packet packet(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_ATTACKSTART));
    packet.writeUInt64(3); packet.writeUInt64(4);
    session.receive(packet);
    CHECK(wolf->getOrientation() == Catch::Approx(.4f));
    CHECK(player->getOrientation() == Catch::Approx(1.7f));
}

namespace {
// A relayed TBC movement packet of another player, standing (no fall block).
wowee::network::Packet remoteMove(wowee::game::Opcode opcode, uint64_t guid, uint32_t flags,
                                  uint32_t time, float orientation) {
    wowee::network::Packet packet(wowee::game::wireOpcode(opcode));
    packet.writePackedGuid(guid);
    packet.writeUInt32(flags);
    packet.writeUInt8(0);
    packet.writeUInt32(time);
    for (float value : {1222.47f, -10235.2f, 43.6274f, orientation}) packet.writeFloat(value);
    packet.writeUInt32(0);
    return packet;
}
} // namespace

// Native Edrikb's keyboard turn as WoWee received it
// (_experiments/20261118T2030.remote_turn, native_key_a): START_TURN_LEFT,
// no heartbeat, STOP_TURN 1025 ms later with the final facing.
TEST_CASE("A remote keyboard turn turns at the turn rate between start and stop", "[remote-turn]") {
    using namespace wowee::game;
    using wowee::core::coords::serverToCanonicalYaw;
    replay::Session session("tbc");
    auto& handler = session.handler();
    constexpr uint64_t guid = 19;
    auto player = std::make_shared<Player>(guid);
    player->setPosition(-10235.2f, 1222.47f, 43.6274f, serverToCanonicalYaw(0.0f));
    player->turnRate = 3.141593f;
    handler.getEntityManager().addEntity(guid, player);
    session.receive(remoteMove(Opcode::MSG_MOVE_START_TURN_LEFT, guid, 0x10, 24028517, 0.0f));
    // The frame that read the packet: its time passed before the packet came.
    player->updateMovement(0.1f);
    CHECK(player->getOrientation() == Catch::Approx(serverToCanonicalYaw(0.0f)));
    for (int frame = 0; frame < 20; ++frame) player->updateMovement(0.025f);
    CHECK(player->getOrientation() == Catch::Approx(serverToCanonicalYaw(0.5f * 3.141593f)).margin(1e-4));
    CHECK_FALSE(player->isActivelyMoving());
    for (int frame = 0; frame < 20; ++frame) player->updateMovement(0.025f);
    session.receive(remoteMove(Opcode::MSG_MOVE_STOP_TURN, guid, 0, 24029542, 3.220f));
    for (int frame = 0; frame < 20; ++frame) player->updateMovement(0.025f);
    CHECK(player->getOrientation() == Catch::Approx(serverToCanonicalYaw(3.220f)).margin(1e-4));
    const float settled = player->getOrientation();
    player->updateMovement(1.0f);
    CHECK(player->getOrientation() == settled);
}

// At 4 fps a frame's time mostly passed before the packet came; the stop must
// not be reached from beyond (a turn back would shuffle the other way).
TEST_CASE("A remote keyboard turn at a low frame rate stops without turning back", "[remote-turn]") {
    using namespace wowee::game;
    using wowee::core::coords::serverToCanonicalYaw;
    using wowee::core::coords::normalizeAngleRad;
    replay::Session session("tbc");
    auto& handler = session.handler();
    constexpr uint64_t guid = 19;
    auto player = std::make_shared<Player>(guid);
    player->setPosition(-10235.2f, 1222.47f, 43.6274f, serverToCanonicalYaw(3.164f));
    player->turnRate = 3.141593f;
    handler.getEntityManager().addEntity(guid, player);
    session.receive(remoteMove(Opcode::MSG_MOVE_START_TURN_LEFT, guid, 0x10, 28250394, 3.164f));
    float previous = player->getOrientation();
    for (int frame = 0; frame < 4; ++frame) {
        player->updateMovement(0.25f);
        CHECK(normalizeAngleRad(player->getOrientation() - previous) >= 0.0f);
        previous = player->getOrientation();
    }
    session.receive(remoteMove(Opcode::MSG_MOVE_STOP_TURN, guid, 0, 28251370, 6.231f));
    for (int frame = 0; frame < 4; ++frame) {
        player->updateMovement(0.25f);
        CHECK(normalizeAngleRad(player->getOrientation() - previous) >= 0.0f);
        previous = player->getOrientation();
    }
    CHECK(normalizeAngleRad(player->getOrientation() - serverToCanonicalYaw(6.231f)) == Catch::Approx(0.0f).margin(1e-4));
}

TEST_CASE("A remote right turn turns clockwise", "[remote-turn]") {
    using namespace wowee::game;
    using wowee::core::coords::serverToCanonicalYaw;
    replay::Session session("tbc");
    auto& handler = session.handler();
    constexpr uint64_t guid = 19;
    auto player = std::make_shared<Player>(guid);
    player->setPosition(-10235.2f, 1222.47f, 43.6274f, serverToCanonicalYaw(3.220f));
    player->turnRate = 3.141593f;
    handler.getEntityManager().addEntity(guid, player);
    session.receive(remoteMove(Opcode::MSG_MOVE_START_TURN_RIGHT, guid, 0x20, 24030843, 3.220f));
    player->updateMovement(0.1f);
    player->updateMovement(0.25f);
    CHECK(player->getOrientation() == Catch::Approx(serverToCanonicalYaw(3.220f - 0.25f * 3.141593f)).margin(1e-4));
}

// Native mouse-look as WoWee received it (native_look): a SET_FACING per mouse
// step, about 100 ms apart, 0.118 rad each. Native draws the turn smoothly.
TEST_CASE("Remote mouse-look facing steps are drawn as a smooth turn", "[remote-turn]") {
    using namespace wowee::game;
    using wowee::core::coords::serverToCanonicalYaw;
    using wowee::core::coords::normalizeAngleRad;
    replay::Session session("tbc");
    auto& handler = session.handler();
    constexpr uint64_t guid = 19;
    auto player = std::make_shared<Player>(guid);
    player->setPosition(-10235.2f, 1222.47f, 43.6274f, serverToCanonicalYaw(0.628f));
    player->turnRate = 3.141593f;
    handler.getEntityManager().addEntity(guid, player);
    const std::pair<uint32_t, float> stream[] = {{24319429, 0.510f}, {24319530, 0.392f}, {24319635, 0.274f},
                                                 {24319748, 0.156f}, {24319847, 0.038f}, {24319947, 6.204f}};
    float largestStep = 0.0f;
    uint32_t previousTime = stream[0].first - 100;
    for (const auto& [time, facing] : stream) {
        session.receive(remoteMove(Opcode::MSG_MOVE_SET_FACING, guid, 0, time, facing));
        // Drawn between packets in 10 ms frames: no frame jumps a whole step.
        for (uint32_t elapsed = 0; elapsed < time - previousTime; elapsed += 10) {
            const float before = player->getOrientation();
            player->updateMovement(0.01f);
            largestStep = std::max(largestStep, std::abs(normalizeAngleRad(player->getOrientation() - before)));
        }
        previousTime = time;
    }
    CHECK(largestStep < 0.118f / 4.0f);
    player->updateMovement(0.5f);
    CHECK(normalizeAngleRad(player->getOrientation() - serverToCanonicalYaw(6.204f)) == Catch::Approx(0.0f).margin(1e-4));
}

TEST_CASE("A remote teleport sets facing at once", "[remote-turn]") {
    using namespace wowee::game;
    using wowee::core::coords::serverToCanonicalYaw;
    replay::Session session("tbc");
    auto& handler = session.handler();
    constexpr uint64_t guid = 19;
    auto player = std::make_shared<Player>(guid);
    player->setPosition(-10235.2f, 1222.47f, 43.6274f, serverToCanonicalYaw(0.0f));
    player->turnRate = 3.141593f;
    handler.getEntityManager().addEntity(guid, player);
    session.receive(remoteMove(Opcode::MSG_MOVE_START_TURN_LEFT, guid, 0x10, 1000, 0.0f));
    session.receive(remoteMove(Opcode::MSG_MOVE_TELEPORT, guid, 0, 1100, 2.0f));
    CHECK(player->getOrientation() == Catch::Approx(serverToCanonicalYaw(2.0f)));
    player->updateMovement(0.5f);
    CHECK(player->getOrientation() == Catch::Approx(serverToCanonicalYaw(2.0f)));
}

TEST_CASE("Skill gain uses SKILL chat category", "[skill-chat]") {
    replay::Session session("tbc");
    auto& handler = session.handler();
    const auto base = wowee::game::fieldIndex(wowee::game::UF::PLAYER_SKILL_INFO_START);
    wowee::game::FlatFieldMap fields;
    fields[base] = 95;
    fields[base + 1] = (200u << 16) | 40;
    handler.extractSkillFields(fields);
    const auto before = handler.getChatHistory().size();
    fields[base + 1] = (200u << 16) | 41;
    handler.extractSkillFields(fields);
    REQUIRE(handler.getChatHistory().size() == before + 1);
    CHECK(handler.getChatHistory().back().type == wowee::game::ChatType::SKILL);
}

TEST_CASE_METHOD(Integration, "Untrained corpse tooltip exposes red skinning requirement", "[skinning][tooltip]") {
    using namespace wowee::game;
    replay::Session session("tbc");
    auto& handler = session.handler();
    lua.setGameHandler(&handler);
    constexpr uint64_t guid = 0xf13000087a000123ull;
    auto corpse = std::make_shared<Unit>(guid);
    corpse->setName("Thistle Bear");
    corpse->setEntry(2163);
    corpse->setLevel(12);
    corpse->setHealth(0);
    corpse->setMaxHealth(200);
    corpse->setUnitFlags(UNIT_FLAG_SKINNABLE);
    corpse->setField(fieldIndex(UF::UNIT_FIELD_LEVEL), 12);
    handler.getEntityManager().addEntity(guid, corpse);
    wowee::network::Packet info(wireOpcode(Opcode::SMSG_CREATURE_QUERY_RESPONSE));
    info.writeUInt32(2163);
    info.writeString("Thistle Bear");
    for (int i = 0; i < 5; ++i) info.writeString("");
    info.writeUInt32(0);
    info.writeUInt32(1); // Beast
    for (int i = 0; i < 8; ++i) info.writeUInt32(0);
    info.writeFloat(1.0f);
    info.writeFloat(1.0f);
    info.writeUInt8(0);
    session.receive(info);
    handler.setTarget(guid);
    run(R"lua(
        UNIT_SKINNABLE_LEATHER = 'Skinnable'
        CORPSE = 'Corpse'
        TOOLTIP_UNIT_LEVEL = 'Level %s'
        TOOLTIP_UNIT_LEVEL_CLASS = 'Level %s %s'
        TOOLTIP_UNIT_LEVEL_CLASS_TYPE = 'Level %s %s (%s)'
        TOOLTIP_UNIT_LEVEL_TYPE = 'Level %s (%s)'
        TOOLTIP_UNIT_LEVEL_RACE_CLASS_TYPE = 'Level %s %s %s (%s)'
        ELITE = 'Elite'
        BOSS = 'Boss'
        PLAYER = 'Player'
        skinTip = CreateFrame('GameTooltip', 'SkinTip', UIParent)
        assert(skinTip:SetUnit('target'))
        assert(select(2, skinTip:GetUnit()) == 'target')
        assert(not SkinTipStatusBar:IsShown())
    )lua");
    const auto* tip = lua.widgets().findByName("SkinTip");
    REQUIRE(tip);
    REQUIRE(tip->tooltipLines.size() == 3);
    CHECK(tip->tooltipLines[1].left == "Level 12 Corpse");
    CHECK(tip->tooltipLines.back().left == "Skinnable");
    CHECK(tip->tooltipLines.back().lc[0] == 1.0f);
    CHECK(tip->tooltipLines.back().lc[1] == 0.1f);
    corpse->setUnitFlags(0);
    run("assert(skinTip:SetUnit('target'))");
    CHECK(tip->tooltipLines.size() == 2);
    corpse->setHealth(200);
    run("assert(skinTip:SetUnit('target'))");
    REQUIRE(tip->tooltipLines.size() == 2);
    CHECK(tip->tooltipLines[1].left == "Level 12 Beast");
    CHECK_FALSE(tip->tooltipLines[1].wrap);
    lua.setGameHandler(nullptr);
}

// Build 8606 UnitIsCorpse looks the token's guid up with TYPEMASK_CORPSE only,
// so TargetFrame_CheckLevel shows a dead creature's level, not the skull.
TEST_CASE_METHOD(Integration, "Dead creature target is not a corpse object and keeps its level", "[corpse][target]") {
    using namespace wowee::game;
    replay::Session session("tbc");
    auto& handler = session.handler();
    lua.setGameHandler(&handler);
    constexpr uint64_t guid = 0xf1300016df000075ull;  // Magma Elemental, entry 5855
    auto elemental = std::make_shared<Unit>(guid);
    elemental->setName("Magma Elemental");
    elemental->setEntry(5855);
    elemental->setLevel(46);
    elemental->setHealth(0);
    elemental->setMaxHealth(2000);
    elemental->setField(fieldIndex(UF::UNIT_FIELD_LEVEL), 46);
    handler.getEntityManager().addEntity(guid, elemental);
    handler.setTarget(guid);
    truth("UnitIsDead('target')");
    truth("not UnitIsCorpse('target')");
    truth("UnitLevel('target') == 46");
    constexpr uint64_t corpseGuid = 0xf101000000000075ull;
    handler.getEntityManager().addEntity(corpseGuid, std::make_shared<Entity>(corpseGuid));
    handler.getEntityManager().getEntity(corpseGuid)->setType(ObjectType::CORPSE);
    handler.setMouseoverGuid(corpseGuid);
    truth("UnitIsCorpse('mouseover')");
    handler.setMouseoverGuid(0);
    lua.setGameHandler(nullptr);
}

// CGUnit_C::CanInteract(GO): the BRD rune doodads carry FactionTemplate 114,
// whose enemy group is players, so they take neither cursor nor click.
TEST_CASE_METHOD(Integration, "Game objects hostile to the player cannot be used", "[faction][quest-object]") {
    using namespace wowee::game;
    replay::Session session("tbc");
    auto& handler = session.handler();
    handler.setFactionObjectReactionMap({{114, 2}, {94, 4}, {35, 5}});
    handler.setFactionHostileMap({{0, false}});
    handler.setFactionFriendlyMap({{0, true}});
    const auto faction = fieldIndex(UF::GAMEOBJECT_FACTION);
    const auto createdBy = fieldIndex(UF::GAMEOBJECT_CREATED_BY);
    REQUIRE(faction == 20);
    REQUIRE(createdBy == 6);
    auto rune = std::make_shared<GameObject>(0xf110029810000280ull);
    rune->setEntry(170584);  // Doodad_DarkIronDwarfRune_G01, type 1 BUTTON, flags 0
    rune->setField(faction, 114);
    handler.getEntityManager().addEntity(rune->getGuid(), rune);
    CHECK(handler.gameObjectReactionToPlayer(*rune) == 2);
    CHECK_FALSE(handler.canInteractWithGameObject(*rune));
    rune->setField(faction, 94);   // treasure chests: neutral
    CHECK(handler.canInteractWithGameObject(*rune));
    rune->setField(faction, 0);    // no faction: neutral
    CHECK(handler.canInteractWithGameObject(*rune));
    // An object a unit created takes its creator's reaction over its own
    // template's (CGGameObject_C::ObjectReaction).
    constexpr uint64_t ownerGuid = 0xf13000087a000999ull;
    auto owner = std::make_shared<Unit>(ownerGuid);
    owner->setFactionTemplate(0);
    handler.getEntityManager().addEntity(ownerGuid, owner);
    rune->setField(faction, 114);
    rune->setField(createdBy, static_cast<uint32_t>(ownerGuid));
    rune->setField(createdBy + 1, static_cast<uint32_t>(ownerGuid >> 32));
    CHECK(handler.gameObjectReactionToPlayer(*rune) == 5);
    CHECK(handler.canInteractWithGameObject(*rune));
    // A template absent from FactionTemplate.dbc is neutral: native lets the
    // player use Blackened Urn 194092 (faction 1924, not in 2.4.3's table).
    rune->setField(createdBy, 0);
    rune->setField(createdBy + 1, 0);
    rune->setField(faction, 999999);
    CHECK(handler.gameObjectReactionToPlayer(*rune) == 4);
}

TEST_CASE_METHOD(Integration, "Companion tooltips show their owner instead of player faction metadata", "[companion][tooltip]") {
    using namespace wowee::game;
    replay::Session session("tbc");
    auto& handler = session.handler();
    lua.setGameHandler(&handler);
    constexpr uint64_t ownerGuid = 123;
    constexpr uint64_t petGuid = 0xf140000000000456ull;
    constexpr uint32_t entry = 21010;
    auto owner = std::make_shared<Player>(ownerGuid);
    owner->setName("Edrik");
    handler.getEntityManager().addEntity(ownerGuid, owner);
    auto pet = std::make_shared<Unit>(petGuid);
    pet->setName("Blue Moth");
    pet->setEntry(entry);
    pet->setField(fieldIndex(UF::UNIT_FIELD_LEVEL), 1);
    // Mini pets carry CREATEDBY but omit SUMMONEDBY (Pet::SetOwnerGuid).
    pet->setField(fieldIndex(UF::UNIT_FIELD_CREATEDBY_LO), ownerGuid);
    pet->setField(fieldIndex(UF::UNIT_FIELD_CREATEDBY_HI), 0);
    pet->setField(fieldIndex(UF::UNIT_FIELD_FLAGS), UNIT_FLAG_PLAYER_CONTROLLED);
    // A companion takes its owner's faction template (Human, 1): the player is
    // its friend, so its level line names no type (native Bombay: "Level 1").
    handler.setFactionFriendlyMap({{1, true}});
    handler.setFactionHostileMap({{1, false}});
    pet->setFactionTemplate(1);
    handler.getEntityManager().addEntity(petGuid, pet);
    // CMaNGOS QueryHandler::HandleCreatureQueryOpcode, TBC response layout.
    wowee::network::Packet info(wireOpcode(Opcode::SMSG_CREATURE_QUERY_RESPONSE));
    info.writeUInt32(entry);
    info.writeString("Blue Moth");
    for (int i = 0; i < 5; ++i) info.writeString("");
    info.writeUInt32(0); // Type flags
    info.writeUInt32(12); // Non-combat Pet
    for (int i = 0; i < 8; ++i) info.writeUInt32(0);
    info.writeFloat(1.0f);
    info.writeFloat(1.0f);
    info.writeUInt8(0);
    session.receive(info);
    REQUIRE(handler.getCreatureType(entry) == 12);
    handler.setTarget(petGuid);
    run(R"lua(
        UNITNAME_TITLE_COMPANION = "%s's Companion"
        TOOLTIP_UNIT_LEVEL = 'Level %s'
        TOOLTIP_UNIT_LEVEL_CLASS = 'Level %s %s'
        TOOLTIP_UNIT_LEVEL_CLASS_TYPE = 'Level %s %s (%s)'
        TOOLTIP_UNIT_LEVEL_TYPE = 'Level %s (%s)'
        TOOLTIP_UNIT_LEVEL_RACE_CLASS_TYPE = 'Level %s %s %s (%s)'
        ELITE = 'Elite'
        BOSS = 'Boss'
        PLAYER = 'Player'
        companionTip = CreateFrame("GameTooltip", "CompanionTip", UIParent)
        assert(companionTip:SetUnit("target"))
        assert(UnitPlayerControlled("target"))
        assert(not UnitIsPlayer("target"))
        assert(GetCVarDefault("UnitNameCompanionName") == "1")
    )lua");
    const auto* tip = lua.widgets().findByName("CompanionTip");
    REQUIRE(tip);
    REQUIRE(tip->tooltipLines.size() == 3);
    CHECK(tip->tooltipLines[0].left == "Blue Moth");
    CHECK(tip->tooltipLines[1].left == "Edrik's Companion");
    CHECK(tip->tooltipLines[2].left == "Level 1");
    lua.setGameHandler(nullptr);
}

TEST_CASE_METHOD(Integration, "Profession trainer recipe headers preserve service indices", "[trainer]") {
    replay::Session session("tbc");
    auto& handler = session.handler();
    handler.spellNameCacheLoadedRef() = true;
    handler.spellNameCacheRef()[2582].name = "Journeyman Miner";
    handler.spellNameCacheRef()[2658].name = "Smelt Silver";
    handler.spellNameCacheRef()[2658].createdItemId = 2842;
    handler.spellNameCacheRef()[2659].name = "Smelt Bronze";
    handler.spellNameCacheRef()[2659].createdItemId = 2841;
    handler.spellToSkillLineRef()[2658] = 186;
    handler.spellToSkillLineRef()[2659] = 186;
    lua.setGameHandler(&handler);
    run("TRADESKILL_SERVICE_LEARN = 'Recipes'; TRADESKILL_SERVICE_STEP = 'Development Skills'");
    run("SetTrainerServiceTypeFilter('available', 1); SetTrainerServiceTypeFilter('unavailable', 1)");
    wowee::network::Packet list(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_TRAINER_LIST));
    list.writeUInt64(123);
    list.writeUInt32(2);
    list.writeUInt32(3);
    for (const uint32_t spell : {2658u, 2582u, 2659u}) {
        list.writeUInt32(spell);
        list.writeUInt8(0);
        list.writeUInt32(spell);
        list.writeUInt32(0);
        list.writeUInt32(0);
        list.writeUInt8(0);
        list.writeUInt32(186);
        list.writeUInt32(spell == 2582 ? 50 : spell == 2659 ? 65 : 75);
        for (int i = 0; i < 3; ++i) list.writeUInt32(0);
    }
    list.writeString("Mining");
    session.receive(list);
    truth("GetNumTrainerServices() == 5");
    truth("GetTrainerSelectionIndex() == 2");
    handler.playerSkillsRef()[186].value = 1;
    truth("GetNumTrainerServices() == 5");
    truth("GetTrainerServiceInfo(1) == 'Development Skills'");
    truth("GetTrainerServiceInfo(2) == 'Journeyman Miner'");
    truth("GetTrainerServiceInfo(3) == 'Recipes'");
    truth("select(3, GetTrainerServiceInfo(3)) == 'header'");
    truth("GetTrainerServiceInfo(4) == 'Smelt Bronze'");
    truth("GetTrainerServiceCost(4) == 2659");
    run("CollapseTrainerSkillLine(3)");
    truth("GetNumTrainerServices() == 3");
    truth("select('#', GetTrainerServiceInfo(3)) == 3");
    run("CollapseTrainerSkillLine(1)");
    truth("GetNumTrainerServices() == 2");
    truth("GetTrainerServiceInfo(2) == 'Recipes'");
    run("ExpandTrainerSkillLine(0)");
    truth("GetTrainerServiceCost(5) == 2658");
    // Native also groups the initial profession ability under Recipes. It
    // creates no item, consumes no reagent and requires no existing skill.
    handler.playerSkillsRef().clear();
    handler.spellNameCacheRef()[2575].name = "Mining";
    handler.spellToSkillLineRef()[2575] = 186;
    wowee::network::Packet apprentice(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_TRAINER_LIST));
    apprentice.writeUInt64(123);
    apprentice.writeUInt32(2);
    apprentice.writeUInt32(1);
    apprentice.writeUInt32(2575);
    apprentice.writeUInt8(0);
    apprentice.writeUInt32(10);
    apprentice.writeUInt32(1);
    apprentice.writeUInt32(1);
    apprentice.writeUInt8(5);
    for (int i = 0; i < 5; ++i) apprentice.writeUInt32(0);
    apprentice.writeString("Mining");
    session.receive(apprentice);
    truth("GetNumTrainerServices() == 2");
    truth("GetTrainerServiceInfo(1) == 'Recipes'");
    truth("select(3, GetTrainerServiceInfo(1)) == 'header'");
    truth("GetTrainerServiceInfo(2) == 'Mining'");
    truth("GetTrainerServiceCost(2) == 10");
    lua.setGameHandler(nullptr);
}

// Native 2.4.3 at Pikkle (Gadgetzan mining) with only "available" showing and
// nothing to learn: _experiments/20261118T191500Z.trainer_empty_list
// (native-F.txt). The count is 0 but the rows past it are the trainer's.
TEST_CASE_METHOD(Integration, "Trainer rows past the shown count are the rest of the list", "[trainer]") {
    replay::Session session("tbc");
    auto& handler = session.handler();
    handler.spellNameCacheLoadedRef() = true;
    handler.spellNameCacheRef()[2575].name = "Mining";
    handler.spellNameCacheRef()[2575].rank = "Apprentice";
    handler.spellNameCacheRef()[2582].name = "Journeyman Miner";
    handler.spellNameCacheRef()[3304].name = "Smelt Tin";
    handler.spellNameCacheRef()[2659].name = "Smelt Bronze";
    for (const uint32_t spell : {2575u, 3304u, 2659u}) handler.spellToSkillLineRef()[spell] = 186;
    handler.playerSkillsRef()[186].value = 1;
    lua.setGameHandler(&handler);
    run("TRADESKILL_SERVICE_LEARN = 'Recipes'; TRADESKILL_SERVICE_STEP = 'Development Skills'");
    run("SetTrainerServiceTypeFilter('available', 1); SetTrainerServiceTypeFilter('unavailable', 0); SetTrainerServiceTypeFilter('used', 0)");
    wowee::network::Packet list(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_TRAINER_LIST));
    list.writeUInt64(8128);
    list.writeUInt32(2);
    const struct { uint32_t id; uint8_t state; uint32_t skill; } services[] = {
        {2582, 1, 50}, {2659, 1, 65}, {3304, 1, 65}, {2575, 2, 0},
    };
    list.writeUInt32(4);
    for (const auto& sv : services) {
        list.writeUInt32(sv.id); list.writeUInt8(sv.state); list.writeUInt32(10);
        list.writeUInt32(0); list.writeUInt32(0); list.writeUInt8(0);
        list.writeUInt32(sv.skill ? 186 : 0); list.writeUInt32(sv.skill);
        for (int i = 0; i < 3; ++i) list.writeUInt32(0);
    }
    list.writeString("You have not lived until you have dug deep into the earth.");
    session.receive(list);
    handler.getSpellHandler()->addKnownSpell(2575);

    truth("GetNumTrainerServices() == 0");
    truth("GetTrainerSelectionIndex() == 2");
    truth("GetTrainerServiceInfo(1) == 'Development Skills' and select(3, GetTrainerServiceInfo(1)) == 'header'");
    truth("GetTrainerServiceInfo(2) == 'Journeyman Miner' and select(3, GetTrainerServiceInfo(2)) == 'unavailable'");
    truth("GetTrainerServiceInfo(3) == 'Recipes'");
    truth("GetTrainerServiceInfo(4) == 'Mining' and select(3, GetTrainerServiceInfo(4)) == 'used'");
    truth("GetTrainerServiceCost(2) == 10");
    truth("select('#', GetTrainerServiceInfo(7)) == 4 and GetTrainerServiceInfo(7) == nil");
    // Blizzard_TrainerUI's ClassTrainer_SelectFirstLearnableSkill with
    // nothing shown: the selected row is a service, so no header is folded.
    truth("select(3, GetTrainerServiceInfo(GetTrainerSelectionIndex())) == 'unavailable'");
    run("SelectTrainerService(5)");
    truth("GetTrainerSelectionIndex() == 5");
    run("SelectTrainerService(3)");
    truth("select('#', GetTrainerSelectionIndex()) == 0");
    run("CollapseTrainerSkillLine(1)");
    truth("GetNumTrainerServices() == 0 and select('#', GetTrainerServiceInfo(1)) == 3");
    run("ExpandTrainerSkillLine(0)");

    // Shown rows come first, the rest after them.
    run("SetTrainerServiceTypeFilter('used', 1)");
    truth("GetNumTrainerServices() == 2");
    truth("GetTrainerServiceInfo(1) == 'Recipes' and GetTrainerServiceInfo(2) == 'Mining'");
    truth("GetTrainerServiceInfo(3) == 'Development Skills' and GetTrainerServiceInfo(4) == 'Journeyman Miner'");
    truth("GetTrainerSelectionIndex() == 2");
    lua.setGameHandler(nullptr);
}

// Native 2.4.3 answers, measured at Fenthwick on the original client:
// _experiments/20261117T203052Z.trainer_skill_headers (native-1.txt, native-3.txt).
TEST_CASE_METHOD(Integration, "Class trainer services sit under skill line headers", "[trainer]") {
    using namespace wowee::game;
    replay::Session session("tbc");
    auto& handler = session.handler();
    handler.spellNameCacheLoadedRef() = true;
    handler.skillLineAbilityLoadedRef() = true;
    handler.skillLineDbcLoadedRef() = true;
    const struct { uint32_t id; const char* name; const char* rank; uint32_t attr; uint32_t line; } spells[] = {
        {2098, "Eviscerate", "Rank 1", 0x50010, 253}, {6760, "Eviscerate", "Rank 2", 0x50010, 253},
        {703, "Garrote", "Rank 1", 0x270010, 253}, {8631, "Garrote", "Rank 2", 0x270010, 253},
        {1784, "Stealth", "Rank 1", 0x1a150010, 39}, {3127, "Parry", "Passive", 0x50, 95},
        {3420, "Crippling Poison", "Rank 1", 0x10020, 40}, {133, "Fireball", "Rank 1", 0x10000, 8},
    };
    for (const auto& sp : spells) {
        auto& entry = handler.spellNameCacheRef()[sp.id];
        entry.name = sp.name;
        entry.rank = sp.rank;
        entry.attr = sp.attr;
        handler.spellToSkillLineRef()[sp.id] = sp.line;
    }
    handler.skillLineNamesRef()[253] = "Assassination";
    handler.skillLineNamesRef()[39] = "Subtlety";
    handler.skillLineNamesRef()[95] = "Defense";
    handler.skillLineNamesRef()[40] = "Poisons";
    handler.serverPlayerLevelRef() = 70;
    handler.getSpellHandler()->addKnownSpell(2098);
    handler.setGlobalStringLookup([](const char* name) -> std::string {
        const std::string n = name;
        if (n == "ERR_LEARN_ABILITY_S") return "You have learned a new ability: %s.";
        if (n == "ERR_LEARN_SPELL_S") return "You have learned a new spell: %s.";
        if (n == "ERR_LEARN_RECIPE_S") return "You have learned how to create a new item: %s.";
        return {};
    });
    std::vector<std::string> events;
    handler.setAddonEventCallback([&](const std::string& name, const std::vector<std::string>&) {
        events.push_back(name);
    });
    lua.setGameHandler(&handler);
    run("SetTrainerServiceTypeFilter('available', 1); SetTrainerServiceTypeFilter('unavailable', 1); SetTrainerServiceTypeFilter('used', 0)");
    CHECK(std::count(events.begin(), events.end(), "TRAINER_UPDATE") == 3);

    // CMaNGOS SendTrainerSpellHelper: spell, state (0 green, 1 red), cost,
    // two point costs, level, skill, skill value, three prerequisites.
    wowee::network::Packet list(wireOpcode(Opcode::SMSG_TRAINER_LIST));
    list.writeUInt64(77);
    list.writeUInt32(0);
    const struct { uint32_t id; uint8_t state; uint8_t level; uint32_t prev; } services[] = {
        {8631, 1, 22, 703}, {1784, 0, 1, 0}, {3127, 0, 12, 0}, {703, 0, 14, 0}, {6760, 0, 8, 2098},
    };
    list.writeUInt32(5);
    for (const auto& sv : services) {
        list.writeUInt32(sv.id); list.writeUInt8(sv.state); list.writeUInt32(100);
        list.writeUInt32(0); list.writeUInt32(0); list.writeUInt8(sv.level);
        list.writeUInt32(0); list.writeUInt32(0);
        list.writeUInt32(sv.prev); list.writeUInt32(0); list.writeUInt32(0);
    }
    list.writeString("Hello, rogue!  Ready for some training?");
    const wowee::network::Packet reopened = list;
    session.receive(list);

    truth("GetNumTrainerServices() == 8");
    truth("GetTrainerSelectionIndex() == 2");
    run("n1, r1, t1, e1 = GetTrainerServiceInfo(1)");
    truth("n1 == 'Assassination' and r1 == '' and t1 == 'header' and e1 == 1");
    truth("GetTrainerServiceInfo(2) == 'Eviscerate' and select(2, GetTrainerServiceInfo(2)) == 'Rank 2'");
    truth("GetTrainerServiceInfo(3) == 'Garrote' and select(3, GetTrainerServiceInfo(3)) == 'available'");
    truth("select(2, GetTrainerServiceInfo(4)) == 'Rank 2' and select(3, GetTrainerServiceInfo(4)) == 'unavailable'");
    truth("GetTrainerServiceInfo(5) == 'Defense' and GetTrainerServiceInfo(6) == 'Parry'");
    truth("GetTrainerServiceInfo(7) == 'Subtlety' and GetTrainerServiceInfo(8) == 'Stealth'");
    truth("select('#', GetTrainerServiceInfo(9)) == 4 and GetTrainerServiceInfo(9) == nil and select(4, GetTrainerServiceInfo(9)) == 1");
    truth("select(4, GetTrainerServiceInfo(2)) == 1");
    truth("GetTrainerServiceSkillLine(2) == 'Assassination' and select('#', GetTrainerServiceSkillLine(1)) == 0");
    run("a, b = GetTrainerServiceAbilityReq(2, 1)");
    truth("a == 'Eviscerate (Rank 1)' and b == 1");
    truth("select('#', GetTrainerServiceAbilityReq(4, 1)) == 1 and GetTrainerServiceAbilityReq(4, 1) == 'Garrote (Rank 1)'");
    run("a, b = GetTrainerServiceAbilityReq(3, 1)");
    truth("a == nil and b == 1");
    run("a, b, c = GetTrainerServiceSkillReq(2)");
    truth("a == nil and b == 0 and c == 1");
    run("a, b = GetTrainerServiceStepReq(2)");
    truth("a == nil and b == 1");
    truth("select('#', IsTradeskillTrainer()) == 0");
    truth("select('#', GetTrainerServiceItemLink(2)) == 0");
    truth("GetTrainerServiceTypeFilter('available') == 1 and GetTrainerServiceTypeFilter('used') == nil");

    // Collapsing selects the first available service left; a collapsed
    // header answers three values.
    run("CollapseTrainerSkillLine(1)");
    truth("GetNumTrainerServices() == 5");
    truth("select('#', GetTrainerServiceInfo(1)) == 3");
    truth("GetTrainerSelectionIndex() == 3");
    run("ok, err = pcall(ExpandTrainerSkillLine, 3)");
    truth("not ok and string.find(err, 'Bad skill line in ExpandTrainerSkillLine', 1, true)");
    run("ok, err = pcall(CollapseTrainerSkillLine, 99)");
    truth("not ok and string.find(err, 'Bad skill line in CollapseTrainerSkillLine', 1, true)");
    run("CollapseTrainerSkillLine(0)");
    truth("GetNumTrainerServices() == 3");
    truth("select('#', GetTrainerSelectionIndex()) == 0");
    run("ExpandTrainerSkillLine(0)");
    truth("GetNumTrainerServices() == 8 and GetTrainerSelectionIndex() == 2");
    run("SelectTrainerService(4)");
    truth("GetTrainerSelectionIndex() == 4");
    run("SelectTrainerService(1)");
    truth("select('#', GetTrainerSelectionIndex()) == 0");
    run("SelectTrainerService(6)");

    // Learning Garrote (Rank 1) in the open list: one ability line, the
    // trainer refreshes, Rank 2 turns available, the selection starts over.
    events.clear();
    wowee::network::Packet learned(wireOpcode(Opcode::SMSG_LEARNED_SPELL));
    learned.writeUInt32(703);
    session.receive(learned);
    CHECK(handler.getChatHistory().back().message == "You have learned a new ability: Garrote (Rank 1).");
    CHECK(std::count(events.begin(), events.end(), "TRAINER_UPDATE") == 1);
    CHECK(std::find(events.begin(), events.end(), "SPELLS_CHANGED") < std::find(events.begin(), events.end(), "TRAINER_UPDATE"));
    truth("GetNumTrainerServices() == 7");
    truth("GetTrainerServiceInfo(3) == 'Garrote' and select(3, GetTrainerServiceInfo(3)) == 'available'");
    truth("GetTrainerSelectionIndex() == 2");

    // CMaNGOS Player::addSpell sends the supercede before the learn; the
    // original prints only the learn line.
    const auto chatBefore = handler.getChatHistory().size();
    wowee::network::Packet superceded(wireOpcode(Opcode::SMSG_SUPERCEDED_SPELL));
    superceded.writeUInt16(703);
    superceded.writeUInt16(8631);
    session.receive(superceded);
    wowee::network::Packet rank2(wireOpcode(Opcode::SMSG_LEARNED_SPELL));
    rank2.writeUInt32(8631);
    session.receive(rank2);
    REQUIRE(handler.getChatHistory().size() == chatBefore + 1);
    CHECK(handler.getChatHistory().back().message == "You have learned a new ability: Garrote (Rank 2).");
    // The replaced rank stays used: both Garrote rows leave the list.
    truth("GetNumTrainerServices() == 6");
    truth("GetTrainerServiceInfo(3) == 'Defense'");

    // A tradeskill recipe drops its rank and is no spellbook entry; a spell
    // without the ability attribute is a spell.
    events.clear();
    wowee::network::Packet poison(wireOpcode(Opcode::SMSG_LEARNED_SPELL));
    poison.writeUInt32(3420);
    session.receive(poison);
    CHECK(handler.getChatHistory().back().message == "You have learned how to create a new item: Crippling Poison.");
    CHECK(std::count(events.begin(), events.end(), "SPELLS_CHANGED") == 0);
    CHECK(std::count(events.begin(), events.end(), "TRAINER_UPDATE") == 1);
    wowee::network::Packet fireball(wireOpcode(Opcode::SMSG_LEARNED_SPELL));
    fireball.writeUInt32(133);
    session.receive(fireball);
    CHECK(handler.getChatHistory().back().message == "You have learned a new spell: Fireball (Rank 1).");

    // Buying prints nothing more.
    const auto chatAfter = handler.getChatHistory().size();
    wowee::network::Packet bought(wireOpcode(Opcode::SMSG_TRAINER_BUY_SUCCEEDED));
    bought.writeUInt64(77);
    bought.writeUInt32(8631);
    session.receive(bought);
    CHECK(handler.getChatHistory().size() == chatAfter);

    // A fresh list opens expanded with the selection restarted.
    run("CollapseTrainerSkillLine(1)");
    wowee::network::Packet again = reopened;
    session.receive(again);
    truth("select(4, GetTrainerServiceInfo(1)) == 1");
    handler.setAddonEventCallback({});
    lua.setGameHandler(nullptr);
}

TEST_CASE_METHOD(Integration, "Recipe item data resolves learned craft and reagents", "[recipe]") {
    replay::Session session("tbc");
    auto& handler = session.handler();
    handler.spellNameCacheLoadedRef() = true;
    auto& spell = handler.spellNameCacheRef()[29566];
    spell.name = "Adamantite Maul";
    spell.createdItemId = 23502;
    spell.reagents[0] = {23446, 8};
    auto& recipe = handler.itemInfoCacheRef()[23590];
    recipe.valid = true;
    recipe.itemClass = 9;
    recipe.name = "Plans: Adamantite Maul";
    recipe.description = "Teaches you how to make an Adamantite Maul.";
    recipe.requiredSkill = 164;
    recipe.requiredSkillRank = 325;
    recipe.spells[0].spellId = 483;
    recipe.spells[1].spellId = 29566;
    recipe.spells[1].spellTrigger = 6;
    handler.skillLineNamesRef()[164] = "Blacksmithing";
    handler.skillLineDbcLoadedRef() = true;
    lua.setGameHandler(&handler);
    run("recipeData = _GetItemTooltipData(23590)");
    truth("recipeData.recipe.itemId == 23502");
    truth("recipeData.recipe.name == 'Adamantite Maul'");
    truth("recipeData.recipe.reagents[1].itemId == 23446");
    truth("recipeData.recipe.reagents[1].count == 8");
    truth("recipeData.requiredSkillName == 'Blacksmithing'");
    truth("recipeData.requiredSkillMet == false");
    handler.playerSkillsRef()[164].value = 325;
    truth("_GetItemTooltipData(23590).requiredSkillMet == true");
    auto& weapon = handler.itemInfoCacheRef()[23502];
    weapon.valid = true;
    weapon.name = "Adamantite Maul";
    weapon.quality = 2;
    weapon.itemClass = 2;
    weapon.subClass = 5;
    weapon.inventoryType = 17;
    weapon.bindType = 2;
    weapon.requiredLevel = 64;
    auto& reagent = handler.itemInfoCacheRef()[23446];
    reagent.valid = true;
    reagent.name = "Adamantite Bar";
    // The tooltip is built from GlobalStrings.lua, which is extracted client
    // data outside this repository, e.g.
    //   WOWEE_TBC_GLOBALSTRINGS=/workspace/azerothcore/client/wowee-data/expansions/tbc/interface/framexml/globalstrings.lua
    const char* globalStrings = std::getenv("WOWEE_TBC_GLOBALSTRINGS");
    if (!globalStrings) SKIP("WOWEE_TBC_GLOBALSTRINGS not set");
    std::ifstream globalStringsFile(globalStrings);
    REQUIRE(globalStringsFile.good());
    std::stringstream globalStringsText;
    globalStringsText << globalStringsFile.rdbuf();
    run(globalStringsText.str());
    run("recipeTip = CreateFrame('GameTooltip', 'RecipeTip', UIParent); assert(_WoweePopulateItemTooltip(recipeTip, 23590))");
    const auto* tip = lua.widgets().findByName("RecipeTip");
    REQUIRE(tip);
    std::vector<std::string> lines;
    for (const auto& line : tip->tooltipLines) lines.push_back(line.left);
    CHECK(std::find(lines.begin(), lines.end(), "Plans: Adamantite Maul") != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), "Use: Teaches you how to make an Adamantite Maul.") != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), "Adamantite Maul") != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), "Binds when equipped") != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), "Requires Adamantite Bar (8)") != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), "Use: Learning") == lines.end());
    truth("select(2, recipeTip:GetItem()):find('item:23590:') ~= nil");
    auto& formula = handler.itemInfoCacheRef()[6349];
    formula.valid = true;
    formula.itemClass = 9;
    formula.name = "Formula: Enchant 2H Weapon - Lesser Intellect";
    formula.description = "Teaches you how to permanently enchant a two-handed weapon so it grants +3 intellect.";
    formula.spells[0].spellId = 483;
    formula.spells[1].spellId = 7793;
    formula.spells[1].spellTrigger = 6;
    handler.spellNameCacheRef()[7793].name = "Enchant 2H Weapon - Lesser Intellect";
    handler.spellNameCacheRef()[7793].reagents[0] = {10939, 3};
    run("assert(_WoweePopulateItemTooltip(recipeTip, 6349))");
    REQUIRE(tip->tooltipLines.size() == 2);
    CHECK(tip->tooltipLines[1].left == "Use: " + formula.description);
    CHECK(tip->tooltipLines[1].lc[0] == 0.0f);
    CHECK(tip->tooltipLines[1].lc[1] == 1.0f);
    lua.setGameHandler(nullptr);
}

TEST_CASE_METHOD(Integration, "TBC local creation restores omitted zero health and ghost state", "[death-login]") {
    using namespace wowee::game;
    for (const bool ghost : {false, true}) {
        replay::Session session("tbc");
        wowee::network::Packet packet(wireOpcode(Opcode::SMSG_UPDATE_OBJECT));
        packet.writeUInt32(1);
        packet.writeUInt8(0);
        packet.writeUInt8(static_cast<uint8_t>(UpdateType::CREATE_OBJECT2));
        packet.writePackedGuid(replay::Session::kPlayerGuid);
        packet.writeUInt8(static_cast<uint8_t>(ObjectType::PLAYER));
        packet.writeUInt8(0x01); // UPDATEFLAG_SELF, no movement payload
        std::map<uint16_t, uint32_t> fields{{fieldIndex(UF::UNIT_FIELD_MAXHEALTH), 100}};
        if (ghost) fields.emplace(fieldIndex(UF::PLAYER_FLAGS), 0x10);
        const auto blocks = static_cast<uint8_t>(fields.rbegin()->first / 32 + 1);
        packet.writeUInt8(blocks);
        std::vector<uint32_t> mask(blocks, 0);
        for (const auto& [index, value] : fields) mask[index / 32] |= 1u << (index % 32);
        for (const auto word : mask) packet.writeUInt32(word);
        for (const auto& [index, value] : fields) packet.writeUInt32(value);
        session.receive(packet);
        CHECK(session.handler().isPlayerDead());
        CHECK(session.handler().isPlayerGhost() == ghost);
        CHECK(session.handler().showDeathDialog() == !ghost);
    }
}

TEST_CASE_METHOD(Integration, "TBC corpse recreation keeps snapshot and distinguishes bones", "[corpse-bones]") {
    using namespace wowee::game;
    replay::Session session("tbc");
    auto& handler = session.handler();
    constexpr uint64_t guid = 0xF101000000000123ull;
    int spawns = 0, equipment = 0, deaths = 0;
    handler.setPlayerSpawnCallback([&](uint64_t id, uint32_t, uint8_t race, uint8_t gender,
                                      uint32_t appearance, uint8_t facial, float, float, float, float) {
        CHECK(id == guid); CHECK(race == 3); CHECK(gender == 1);
        CHECK(appearance == 0x04030201); CHECK(facial == 5); ++spawns;
    });
    handler.setPlayerEquipmentCallback([&](uint64_t, const auto& display, const auto& types) {
        CHECK(display[0] == 1234); CHECK(types[0] == 1); ++equipment;
    });
    handler.setNpcDeathCallback([&](uint64_t, bool alreadyDead) { CHECK(alreadyDead); ++deaths; });
    for (const bool bones : {false, true}) {
        if (bones) {
            wowee::network::Packet destroy(wireOpcode(Opcode::SMSG_DESTROY_OBJECT));
            destroy.writeUInt64(guid);
            session.receive(destroy);
            handler.corpseGuidRef() = 0; // resurrection cleared reclaim state
        }
        wowee::network::Packet packet(wireOpcode(Opcode::SMSG_UPDATE_OBJECT));
        packet.writeUInt32(1); packet.writeUInt8(0);
        packet.writeUInt8(static_cast<uint8_t>(UpdateType::CREATE_OBJECT2));
        packet.writePackedGuid(guid); packet.writeUInt8(static_cast<uint8_t>(ObjectType::CORPSE));
        packet.writeUInt8(0);
        std::map<uint16_t, uint32_t> fields{{6, replay::Session::kPlayerGuid},
            {34, 3u << 8 | 1u << 16 | 1u << 24}, {35, 0x05040302}};
        if (bones) fields.emplace(37, 1);
        else fields.emplace(15, 1234u | 1u << 24);
        packet.writeUInt8(2);
        std::array<uint32_t, 2> mask{};
        for (const auto& [index, value] : fields) mask[index / 32] |= 1u << (index % 32);
        for (auto word : mask) packet.writeUInt32(word);
        for (const auto& [index, value] : fields) packet.writeUInt32(value);
        session.receive(packet);
        CHECK(handler.corpseGuidRef() == (bones ? 0 : guid));
    }
    CHECK(spawns == 2);
    CHECK(equipment == 1);
    CHECK(deaths == 1);
}
TEST_CASE_METHOD(Integration, "Death effect CVar controls compositor independently of glow", "[death-cvar]") {
    bool deathEnabled = true;
    int glowCalls = 0;
    addons::LuaServices services;
    services.setDeathEffectEnabled = [&](bool enabled) { deathEnabled = enabled; };
    services.setGlowEnabled = [&](bool) { ++glowCalls; };
    lua.setLuaServices(services);
    truth("GetCVarDefault('ffxDeath') == '1'");
    run("SetCVar('ffxDeath', '0')");
    CHECK_FALSE(deathEnabled);
    truth("GetCVar('ffxDeath') == '0'");
    run("SetCVar('FFXDEATH', '1')");
    CHECK(deathEnabled);
    CHECK(glowCalls == 0);
    lua.setLuaServices({});
}

TEST_CASE_METHOD(Integration, "Nether World CVar controls compositor independently and honors ffx master switch", "[nether-cvar]") {
    bool netherEnabled = true;
    bool glowEnabled = true;
    bool deathEnabled = true;
    int glowCalls = 0;
    int deathCalls = 0;
    addons::LuaServices services;
    services.setNetherWorldEnabled = [&](bool enabled) { netherEnabled = enabled; };
    services.setGlowEnabled = [&](bool enabled) { glowEnabled = enabled; ++glowCalls; };
    services.setDeathEffectEnabled = [&](bool enabled) { deathEnabled = enabled; ++deathCalls; };
    lua.setLuaServices(services);

    truth("GetCVarDefault('ffxNetherWorld') == '1'");
    truth("GetCVarDefault('ffx') == '1'");
    run("SetCVar('ffxNetherWorld', '0')");
    CHECK_FALSE(netherEnabled);
    truth("GetCVar('ffxNetherWorld') == '0'");
    CHECK(glowCalls == 0);
    CHECK(deathCalls == 0);

    run("SetCVar('FFXNETHERWORLD', '1')");
    CHECK(netherEnabled);
    CHECK(glowCalls == 0);
    CHECK(deathCalls == 0);

    // Master ffx switch disables all fullscreen postprocess effects when 0.
    run("SetCVar('ffx', '0')");
    CHECK_FALSE(netherEnabled);
    CHECK_FALSE(glowEnabled);
    CHECK_FALSE(deathEnabled);

    run("SetCVar('ffx', '1')");
    CHECK(netherEnabled);
    CHECK(glowEnabled);
    CHECK(deathEnabled);
    lua.setLuaServices({});
}

// Build 8606, measured (pair 19, _experiments/20261118T1650Z.half_brightness):
// GetGamma() is 1 minus the gamma CVar, 0 untouched; SetGamma(v) stores 1 - v.
// The TBC video options set their -0.5..0.5 Gamma slider to GetGamma() when they
// open and pass every change to SetGamma, so a neutral reading must stay put.
TEST_CASE_METHOD(Integration, "GetGamma and SetGamma are offsets from the gamma CVar", "[gamma]") {
    truth("GetCVarDefault('gamma') == '1.0'");
    run("SetGamma(0.2)");
    truth("GetCVar('gamma') == '0.800000'");
    truth("math.abs(GetGamma() - 0.19999998807907) < 1e-12");
    run("SetGamma(-0.3)");
    truth("GetCVar('gamma') == '1.300000'");
    truth("math.abs(GetGamma() + 0.29999995231628) < 1e-12");
    run("SetGamma(0)");
    truth("GetGamma() == 0 and GetCVar('gamma') == '1.000000'");
    run(R"lua(
        gammaSlider = CreateFrame('Slider', nil, UIParent)
        gammaSlider:SetScript('OnValueChanged', function() SetGamma(arg1) end)
        gammaSlider:SetMinMaxValues(-0.5, 0.5)
        gammaSlider:SetValueStep(0.1)
        gammaSlider:SetValue(GetGamma())
    )lua");
    truth("GetGamma() == 0 and GetCVar('gamma') == '1.000000'");
}

// Build 8606, measured (pair 4, _experiments/20261118T1516.native_mouselook):
// a drag turns cameraYawMoveSpeed/800 degrees of yaw and cameraPitchMoveSpeed/600
// of pitch per pixel, the same at 1280x720 and 800x600; mouseSpeed changes
// neither the turn nor the cursor.
TEST_CASE_METHOD(Integration, "Mouse-look turns by the move-speed CVars per pixel", "[camera][mouselook]") {
    rendering::Camera camera;
    rendering::CameraController controller(&camera);
    controller.setIdleOrbitEnabled(false);
    controller.setFollowTarget(nullptr);
    CHECK(controller.yawDegreesPerPixel() == Catch::Approx(0.225f));
    CHECK(controller.pitchDegreesPerPixel() == Catch::Approx(0.15f));

    controller.setDefaultSpawn(glm::vec3(0), 100, -30);
    controller.resetAngles();
    controller.applyLookDelta(100, 0);
    CHECK(controller.getYaw() == Catch::Approx(100 - 22.5f));
    controller.applyLookDelta(0, 100);  // mouse down looks down
    CHECK(controller.getPitch() == Catch::Approx(-30 - 15.0f));

    // The Mouse Look Speed slider's top: 270, and half of it for pitch.
    controller.setYawMoveSpeed(270);
    controller.setPitchMoveSpeed(135);
    CHECK(controller.yawDegreesPerPixel() == Catch::Approx(0.3375f));
    CHECK(controller.pitchDegreesPerPixel() == Catch::Approx(0.225f));
    // The two are separate CVars: 40 and 300 measured 0.05 and 0.5.
    controller.setYawMoveSpeed(40);
    controller.setPitchMoveSpeed(300);
    CHECK(controller.yawDegreesPerPixel() == Catch::Approx(0.05f));
    CHECK(controller.pitchDegreesPerPixel() == Catch::Approx(0.5f));

    // A controller stick turns in degrees, whatever the mouse is set to.
    controller.resetAngles();
    controller.applyLookDegrees(10, 5);
    CHECK(controller.getYaw() == Catch::Approx(90));
    CHECK(controller.getPitch() == Catch::Approx(-35));
}

TEST_CASE_METHOD(Integration, "Mouse-look CVars reach the camera with build 8606's defaults and range", "[camera][mouselook]") {
    std::vector<float> yaw, pitch;
    std::vector<std::string> settings;
    addons::LuaServices services;
    services.setCameraYawMoveSpeed = [&](float v) { yaw.push_back(v); };
    services.setCameraPitchMoveSpeed = [&](float v) { pitch.push_back(v); };
    services.setClientSetting = [&](const std::string& key, const std::string&) { settings.push_back(key); };
    lua.setLuaServices(services);
    run("SetCVar('cameraYawMoveSpeed', 180) SetCVar('cameraPitchMoveSpeed', 90) SetCVar('mouseSpeed', 1)");
    truth("GetCVarDefault('cameraYawMoveSpeed') == '180'");
    truth("GetCVarDefault('cameraPitchMoveSpeed') == '90'");
    truth("GetCVarDefault('mouseSpeed') == '1'");
    // No redefined slider range: 2.4.3 has no GetCVarMin, and the panel's own
    // table (90 to 270, 0.5 to 1.5) is the range.
    truth("GetCVarMin('cameraYawMoveSpeed') == nil and GetCVarMax('mouseSpeed') == nil");
    yaw.clear(); pitch.clear(); settings.clear();

    run("SetCVar('cameraYawMoveSpeed', 270)");
    REQUIRE(yaw.size() == 1);
    CHECK(yaw.back() == Catch::Approx(270));
    truth("GetCVar('cameraYawMoveSpeed') == '270'");
    run("SetCVar('cameraPitchMoveSpeed', '135')");
    REQUIRE(pitch.size() == 1);
    CHECK(pitch.back() == Catch::Approx(135));

    // Kept as written: 0.1, 360, "1e2", "200x" (as 200).
    run("SetCVar('cameraYawMoveSpeed', 0.1)");
    CHECK(yaw.back() == Catch::Approx(0.1f));
    run("SetCVar('cameraYawMoveSpeed', 360)");
    CHECK(yaw.back() == Catch::Approx(360));
    run("SetCVar('cameraYawMoveSpeed', '1e2')");
    CHECK(yaw.back() == Catch::Approx(100));
    run("SetCVar('cameraYawMoveSpeed', '200x')");
    CHECK(yaw.back() == Catch::Approx(200));
    truth("GetCVar('cameraYawMoveSpeed') == '200x'");
    // Refused, the previous value kept: 0.0999, 0, -1, 360.001, "abc", "", " 5".
    const auto applied = yaw.size();
    for (const char* bad : {"0.0999", "0", "-1", "360.001", "'abc'", "''", "' 5'"}) {
        run(std::string("SetCVar('cameraYawMoveSpeed', ") + bad + ")");
        run(std::string("SetCVar('cameraPitchMoveSpeed', ") + bad + ")");
    }
    CHECK(yaw.size() == applied);
    CHECK(pitch.size() == 1);
    truth("GetCVar('cameraYawMoveSpeed') == '200x'");
    truth("GetCVar('cameraPitchMoveSpeed') == '135'");

    // mouseSpeed takes anything and turns nothing.
    run("SetCVar('mouseSpeed', 1.5) SetCVar('mouseSpeed', -1)");
    truth("GetCVar('mouseSpeed') == '-1'");
    CHECK(yaw.size() == applied);
    CHECK(pitch.size() == 1);
    CHECK(settings.empty());

    run("SetCVar('cameraYawMoveSpeed', 180) SetCVar('cameraPitchMoveSpeed', 90) SetCVar('mouseSpeed', 1)");
    lua.setLuaServices({});
}

TEST_CASE("Stored mouse-look CVars from this client's old sensitivity scale are dropped", "[camera][mouselook]") {
    // A cvars.cfg from before the format was recorded: all three in the old
    // 0.05-1 sensitivity, as found in the shared and pair configs.
    std::unordered_map<std::string, std::string> rows{
        {"camerayawmovespeed", "0.2"}, {"camerapitchmovespeed", "0.1"},
        {"mousespeed", "1"}, {"farclip", "1900"}};
    CHECK(addons::migrateStoredCVarRows(rows));
    CHECK_FALSE(rows.contains("camerayawmovespeed"));
    CHECK_FALSE(rows.contains("camerapitchmovespeed"));
    CHECK_FALSE(rows.contains("mousespeed"));
    CHECK(rows.at("farclip") == "1900");
    CHECK(rows.at("wowee_cvarsversion") == "1");
    // Once recorded, the player's own values stay.
    rows["camerayawmovespeed"] = "90";
    rows["camerapitchmovespeed"] = "45";
    CHECK_FALSE(addons::migrateStoredCVarRows(rows));
    CHECK(rows.at("camerayawmovespeed") == "90");
    CHECK(rows.at("camerapitchmovespeed") == "45");
}

TEST_CASE_METHOD(Integration, "TBC remote aura fields survive target selection and update", "[remote-aura]") {
    using namespace wowee::game;
    replay::Session session("tbc");
    auto& handler = session.handler();
    lua.setGameHandler(&handler);
    constexpr uint64_t guid = 0xf13000087a000777ull;
    auto unit = std::make_shared<Unit>(guid);
    unit->setName("Ghost reference");
    handler.getEntityManager().addEntity(guid, unit);
    handler.spellNameCacheLoadedRef() = true;
    handler.spellNameCacheRef()[8326].name = "Ghost";
    const uint16_t field = fieldIndex(UF::UNIT_FIELD_AURAS) + 40;
    const auto receiveAura = [&](uint32_t spell) {
        wowee::network::Packet packet(wireOpcode(Opcode::SMSG_UPDATE_OBJECT));
        packet.writeUInt32(1); packet.writeUInt8(0);
        packet.writeUInt8(static_cast<uint8_t>(UpdateType::VALUES));
        packet.writePackedGuid(guid);
        const uint8_t words = field / 32 + 1;
        packet.writeUInt8(words);
        for (uint8_t i = 0; i < words; ++i) packet.writeUInt32(i == field / 32 ? 1u << (field % 32) : 0);
        packet.writeUInt32(spell);
        session.receive(packet);
    };
    receiveAura(8326);
    handler.setTarget(guid);
    truth("UnitDebuff('target', 1) == 'Ghost'");
    truth("UnitBuff('target', 1) == nil");
    handler.setTarget(0);
    handler.setTarget(guid);
    truth("UnitDebuff('target', 1) == 'Ghost'");
    receiveAura(0);
    truth("UnitDebuff('target', 1) == nil");
    lua.setGameHandler(nullptr);
}

TEST_CASE_METHOD(Integration, "Mining loot survives rejected duplicate open and uses server slot", "[mining-loot]") {
    using namespace wowee::game;
    replay::Session session("tbc");
    auto& handler = session.handler();
    lua.setGameHandler(&handler);
    handler.setAutoLoot(false);
    constexpr uint64_t guid = 0xf1100006c3596cedull;
    auto node = std::make_shared<GameObject>(guid);
    node->setEntry(1731);
    node->setName("Copper Vein");
    handler.getEntityManager().addEntity(guid, node);
    wowee::network::Packet query(wireOpcode(Opcode::SMSG_GAMEOBJECT_QUERY_RESPONSE));
    query.writeUInt32(1731); query.writeUInt32(3); query.writeUInt32(310);
    query.writeString("Copper Vein");
    for (int i = 0; i < 6; ++i) query.writeString("");
    for (int i = 0; i < 24; ++i) query.writeUInt32(0);
    session.receive(query);
    handler.getSpellHandler()->addKnownSpell(2575);
    // Gathering nodes are recognised from Lock.dbc/LockType.dbc (lock slot
    // 0's skill), which this packet-only fixture has no asset manager for.
    if (!handler.services().assetManager) SKIP("needs Lock.dbc: no asset manager in the replay fixture");
    handler.interactWithGameObject(guid);
    REQUIRE(session.sent().of(Opcode::CMSG_CAST_SPELL).size() == 1);
    CHECK_FALSE(handler.hasPendingGameObjectLootOpen(guid));
    handler.lastInteractedGoGuidRef() = guid;
    handler.spellNameCacheLoadedRef() = true;
    handler.spellNameCacheRef()[2575].name = "Mining";
    wowee::network::Packet start(wireOpcode(Opcode::SMSG_SPELL_START));
    start.writePackedGuid(replay::Session::kPlayerGuid);
    start.writePackedGuid(replay::Session::kPlayerGuid);
    start.writeUInt32(2575); start.writeUInt8(1); start.writeUInt16(0);
    start.writeUInt32(3000); start.writeUInt32(0);
    session.receive(start);
    REQUIRE(handler.isCasting());
    wowee::network::Packet go(wireOpcode(Opcode::SMSG_SPELL_GO));
    go.writePackedGuid(replay::Session::kPlayerGuid);
    go.writePackedGuid(replay::Session::kPlayerGuid);
    go.writeUInt32(2575); go.writeUInt16(0); go.writeUInt32(1000);
    go.writeUInt8(1); go.writeUInt64(guid); go.writeUInt8(0); go.writeUInt32(0);
    session.receive(go);
    CHECK_FALSE(handler.isCasting());
    CHECK(session.sent().of(Opcode::CMSG_LOOT).empty());
    for (const uint8_t type : {2, 3}) {
        wowee::network::Packet loot(wireOpcode(Opcode::SMSG_LOOT_RESPONSE));
        loot.writeUInt64(guid); loot.writeUInt8(type); loot.writeUInt32(0); loot.writeUInt8(1);
        loot.writeUInt8(4); // Server slot is independent of Lua's visible slot 1.
        loot.writeUInt32(2770); loot.writeUInt32(2); loot.writeUInt32(4681);
        loot.writeUInt32(0); loot.writeUInt32(0); loot.writeUInt8(0);
        session.receive(loot);
        truth(type == 3 ? "IsFishingLoot()" : "not IsFishingLoot()");
        truth("GetNumLootItems() == 1 and LootSlotIsItem(1)");
        wowee::network::Packet denied(wireOpcode(Opcode::SMSG_LOOT_RESPONSE));
        denied.writeUInt64(guid); denied.writeUInt8(0); denied.writeUInt8(6);
        session.receive(denied);
        REQUIRE(handler.getCurrentLoot().items.size() == 1);
        CHECK(handler.getCurrentLoot().lootGuid == guid);
        CHECK(handler.getCurrentLoot().items[0].itemId == 2770);
        truth("GetNumLootItems() == 1 and LootSlotIsItem(1)");
        run("LootSlot(1)");
    }
    auto sent = session.sent().of(Opcode::CMSG_AUTOSTORE_LOOT_ITEM);
    REQUIRE(sent.size() == 2);
    for (auto& packet : sent) CHECK(packet.readUInt8() == 4);
    lua.setGameHandler(nullptr);
}

// Original 2.4.3 (pair 17): using Zul'Aman's Strange Gong 187359 channels
// Zul'Aman Object Visual 45222; UnitChannelInfo("player") answers that name,
// rank "" and text "Channeling" (GlobalStrings CHANNELING), and the bar reads
// "Channeling". A cast keeps its own name as text.
TEST_CASE_METHOD(Integration, "A channel's bar text is CHANNELING, not the spell name", "[channel-text]") {
    using namespace wowee::game;
    replay::Session session("tbc");
    auto& handler = session.handler();
    lua.setGameHandler(&handler);
    handler.spellNameCacheLoadedRef() = true;
    handler.spellNameCacheRef()[45222].name = "Zul'Aman Object Visual";
    run("CHANNELING = 'Channeling'");
    wowee::network::Packet start(wireOpcode(Opcode::MSG_CHANNEL_START));
    start.writePackedGuid(replay::Session::kPlayerGuid);
    start.writeUInt32(45222); start.writeUInt32(10000);
    session.receive(start);
    REQUIRE(handler.isChanneling());
    truth("UnitChannelInfo('player') == \"Zul'Aman Object Visual\"");
    truth("select(3, UnitChannelInfo('player')) == 'Channeling'");
    truth("UnitCastingInfo('player') == nil");
    lua.setGameHandler(nullptr);
}

// Altar of Archaedas (133234, GAMEOBJECT_TYPE_SUMMONING_RITUAL): clicking sends
// CMSG_GAMEOBJ_USE and the server casts Uldaman Boss Object Visual (11206,
// RangeIndex 12 = 5 yd, AttributesEx 0x10000004 channeled) via SMSG_SPELL_GO +
// MSG_CHANNEL_START. Because it is channeled, SMSG_SPELL_GO must not classify it
// as a melee swing (which previously overwrote the channel animation with
// SPECIAL_UNARMED 118 and left the player standing with weapons unsheathed).
TEST_CASE_METHOD(Integration, "Server-started 5-yard ritual channel does not trigger melee swing", "[ritual-channel]") {
    using namespace wowee::game;
    replay::Session session("tbc");
    auto& handler = session.handler();
    handler.spellNameCacheLoadedRef() = true;
    auto& spell = handler.spellNameCacheRef()[11206];
    spell.name = "Uldaman Boss Object Visual";
    spell.maxRange = 5.0f;
    spell.attrEx = 0x10000004u;

    std::vector<uint32_t> meleeSwings;
    handler.setMeleeSwingCallback([&](uint32_t spellId) { meleeSwings.push_back(spellId); });
    std::vector<std::pair<SpellAnimPhase, uint32_t>> animPhases;
    handler.setSpellCastAnimCallback([&](uint64_t guid, SpellAnimPhase phase, uint32_t spellId) {
        if (guid == replay::Session::kPlayerGuid) animPhases.emplace_back(phase, spellId);
    });

    // Server sends SMSG_SPELL_GO (hit self, no prior CMSG_CAST_SPELL) then MSG_CHANNEL_START.
    wowee::network::Packet go(wireOpcode(Opcode::SMSG_SPELL_GO));
    go.writePackedGuid(replay::Session::kPlayerGuid);
    go.writePackedGuid(replay::Session::kPlayerGuid);
    go.writeUInt32(11206);
    go.writeUInt16(0);        // castFlags
    go.writeUInt32(0);        // timestamp
    go.writeUInt8(1);         // hitCount
    go.writeUInt64(replay::Session::kPlayerGuid);
    go.writeUInt8(0);         // missCount
    go.writeUInt32(0x2);      // TARGET_FLAG_UNIT
    go.writePackedGuid(replay::Session::kPlayerGuid);
    session.receive(go);

    wowee::network::Packet start(wireOpcode(Opcode::MSG_CHANNEL_START));
    start.writePackedGuid(replay::Session::kPlayerGuid);
    start.writeUInt32(11206);
    start.writeUInt32(600000);
    session.receive(start);

    REQUIRE(meleeSwings.empty());
    REQUIRE(handler.isChanneling());
    REQUIRE(animPhases.size() == 2);
    CHECK(animPhases[0] == std::pair{SpellAnimPhase::CAST, 11206u});
    CHECK(animPhases[1] == std::pair{SpellAnimPhase::CHANNEL, 11206u});

    wowee::network::Packet stop(wireOpcode(Opcode::MSG_CHANNEL_UPDATE));
    stop.writePackedGuid(replay::Session::kPlayerGuid);
    stop.writeUInt32(0);
    session.receive(stop);

    REQUIRE(!handler.isChanneling());
    REQUIRE(animPhases.size() == 3);
    CHECK(animPhases[2] == std::pair{SpellAnimPhase::STOP, 0u});
}

// Original 2.4.3 (pair 8, _experiments/20261119T1330Z.castbar_text):
// UnitCastingInfo answers seven values (Hearthstone, "", "Hearthstone",
// texture, start, end, false) and UnitChannelInfo none at all meanwhile. A
// spell with AttributesEx3 NO_CASTING_BAR_TEXT (Opening 6477) shows no text.
TEST_CASE_METHOD(Integration, "UnitCastingInfo text and values as 2.4.3", "[cast-text]") {
    using namespace wowee::game;
    replay::Session session("tbc");
    auto& handler = session.handler();
    lua.setGameHandler(&handler);
    handler.spellNameCacheLoadedRef() = true;
    handler.spellNameCacheRef()[8690].name = "Hearthstone";
    handler.spellNameCacheRef()[6477].name = "Opening";
    handler.spellNameCacheRef()[6477].attrEx3 = 0x4;
    const auto cast = [&](uint32_t spell) {
        wowee::network::Packet start(wireOpcode(Opcode::SMSG_SPELL_START));
        start.writePackedGuid(replay::Session::kPlayerGuid);
        start.writePackedGuid(replay::Session::kPlayerGuid);
        start.writeUInt32(spell); start.writeUInt8(1); start.writeUInt16(0);
        start.writeUInt32(10000); start.writeUInt32(0);
        session.receive(start);
        REQUIRE(handler.isCasting());
    };
    truth("select('#', UnitCastingInfo('player')) == 0");
    cast(8690);
    truth("select('#', UnitCastingInfo('player')) == 7");
    truth("select('#', UnitChannelInfo('player')) == 0");
    truth("select(3, UnitCastingInfo('player')) == 'Hearthstone'");
    truth("select(2, UnitCastingInfo('player')) == ''");
    truth("select(6, UnitCastingInfo('player')) - select(5, UnitCastingInfo('player')) == 10000");
    truth("select(5, UnitCastingInfo('player')) == math.floor(select(5, UnitCastingInfo('player')))");
    handler.cancelCast();
    cast(6477);
    truth("UnitCastingInfo('player') == 'Opening'");
    truth("select(3, UnitCastingInfo('player')) == ''");
    lua.setGameHandler(nullptr);
}

TEST_CASE_METHOD(Integration, "TargetUnit resolves nearby names and exact commands", "[target-name]") {
    using namespace wowee::game;
    replay::Session session("tbc");
    auto& handler = session.handler();
    lua.setGameHandler(&handler);
    auto player = std::make_shared<Player>(replay::Session::kPlayerGuid);
    player->setName("Edric");
    handler.getEntityManager().addEntity(player->getGuid(), player);
    auto near = std::make_shared<Unit>(0xf130000001000001ull);
    near->setName("Nessa Shadowsong");
    near->setPosition(4, 0, 0, 0);
    handler.getEntityManager().addEntity(near->getGuid(), near);
    auto far = std::make_shared<Unit>(0xf130000001000002ull);
    far->setName("Nessa Shadowsong");
    far->setPosition(20, 0, 0, 0);
    handler.getEntityManager().addEntity(far->getGuid(), far);
    run("TargetUnit('nEsSa sHaDoWsOnG', 1)");
    CHECK(handler.getTargetGuid() == near->getGuid());
    run("ClearTarget(); TargetUnit('Nessa')");
    CHECK(handler.getTargetGuid() == near->getGuid());
    run("TargetUnit('player'); TargetUnit('Shadowsong')");
    CHECK(handler.getTargetGuid() == player->getGuid());
    run("ClearTarget(); TargetUnit('Nessa', 1)");
    CHECK(handler.getTargetGuid() == 0);
    run("TargetUnit('player'); TargetUnit('Nobody here'); TargetUnit('')");
    CHECK(handler.getTargetGuid() == player->getGuid());
    run("TargetUnit('edric', 1)");
    CHECK(handler.getTargetGuid() == player->getGuid());
    lua.setGameHandler(nullptr);
}

TEST_CASE_METHOD(Integration, "Moonkin tooltip keeps creature type on unwrapped level line", "[unit-tooltip]") {
    using namespace wowee::game;
    replay::Session session("tbc");
    auto& handler = session.handler();
    lua.setGameHandler(&handler);
    constexpr uint64_t guid = 0xf1300027ae000777ull;
    auto unit = std::make_shared<Unit>(guid);
    unit->setName("Moonkin");
    unit->setEntry(10158);
    unit->setLevel(12);
    unit->setHealth(100);
    unit->setMaxHealth(100);
    unit->setField(fieldIndex(UF::UNIT_FIELD_LEVEL), 12);
    handler.getEntityManager().addEntity(guid, unit);
    wowee::network::Packet info(wireOpcode(Opcode::SMSG_CREATURE_QUERY_RESPONSE));
    info.writeUInt32(10158);
    info.writeString("Moonkin");
    for (int i = 0; i < 5; ++i) info.writeString("");
    info.writeUInt32(0);
    info.writeUInt32(7); // Humanoid, as declared by the server template.
    for (int i = 0; i < 8; ++i) info.writeUInt32(0);
    info.writeFloat(1); info.writeFloat(1); info.writeUInt8(0);
    session.receive(info);
    handler.setTarget(guid);
    run(R"lua(
        TOOLTIP_UNIT_LEVEL = 'Level %s'
        TOOLTIP_UNIT_LEVEL_CLASS = 'Level %s %s'
        TOOLTIP_UNIT_LEVEL_CLASS_TYPE = 'Level %s %s (%s)'
        TOOLTIP_UNIT_LEVEL_TYPE = 'Level %s (%s)'
        TOOLTIP_UNIT_LEVEL_RACE_CLASS_TYPE = 'Level %s %s %s (%s)'
        ELITE = 'Elite'
        BOSS = 'Boss'
        PLAYER = 'Player'
    )lua");
    run("moonkinTip = CreateFrame('GameTooltip', 'MoonkinTip', UIParent); assert(moonkinTip:SetUnit('target'))");
    const auto* tip = lua.widgets().findByName("MoonkinTip");
    REQUIRE(tip);
    REQUIRE(tip->tooltipLines.size() == 2);
    CHECK(tip->tooltipLines[1].left == "Level 12 Humanoid");
    CHECK(tip->tooltipLines[0].lc[0] == 1.0f);
    CHECK(tip->tooltipLines[0].lc[1] == 0.82f);
    CHECK(tip->tooltipLines[0].lc[2] == 0.0f);
    CHECK_FALSE(tip->tooltipLines[1].wrap);
    lua.setGameHandler(nullptr);
}

// Original 2.4.3 (pair 7, GM Island and Gadgetzan): the creature type is in
// the level line only when UnitIsFriend("player", unit) is false. A Gadgetzan
// Bruiser (template 475, Gadgetzan at Neutral) reads "Level 67" and
// UnitIsFriend true; neutral wildlife (template 7) "Level 9 Beast"; a
// Wastewander Bandit (template 16) "Level 41 Humanoid". UnitCreatureType
// answers the type for all three.
TEST_CASE_METHOD(Integration, "Unit tooltip names the creature type only for non-friends", "[unit-tooltip]") {
    using namespace wowee::game;
    replay::Session session("tbc");
    auto& handler = session.handler();
    lua.setGameHandler(&handler);
    // Built by buildFactionHostilityMap: a faction with a standing the player
    // is not at war with is friendly, whatever the standing.
    handler.setFactionFriendlyMap({{475, true}, {7, false}, {16, false}});
    handler.setFactionHostileMap({{475, false}, {7, false}, {16, true}});
    run(R"lua(
        TOOLTIP_UNIT_LEVEL = 'Level %s'
        TOOLTIP_UNIT_LEVEL_CLASS = 'Level %s %s'
        TOOLTIP_UNIT_LEVEL_CLASS_TYPE = 'Level %s %s (%s)'
        TOOLTIP_UNIT_LEVEL_TYPE = 'Level %s (%s)'
        TOOLTIP_UNIT_LEVEL_RACE_CLASS_TYPE = 'Level %s %s %s (%s)'
        ELITE = 'Elite'
        BOSS = 'Boss'
        PLAYER = 'Player'
        CORPSE = 'Corpse'
        typeTip = CreateFrame('GameTooltip', 'TypeTip', UIParent)
    )lua");
    struct Case { uint32_t entry; const char* name; uint32_t creatureType; uint32_t level; uint32_t factionTemplate;
                  const char* line; bool friendOf; };
    const Case cases[] = {
        {9460, "Gadgetzan Bruiser", 7, 67, 475, "Level 67", true},
        {330, "Princess", 1, 9, 7, "Level 9 Beast", false},
        {5618, "Wastewander Bandit", 7, 41, 16, "Level 41 Humanoid", false},
    };
    uint64_t guid = 0xf130000000100000ull;
    for (const auto& c : cases) {
        auto unit = std::make_shared<Unit>(++guid);
        unit->setName(c.name);
        unit->setEntry(c.entry);
        unit->setLevel(c.level);
        unit->setHealth(100);
        unit->setMaxHealth(100);
        unit->setField(fieldIndex(UF::UNIT_FIELD_LEVEL), c.level);
        unit->setFactionTemplate(c.factionTemplate);
        unit->setHostile(handler.isHostileFactionPublic(c.factionTemplate));
        handler.getEntityManager().addEntity(guid, unit);
        wowee::network::Packet info(wireOpcode(Opcode::SMSG_CREATURE_QUERY_RESPONSE));
        info.writeUInt32(c.entry);
        info.writeString(c.name);
        for (int i = 0; i < 5; ++i) info.writeString("");
        info.writeUInt32(0);
        info.writeUInt32(c.creatureType);
        for (int i = 0; i < 8; ++i) info.writeUInt32(0);
        info.writeFloat(1); info.writeFloat(1); info.writeUInt8(0);
        session.receive(info);
        handler.setTarget(guid);
        INFO(c.name);
        run("assert(typeTip:SetUnit('target'))");
        const auto* tip = lua.widgets().findByName("TypeTip");
        REQUIRE(tip);
        REQUIRE(tip->tooltipLines.size() >= 2);
        CHECK(tip->tooltipLines[1].left == c.line);
        truth(std::string(c.friendOf ? "" : "not ") + "UnitIsFriend('player', 'target')");
        truth(std::string("UnitCreatureType('target') == '") + (c.creatureType == 1 ? "Beast" : "Humanoid") + "'");
        // A corpse names itself whatever it was: "Level 35 Corpse" for Quinn.
        unit->setHealth(0);
        run("assert(typeTip:SetUnit('target'))");
        CHECK(tip->tooltipLines[1].left == std::string("Level ") + std::to_string(c.level) + " Corpse");
    }
    lua.setGameHandler(nullptr);
}

// Original 2.4.3 (pair 5, Zul'Gurub and GM Island), all not friends of the
// player: type 10 "Not specified" never reaches the level line though
// UnitCreatureType answers it - Hakkar "Level ?? (Boss)", Green Sludge
// "Level 46". A creature of type 0 (Rankist) answers nil and reads "Level 6".
// Critter, Mechanical, Totem, Non-combat Pet and Gas Cloud are named: a wild
// Cockroach reads "Level 1 Non-combat Pet".
TEST_CASE_METHOD(Integration, "Unit tooltip never names creature type Not specified", "[unit-tooltip]") {
    using namespace wowee::game;
    replay::Session session("tbc");
    auto& handler = session.handler();
    lua.setGameHandler(&handler);
    handler.setFactionFriendlyMap({{28, false}, {14, false}, {188, false}, {31, false}, {1813, false}, {190, false}});
    handler.setFactionHostileMap({{28, true}, {14, true}, {188, false}, {31, false}, {1813, true}, {190, false}});
    run(R"lua(
        TOOLTIP_UNIT_LEVEL = 'Level %s'
        TOOLTIP_UNIT_LEVEL_CLASS = 'Level %s %s'
        TOOLTIP_UNIT_LEVEL_CLASS_TYPE = 'Level %s %s (%s)'
        TOOLTIP_UNIT_LEVEL_TYPE = 'Level %s (%s)'
        TOOLTIP_UNIT_LEVEL_RACE_CLASS_TYPE = 'Level %s %s %s (%s)'
        ELITE = 'Elite'
        BOSS = 'Boss'
        PLAYER = 'Player'
        CORPSE = 'Corpse'
        kindTip = CreateFrame('GameTooltip', 'KindTip', UIParent)
    )lua");
    struct Case { uint32_t entry; const char* name; uint32_t creatureType; uint32_t rank; uint32_t level;
                  uint32_t factionTemplate; const char* line; const char* api; };
    const Case cases[] = {
        {14834, "Hakkar", 10, 3, 63, 28, "Level ?? (Boss)", "'Not specified'"},
        {2655, "Green Sludge", 10, 0, 46, 14, "Level 46", "'Not specified'"},
        {71, "Rankist", 0, 0, 6, 14, "Level 6", "nil"},
        {721, "Rabbit", 8, 0, 1, 31, "Level 1 Critter", "'Critter'"},
        {36, "Harvest Golem", 9, 0, 11, 14, "Level 11 Mechanical", "'Mechanical'"},
        {21420, "Corrupt Water Totem", 11, 0, 69, 1813, "Level 69 Totem", "'Totem'"},
        {7395, "Cockroach", 12, 0, 1, 188, "Level 1 Non-combat Pet", "'Non-combat Pet'"},
        {17378, "Swamp Gas", 13, 0, 60, 190, "Level 60 Gas Cloud", "'Gas Cloud'"},
    };
    uint64_t guid = 0xf130000000200000ull;
    for (const auto& c : cases) {
        auto unit = std::make_shared<Unit>(++guid);
        unit->setName(c.name);
        unit->setEntry(c.entry);
        unit->setLevel(c.level);
        unit->setHealth(100);
        unit->setMaxHealth(100);
        unit->setField(fieldIndex(UF::UNIT_FIELD_LEVEL), c.level);
        unit->setFactionTemplate(c.factionTemplate);
        unit->setHostile(handler.isHostileFactionPublic(c.factionTemplate));
        handler.getEntityManager().addEntity(guid, unit);
        wowee::network::Packet info(wireOpcode(Opcode::SMSG_CREATURE_QUERY_RESPONSE));
        info.writeUInt32(c.entry);
        info.writeString(c.name);
        for (int i = 0; i < 5; ++i) info.writeString("");
        info.writeUInt32(0);
        info.writeUInt32(c.creatureType);
        info.writeUInt32(0);
        info.writeUInt32(c.rank);
        for (int i = 0; i < 6; ++i) info.writeUInt32(0);
        info.writeFloat(1); info.writeFloat(1); info.writeUInt8(0);
        session.receive(info);
        handler.setTarget(guid);
        INFO(c.name);
        truth("not UnitIsFriend('player', 'target')");
        truth(std::string("UnitCreatureType('target') == ") + c.api);
        run("assert(kindTip:SetUnit('target'))");
        const auto* tip = lua.widgets().findByName("KindTip");
        REQUIRE(tip);
        REQUIRE(tip->tooltipLines.size() >= 2);
        CHECK(tip->tooltipLines[1].left == c.line);
    }
    lua.setGameHandler(nullptr);
}

TEST_CASE_METHOD(Integration, "Racial leader creature tooltip shows Leader line", "[unit-tooltip]") {
    using namespace wowee::game;
    replay::Session session("tbc");
    auto& handler = session.handler();
    lua.setGameHandler(&handler);
    // 1604 = Silvermoon City (friendly to Horde, hostile to Alliance); 14 = hostile monster
    handler.setFactionFriendlyMap({{1604, true}, {1605, false}, {14, false}});
    handler.setFactionHostileMap({{1604, false}, {1605, true}, {14, true}});
    run(R"lua(
        TOOLTIP_UNIT_LEVEL = 'Level %s'
        TOOLTIP_UNIT_LEVEL_CLASS = 'Level %s %s'
        TOOLTIP_UNIT_LEVEL_CLASS_TYPE = 'Level %s %s (%s)'
        TOOLTIP_UNIT_LEVEL_TYPE = 'Level %s (%s)'
        TOOLTIP_UNIT_LEVEL_RACE_CLASS_TYPE = 'Level %s %s %s (%s)'
        ELITE = 'Elite'
        BOSS = 'Boss'
        PLAYER = 'Player'
        CORPSE = 'Corpse'
        PVP_RANK_LEADER = 'Leader'
        leaderTip = CreateFrame('GameTooltip', 'LeaderTip', UIParent)
    )lua");

    auto sendQuery = [&](uint32_t entry, const char* name, const char* subName,
                         uint32_t creatureType, uint32_t rank, uint8_t racialLeader) {
        wowee::network::Packet info(wireOpcode(Opcode::SMSG_CREATURE_QUERY_RESPONSE));
        info.writeUInt32(entry);
        info.writeString(name);
        info.writeString("");
        info.writeString("");
        info.writeString("");
        info.writeString(subName);
        info.writeString(""); // iconName
        info.writeUInt32(0);  // typeFlags
        info.writeUInt32(creatureType);
        info.writeUInt32(0);  // family
        info.writeUInt32(rank);
        for (int i = 0; i < 6; ++i) info.writeUInt32(0); // wdbField11, petSpellDataId, displayId[4]
        info.writeFloat(1.0f);
        info.writeFloat(1.0f);
        info.writeUInt8(racialLeader);
        session.receive(info);
    };

    // 1. Friendly racial leader (Lor'themar Theron, entry 16802)
    sendQuery(16802, "Lor'themar Theron", "Regent Lord of Quel'Thalas", 7, 3, 1);
    REQUIRE(handler.isCreatureRacialLeader(16802));
    {
        constexpr uint64_t guid = 0xf130000000300001ull;
        auto unit = std::make_shared<Unit>(guid);
        unit->setName("Lor'themar Theron");
        unit->setEntry(16802);
        unit->setLevel(73);
        unit->setHealth(100);
        unit->setMaxHealth(100);
        unit->setField(fieldIndex(UF::UNIT_FIELD_LEVEL), 73);
        unit->setField(fieldIndex(UF::UNIT_FIELD_FLAGS), 0x1000); // UNIT_FLAG_PVP
        unit->setFactionTemplate(1604);
        unit->setHostile(false);
        handler.getEntityManager().addEntity(guid, unit);
        handler.setTarget(guid);
        run("assert(leaderTip:SetUnit('target'))");
        const auto* tip = lua.widgets().findByName("LeaderTip");
        REQUIRE(tip);
        REQUIRE(tip->tooltipLines.size() == 5);
        CHECK(tip->tooltipLines[0].left == "Lor'themar Theron");
        CHECK(tip->tooltipLines[1].left == "Regent Lord of Quel'Thalas");
        CHECK(tip->tooltipLines[2].left == "Level ?? (Boss)");
        CHECK(tip->tooltipLines[3].left == "PvP");
        CHECK(tip->tooltipLines[4].left == "Leader");
        CHECK(tip->tooltipLines[4].lc[0] == Catch::Approx(1.0f));
        CHECK(tip->tooltipLines[4].lc[1] == Catch::Approx(1.0f));
        CHECK(tip->tooltipLines[4].lc[2] == Catch::Approx(1.0f));
    }

    // 2. Hostile racial leader (same creature viewed as hostile)
    {
        constexpr uint64_t guid = 0xf130000000300002ull;
        auto unit = std::make_shared<Unit>(guid);
        unit->setName("Lor'themar Theron");
        unit->setEntry(16802);
        unit->setLevel(73);
        unit->setHealth(100);
        unit->setMaxHealth(100);
        unit->setField(fieldIndex(UF::UNIT_FIELD_LEVEL), 73);
        unit->setField(fieldIndex(UF::UNIT_FIELD_FLAGS), 0x1000); // UNIT_FLAG_PVP
        unit->setFactionTemplate(1605);
        unit->setHostile(true);
        handler.getEntityManager().addEntity(guid, unit);
        handler.setTarget(guid);
        run("assert(leaderTip:SetUnit('target'))");
        const auto* tip = lua.widgets().findByName("LeaderTip");
        REQUIRE(tip);
        REQUIRE(tip->tooltipLines.size() == 5);
        CHECK(tip->tooltipLines[0].left == "Lor'themar Theron");
        CHECK(tip->tooltipLines[1].left == "Regent Lord of Quel'Thalas");
        CHECK(tip->tooltipLines[2].left == "Level ?? Humanoid (Boss)");
        CHECK(tip->tooltipLines[3].left == "PvP");
        CHECK(tip->tooltipLines[4].left == "Leader");
    }

    // 3. Non-leader boss (racialLeader = 0): must NOT show Leader line
    sendQuery(15990, "Kel'Thuzad", "", 6, 3, 0);
    REQUIRE_FALSE(handler.isCreatureRacialLeader(15990));
    {
        constexpr uint64_t guid = 0xf130000000300003ull;
        auto unit = std::make_shared<Unit>(guid);
        unit->setName("Kel'Thuzad");
        unit->setEntry(15990);
        unit->setLevel(63);
        unit->setHealth(100);
        unit->setMaxHealth(100);
        unit->setField(fieldIndex(UF::UNIT_FIELD_LEVEL), 63);
        unit->setField(fieldIndex(UF::UNIT_FIELD_FLAGS), 0x1000); // UNIT_FLAG_PVP
        unit->setFactionTemplate(14);
        unit->setHostile(true);
        handler.getEntityManager().addEntity(guid, unit);
        handler.setTarget(guid);
        run("assert(leaderTip:SetUnit('target'))");
        const auto* tip = lua.widgets().findByName("LeaderTip");
        REQUIRE(tip);
        REQUIRE(tip->tooltipLines.size() == 3);
        CHECK(tip->tooltipLines[0].left == "Kel'Thuzad");
        CHECK(tip->tooltipLines[1].left == "Level ?? Undead (Boss)");
        CHECK(tip->tooltipLines[2].left == "PvP");
    }

    // 4. Hidden vs visible faction flags (SMSG_INITIALIZE_FACTIONS)
    {
        wowee::network::Packet initFactions(wireOpcode(Opcode::SMSG_INITIALIZE_FACTIONS));
        initFactions.writeUInt32(4);
        initFactions.writeUInt8(GameHandler::FACTION_FLAG_VISIBLE | GameHandler::FACTION_FLAG_PEACE_FORCED);
        initFactions.writeUInt32(0); // repListId 0: visible (e.g. Ironforge for Alliance)
        initFactions.writeUInt8(0);
        initFactions.writeUInt32(0); // repListId 1: unmet neutral (e.g. Gadgetzan)
        initFactions.writeUInt8(GameHandler::FACTION_FLAG_AT_WAR | GameHandler::FACTION_FLAG_HIDDEN);
        initFactions.writeUInt32(0); // repListId 2: hidden opposing city (e.g. Silvermoon City for Alliance)
        initFactions.writeUInt8(GameHandler::FACTION_FLAG_INVISIBLE_FORCED);
        initFactions.writeUInt32(0); // repListId 3: forced invisible
        session.receive(initFactions);
        CHECK_FALSE(handler.isFactionHidden(0));
        CHECK_FALSE(handler.isFactionHidden(1));
        CHECK(handler.isFactionHidden(2));
        CHECK(handler.isFactionHidden(3));
        CHECK_FALSE(handler.isFactionHidden(99));
    }

    lua.setGameHandler(nullptr);
}

TEST_CASE_METHOD(Integration, "Party roster identity and map coordinates survive reconnect", "[party-reconnect]") {
    using namespace wowee::game;
    replay::Session session("tbc");
    auto& handler = session.handler();
    lua.setGameHandler(&handler);
    const auto roster = [&](bool online) {
        wowee::network::Packet packet(wireOpcode(Opcode::SMSG_GROUP_LIST));
        packet.writeUInt8(0); packet.writeUInt8(0); packet.writeUInt8(0); packet.writeUInt8(0);
        packet.writeUInt64(1); packet.writeUInt32(1);
        packet.writeString("Graulberto"); packet.writeUInt64(3);
        packet.writeUInt8(online); packet.writeUInt8(0); packet.writeUInt8(0);
        packet.writeUInt64(3);
        session.receive(packet);
    };
    roster(true);
    REQUIRE_FALSE(handler.getPartyData().members.front().hasPartyStats);
    truth("UnitName('party1') == 'Graulberto'");
    truth("UnitExists('party1') and not UnitIsDead('party1')");
    int mapCalls = 0;
    addons::LuaServices services;
    services.mapUVForMapPos = [&](uint32_t map, float x, float y, float, float& u, float& v) {
        ++mapCalls;
        CHECK(map == 1);
        CHECK(x == -350);
        CHECK(y == 7300);
        u = .25f; v = .75f;
        return true;
    };
    lua.setLuaServices(services);
    GameHandlerTestAccess::setAreaMap(handler, 148, 1);
    truth("GetPlayerMapPosition('party1') == 0");
    CHECK(mapCalls == 0);
    wowee::network::Packet stats(wireOpcode(Opcode::SMSG_PARTY_MEMBER_STATS_FULL));
    stats.writePackedGuid(3); stats.writeUInt32(0x187);
    stats.writeUInt16(1); stats.writeUInt16(417); stats.writeUInt16(530);
    stats.writeUInt16(148);
    stats.writeUInt16(7300); stats.writeUInt16(static_cast<uint16_t>(-350));
    session.receive(stats);
    truth("select(1, GetPlayerMapPosition('party1')) == .25 and select(2, GetPlayerMapPosition('party1')) == .75");
    roster(false);
    roster(true);
    truth("UnitName('party1') == 'Graulberto' and UnitHealth('party1') == 417");
    truth("GetPlayerMapPosition('party1') == .25");
    bool partyNameEvent = false;
    handler.setAddonEventCallback([&](const std::string& event, const std::vector<std::string>& args) {
        if (event == "UNIT_NAME_UPDATE" && args == std::vector<std::string>{"party1"}) partyNameEvent = true;
    });
    wowee::network::Packet name(wireOpcode(Opcode::SMSG_NAME_QUERY_RESPONSE));
    name.writeUInt64(3); name.writeString("Graulberto"); name.writeString("");
    name.writeUInt32(3); name.writeUInt32(0); name.writeUInt32(4); name.writeUInt8(0);
    session.receive(name);
    CHECK(partyNameEvent);
    handler.setAddonEventCallback({});
    lua.setLuaServices({});
}

TEST_CASE_METHOD(Integration, "TBC flight discovery queries once and follows server knowledge", "[taxi-discovery]") {
    using namespace wowee::game;
    replay::Session session("tbc");
    auto& handler = session.handler();
    constexpr uint64_t guid = 0xf13000000100f001ull;
    auto master = std::make_shared<Unit>(guid);
    master->setHealth(100);
    master->setNpcFlags(NPC_FLAG_FLIGHT_MASTER);
    master->setFactionTemplate(35);
    handler.getEntityManager().addEntity(guid, master);
    CHECK(handler.getTaxiNodeStatus(guid) == TaxiNodeStatus::Unqueried);
    CHECK_FALSE(taxiDiscoveryMarkerVisible(handler.getTaxiNodeStatus(guid), false));
    handler.updateTaxiNodeStatuses();
    CHECK(handler.getTaxiNodeStatus(guid) == TaxiNodeStatus::Pending);
    handler.updateTaxiNodeStatuses();
    auto queries = session.sent().of(Opcode::CMSG_TAXINODE_STATUS_QUERY);
    REQUIRE(queries.size() == 1);
    REQUIRE(queries[0].getRemainingSize() == 8);
    CHECK(queries[0].readUInt64() == guid);
    const auto status = [&](uint8_t known) {
        wowee::network::Packet packet(wireOpcode(Opcode::SMSG_TAXINODE_STATUS));
        packet.writeUInt64(guid);
        packet.writeUInt8(known);
        session.receive(packet);
    };
    status(0);
    CHECK(handler.getTaxiNodeStatus(guid) == TaxiNodeStatus::Undiscovered);
    CHECK(taxiDiscoveryMarkerVisible(handler.getTaxiNodeStatus(guid), false));
    CHECK_FALSE(taxiDiscoveryMarkerVisible(handler.getTaxiNodeStatus(guid), true));
    status(1); // CMaNGOS SendLearnNewTaxiNode refresh after first interaction.
    CHECK(handler.getTaxiNodeStatus(guid) == TaxiNodeStatus::Discovered);
    CHECK_FALSE(taxiDiscoveryMarkerVisible(handler.getTaxiNodeStatus(guid), false));
    master->setNpcFlags(0);
    handler.updateTaxiNodeStatuses();
    CHECK(handler.getTaxiNodeStatus(guid) == TaxiNodeStatus::Unqueried);
    master->setNpcFlags(NPC_FLAG_FLIGHT_MASTER);
    handler.updateTaxiNodeStatuses();
    CHECK(session.sent().of(Opcode::CMSG_TAXINODE_STATUS_QUERY).size() == 2);
    master->setHealth(0);
    handler.updateTaxiNodeStatuses();
    CHECK(handler.getTaxiNodeStatus(guid) == TaxiNodeStatus::Unqueried);
    CHECK(session.sent().of(Opcode::CMSG_TAXINODE_STATUS_QUERY).size() == 2);
    master->setHealth(100);
    master->setHostile(true);
    handler.updateTaxiNodeStatuses();
    CHECK(handler.getTaxiNodeStatus(guid) == TaxiNodeStatus::Unqueried);
    CHECK(session.sent().of(Opcode::CMSG_TAXINODE_STATUS_QUERY).size() == 2);
    master->setHostile(false);
    handler.updateTaxiNodeStatuses();
    CHECK(session.sent().of(Opcode::CMSG_TAXINODE_STATUS_QUERY).size() == 3);
    handler.getEntityManager().removeEntity(guid);
    handler.updateTaxiNodeStatuses();
    status(0); // Delayed reply cannot recreate a despawned marker.
    CHECK(handler.getTaxiNodeStatus(guid) == TaxiNodeStatus::Unqueried);
    handler.getEntityManager().addEntity(guid, master);
    handler.updateTaxiNodeStatuses();
    status(0);
    handler.clearTaxiNodeStatuses(); // World transfer / character login path.
    CHECK_FALSE(taxiDiscoveryMarkerVisible(handler.getTaxiNodeStatus(guid), false));
}

TEST_CASE_METHOD(Integration, "A click on a nameplate acts on the plate's unit, not the current target", "[input][nameplate][O94]") {
    using namespace wowee::game;
    // Ledger #94: right-clicking an enemy's plate with a corpse selected
    // opened the corpse's loot. The world ray misses (the plate floats above
    // its unit) and the click fell back to the current target.
    constexpr uint64_t kCorpse = 0xF130000000000C01ull;
    constexpr uint64_t kPlateUnit = 0xF130000000000D02ull;
    replay::Session session;
    session.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}});
    session.receive(replay::UpdateObject()
        .create(kCorpse, ObjectType::UNIT, {{UF::UNIT_FIELD_HEALTH, 0}, {UF::UNIT_FIELD_MAXHEALTH, 50}})
        .create(kPlateUnit, ObjectType::UNIT, {{UF::UNIT_FIELD_HEALTH, 50}, {UF::UNIT_FIELD_MAXHEALTH, 50}}));
    auto& handler = session.handler();
    handler.setTarget(kCorpse);
    REQUIRE(handler.getTargetGuid() == kCorpse);

    ui::GameScreen screen;
    auto& input = core::Input::getInstance();
    // Earlier cases leave a FrameXML frame owning the pointer; nothing is
    // drawn over the plate here.
    ui::frameXmlNoteMouseOwned(false);
    // No cursor artwork in the test tree: hide the cursor so the frame skips
    // selecting the gauntlet texture, which is all that needs assets here.
    const auto frame = [&] {
        ImGui::NewFrame();
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
        screen.render(handler);
        ImGui::EndFrame();
    };
    const auto click = [&](uint8_t button, float x, float y) {
        ui::GameScreenTestAccess::setPlateHits(screen,
            {{.guid = kPlateUnit, .box = ui::PlateBox{.x0 = 100, .y0 = 50, .x1 = 230, .y1 = 80}}});
        SDL_Event event{};
        event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        event.button.button = button;
        event.button.x = x;
        event.button.y = y;
        input.processEvent(event);
        input.update();
        frame();
        event.type = SDL_EVENT_MOUSE_BUTTON_UP;
        input.processEvent(event);
        input.update();
        ui::GameScreenTestAccess::setPlateHits(screen,
            {{.guid = kPlateUnit, .box = ui::PlateBox{.x0 = 100, .y0 = 50, .x1 = 230, .y1 = 80}}});
        frame();
        input.update();
    };

    click(SDL_BUTTON_RIGHT, 150, 60);
    CHECK(handler.getTargetGuid() == kPlateUnit);
    CHECK(session.sent().of(Opcode::CMSG_LOOT).empty());

    handler.setTarget(kCorpse);
    click(SDL_BUTTON_LEFT, 150, 60);
    CHECK(handler.getTargetGuid() == kPlateUnit);
    CHECK(session.sent().of(Opcode::CMSG_LOOT).empty());
}

TEST_CASE_METHOD(Integration, "Every widget carries its C handle in slot 0", "[lua][widget-handle]") {
    // Tablet-2.0 Register(frame, otherFrame) and AceHook-2.1 HookScript tell a
    // widget from a plain table by t[0]; the original client fills it with the
    // widget's userdata on frames and regions alike.
    run(R"lua(
        handleFrame = CreateFrame('Button', nil, UIParent)
        handleOther = CreateFrame('Frame')
        handleTex = handleFrame:CreateTexture()
        handleText = handleFrame:CreateFontString()
    )lua");
    truth("type(handleFrame[0]) == 'userdata' and type(handleTex[0]) == 'userdata' and type(handleText[0]) == 'userdata'");
    truth("handleFrame[0] ~= handleOther[0] and handleFrame[0] ~= handleTex[0]");
    truth("#handleFrame == 0");
}

TEST_CASE_METHOD(Integration, "GetItemIcon answers an item's icon path", "[lua][item-icon]") {
    // MangAdmin's item search sets every result row's icon with
    // GetItemIcon(id); while it was missing the first row raised and the
    // list stayed empty.
    replay::Session session("tbc");
    auto& handler = session.handler();
    lua.setGameHandler(&handler);
    handler.setItemIconPathResolver([](uint32_t displayId) {
        return displayId == 6442 ? std::string("Interface\\Icons\\INV_Weapon_ShortBlade_05") : std::string();
    });
    auto& info = handler.itemInfoCacheRef()[2092];
    info.valid = true;
    info.name = "Worn Dagger";
    info.displayInfoId = 6442;
    truth("GetItemIcon(2092) == 'Interface\\\\Icons\\\\INV_Weapon_ShortBlade_05'");
    truth("GetItemIcon('item:2092:0:0:0:0:0:0:0') == GetItemIcon(2092)");
    truth("GetItemIcon('|cffffffff|Hitem:2092:0:0:0:0:0:0:0|h[Worn Dagger]|h|r') == GetItemIcon(2092)");
    truth("GetItemIcon(0) == nil");
    lua.setGameHandler(nullptr);
}

TEST_CASE_METHOD(Integration, "An action swapped onto the cursor can be placed or put down", "[action-cursor]") {
    // Original 2.4.3: a spell dropped from the book onto an occupied button
    // leaves the button's old spell on the cursor; a click on another button
    // (SecureActionButton_OnClick -> UseAction) places it instead of using
    // that button, and a click on the world puts a carried action down.
    using namespace wowee::game;
    replay::Session session("tbc");
    auto& handler = session.handler();
    lua.setGameHandler(&handler);
    session.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}});
    handler.spellNameCacheLoadedRef() = true;
    handler.setActionBarSlot(1, ActionBarSlot::SPELL, 1000);
    handler.setActionBarSlot(2, ActionBarSlot::SPELL, 2000);
    const auto casts = [&] { return session.sent().of(Opcode::CMSG_CAST_SPELL).size(); };
    const auto before = casts();

    run("PickupAction(2)");
    truth("CursorHasSpell() and select(4, GetCursorInfo()) == 1000");
    CHECK(handler.getActionBar()[1].isEmpty());

    run("UseAction(3)");  // occupied: swap
    CHECK(handler.getActionBar()[2].id == 1000);
    truth("select(4, GetCursorInfo()) == 2000");
    run("UseAction(5)");  // empty: place, cursor empties
    CHECK(handler.getActionBar()[4].id == 2000);
    truth("not CursorHasSpell() and GetCursorInfo() == nil");
    CHECK(casts() == before);

    run("PickupAction(5)");
    truth("CursorHasSpell()");
    addons::LuaEngine::MouseButtons down;
    down.left = true;
    lua.dispatchMouse(400, 300, 600, down);
    lua.dispatchMouse(400, 300, 600, {});
    truth("not CursorHasSpell()");
    CHECK(handler.getActionBar()[4].isEmpty());
    lua.setGameHandler(nullptr);
}

TEST_CASE_METHOD(Integration, "The held cursor is one state: swaps, toggles, returns, picture", "[action-cursor]") {
    // Build 8606, measured with a probe addon (pair 14,
    // _experiments/20261118T203647Z.action_cursor): a swap puts the held action
    // down (HIDEGRID) before the old one comes up (SHOWGRID) and announces the
    // slot last; dropping the action a slot already holds empties the cursor
    // and leaves the slot; a bag item placed on the bar goes back to its bag
    // (ITEM_LOCK_CHANGED first); an item reference off the bar is "item" to
    // GetCursorInfo and nil to CursorHasItem; money shows no grid; a
    // right-click (returnHeldCursor) puts any of it back.
    using namespace wowee::game;
    replay::Session session("tbc");
    auto& handler = session.handler();
    lua.setGameHandler(&handler);
    handler.setAddonEventCallback([this](const std::string& event, const std::vector<std::string>& args) {
        lua.fireEvent(event, args);
    });
    session.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}});
    handler.spellNameCacheLoadedRef() = true;
    handler.setActionBarSlot(1, ActionBarSlot::SPELL, 1000);
    handler.setActionBarSlot(2, ActionBarSlot::SPELL, 2000);
    run(R"(
        seen = {}
        local f = CreateFrame('Frame')
        for _, e in ipairs({'CURSOR_UPDATE', 'ACTIONBAR_SHOWGRID', 'ACTIONBAR_HIDEGRID',
                            'ACTIONBAR_SLOT_CHANGED', 'ITEM_LOCK_CHANGED'}) do f:RegisterEvent(e) end
        f:SetScript('OnEvent', function(self, e, a1) table.insert(seen, e .. (a1 and (':' .. a1) or '')) end)
        function heard() local s = table.concat(seen, ' ') seen = {} return s end
        function at(s, e) return string.find(' ' .. s .. ' ', ' ' .. e .. ' ', 1, true) end
    )");
    const auto held = [&] { return wowee::ui::frameXmlCursorItem(); };

    run("heard() PickupAction(2) s = heard()");
    truth("s == 'CURSOR_UPDATE ACTIONBAR_SHOWGRID ACTIONBAR_SLOT_CHANGED:2'");

    run("PlaceAction(3) s = heard()");   // occupied: swap
    truth("at(s, 'ACTIONBAR_HIDEGRID') < at(s, 'ACTIONBAR_SHOWGRID')");
    truth("string.find(s, 'ACTIONBAR_SLOT_CHANGED:3$') ~= nil");
    CHECK(handler.getActionBar()[2].id == 1000);
    truth("select(4, GetCursorInfo()) == 2000");

    handler.setActionBarSlot(3, ActionBarSlot::SPELL, 2000);
    run("heard() PlaceAction(4) s = heard()");   // the same spell: cursor empties
    truth("s == 'ACTIONBAR_HIDEGRID CURSOR_UPDATE'");
    truth("GetCursorInfo() == nil");
    CHECK(handler.getActionBar()[3].id == 2000);

    // A bag item onto the bar: placed, and back in its bag unlocked.
    auto& info = handler.itemInfoCacheRef()[300];
    info.valid = true;
    info.name = "Test cloak";
    info.inventoryType = 16;
    ItemDef cloak;
    cloak.itemId = 300;
    cloak.stackCount = 1;
    handler.inventoryRef().setBackpackSlot(0, cloak);
    run("PickupContainerItem(0, 1)");
    truth("CursorHasItem() and select(3, GetContainerItemInfo(0, 1))");
    run("heard() PlaceAction(6) s = heard()");
    truth("at(s, 'ITEM_LOCK_CHANGED:0') == 1 and at(s, 'ITEM_LOCK_CHANGED:0') < at(s, 'ACTIONBAR_HIDEGRID')");
    truth("not select(3, GetContainerItemInfo(0, 1)) and GetCursorInfo() == nil");
    CHECK(handler.getActionBar()[5].type == ActionBarSlot::ITEM);

    // Lifted off the bar it is a reference: "item", but not an item held.
    run("PickupAction(6)");
    truth("GetCursorInfo() == 'item' and not CursorHasItem()");
    CHECK(addons::returnHeldCursor(lua.getState()));
    truth("GetCursorInfo() == nil");

    // A bag item put back by a right-click unlocks its slot first.
    run("PickupContainerItem(0, 1) heard()");
    CHECK(addons::returnHeldCursor(lua.getState()));
    run("s = heard()");
    truth("s == 'ITEM_LOCK_CHANGED:0 ACTIONBAR_HIDEGRID CURSOR_UPDATE'");
    CHECK_FALSE(addons::returnHeldCursor(lua.getState()));

    // Money: no grid, and its coin is the picture.
    addons::setCursorMoney(lua.getState(), 5);
    run("s = heard()");
    truth("s == 'CURSOR_UPDATE' and CursorHasMoney()");
    CHECK(held() == "Interface\\Icons\\INV_Misc_Coin_05");
    CHECK(addons::returnHeldCursor(lua.getState()));
    CHECK(held().empty());
    run("heard() ClearCursor() s = heard()");   // nothing held: still announced
    truth("s == 'CURSOR_UPDATE'");
    lua.setGameHandler(nullptr);
}

namespace {
wowee::network::Packet npcTextUpdate(uint32_t textId, std::vector<std::pair<uint32_t, uint32_t>> emotes) {
    wowee::network::Packet text(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_NPC_TEXT_UPDATE));
    text.writeUInt32(textId);
    for (int option = 0; option < 8; ++option) {
        text.writeFloat(option == 0 ? 1.0f : 0.0f);
        text.writeString(option == 0 ? "Ach, it's hard enough keeping order around here" : "");
        text.writeString(option == 0 ? "Ach, it's hard enough keeping order around here" : "");
        text.writeUInt32(0); // language
        for (int e = 0; e < 3; ++e) {
            text.writeUInt32(option == 0 ? emotes[e].first : 0);
            text.writeUInt32(option == 0 ? emotes[e].second : 0);
        }
    }
    return text;
}

wowee::network::Packet gossipWithText(uint64_t npc, uint32_t textId) {
    wowee::network::Packet gossip(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_GOSSIP_MESSAGE));
    gossip.writeUInt64(npc);
    gossip.writeUInt32(11611); // menu
    gossip.writeUInt32(textId);
    gossip.writeUInt32(0); // options
    gossip.writeUInt32(0); // quests
    return gossip;
}
} // namespace

TEST_CASE_METHOD(Integration, "Gossip and quest greeting panels hand their emotes to the NPC", "[npc-panel-emote]") {
    // Marshal Dughan (240): npc_text 16211 = (5 ms, ONESHOT_EXCLAMATION).
    replay::Session session("tbc");
    auto& handler = session.handler();
    std::vector<std::pair<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>>> played;
    handler.setNpcPanelEmotesCallback([&](uint64_t guid, const std::vector<std::pair<uint32_t, uint32_t>>& e) {
        played.emplace_back(guid, e);
    });
    const std::vector<std::pair<uint32_t, uint32_t>> dughan{{5, 5}, {0, 0}, {0, 0}};
    // First visit: the text arrives after the gossip message.
    session.receive(gossipWithText(240, 16211));
    CHECK(played.empty());
    session.receive(npcTextUpdate(16211, dughan));
    REQUIRE(played.size() == 1);
    CHECK(played[0].first == 240);
    CHECK(played[0].second == dughan);
    // Repeat visit with cached text: native plays again.
    handler.closeGossip();
    session.receive(gossipWithText(240, 16211));
    REQUIRE(played.size() == 2);
    CHECK(played[1].second == dughan);
    // Quest greeting (Gryan Stoutmantle 234: EmoteDelay 0, EmoteId 1 talk).
    wowee::network::Packet list(wowee::game::wireOpcode(wowee::game::Opcode::SMSG_QUESTGIVER_QUEST_LIST));
    list.writeUInt64(234);
    list.writeString("A foul corruption has crept into Westfall.");
    list.writeUInt32(0);
    list.writeUInt32(1);
    list.writeUInt8(0);
    session.receive(list);
    REQUIRE(played.size() == 3);
    CHECK(played[2].first == 234);
    CHECK(played[2].second == std::vector<std::pair<uint32_t, uint32_t>>{{0, 1}});
}

TEST_CASE("Panel emotes run in order, each after the previous animation", "[npc-panel-emote]") {
    wowee::core::NpcPanelEmotes emotes;
    std::vector<uint32_t> started;
    bool playing = false;
    auto still = [&](uint64_t, uint32_t) { return playing; };
    auto play = [&](uint64_t, uint32_t emote) { started.push_back(emote); playing = true; };
    // Mizli Crankwheel: (0, TALK), (3, EXCLAMATION).
    emotes.start(7, {{0, 1}, {3, 5}, {0, 0}});
    emotes.advance(0.0f, still, play);
    REQUIRE(started == std::vector<uint32_t>{1});
    emotes.advance(1.0f, still, play); // talk still running
    CHECK(started.size() == 1);
    playing = false;
    emotes.advance(0.002f, still, play); // talk ended; 3 ms delay not yet over
    CHECK(started.size() == 1);
    emotes.advance(0.002f, still, play);
    CHECK(started == std::vector<uint32_t>{1, 5});
    CHECK_FALSE(emotes.pending(7));
    // All-zero emotes (and ONESHOT_NONE entries) play nothing.
    emotes.start(8, {{5, 0}, {0, 0}, {0, 0}});
    CHECK_FALSE(emotes.pending(8));
    // Dughan's 5 ms delay.
    emotes.start(9, {{5, 5}, {0, 0}, {0, 0}});
    emotes.advance(0.004f, still, play);
    CHECK(started.size() == 2);
    emotes.advance(0.002f, still, play);
    CHECK(started.back() == 5);
}

TEST_CASE_METHOD(Integration, "Hidden and debug bindings are not listed", "[input][lua][bindings]") {
    // 2.4.3 GetNumBindings counts 226: Bindings.xml without its thirteen
    // hidden/debug rows (TURNORACTION, TOGGLESTATS ...), which stay bound.
    ui::XmlNode root;
    std::string error;
    REQUIRE(ui::parseXml("<Bindings><Binding name='JUMP'>JumpOrAscendStart();</Binding>"
                         "<Binding name='TURNORACTION' runOnUp='true' hidden='true'>TurnOrActionStart();</Binding>"
                         "<Binding name='TOGGLESTATS' hidden='true' debug='true'>ToggleStats();</Binding></Bindings>",
                         root, error));
    run(ui::emitFrameXml(root).lua);
    run(R"lua(
        local listed = {}
        for i = 1, GetNumBindings() do listed[(GetBinding(i))] = true end
        assert(listed.JUMP and not listed.TURNORACTION and not listed.TOGGLESTATS)
        assert(__WoweeBindingHidden.TURNORACTION and __WoweeBindingHidden.TOGGLESTATS)
        assert(type(__WoweeBindingScripts.TURNORACTION) == 'function')
    )lua");
}

TEST_CASE_METHOD(Integration, "MessageFrame and ScrollingMessageFrame defaults, fade window, and scroll offset", "[input][lua][message-frame]") {
    run(R"lua(
        MF = CreateFrame("MessageFrame", "TestMF", UIParent)
        MF:SetFont("Fonts\\FRIZQT__.TTF", 16)
        MF:SetTextColor(1, 0.82, 0)
        assert(MF:GetInsertMode() == "BOTTOM")
        assert(MF:GetTimeVisible() == 10)
        assert(MF:GetFadeDuration() == 3)
        assert(MF:GetFading())
        MF:SetInsertMode("TOP")
        MF:SetTimeVisible(5)
        MF:AddMessage("First")
        MF:AddMessage("Second", 1, 0.1, 0.1)

        SMF = CreateFrame("ScrollingMessageFrame", "TestSMF", UIParent)
        SMF:SetFont("Fonts\\FRIZQT__.TTF", 12)
        SMF:SetTextColor(1, 0.82, 0)
        assert(SMF:GetInsertMode() == "BOTTOM")
        assert(SMF:GetTimeVisible() == 10)
        assert(SMF:GetFadeDuration() == 3)
        assert(SMF:GetMaxLines() == 8)
        SMF:SetInsertMode("TOP")
        SMF:SetFading(false)
        for i = 1, 5 do SMF:AddMessage("E" .. i) end
        SMF:SetScrollOffset(2)
        assert(SMF:GetCurrentScroll() == 2)
    )lua");

    const auto* mf = lua.widgets().findByName("TestMF");
    REQUIRE(mf);
    REQUIRE(mf->messages.size() == 2);
    // Chronological storage: [0] is oldest, [1] is newest.
    CHECK(mf->messages[0].text == "First");
    CHECK(mf->messages[1].text == "Second");
    // MessageFrame defaults omitted r,g,b to white despite gold SetTextColor.
    CHECK(mf->messages[0].color[1] == Catch::Approx(1.0f));
    CHECK(mf->messages[0].color[2] == Catch::Approx(1.0f));
    CHECK(mf->messages[1].color[1] == Catch::Approx(0.1f));

    const auto* smf = lua.widgets().findByName("TestSMF");
    REQUIRE(smf);
    REQUIRE(smf->messages.size() == 5);
    // ScrollingMessageFrame falls back to the frame's text colour.
    CHECK(smf->messages[0].color[1] == Catch::Approx(0.82f));
    CHECK(smf->messages[0].color[2] == Catch::Approx(0.0f));
    CHECK(smf->messageScroll == 2);

    // Hold full opacity for timeVisible (5 s), then linear fade over fadeDuration (3 s).
    lua.expireMessages(4.9f);
    CHECK(mf->messages[1].color[3] == Catch::Approx(1.0f));
    lua.expireMessages(1.6f); // age = 6.5 s -> halfway through 3 s fade
    CHECK(mf->messages[1].color[3] == Catch::Approx(0.5f));
    lua.expireMessages(1.6f); // age = 8.1 s -> fully faded
    CHECK(mf->messages[1].color[3] == Catch::Approx(0.0f));
}

TEST_CASE_METHOD(Integration, "Spell unit targeting, autoSelfCast default, and SELFCAST modified click follow 2.4.3", "[input][screen][selfcast]") {
    replay::Session session;
    session.spawnPlayer({{wowee::game::UF::UNIT_FIELD_HEALTH, 100}});
    auto& gh = session.handler();
    gh.spellNameCacheLoadedRef() = true;

    // Fresh 2.4.3 default: autoSelfCast is off ("0").
    CHECK_FALSE(gh.isAutoSelfCast());
    lua.setGameHandler(&gh);
    run("assert(GetCVar('autoSelfCast') == '0')");

    constexpr uint32_t kArcaneIntellect = 27126; // EffectImplicitTargetA = 21 (Friend)
    constexpr uint32_t kPowerWordShield = 25218; // EffectImplicitTargetA = 57 (GroupMember)
    constexpr uint32_t kSmite           = 25364; // EffectImplicitTargetA = 6  (Enemy)
    gh.spellNameCacheRef()[kArcaneIntellect] = {.name = "Arcane Intellect", .rank = "Rank 6", .maxRange = 30.0f, .implicitTargetA = 21};
    gh.spellNameCacheRef()[kPowerWordShield] = {.name = "Power Word: Shield", .rank = "Rank 12", .maxRange = 40.0f, .implicitTargetA = 57};
    gh.spellNameCacheRef()[kSmite]           = {.name = "Smite", .rank = "Rank 10", .maxRange = 30.0f, .implicitTargetA = 6};
    gh.setActionBarSlot(0, wowee::game::ActionBarSlot::SPELL, kArcaneIntellect);
    gh.setActionBarSlot(1, wowee::game::ActionBarSlot::SPELL, kPowerWordShield);
    gh.setActionBarSlot(2, wowee::game::ActionBarSlot::SPELL, kSmite);

    ui::GameScreen screen;
    const auto render = [&] {
        ImGui::NewFrame();
        screen.render(gh);
        ImGui::EndFrame();
    };
    const auto casts = [&] { return session.sent().of(wowee::game::Opcode::CMSG_CAST_SPELL); };
    render();

    // 1. With no target and autoSelfCast "0", pressing '1' (Arcane Intellect)
    // arms the targeting cursor rather than self-casting.
    REQUIRE(manager.setBinding("1", "ACTIONBUTTON1"));
    const auto before = casts().size();
    manager.beginInputFrame();
    event(SDL_SCANCODE_1);
    event(SDL_SCANCODE_1, false);
    render();
    CHECK(casts().size() == before);
    CHECK(gh.pendingUnitTargetSpell() == kArcaneIntellect);
    run("assert(SpellIsTargeting())");
    run("assert(SpellCanTargetUnit('player'))");

    // SpellStopTargeting puts the cursor down without sending a cast.
    run("assert(SpellStopTargeting())");
    CHECK(gh.pendingUnitTargetSpell() == 0);
    run("assert(not SpellIsTargeting())");

    // 2. Holding Alt (default SELFCAST = ALT) self-casts Arcane Intellect.
    manager.beginInputFrame();
    event(SDL_SCANCODE_1, true, SDL_KMOD_ALT);
    event(SDL_SCANCODE_1, false, SDL_KMOD_ALT);
    render();
    REQUIRE(casts().size() == before + 1);
    CHECK(gh.pendingUnitTargetSpell() == 0);

    // 3. Smite with no target refuses without sending or arming the cursor.
    REQUIRE(manager.setBinding("3", "ACTIONBUTTON3"));
    manager.beginInputFrame();
    event(SDL_SCANCODE_3);
    event(SDL_SCANCODE_3, false);
    render();
    CHECK(casts().size() == before + 1);
    CHECK(gh.pendingUnitTargetSpell() == 0);

    // 4. With autoSelfCast "1" and no target, Arcane Intellect self-casts.
    gh.setAutoSelfCast(true);
    manager.beginInputFrame();
    event(SDL_SCANCODE_1);
    event(SDL_SCANCODE_1, false);
    render();
    CHECK(casts().size() == before + 2);
    gh.setAutoSelfCast(false);

    // 5. SecureCmdOptionParse returns (action, target) for /cast [target=player].
    run(R"lua(
        local a, t = SecureCmdOptionParse("[target=player] Arcane Intellect")
        assert(a == "Arcane Intellect" and t == "player")
        local a2, t2 = SecureCmdOptionParse("Arcane Intellect")
        assert(a2 == "Arcane Intellect" and t2 == nil)
        assert(SecureCmdOptionParse("") == "")
        assert(SecureCmdOptionParse("[combat] Smite") == nil)
    )lua");

    // 6. Hover-selfcast / SpellTargetUnit("player") with an existing hostile target:
    // casts Thorns on self while preserving the hostile target, and refuses hostile-only Wrath on self.
    constexpr uint32_t kThorns = 467;  // EffectImplicitTargetA = 21 (Friend)
    constexpr uint32_t kWrath  = 5176; // EffectImplicitTargetA = 6  (Enemy)
    gh.spellNameCacheRef()[kThorns] = {.name = "Thorns", .rank = "Rank 1", .maxRange = 30.0f, .implicitTargetA = 21};
    gh.spellNameCacheRef()[kWrath]  = {.name = "Wrath",  .rank = "Rank 1", .maxRange = 30.0f, .implicitTargetA = 6};
    constexpr uint64_t kHostileMob = 0xF130000100ull;
    session.receive(replay::UpdateObject().create(
        kHostileMob, wowee::game::ObjectType::UNIT,
        {{wowee::game::UF::UNIT_FIELD_HEALTH, 100},
         {wowee::game::UF::UNIT_FIELD_MAXHEALTH, 100},
         {wowee::game::UF::UNIT_FIELD_FACTIONTEMPLATE, 14}},
        false));
    gh.setTarget(kHostileMob);
    REQUIRE(gh.getTargetGuid() == kHostileMob);

    // Casting Thorns while targeting a hostile mob arms SpellIsTargeting().
    const auto beforeThorns = casts().size();
    gh.castSpell(kThorns, 0);
    CHECK(casts().size() == beforeThorns);
    CHECK(gh.isAwaitingUnitTarget());
    CHECK(gh.pendingUnitTargetSpell() == kThorns);
    CHECK(gh.canSpellTargetUnit(kThorns, gh.getPlayerGuid()));
    CHECK_FALSE(gh.canSpellTargetUnit(kWrath, gh.getPlayerGuid()));

    // Completing on local player (via 3D hover-click or PlayerFrame) casts on self and preserves hostile target.
    run("assert(SpellTargetUnit('player'))");
    REQUIRE(casts().size() == beforeThorns + 1);
    CHECK_FALSE(gh.isAwaitingUnitTarget());
    CHECK(gh.getTargetGuid() == kHostileMob);
}

TEST_CASE("TBC SMSG_LEARNED_SPELL and SMSG_SUPERCEDED_SPELL auto-upgrade action bar ranks",
          "[rankupgrade]") {
    using namespace wowee::game;

    replay::Session session("tbc");
    auto& gh = session.handler();
    gh.spellNameCacheLoadedRef() = true;
    gh.spellNameCacheRef()[686]  = {.name = "Shadow Bolt", .rank = "Rank 1"};
    gh.spellNameCacheRef()[695]  = {.name = "Shadow Bolt", .rank = "Rank 2"};
    gh.spellNameCacheRef()[705]  = {.name = "Shadow Bolt", .rank = "Rank 3"};
    gh.spellNameCacheRef()[703]  = {.name = "Garrote",     .rank = "Rank 1"};
    gh.spellNameCacheRef()[8631] = {.name = "Garrote",     .rank = "Rank 2"};

    auto decodeSetButtons = [&session]() {
        std::vector<std::pair<uint8_t, uint32_t>> out;
        for (auto pkt : session.sent().of(Opcode::CMSG_SET_ACTION_BUTTON)) {
            const uint8_t btn = pkt.readUInt8();
            const uint32_t packed = pkt.readUInt32();
            out.emplace_back(btn, packed);
        }
        return out;
    };

    // 1. SMSG_INITIAL_SPELLS on login must NOT emit CMSG_SET_ACTION_BUTTON.
    gh.actionBarRef()[4]  = {.type = ActionBarSlot::SPELL, .id = 686}; // slot 5 (main bar)
    gh.actionBarRef()[60] = {.type = ActionBarSlot::SPELL, .id = 686}; // slot 61 (multi-bar)
    {
        wowee::network::Packet init(wireOpcode(Opcode::SMSG_INITIAL_SPELLS));
        init.writeUInt8(0);      // talentSpec / unk
        init.writeUInt16(1);     // spellCount
        init.writeUInt16(686);   // Shadow Bolt (Rank 1)
        init.writeUInt16(0);     // slot
        init.writeUInt16(0);     // cooldownCount
        session.receive(init);
    }
    CHECK(gh.getKnownSpells().count(686) == 1);
    CHECK(decodeSetButtons().empty());

    // 2. Learning Shadow Bolt Rank 2 upgrades both main-bar slot 5 and multi-bar slot 61,
    //    fires ACTIONBAR_SLOT_CHANGED before SPELLS_CHANGED, and persists via CMSG_SET_ACTION_BUTTON.
    std::vector<std::pair<std::string, std::string>> events;
    gh.setAddonEventCallback([&events](const std::string& name, const std::vector<std::string>& args) {
        events.emplace_back(name, args.empty() ? "" : args[0]);
    });
    {
        wowee::network::Packet learned(wireOpcode(Opcode::SMSG_LEARNED_SPELL));
        learned.writeUInt32(695);
        session.receive(learned);
    }
    CHECK(gh.actionBarRef()[4].id == 695);
    CHECK(gh.actionBarRef()[60].id == 695);
    const auto sentAfterR2 = decodeSetButtons();
    REQUIRE(sentAfterR2.size() == 2);
    CHECK(sentAfterR2[0] == std::pair<uint8_t, uint32_t>{4, 695u});
    CHECK(sentAfterR2[1] == std::pair<uint8_t, uint32_t>{60, 695u});
    REQUIRE(events.size() >= 2);
    CHECK(events[0] == std::pair<std::string, std::string>{"ACTIONBAR_SLOT_CHANGED", "5"});
    CHECK(events[1] == std::pair<std::string, std::string>{"ACTIONBAR_SLOT_CHANGED", "61"});

    // 3. Intentional downrank preservation: slot 5 holds Rank 1, slot 6 & 61 hold Rank 2.
    //    Learning Rank 3 upgrades ONLY slots 6 and 61, leaving slot 5 at Rank 1.
    gh.actionBarRef()[4] = {.type = ActionBarSlot::SPELL, .id = 686};
    gh.actionBarRef()[5] = {.type = ActionBarSlot::SPELL, .id = 695};
    events.clear();
    {
        wowee::network::Packet learned(wireOpcode(Opcode::SMSG_LEARNED_SPELL));
        learned.writeUInt32(705);
        session.receive(learned);
    }
    CHECK(gh.actionBarRef()[4].id == 686);
    CHECK(gh.actionBarRef()[5].id == 705);
    CHECK(gh.actionBarRef()[60].id == 705);
    const auto sentAfterR3 = decodeSetButtons();
    REQUIRE(sentAfterR3.size() == 4);
    CHECK(sentAfterR3[2] == std::pair<uint8_t, uint32_t>{5, 705u});
    CHECK(sentAfterR3[3] == std::pair<uint8_t, uint32_t>{60, 705u});

    // 4. SMSG_SUPERCEDED_SPELL upgrades slot and sends CMSG_SET_ACTION_BUTTON once;
    //    a trailing SMSG_LEARNED_SPELL for the same new spell does not double-send.
    gh.actionBarRef()[72] = {.type = ActionBarSlot::SPELL, .id = 703}; // slot 73 (stance bar)
    {
        wowee::network::Packet learn1(wireOpcode(Opcode::SMSG_LEARNED_SPELL));
        learn1.writeUInt32(703);
        session.receive(learn1);
    }
    const auto beforeSupercede = decodeSetButtons().size();
    {
        wowee::network::Packet superceded(wireOpcode(Opcode::SMSG_SUPERCEDED_SPELL));
        superceded.writeUInt16(703);
        superceded.writeUInt16(8631);
        session.receive(superceded);
        wowee::network::Packet learn2(wireOpcode(Opcode::SMSG_LEARNED_SPELL));
        learn2.writeUInt32(8631);
        session.receive(learn2);
    }
    CHECK(gh.actionBarRef()[72].id == 8631);
    const auto afterSupercede = decodeSetButtons();
    REQUIRE(afterSupercede.size() == beforeSupercede + 1);
    CHECK(afterSupercede.back() == std::pair<uint8_t, uint32_t>{72, 8631u});

    gh.setAddonEventCallback({});
}

TEST_CASE_METHOD(Integration,
                 "Third-person /follow applies hysteresis, walk-speed matching, and causal pre-step facing",
                 "[input][camera][follow]") {
    auto& input = core::Input::getInstance();
    rendering::Camera camera;
    rendering::CameraController controller(&camera);
    glm::vec3 playerPos(0.0f, 0.0f, 0.0f);
    glm::vec3 leaderPos(0.0f, 5.0f, 0.0f);  // +Y => 90 deg render yaw
    controller.setIdleOrbitEnabled(false);
    controller.setFollowTarget(&playerPos);
    controller.setUseWoWSpeed(true);
    controller.setGravityDisabled(true);
    controller.setFacingYaw(0.0f);

    struct EmittedMove {
        uint32_t opcode = 0;
        glm::vec3 pos{0.0f};
        float syncedYawDeg = 0.0f;
    };
    float syncedPreStepYawDeg = 0.0f;
    std::vector<EmittedMove> emitted;
    controller.setPreIntentFacingSyncCallback([&](float yawDeg) {
        syncedPreStepYawDeg = yawDeg;
    });
    controller.setMovementCallback([&](uint32_t opcode) {
        emitted.push_back({opcode, playerPos, syncedPreStepYawDeg});
    });

    controller.setAutoFollow(&leaderPos);
    controller.setAutoFollowTargetMotion(true, true);  // Leader is walking within 6 yd
    controller.setGrounded(true);
    input.update();
    controller.update(0.05f);

    // Both SET_WALK_MODE and START_FORWARD must fire BEFORE playerPos steps away
    // from (0, 0, 0), and already carry the 90 deg heading toward leaderPos.
    REQUIRE(emitted.size() == 2);
    CHECK(emitted[0].opcode == static_cast<uint32_t>(wowee::game::Opcode::MSG_MOVE_SET_WALK_MODE));
    CHECK(emitted[1].opcode == static_cast<uint32_t>(wowee::game::Opcode::MSG_MOVE_START_FORWARD));
    CHECK(glm::length(emitted[1].pos) < 1e-5f);
    CHECK(emitted[1].syncedYawDeg == Catch::Approx(90.0f).margin(1e-3f));
    // Stepped at walk speed (2.5 yd/s * 0.05 s = 0.125 yd), not run speed (0.35 yd).
    CHECK(playerPos.y == Catch::Approx(0.125f).margin(1e-3f));
    CHECK(std::abs(playerPos.x) < 1e-4f);

    // Move into the [2.65, 3.75] hysteresis band while actively following: must NOT stop.
    emitted.clear();
    playerPos = glm::vec3(0.0f, 1.9f, 0.0f);  // 3.1 yd from leaderPos (0, 5, 0)
    controller.setGrounded(true);
    input.update();
    controller.update(0.05f);
    CHECK(emitted.empty());
    CHECK(controller.isMovingForward());

    // Enter stop threshold (< 2.65 yd): emits MSG_MOVE_STOP at pre-step position.
    playerPos = glm::vec3(0.0f, 2.5f, 0.0f);  // 2.5 yd from leaderPos
    controller.setGrounded(true);
    input.update();
    controller.update(0.05f);
    REQUIRE(emitted.size() == 1);
    CHECK(emitted[0].opcode == static_cast<uint32_t>(wowee::game::Opcode::MSG_MOVE_STOP));
    CHECK(emitted[0].pos.y == Catch::Approx(2.5f).margin(1e-4f));
    CHECK_FALSE(controller.isMovingForward());

    // While stationary inside resume band (< 3.75 yd), shifting leader laterally updates
    // facingYaw toward leader without chattering START_FORWARD.
    emitted.clear();
    leaderPos = glm::vec3(3.2f, 2.5f, 0.0f);  // 3.2 yd along +X => 0 deg yaw
    controller.setGrounded(true);
    input.update();
    controller.update(0.05f);
    CHECK(emitted.empty());
    CHECK_FALSE(controller.isMovingForward());
    CHECK(controller.getFacingYaw() == Catch::Approx(0.0f).margin(1e-3f));

    // Leader runs past 3.75 yd: restores MSG_MOVE_SET_RUN_MODE + MSG_MOVE_START_FORWARD at 7.0 yd/s.
    leaderPos = glm::vec3(5.0f, 2.5f, 0.0f);  // 5.0 yd along +X
    controller.setAutoFollowTargetMotion(true, false);
    controller.setGrounded(true);
    input.update();
    controller.update(0.05f);
    REQUIRE(emitted.size() == 2);
    CHECK(emitted[0].opcode == static_cast<uint32_t>(wowee::game::Opcode::MSG_MOVE_SET_RUN_MODE));
    CHECK(emitted[1].opcode == static_cast<uint32_t>(wowee::game::Opcode::MSG_MOVE_START_FORWARD));
    CHECK(playerPos.x == Catch::Approx(0.35f).margin(1e-3f));
}

TEST_CASE_METHOD(Integration,
                 "Third-person keyboard turn chord matches analytical circle and clears opposite turn flags",
                 "[input][camera][movement]") {
    auto& input = core::Input::getInstance();
    rendering::Camera camera;
    rendering::CameraController controller(&camera);
    glm::vec3 playerPos(0.0f, 0.0f, 0.0f);
    controller.setIdleOrbitEnabled(false);
    controller.setFollowTarget(&playerPos);
    controller.setUseWoWSpeed(true);
    controller.setGravityDisabled(true);
    controller.setFacingYaw(0.0f);

    std::vector<uint32_t> opcodes;
    controller.setMovementCallback([&](uint32_t op) { opcodes.push_back(op); });

    // Hold W + A across variable-dt frames: position must lie on exact circle R = v / omega.
    event(SDL_SCANCODE_W);
    event(SDL_SCANCODE_A);
    const float dts[] = {0.016f, 0.033f, 0.050f, 0.021f, 0.040f};
    float totalDt = 0.0f;
    for (float dt : dts) {
        controller.setGrounded(true);
        input.update();
        controller.update(dt);
        totalDt += dt;
    }
    const float omega = glm::radians(180.0f);
    const float radius = 7.0f / omega;
    const float theta = omega * totalDt;
    CHECK(playerPos.x == Catch::Approx(radius * std::sin(theta)).margin(1e-4f));
    CHECK(playerPos.y == Catch::Approx(radius * (1.0f - std::cos(theta))).margin(1e-4f));

    // Same-frame switch from A (TURNLEFT released) to D (TURNRIGHT pressed) must emit
    // MSG_MOVE_STOP_TURN BEFORE MSG_MOVE_START_TURN_RIGHT.
    opcodes.clear();
    manager.beginInputFrame();
    event(SDL_SCANCODE_A, false);
    event(SDL_SCANCODE_D, true);
    input.update();
    controller.update(0.016f);
    REQUIRE(opcodes.size() == 2);
    CHECK(opcodes[0] == static_cast<uint32_t>(wowee::game::Opcode::MSG_MOVE_STOP_TURN));
    CHECK(opcodes[1] == static_cast<uint32_t>(wowee::game::Opcode::MSG_MOVE_START_TURN_RIGHT));

    // Holding A and D simultaneously cancels turning and emits MSG_MOVE_STOP_TURN.
    opcodes.clear();
    manager.beginInputFrame();
    event(SDL_SCANCODE_A, true);
    input.update();
    controller.update(0.016f);
    REQUIRE(opcodes.size() == 1);
    CHECK(opcodes[0] == static_cast<uint32_t>(wowee::game::Opcode::MSG_MOVE_STOP_TURN));
    CHECK_FALSE(controller.isTurningLeft());
    CHECK_FALSE(controller.isTurningRight());

    event(SDL_SCANCODE_W, false);
    event(SDL_SCANCODE_A, false);
    event(SDL_SCANCODE_D, false);
}

TEST_CASE_METHOD(Integration, "TBC video CVar aliases and gamma brightness bridge stay synchronized", "[options][tbc]") {
    std::unordered_map<std::string, std::string> settings = {
        {"particledensity", "60"},
        {"environmentdetail", "150"},
        {"shadows", "0"},
        {"brightness", "50"},
        {"viewdistance", "2200"},
    };
    float particleDensityApplied = -1.0f;
    float envDetailApplied = -1.0f;

    addons::LuaServices svc{};
    svc.getClientSetting = [&](const std::string& key) -> std::string {
        const auto it = settings.find(key);
        return it == settings.end() ? "" : it->second;
    };
    svc.setClientSetting = [&](const std::string& key, const std::string& val) {
        settings[key] = val;
    };
    svc.setParticleDensity = [&](float d) { particleDensityApplied = d; };
    svc.setEnvironmentDetail = [&](float d) { envDetailApplied = d; };
    lua.setLuaServices(svc);

    truth("GetCVar('spellEffectLevel') == '5'");
    truth("GetCVar('WorldDetail') == '2'");
    truth("GetCVar('shadowLOD') == '0'");
    truth("GetCVarDefault('farclip') == '1900'");

    run("SetCVar('spellEffectLevel', '9')");
    CHECK(settings["particledensity"] == "100");
    CHECK(particleDensityApplied == Catch::Approx(1.0f));

    run("SetCVar('WorldDetail', '0')");
    CHECK(settings["environmentdetail"] == "50");
    CHECK(envDetailApplied == Catch::Approx(0.5f));

    run("SetCVar('shadowLOD', '1')");
    CHECK(settings["shadows"] == "1");

    // With desktopGamma on (default "1"), SetGamma leaves WoWee brightness untouched.
    settings["brightness"] = "70";
    run("SetCVar('desktopGamma', '1')");
    run("SetGamma(0.2)");
    CHECK(settings["brightness"] == "70");

    // With desktopGamma off ("0"), dragging TBC Gamma drives brightness (0 -> 50, -0.2 -> 30).
    run("SetCVar('desktopGamma', '0')");
    run("SetGamma(-0.2)");
    CHECK(settings["brightness"] == "30");

    run("RestoreVideoDefaults()");
    CHECK(settings["viewdistance"] == "1900");
    CHECK(settings["brightness"] == "50");
    truth("GetCVar('desktopGamma') == '1'");
    truth("GetCVar('gamma') == '1.0'");

    lua.setLuaServices({});
}

TEST_CASE("Bundled Wowee addons load on TBC 20400 even when checkAddonVersion is enabled", "[options][addons][tbc]") {
    namespace fs = std::filesystem;
    const fs::path tempDir = fs::temp_directory_path() / "wowee_test_tbc_addons";
    std::error_code ec;
    fs::remove_all(tempDir, ec);
    fs::create_directories(tempDir / "WoweeAllBags", ec);
    fs::create_directories(tempDir / "OutdatedThirdParty", ec);

    {
        std::ofstream toc(tempDir / "WoweeAllBags" / "WoweeAllBags.toc");
        toc << "## Interface: 30300\n## Title: Wowee All Bags\nWoweeAllBags.lua\n";
        std::ofstream luaFile(tempDir / "WoweeAllBags" / "WoweeAllBags.lua");
        luaFile << "WOWEE_ALL_BAGS_TEST_LOADED = true\n";
    }
    {
        std::ofstream toc(tempDir / "OutdatedThirdParty" / "OutdatedThirdParty.toc");
        toc << "## Interface: 30300\n## Title: Outdated Third Party\nOutdatedThirdParty.lua\n";
        std::ofstream luaFile(tempDir / "OutdatedThirdParty" / "OutdatedThirdParty.lua");
        luaFile << "OUTDATED_THIRD_PARTY_LOADED = true\n";
    }

    addons::AddonManager am;
    REQUIRE(am.initialize(nullptr));
    am.scanAddons(tempDir.string());
    REQUIRE(am.getLuaEngine()->executeString(
        "__WoweeInterfaceVersion = 20400\nSetCVar('checkAddonVersion', '1')\n"));
    am.loadAllAddons();

    CHECK(am.isAddOnLoadedByName("WoweeAllBags"));
    CHECK_FALSE(am.isAddOnLoadedByName("OutdatedThirdParty"));

    am.shutdown();
    fs::remove_all(tempDir, ec);
}

TEST_CASE_METHOD(Integration, "WoweeSettingList filters Wrath-only rows on TBC 20400 with dense array indices", "[options][tbc]") {
    run("__WoweeInterfaceVersion = 20400");
    run(R"LUA(
        local list = WoweeSettingList()
        __tbcCount = #list
        __tbcDense = true
        local keys = {}
        local cats = {}
        for i = 1, #list do
            if not list[i] then __tbcDense = false end
            keys[list[i].key] = true
            cats[list[i].category] = (cats[list[i].category] or 0) + 1
        end
        __tbcHasWrathRows = keys.equipmentmanager or keys.previewtalents or keys.colorblindmode
            or keys.arenaframes or keys.threatwarning or keys.widerquesttracker
            or keys.clicktofocus or keys.chattimestamps or keys.critternames
            or keys.micromenu or false
        __tbcHasTbcRows = (keys.friendlycreationnames and keys.enemycreationnames
            and keys.companionnames and keys.tbcdispellabledebuffs
            and keys.worldpvpdisplay and keys.showchaticons and keys.raidrange
            and keys.fcttargetdamage and keys.showbottomrightbar) or false
    )LUA");
    truth("__tbcCount >= 80");
    truth("__tbcDense == true");
    truth("__tbcHasWrathRows == false");
    truth("__tbcHasTbcRows == true");

    run("__WoweeInterfaceVersion = 30300");
    run(R"LUA(
        local list = WoweeSettingList()
        local keys = {}
        for i = 1, #list do keys[list[i].key] = true end
        __wotlkHasWrathRows = (keys.equipmentmanager and keys.previewtalents
            and keys.arenaframes and keys.threatwarning and keys.chattimestamps) or false
        __wotlkHasTbcRows = keys.friendlycreationnames or keys.worldpvpdisplay
            or keys.tbcdispellabledebuffs or keys.showchaticons or false
    )LUA");
    truth("__wotlkHasWrathRows == true");
    truth("__wotlkHasTbcRows == false");

    // Modified click AUTOLOOTTOGGLE respects SetModifiedClick changes.
    run("SetModifiedClick('AUTOLOOTTOGGLE', 'ALT')");
    CHECK(addons::isModifiedClickActive(lua.getState(), "AUTOLOOTTOGGLE", false, false, true));
    CHECK_FALSE(addons::isModifiedClickActive(lua.getState(), "AUTOLOOTTOGGLE", true, false, false));
    run("SetModifiedClick('AUTOLOOTTOGGLE', 'SHIFT')");
    CHECK(addons::isModifiedClickActive(lua.getState(), "AUTOLOOTTOGGLE", true, false, false));
}

TEST_CASE_METHOD(Integration, "GameTooltip:SetShapeshift shows full spell tooltip and GetShapeshiftFormInfo reflects usability", "[shapeshift][tooltip]") {
    using namespace wowee::game;
    replay::Session session("tbc");
    auto& handler = session.handler();
    lua.setGameHandler(&handler);
    session.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}});
    handler.spellNameCacheLoadedRef() = true;

    auto registerStance = [&](uint32_t spellId, const char* name, const char* desc,
                              uint8_t form, uint32_t order, uint32_t attr = 0) {
        auto& entry = handler.spellNameCacheRef()[spellId];
        entry.name = name;
        entry.description = desc;
        entry.shapeshiftForm = form;
        entry.stanceBarOrder = order;
        entry.attr = attr;
        entry.recoveryMs = 1000;
        wowee::network::Packet learned(wireOpcode(Opcode::SMSG_LEARNED_SPELL));
        learned.writeUInt32(spellId);
        session.receive(learned);
    };
    registerStance(2457, "Battle Stance", "A balanced combat stance.", 17, 1);
    registerStance(71, "Defensive Stance", "A defensive combat stance.", 18, 2);
    registerStance(2458, "Berserker Stance", "An aggressive stance.", 19, 3);
    // Stealth (1784): SPELL_ATTR0_NOT_IN_COMBAT (0x00010000) -> uncastable in combat.
    registerStance(1784, "Stealth", "Allows the rogue to sneak around.", 30, 4, 0x00010000u);

    run(R"LUA(
        SPELL_CAST_TIME_INSTANT_NO_MANA = 'Instant'
        __tt = CreateFrame('GameTooltip', 'StanceTestTooltip', UIParent)
        __tt:SetOwner(UIParent, 'ANCHOR_NONE')
        __ok1 = __tt:SetShapeshift(1)
        __okBad = __tt:SetShapeshift(99)
    )LUA");
    truth("__ok1 == true");
    truth("__okBad == false");

    const ui::Widget* tt = lua.widgets().findByName("StanceTestTooltip");
    REQUIRE(tt != nullptr);
    REQUIRE(tt->tooltipLines.size() >= 3);
    CHECK(tt->tooltipLines[0].left == "Battle Stance");
    CHECK(tt->tooltipLines[1].left == "Instant");
    CHECK(tt->tooltipLines[1].right == "1 sec cooldown");
    CHECK(tt->tooltipLines[2].left == "A balanced combat stance.");

    // Defensive Stance tooltip
    run("__ok2 = __tt:SetShapeshift(2)");
    truth("__ok2 == true");
    REQUIRE(tt->tooltipLines.size() >= 3);
    CHECK(tt->tooltipLines[0].left == "Defensive Stance");
    CHECK(tt->tooltipLines[2].left == "A defensive combat stance.");

    // Out of combat: Stealth (index 4) is castable; in combat: not castable (nil).
    run("_, _, _, __castOut = GetShapeshiftFormInfo(4)");
    truth("__castOut == 1");
    session.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_FLAGS, 0x00080000u}}); // UNIT_FLAG_IN_COMBAT
    run("_, _, _, __castIn = GetShapeshiftFormInfo(4)");
    truth("__castIn == nil");
    // Warrior Battle Stance stays castable in combat.
    run("_, _, _, __battleIn = GetShapeshiftFormInfo(1)");
    truth("__battleIn == 1");

    lua.setGameHandler(nullptr);
}

TEST_CASE_METHOD(Integration, "Bonus action bar offset, stance hotkeys, empty-to-populated swap, and clearWorld reset", "[shapeshift][bonusbar][tbc]") {
    using namespace wowee::game;
    CHECK(addons::clientActsOnBinding("SHAPESHIFTBUTTON9"));
    CHECK(addons::clientActsOnBinding("SHAPESHIFTBUTTON10"));

    replay::Session session("tbc");
    auto& handler = session.handler();
    session.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}});
    handler.spellNameCacheLoadedRef() = true;

    // Querying before AssetManager is attached must not permanently poison the cache with 0.
    handler.shapeshiftFormIdRef() = 17;
    CHECK(handler.getBonusActionBarOffset() == 0);

    // returnToCharacterSelect -> clearWorld resets shapeshiftFormId_ so logout/character switch cannot leak old stance.
    handler.returnToCharacterSelect();
    CHECK(handler.getShapeshiftFormId() == 0);

    const char* tbcRoot = std::getenv("WOWEE_TBC_DATA_DIR");
    if (!tbcRoot || !*tbcRoot) return;

    replay::Session worldSession("tbc");
    auto& gh = worldSession.handler();
    worldSession.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}});
    gh.spellNameCacheLoadedRef() = true;

    wowee::pipeline::AssetManager assets;
    REQUIRE(assets.initialize(tbcRoot));
    wowee::pipeline::DBCLayout dbcLayout;
    REQUIRE(dbcLayout.loadFromJson(std::string(tbcRoot) + "/dbc_layouts.json"));
    wowee::pipeline::setActiveDBCLayout(&dbcLayout);
    const_cast<GameServices&>(gh.services()).assetManager = &assets;

    // Verify TBC 2.4.3 SpellShapeshiftForm.dbc bonus bar offsets for all classes/forms:
    // Warrior: Battle=1, Defensive=2, Berserker=3
    // Druid: Cat=1, Tree=2, Bear/Dire Bear=3, Moonkin=4, Travel/Aquatic/Flight=0
    // Rogue: Stealth=1; Priest: Shadowform=1
    const std::pair<uint8_t, uint32_t> expected[] = {
        {17, 1}, {18, 2}, {19, 3},           // Warrior stances
        {1, 1}, {2, 2}, {5, 3}, {8, 3}, {31, 4}, // Druid bonus forms
        {3, 0}, {4, 0}, {27, 0}, {29, 0},    // Druid non-bonus travel/flight forms
        {30, 1},                             // Rogue Stealth
        {28, 1},                             // Priest Shadowform
    };
    for (const auto& [form, offset] : expected) {
        gh.shapeshiftFormIdRef() = form;
        CHECK(gh.getBonusActionBarOffset() == offset);
    }

    // Populate Battle Stance (offset 1 -> 0-based slots 72..83) and leave Defensive Stance (84..95) empty.
    constexpr uint32_t kBattleStanceAbility = 99101;
    gh.setActionBarSlot(72, ActionBarSlot::SPELL, kBattleStanceAbility);
    gh.spellNameCacheRef()[kBattleStanceAbility].name = "Battle Stance Test Strike";
    wowee::network::Packet learned(wireOpcode(Opcode::SMSG_LEARNED_SPELL));
    learned.writeUInt32(kBattleStanceAbility);
    worldSession.receive(learned);

    // Start in empty Defensive Stance (form 18, offset 2 -> slots 84..95): pressing ACTIONBUTTON1 casts nothing.
    ui::GameScreen screen;
    const auto render = [&] {
        ImGui::NewFrame();
        screen.render(gh);
        ImGui::EndFrame();
    };
    const auto casts = [&] { return worldSession.sent().of(Opcode::CMSG_CAST_SPELL).size(); };

    gh.shapeshiftFormIdRef() = 18; // Defensive Stance (empty bar)
    screen.actionBarPanel().setMainActionBarPage(1);
    REQUIRE(manager.setBinding("1", "ACTIONBUTTON1"));
    const auto beforeDef = casts();
    manager.beginInputFrame();
    event(SDL_SCANCODE_1); event(SDL_SCANCODE_1, false);
    render();
    CHECK(casts() == beforeDef);

    // Switch to Battle Stance (form 17, offset 1 -> slots 72..83): pressing ACTIONBUTTON1 casts slot 72.
    gh.shapeshiftFormIdRef() = 17;
    manager.beginInputFrame();
    event(SDL_SCANCODE_1); event(SDL_SCANCODE_1, false);
    render();
    REQUIRE(casts() == beforeDef + 1);
    // TBC CMSG_CAST_SPELL starts with uint32 spellId (bytes 0..3), then uint8 castCount.
    const auto sent = worldSession.sent().of(Opcode::CMSG_CAST_SPELL);
    const auto& bytes = sent.back().getData();
    REQUIRE(bytes.size() >= 4);
    const uint32_t castSpellId = uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 |
                                 uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
    CHECK(castSpellId == kBattleStanceAbility);

    const_cast<GameServices&>(gh.services()).assetManager = nullptr;
    wowee::pipeline::setActiveDBCLayout(nullptr);
}

TEST_CASE_METHOD(Integration,
                 "Third-person orbit camera preserves exact zoom distance while moving and turning without snapping",
                 "[input][camera][rmb][movement]") {
    auto& input = core::Input::getInstance();

    for (float stiffness : {5.0f, 30.0f}) {
        for (bool smoothFollow : {false, true}) {
            INFO("stiffness=" << stiffness << " smoothFollow=" << smoothFollow);
            rendering::Camera camera;
            rendering::CameraController controller(&camera);
            glm::vec3 playerPos(100.0f, 200.0f, 50.0f);
            controller.setIdleOrbitEnabled(false);
            controller.setFollowTarget(&playerPos);
            controller.setUseWoWSpeed(true);
            controller.setGravityDisabled(true);
            controller.setCameraSmoothSpeed(stiffness);
            controller.setSmoothCameraFollow(smoothFollow);
            controller.setFacingYaw(0.0f);
            controller.setGrounded(true);

            auto pivotFor = [&]() {
                return playerPos + glm::vec3(0.0f, 0.0f, controller.followedPivotHeight());
            };
            auto stepAndCheck = [&](float dt) {
                controller.setGrounded(true);
                input.update();
                controller.update(dt);
                const float dist = glm::distance(camera.getPosition(), pivotFor());
                CHECK(dist == Catch::Approx(controller.zoomDistance()).margin(1e-4f));
                return dist;
            };

            // Baseline idle
            const float idleDist = stepAndCheck(1.0f / 60.0f);
            REQUIRE(idleDist == Catch::Approx(controller.zoomDistance()).margin(1e-4f));

            // Run forward (W) for 1 second: camera must never trail behind pivot.
            event(SDL_SCANCODE_W, true);
            float preTurnDist = idleDist;
            for (int i = 0; i < 60; ++i) {
                preTurnDist = stepAndCheck(1.0f / 60.0f);
            }
            CHECK(playerPos.x > 105.0f);

            // Engage steering/turning while still running forward: zero zoom/position snap.
            controller.setSteering(true);
            const float steerStartDist = stepAndCheck(1.0f / 60.0f);
            CHECK(std::abs(steerStartDist - preTurnDist) < 1e-4f);
            controller.applyLookDelta(40.0f, -10.0f);
            const float steerDragDist = stepAndCheck(1.0f / 60.0f);
            CHECK(std::abs(steerDragDist - preTurnDist) < 1e-4f);
            controller.setSteering(false);
            const float steerEndDist = stepAndCheck(1.0f / 60.0f);
            CHECK(std::abs(steerEndDist - preTurnDist) < 1e-4f);

            // Keyboard turn (A) while running forward: zero snap on press or release.
            event(SDL_SCANCODE_A, true);
            const float turnStartDist = stepAndCheck(1.0f / 60.0f);
            CHECK(std::abs(turnStartDist - preTurnDist) < 1e-4f);
            for (int i = 0; i < 15; ++i) stepAndCheck(1.0f / 60.0f);
            event(SDL_SCANCODE_A, false);
            stepAndCheck(1.0f / 60.0f);
            event(SDL_SCANCODE_W, false);

            // Backpedal (S) and strafe (Q/E): zero forward or lateral camera lag.
            for (SDL_Scancode sc : {SDL_SCANCODE_S, SDL_SCANCODE_Q, SDL_SCANCODE_E}) {
                event(sc, true);
                for (int i = 0; i < 30; ++i) stepAndCheck(1.0f / 60.0f);
                controller.setSteering(true);
                stepAndCheck(1.0f / 60.0f);
                controller.setSteering(false);
                stepAndCheck(1.0f / 60.0f);
                event(sc, false);
            }

            // Taxi / externalFollow_ moving at high speed (32 yd/s): zero lag or snap.
            controller.setExternalFollow(true);
            for (int i = 0; i < 30; ++i) {
                playerPos.x += 32.0f / 60.0f;
                controller.update(1.0f / 60.0f);
                const float dist = glm::distance(camera.getPosition(), pivotFor());
                CHECK(dist == Catch::Approx(controller.zoomDistance()).margin(1e-4f));
            }
            controller.setExternalFollow(false);
        }
    }
}

TEST_CASE_METHOD(Integration,
                 "TBC boat camera and character yaw stay locked to hull across boarding, harbor turns, LMB/RMB/A/D, and disembark",
                 "[input][camera][transport][boatcam][tbc]") {
    auto& input = core::Input::getInstance();

    rendering::Camera camera;
    rendering::CameraController controller(&camera);
    glm::vec3 playerPos(-6423.5f, 820.2f, 5.62f);
    controller.setIdleOrbitEnabled(false);
    controller.setFollowTarget(&playerPos);
    controller.setUseWoWSpeed(true);
    controller.setGravityDisabled(true);
    controller.setDefaultSpawn(playerPos, 171.887f, -5.0f);
    controller.setOnlineMode(true);
    controller.reset();
    controller.setGrounded(true);

    // 1. Boarding onto The Bravery (hull server yaw = -107.092 deg):
    // zero sudden camera or character yaw snap on board.
    float shipYawDeg = -107.092f;
    const float preBoardCamYaw = controller.getYaw();
    const float preBoardFacingYaw = controller.getFacingYaw();
    const float initialBoatLocalCamYaw =
        std::remainder(preBoardCamYaw - shipYawDeg, 360.0f);
    const float initialBoatLocalFacingYaw =
        std::remainder(preBoardFacingYaw - shipYawDeg, 360.0f);
    CHECK(initialBoatLocalCamYaw == Catch::Approx(-81.021f).margin(0.01f));
    CHECK(initialBoatLocalFacingYaw == Catch::Approx(-81.021f).margin(0.01f));

    // Anchor frame on board (sameRideFrame = false on frame 0 -> delta = 0).
    controller.applyTransportYawDelta(0.0f);
    CHECK(controller.getYaw() == Catch::Approx(preBoardCamYaw).margin(1e-5f));
    CHECK(controller.getFacingYaw() == Catch::Approx(preBoardFacingYaw).margin(1e-5f));

    // 2. Standing still across a sharp 135-degree harbor turn crossing +/-180 deg:
    // boat-local camera yaw and character facing yaw stay constant to 0.000 deg.
    for (int step = 0; step < 90; ++step) {
        const float nextShipYawDeg = shipYawDeg - 1.5f; // -107.092 -> -242.092 (+117.908)
        const float deltaYawDeg = std::remainder(nextShipYawDeg - shipYawDeg, 360.0f);
        shipYawDeg = std::remainder(nextShipYawDeg, 360.0f);
        controller.applyTransportYawDelta(deltaYawDeg);
        input.update();
        controller.update(1.0f / 60.0f);

        const float localCamYaw = std::remainder(controller.getYaw() - shipYawDeg, 360.0f);
        const float localFacingYaw = std::remainder(controller.getFacingYaw() - shipYawDeg, 360.0f);
        CHECK(localCamYaw == Catch::Approx(initialBoatLocalCamYaw).margin(1e-3f));
        CHECK(localFacingYaw == Catch::Approx(initialBoatLocalFacingYaw).margin(1e-3f));
    }

    // 3. LMB camera orbit (+160 px = -36.0 deg) while the boat continues turning:
    // camera orbits in boat-local space while character facing stays deck-locked.
    controller.applyLookDelta(160.0f, 0.0f);
    for (int step = 0; step < 30; ++step) {
        const float nextShipYawDeg = shipYawDeg + 1.2f;
        const float deltaYawDeg = std::remainder(nextShipYawDeg - shipYawDeg, 360.0f);
        shipYawDeg = std::remainder(nextShipYawDeg, 360.0f);
        controller.applyTransportYawDelta(deltaYawDeg);
        input.update();
        controller.update(1.0f / 60.0f);

        const float localCamYaw = std::remainder(controller.getYaw() - shipYawDeg, 360.0f);
        const float localFacingYaw = std::remainder(controller.getFacingYaw() - shipYawDeg, 360.0f);
        CHECK(localCamYaw == Catch::Approx(initialBoatLocalCamYaw - 36.0f).margin(1e-3f));
        CHECK(localFacingYaw == Catch::Approx(initialBoatLocalFacingYaw).margin(1e-3f));
    }

    // 4. RMB steer (-160 px = +36.0 deg) while turning: aligns character + camera in boat-local space.
    controller.setSteering(true);
    controller.applyLookDelta(-160.0f, 0.0f);
    input.update();
    controller.update(1.0f / 60.0f);
    controller.setSteering(false);
    for (int step = 0; step < 20; ++step) {
        const float nextShipYawDeg = shipYawDeg - 1.0f;
        const float deltaYawDeg = std::remainder(nextShipYawDeg - shipYawDeg, 360.0f);
        shipYawDeg = std::remainder(nextShipYawDeg, 360.0f);
        controller.applyTransportYawDelta(deltaYawDeg);
        input.update();
        controller.update(1.0f / 60.0f);

        const float localCamYaw = std::remainder(controller.getYaw() - shipYawDeg, 360.0f);
        const float localFacingYaw = std::remainder(controller.getFacingYaw() - shipYawDeg, 360.0f);
        CHECK(localCamYaw == Catch::Approx(initialBoatLocalCamYaw).margin(1e-3f));
        CHECK(localFacingYaw == Catch::Approx(initialBoatLocalFacingYaw).margin(1e-3f));
    }

    // 5. Keyboard A (turn left, +180 deg/s for 0.5 s = +90 deg) on a turning boat:
    // boat-local yaw advances by +90 deg on top of hull rotation.
    event(SDL_SCANCODE_A, true);
    for (int step = 0; step < 30; ++step) {
        const float nextShipYawDeg = shipYawDeg + 0.8f;
        const float deltaYawDeg = std::remainder(nextShipYawDeg - shipYawDeg, 360.0f);
        shipYawDeg = std::remainder(nextShipYawDeg, 360.0f);
        controller.applyTransportYawDelta(deltaYawDeg);
        input.update();
        controller.update(1.0f / 60.0f);
    }
    event(SDL_SCANCODE_A, false);
    input.update();
    controller.update(1.0f / 60.0f);
    {
        const float localFacingAfterTurn =
            std::remainder(controller.getFacingYaw() - shipYawDeg, 360.0f);
        const float expectedAfterTurn =
            std::remainder(initialBoatLocalFacingYaw + 90.0f, 360.0f);
        CHECK(std::abs(std::remainder(localFacingAfterTurn - expectedAfterTurn, 360.0f)) < 0.25f);
    }

    // 6. Disembarking back onto dock: world camera yaw & character facing remain
    // exactly where they were on the final deck frame (zero snap).
    const float preDisembarkCamYaw = controller.getYaw();
    const float preDisembarkFacingYaw = controller.getFacingYaw();
    input.update();
    controller.update(1.0f / 60.0f);
    CHECK(controller.getYaw() == Catch::Approx(preDisembarkCamYaw).margin(1e-4f));
    CHECK(controller.getFacingYaw() == Catch::Approx(preDisembarkFacingYaw).margin(1e-4f));
}

TEST_CASE_METHOD(Integration,
                 "TBC boat rider suppresses MSG_MOVE_SET_FACING while hull rotates, emits on RMB turn, and preserves local facing + LMB orbit across SMSG_NEW_WORLD",
                 "[transport][boatcam][tbc]") {
    using namespace wowee::game;
    replay::Session session("tbc");
    auto& handler = session.handler();
    auto* tm = handler.getTransportManager();
    auto* mh = handler.getMovementHandler();
    REQUIRE(tm != nullptr);
    REQUIRE(mh != nullptr);

    constexpr uint64_t boatOrigin = 0x01fc00000002b0abULL;
    announceTbcBoat(session, boatOrigin);
    tm->loadPathFromNodes(176310, {{0.0f, 0.0f, 0.0f}});
    const float initialBoatServerYaw = glm::radians(-107.092f);
    tm->registerTransport(
        boatOrigin, 0, 176310, {823.0f, -6406.0f, 0.0f},
        176310, 3015, false, initialBoatServerYaw);

    // Board facing localO = -1.41408 rad (-81.021 deg), matching live Auberdine capture.
    const float initialLocalO = -1.41408f;
    auto* trOrigin = tm->getTransport(boatOrigin);
    REQUIRE(trOrigin != nullptr);
    handler.setPlayerOnTransport(boatOrigin, {-1.05f, -0.02f, 6.13f});
    const glm::vec3 startWorld = tm->getPlayerWorldPosition(boatOrigin, {-1.05f, -0.02f, 6.13f});
    const float startCanonicalYaw = core::coords::normalizeAngleRad(
        trOrigin->canonicalYawRad() + initialLocalO);
    handler.setPosition(startWorld.x, startWorld.y, startWorld.z);
    handler.setOrientation(startCanonicalYaw);
    mh->commitTransportArrival(startWorld, startCanonicalYaw, boatOrigin, initialLocalO);

    // Rotate the hull through 120 degrees while standing still / LMB-orbiting
    // (player canonical facing rotates with hull so localO stays constant).
    for (int step = 1; step <= 24; ++step) {
        const float newServerYaw = initialBoatServerYaw + glm::radians(5.0f * step);
        tm->updateServerTransport(boatOrigin, {823.0f + step, -6406.0f, 0.0f}, newServerYaw);
        const float syncedCanonicalYaw = core::coords::normalizeAngleRad(
            trOrigin->canonicalYawRad() + initialLocalO);
        handler.setOrientation(syncedCanonicalYaw);

        const auto delta = mh->unsentFacingDeltaRad();
        REQUIRE(delta.has_value());
        CHECK(std::abs(*delta) < 1e-5f);

        // Even if MSG_MOVE_SET_FACING is requested directly, pre-Wotlk dedup drops it.
        handler.sendMovement(Opcode::MSG_MOVE_SET_FACING);
    }
    CHECK(session.sent().of(Opcode::MSG_MOVE_SET_FACING).empty());

    // Deliberate RMB character turn of -36 deg (-0.6283185 rad) in boat-local space:
    // unsentFacingDeltaRad reports the exact local turn and MSG_MOVE_SET_FACING goes out once.
    const float turnedLocalO = core::coords::normalizeAngleRad(initialLocalO - glm::radians(36.0f));
    handler.setOrientation(core::coords::normalizeAngleRad(
        trOrigin->canonicalYawRad() + turnedLocalO));
    {
        const auto delta = mh->unsentFacingDeltaRad();
        REQUIRE(delta.has_value());
        CHECK(*delta == Catch::Approx(-glm::radians(36.0f)).margin(1e-4f));
    }
    handler.sendMovement(Opcode::MSG_MOVE_SET_FACING);
    REQUIRE(session.sent().of(Opcode::MSG_MOVE_SET_FACING).size() == 1);

    // Continued hull rotation after RMB release sends zero extra MSG_MOVE_SET_FACING.
    for (int step = 25; step <= 36; ++step) {
        const float newServerYaw = initialBoatServerYaw + glm::radians(5.0f * step);
        tm->updateServerTransport(boatOrigin, {823.0f + step, -6406.0f, 0.0f}, newServerYaw);
        handler.setOrientation(core::coords::normalizeAngleRad(
            trOrigin->canonicalYawRad() + turnedLocalO));
        CHECK(std::abs(mh->unsentFacingDeltaRad().value_or(1.0f)) < 1e-5f);
    }

    // Cross-continent SMSG_TRANSFER_PENDING + SMSG_NEW_WORLD carrying transport-local t_pos.o
    // plus a -25 deg LMB camera orbit offset.
    wowee::network::Packet pending(wireOpcode(Opcode::SMSG_TRANSFER_PENDING));
    pending.writeUInt32(0);
    pending.writeUInt32(176310);
    pending.writeUInt32(handler.getCurrentMapId());
    session.receive(pending);

    wowee::network::Packet newWorld(wireOpcode(Opcode::SMSG_NEW_WORLD));
    newWorld.writeUInt32(0);
    for (float v : {-1.05f, -0.02f, 6.13f, turnedLocalO}) newWorld.writeFloat(v);
    session.receive(newWorld);
    REQUIRE(handler.hasPendingPlayerTransportWorldTransfer());
    handler.setPendingPlayerTransportCameraOrbitDeg(-25.0f);

    // Destination boat spawns on Map 0 with arrival server heading +25.0 deg.
    tm->clearTransports();
    constexpr uint64_t boatDest = 0x01fc00000002b0acULL;
    announceTbcBoat(session, boatDest);
    tm->loadPathFromNodes(176310, {{0.0f, 0.0f, 0.0f}});
    const float destBoatServerYaw = glm::radians(25.0f);
    tm->registerTransport(
        boatDest, 0, 176310, {-3890.0f, -600.0f, 0.0f},
        176310, 3015, false, destBoatServerYaw);

    glm::vec3 restoredPos(0.0f);
    float restoredCanonicalYaw = 0.0f;
    float restoredCamYawDeg = 0.0f;
    REQUIRE(handler.completePlayerTransportWorldTransfer(
        boatDest, restoredPos, restoredCanonicalYaw, restoredCamYawDeg));

    const float expectedWorldServerYaw =
        core::coords::normalizeAngleRad(destBoatServerYaw + turnedLocalO);
    const float expectedFacingDeg = glm::degrees(expectedWorldServerYaw);
    CHECK(std::abs(std::remainder(
              core::coords::canonicalToCharacterYawDeg(restoredCanonicalYaw) - expectedFacingDeg,
              360.0f)) < 1e-3f);
    CHECK(std::abs(std::remainder(
              (restoredCamYawDeg - expectedFacingDeg) - (-25.0f),
              360.0f)) < 1e-3f);
    // And arrival commits the transport baseline so standing on arrival emits 0 packets.
    CHECK(std::abs(mh->unsentFacingDeltaRad().value_or(1.0f)) < 1e-5f);
}

TEST_CASE_METHOD(Integration, "TBC mail Inbox/OpenMail APIs and empty hover tooltip parity", "[mail][tbc]") {
    replay::Session session("tbc");
    auto& handler = session.handler();
    lua.setGameHandler(&handler);

    // Seed item metadata for Linen Cloth (2589) and Wool Cloth (2592).
    {
        wowee::game::ItemQueryResponseData linen;
        linen.entry = 2589;
        linen.name = "Linen Cloth";
        linen.displayInfoId = 5773;
        linen.quality = 1;
        linen.valid = true;
        handler.itemInfoCacheRef()[2589] = linen;

        wowee::game::ItemQueryResponseData wool;
        wool.entry = 2592;
        wool.name = "Wool Cloth";
        wool.displayInfoId = 5774;
        wool.quality = 1;
        wool.valid = true;
        handler.itemInfoCacheRef()[2592] = wool;
    }

    // 1. No-item NPC MailTemplate mail (e.g. Velma Warnam / Randal Hunter)
    wowee::game::MailMessage npcMail;
    npcMail.messageId = 101;
    npcMail.messageType = 3; // MAIL_CREATURE
    npcMail.senderEntry = 4773;
    npcMail.stationeryId = 41;
    npcMail.mailTemplateId = 233;
    npcMail.expirationTime = 29.5f;
    npcMail.flags = 0; // unread, not copied
    {
        wowee::network::Packet info(wireOpcode(wowee::game::Opcode::SMSG_CREATURE_QUERY_RESPONSE));
        info.writeUInt32(4773);
        info.writeString("Velma Warnam");
        for (int i = 0; i < 5; ++i) info.writeString("");
        info.writeUInt32(0);
        info.writeUInt32(7); // Humanoid
        for (int i = 0; i < 8; ++i) info.writeUInt32(0);
        info.writeFloat(1.0f);
        info.writeFloat(1.0f);
        info.writeUInt8(0);
        session.receive(info);
    }

    // 2. Single-item player mail (sparse wire slot 3, stack of 7 Linen Cloth, HAS_BODY)
    wowee::game::MailMessage singleItemMail;
    singleItemMail.messageId = 102;
    singleItemMail.messageType = 0; // MAIL_NORMAL
    singleItemMail.senderGuid = 0x2DULL;
    singleItemMail.senderName = "Edrico";
    singleItemMail.stationeryId = 41;
    singleItemMail.subject = "Cloth Shipment";
    singleItemMail.body = "Enjoy the cloth!";
    singleItemMail.flags = 0x10; // MAIL_CHECK_MASK_HAS_BODY
    singleItemMail.expirationTime = 29.0f;
    {
        wowee::game::MailAttachment att;
        att.slot = 3;
        att.itemGuidLow = 9001;
        att.itemId = 2589;
        att.stackCount = 7;
        singleItemMail.attachments.push_back(att);
    }

    // 3. Multi-item + COD player mail
    wowee::game::MailMessage codMultiMail;
    codMultiMail.messageId = 103;
    codMultiMail.messageType = 0;
    codMultiMail.senderGuid = 0x2EULL;
    codMultiMail.senderName = "Merchant";
    codMultiMail.stationeryId = 41;
    codMultiMail.subject = "COD Bundle";
    codMultiMail.cod = 25000;
    codMultiMail.flags = 0x04; // empty body -> MAIL_CHECK_MASK_COPIED
    codMultiMail.expirationTime = 2.9f;
    for (uint8_t s = 0; s < 2; ++s) {
        wowee::game::MailAttachment att;
        att.slot = s;
        att.itemGuidLow = 9100u + s;
        att.itemId = s == 0 ? 2589u : 2592u;
        att.stackCount = s == 0 ? 5u : 3u;
        codMultiMail.attachments.push_back(att);
    }

    handler.mailInboxRef() = {npcMail, singleItemMail, codMultiMail};

    // Verify GetInboxHeaderInfo, GetInboxText, and InboxItemCanDelete across all 3 mails.
    run("h1 = {GetInboxHeaderInfo(1)}; t1 = {GetInboxText(1)}");
    truth("h1[1] == nil and h1[2] ~= nil and h1[3] == 'Velma Warnam'");
    truth("h1[8] == nil and h1[10] == false and h1[11] == false and h1[12] == false and h1[13] == false");
    truth("t1[3] == true and t1[4] == false and InboxItemCanDelete(1) == true");

    run("h2 = {GetInboxHeaderInfo(2)}; t2 = {GetInboxText(2)}");
    truth("h2[1] ~= nil and h2[3] == 'Edrico' and h2[4] == 'Cloth Shipment'");
    truth("h2[8] == 1 and h2[11] == false and h2[12] == true and h2[14] == 7");
    truth("t2[1] == 'Enjoy the cloth!' and t2[3] == true and InboxItemCanDelete(2) == false");
    // Sparse wire slot 3 resolves at slot 4 for OpenMailFrame AND without slot for Inbox hover!
    run("i2_none = {GetInboxItem(2)}; i2_s1 = {GetInboxItem(2, 1)}; i2_s4 = {GetInboxItem(2, 4)}");
    truth("i2_none[1] == 'Linen Cloth' and i2_none[3] == 7 and i2_s1[1] == nil and i2_s4[1] == 'Linen Cloth'");

    run("h3 = {GetInboxHeaderInfo(3)}; t3 = {GetInboxText(3)}");
    truth("h3[1] == 'Interface\\\\Icons\\\\INV_Crate_02' and h3[14] == 0");
    truth("h3[6] == 25000 and h3[8] == 2 and h3[11] == true and t3[3] == false and InboxItemCanDelete(3) == false");
    truth("CanComplainInboxItem(1) == false and CanComplainInboxItem(2) == true and CanComplainInboxItem(3) == true");

    // Verify GameTooltip hover behavior:
    // - Hovering single-item mail populates GameTooltip, shows it, and keeps it in drawOrder across OnUpdate SetOwner refreshes.
    // - SetOwner fires OnTooltipCleared (clearing stale money frames).
    // - Subsequently calling SetOwner + Show with 0 lines (no-item NPC mail hover) keeps GameTooltip hidden!
    run("tipCleared = 0; tip = CreateFrame('GameTooltip', 'TestMailTip', UIParent); tip:SetScript('OnTooltipCleared', function() tipCleared = tipCleared + 1 end); btn = CreateFrame('Button', 'TestMailBtn', UIParent); btn:SetSize(37, 37); btn:SetPoint('TOPLEFT', UIParent, 'TOPLEFT', 40, -180)");
    run("tip:SetOwner(btn, 'ANCHOR_RIGHT'); tip:SetInboxItem(2); tip:Show()");
    truth("tip:IsShown() == true and tip:NumLines() >= 1 and tipCleared >= 2");
    {
        lua.widgets().layout(1280.0f, 720.0f);
        const auto& order = lua.widgets().drawOrder();
        const auto* tipW = lua.widgets().findByName("TestMailTip");
        REQUIRE(tipW != nullptr);
        CHECK(std::find(order.begin(), order.end(), tipW) != order.end());
    }
    run("tip:SetOwner(btn, 'ANCHOR_RIGHT'); tip:Show()");
    truth("tip:NumLines() == 0 and tip:IsShown() == false and tip:IsOwned(btn) == true");

    // Implicit ButtonText NormalFont justifyH='RIGHT' (MailItemExpireTime "29 Days ") anchors to RIGHT edge.
    run("exp = CreateFrame('Button', 'TestExpireBtn', UIParent); exp:SetSize(100, 16); exp:SetPoint('TOPLEFT', UIParent, 'TOPLEFT', 20, -20)");
    run("exp:GetFontString():SetJustifyH('RIGHT'); exp:SetText('29 Days ')");
    lua.widgets().layout(1280.0f, 720.0f);
    run("pt, _, relPt = exp:GetFontString():GetPoint(1)");
    truth("pt == 'RIGHT' and relPt == 'RIGHT'");

    // TBC wire replay: SMSG_MAIL_LIST_RESULT with itemTextId triggers CMSG_ITEM_TEXT_QUERY,
    // and SMSG_ITEM_TEXT_QUERY_RESPONSE backfills mail.body + fires MAIL_INBOX_UPDATE
    // without opening ItemTextFrame (ITEM_TEXT_READY).
    std::vector<std::string> events;
    handler.setAddonEventCallback([&](const std::string& ev, const std::vector<std::string>&) {
        events.push_back(ev);
    });
    handler.openMailbox(0xF1100022AE0068A0ULL);
    {
        wowee::network::Packet entry;
        entry.writeUInt32(901);                 // messageId
        entry.writeUInt8(0);                    // MAIL_NORMAL
        entry.writeUInt64(0x2DULL);             // senderGuid
        entry.writeUInt32(0);                   // COD
        entry.writeUInt32(777);                 // itemTextId
        entry.writeUInt32(0);                   // packageId
        entry.writeUInt32(41);                  // stationeryId
        entry.writeUInt32(0);                   // money
        entry.writeUInt32(0x10);                // MAIL_CHECK_MASK_HAS_BODY
        entry.writeFloat(29.9f);                // expiration
        entry.writeUInt32(0);                   // mailTemplateId
        entry.writeString("Wire Letter");       // subject
        entry.writeUInt8(0);                    // 0 attachments

        wowee::network::Packet mailList(wireOpcode(wowee::game::Opcode::SMSG_MAIL_LIST_RESULT));
        mailList.writeUInt8(1);
        mailList.writeUInt16(static_cast<uint16_t>(entry.getSize() + 2));
        mailList.writeBytes(entry.getData().data(), entry.getSize());
        session.receive(mailList);
    }
    const auto textQueries = session.sent().of(wowee::game::Opcode::CMSG_ITEM_TEXT_QUERY);
    REQUIRE(textQueries.size() == 1);
    {
        wowee::network::Packet q(textQueries[0].getOpcode(), textQueries[0].getData());
        CHECK(q.readUInt32() == 777);
        CHECK(q.readUInt32() == 901);
        CHECK(q.readUInt32() == 0);
    }
    events.clear();
    {
        wowee::network::Packet resp(wireOpcode(wowee::game::Opcode::SMSG_ITEM_TEXT_QUERY_RESPONSE));
        resp.writeUInt32(777);
        resp.writeString("Meet me in Goldshire!");
        session.receive(resp);
    }
    CHECK(std::count(events.begin(), events.end(), "MAIL_INBOX_UPDATE") >= 1);
    CHECK(std::count(events.begin(), events.end(), "ITEM_TEXT_READY") == 0);
    run("wt = {GetInboxText(1)}");
    truth("wt[1] == 'Meet me in Goldshire!' and wt[3] == true");

    // Live TBC MailTemplate.dbc check when extracted TBC data is available.
    if (const char* tbcDataDir = std::getenv("WOWEE_TBC_DATA_DIR")) {
        wowee::pipeline::AssetManager assets;
        if (assets.initialize(tbcDataDir)) {
            const_cast<wowee::game::GameServices&>(handler.services()).assetManager = &assets;
            handler.mailInboxRef() = {npcMail};
            run("mh = {GetInboxHeaderInfo(1)}; mt = {GetInboxText(1)}");
            truth("mh[4] == 'Learn to Ride in Tirisfal Glades'");
            truth("string.find(mt[1], 'Velma Warnam', 1, true) ~= nil");
            truth("string.find(mt[1], '$n', 1, true) == nil and string.find(mt[1], '$B', 1, true) == nil");
            const_cast<wowee::game::GameServices&>(handler.services()).assetManager = nullptr;
        }
    }

    handler.setAddonEventCallback({});
    lua.setGameHandler(nullptr);
}
