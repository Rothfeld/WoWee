#include "core/npc_interaction_facing.hpp"
#include "core/coordinates.hpp"
#include "input_test_support.hpp"
#include "game_replay.hpp"
#include "game/packet_parsers.hpp"
#include "game/transport_manager.hpp"
#include "ui/game_screen.hpp"
#include "addons/lua_engine.hpp"
#include "addons/addon_lua_snippets.hpp"
#include "addons/lua_api_registrations.hpp"
#include "core/gamepad.hpp"
#include "core/input.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "ui/gamepad_controls.hpp"
#include "ui/framexml_emitter.hpp"
#include "ui/xml_parser.hpp"
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
        poi.writeUInt32(0);
        poi.writeFloat(-4957);
        poi.writeFloat(-911.604f);
        poi.writeUInt32(6);
        poi.writeUInt32(0);
        poi.writeString(name);
        session.receive(poi);
    }
    REQUIRE(handler.getGossipPois().size() == 1);
    CHECK(handler.getGossipPois().front().name == "Ironforge Bank");
    CHECK(handler.getGossipPois().front().icon == 6);
    CHECK(updates == 2);
}

TEST_CASE_METHOD(Integration, "GM stationery identifies support mail without a player sender", "[gm-mail]") {
    replay::Session session("tbc");
    auto& handler = session.handler();
    lua.setGameHandler(&handler);
    wowee::game::MailMessage mail;
    mail.stationeryId = 61;
    mail.money = 1000000;
    handler.mailInboxRef().push_back(mail);
    CHECK(handler.getMailSenderName(mail) == "Blizzard Support");
    run("header = {GetInboxHeaderInfo(1)}");
    truth("header[3] == 'Blizzard Support' and header[5] == 1000000 and header[12] == false and header[13] == true");
    mail.senderGuid = 123;
    mail.senderName = "Player";
    CHECK(handler.getMailSenderName(mail) == "Blizzard Support");
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
    handler.setNpcSwingCallback([&](uint64_t guid) { swings.push_back(guid); });
    handler.setHitReactionCallback([&](uint64_t guid, HR reaction) { reactions.emplace_back(guid, reaction); });
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
    run("SetTrainerServiceTypeFilter('available', 1); SetTrainerServiceTypeFilter('unavailable', 1); ExpandTrainerSkillLine(0)");
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
    truth("select(4, GetTrainerServiceInfo(3)) == false");
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
    handler.setNpcDeathCallback([&](uint64_t) { ++deaths; });
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
