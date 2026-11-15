#include "core/input.hpp"

namespace wowee {
namespace core {

Input& Input::getInstance() {
    static Input instance;
    return instance;
}

void Input::processEvent(const SDL_Event& event) {
    if (event.type != SDL_EVENT_MOUSE_BUTTON_DOWN && event.type != SDL_EVENT_MOUSE_BUTTON_UP) return;
    const auto button = event.button.button;
    if (button == 0 || button >= NUM_MOUSE_BUTTONS) return;
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        pendingMousePress[button] = true;
        mousePressPosition[button] = {event.button.x, event.button.y};
    } else {
        pendingMouseRelease[button] = true;
        mouseReleasePosition[button] = {event.button.x, event.button.y};
    }
}

void Input::update() {
    // Copy current state to previous
    previousKeyState = currentKeyState;
    previousMouseState = currentMouseState;
    previousMousePosition = mousePosition;

    // Get current keyboard state
    // bool per key in SDL3, where SDL2 gave a byte.
    const bool* keyState = SDL_GetKeyboardState(nullptr);
    for (int i = 0; i < NUM_KEYS; ++i) {
        currentKeyState[i] = keyState[i] || virtualKeyState[i];
    }

    // Get current mouse state
    // Floats in SDL3: the pointer can sit between pixels on a scaled display.
    float mouseX = 0.0f;
    float mouseY = 0.0f;
    const SDL_MouseButtonFlags mouseState = SDL_GetMouseState(&mouseX, &mouseY);
    mousePosition = glm::vec2(mouseX, mouseY);

    // SDL_BUTTON_MASK(x) is defined as (1 << (x-1)), so button indices are 1-based.
    // SDL_BUTTON_MASK(0) is undefined behavior (negative shift). Start at 1.
    currentMouseState[0] = false;
    for (int i = 1; i < NUM_MOUSE_BUTTONS; ++i) {
        currentMouseState[i] = ((mouseState & SDL_BUTTON_MASK(i)) != 0) || virtualMouseState[i];
        // Both edges can arrive between rendered frames. Sampling only held
        // state loses that click entirely, including NPC interaction and loot.
        const bool pressed = currentMouseState[i] && !previousMouseState[i];
        const bool released = !currentMouseState[i] && previousMouseState[i];
        mousePressed[i] = pendingMousePress[i] || pressed;
        mouseReleased[i] = pendingMouseRelease[i] || released;
        if (pressed && !pendingMousePress[i]) mousePressPosition[i] = mousePosition;
        if (released && !pendingMouseRelease[i]) mouseReleasePosition[i] = mousePosition;
    }

    pendingMousePress.fill(false);
    pendingMouseRelease.fill(false);

    // Calculate mouse delta
    mouseDelta = mousePosition - previousMousePosition;
}

void Input::setVirtualKey(SDL_Scancode key, bool held) {
    if (key < 0 || key >= NUM_KEYS) return;
    virtualKeyState[key] = held;
}

void Input::setVirtualMouseButton(int button, bool held) {
    if (button < 0 || button >= NUM_MOUSE_BUTTONS) return;
    virtualMouseState[button] = held;
}

void Input::clearVirtualKeys() {
    virtualKeyState.fill(false);
}

bool Input::isKeyPressed(SDL_Scancode key) const {
    if (key < 0 || key >= NUM_KEYS) return false;
    return currentKeyState[key];
}

bool Input::isKeyJustPressed(SDL_Scancode key) const {
    if (key < 0 || key >= NUM_KEYS) return false;
    return currentKeyState[key] && !previousKeyState[key];
}
bool Input::isMouseButtonPressed(int button) const {
    if (button < 0 || button >= NUM_MOUSE_BUTTONS) return false;
    return currentMouseState[button];
}

bool Input::isMouseButtonJustPressed(int button) const {
    if (button < 0 || button >= NUM_MOUSE_BUTTONS) return false;
    return mousePressed[button];
}

bool Input::isMouseButtonJustReleased(int button) const {
    if (button < 0 || button >= NUM_MOUSE_BUTTONS) return false;
    return mouseReleased[button];
}
} // namespace core
} // namespace wowee
