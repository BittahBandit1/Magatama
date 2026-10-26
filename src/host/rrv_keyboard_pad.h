#pragma once

#include "runtime/host_pad.h"
#include <SDL_scancode.h>

namespace rrv::host
{
// Convert the current key state, rather than key-repeat events, so releasing
// keys always returns both sticks and buttons to neutral.
inline HostPadState keyboardPadState(const uint8_t *keys, bool hostMenuAvailable,
                                     bool leftMouseDown = false)
{
    HostPadState pad{};
    if (!keys)
        return pad;
    pad.connected = true;
    pad.deviceId = 0x6b6579626f617264ULL; // "keyboard"
    const auto held = [&](SDL_Scancode key) { return keys[key] != 0; };
    const auto button = [&](HostPadButton value, bool down) {
        if (down) pad.pressedButtons |= value;
    };
    const bool up = held(SDL_SCANCODE_UP) || held(SDL_SCANCODE_W);
    const bool down = held(SDL_SCANCODE_DOWN) || held(SDL_SCANCODE_S);
    const bool left = held(SDL_SCANCODE_LEFT) || held(SDL_SCANCODE_A);
    const bool right = held(SDL_SCANCODE_RIGHT) || held(SDL_SCANCODE_D);
    button(HostPadButtonUp, up && !down);
    button(HostPadButtonDown, down && !up);
    button(HostPadButtonLeft, left && !right);
    button(HostPadButtonRight, right && !left);
    // Keep digital menu navigation, and also supply the analog movement that
    // games such as Nocturne expect in DualShock mode. Y is positive-up here.
    pad.leftX = float(right) - float(left);
    pad.leftY = float(up) - float(down);
    pad.rightX = float(held(SDL_SCANCODE_L)) - float(held(SDL_SCANCODE_J));
    pad.rightY = float(held(SDL_SCANCODE_I)) - float(held(SDL_SCANCODE_K));
    const auto normalize = [](float &x, float &y) {
        if (x != 0.0f && y != 0.0f)
        {
            constexpr float diagonal = 0.7071067812f;
            x *= diagonal;
            y *= diagonal;
        }
    };
    normalize(pad.leftX, pad.leftY);
    normalize(pad.rightX, pad.rightY);

    button(HostPadButtonCross, held(SDL_SCANCODE_SPACE) || leftMouseDown);
    // Escape belongs to the host settings menu when that menu is available.
    button(HostPadButtonCircle, held(SDL_SCANCODE_BACKSPACE) || held(SDL_SCANCODE_C) ||
                               (held(SDL_SCANCODE_ESCAPE) && !hostMenuAvailable));
    button(HostPadButtonSquare, held(SDL_SCANCODE_KP_0) || held(SDL_SCANCODE_Z));
    button(HostPadButtonTriangle, held(SDL_SCANCODE_KP_1) || held(SDL_SCANCODE_X));
    button(HostPadButtonL1, held(SDL_SCANCODE_Q));
    button(HostPadButtonR1, held(SDL_SCANCODE_E));
    button(HostPadButtonL2, held(SDL_SCANCODE_LSHIFT));
    button(HostPadButtonR2, held(SDL_SCANCODE_RSHIFT));
    button(HostPadButtonL3, held(SDL_SCANCODE_1));
    button(HostPadButtonR3, held(SDL_SCANCODE_3));
    button(HostPadButtonStart, held(SDL_SCANCODE_RETURN) || held(SDL_SCANCODE_KP_ENTER));
    button(HostPadButtonSelect, held(SDL_SCANCODE_TAB));
    pad.leftTrigger = held(SDL_SCANCODE_LSHIFT) ? 1.0f : 0.0f;
    pad.rightTrigger = held(SDL_SCANCODE_RSHIFT) ? 1.0f : 0.0f;
    return pad;
}
} // namespace rrv::host
