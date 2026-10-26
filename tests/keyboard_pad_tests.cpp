#include "rrv_keyboard_pad.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

int main()
{
    std::array<uint8_t, SDL_NUM_SCANCODES> keys{};
    unsigned checks = 0;
    const auto check = [&](bool ok, const char *message) {
        ++checks;
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
    };
    const auto sample = [&] { return rrv::host::keyboardPadState(keys.data(), true); };
    auto pad = sample();
    check(pad.connected && !pad.pressedButtons && !pad.leftX && !pad.leftY &&
          !pad.rightX && !pad.rightY, "idle keyboard is a connected, neutral pad");
    for (const auto key : {SDL_SCANCODE_W, SDL_SCANCODE_UP})
    {
        keys.fill(0); keys[key] = 1; pad = sample();
        check(pad.leftY == 1.0f && pad.leftX == 0.0f &&
              pad.pressedButtons == HostPadButtonUp, "up drives both movement and menu direction");
    }
    keys.fill(0); keys[SDL_SCANCODE_S] = 1; keys[SDL_SCANCODE_D] = 1; pad = sample();
    check(pad.leftX > 0.7f && pad.leftY < -0.7f &&
          std::fabs(pad.leftX * pad.leftX + pad.leftY * pad.leftY - 1.0f) < 0.0001f,
          "diagonal movement has unit length and correct positive-up convention");
    keys[SDL_SCANCODE_W] = 1; keys[SDL_SCANCODE_A] = 1; pad = sample();
    check(!pad.leftX && !pad.leftY && !pad.pressedButtons, "opposite directions cancel");
    keys.fill(0); keys[SDL_SCANCODE_I] = 1; keys[SDL_SCANCODE_J] = 1; pad = sample();
    check(pad.rightX < -0.7f && pad.rightY > 0.7f && !pad.leftX && !pad.leftY &&
          !pad.pressedButtons, "camera keys drive only right stick");
    keys[SDL_SCANCODE_K] = 1; keys[SDL_SCANCODE_L] = 1; pad = sample();
    check(!pad.rightX && !pad.rightY, "opposite camera keys cancel");
    struct Binding { SDL_Scancode key; HostPadButton button; };
    for (const Binding b : {
        Binding{SDL_SCANCODE_SPACE, HostPadButtonCross},
        {SDL_SCANCODE_C, HostPadButtonCircle}, {SDL_SCANCODE_BACKSPACE, HostPadButtonCircle},
        {SDL_SCANCODE_Z, HostPadButtonSquare}, {SDL_SCANCODE_KP_0, HostPadButtonSquare},
        {SDL_SCANCODE_X, HostPadButtonTriangle}, {SDL_SCANCODE_KP_1, HostPadButtonTriangle},
        {SDL_SCANCODE_RETURN, HostPadButtonStart}, {SDL_SCANCODE_KP_ENTER, HostPadButtonStart},
        {SDL_SCANCODE_TAB, HostPadButtonSelect}, {SDL_SCANCODE_Q, HostPadButtonL1},
        {SDL_SCANCODE_E, HostPadButtonR1}, {SDL_SCANCODE_LSHIFT, HostPadButtonL2},
        {SDL_SCANCODE_RSHIFT, HostPadButtonR2}, {SDL_SCANCODE_1, HostPadButtonL3},
        {SDL_SCANCODE_3, HostPadButtonR3}})
    {
        keys.fill(0); keys[b.key] = 1;
        check(sample().pressedButtons == b.button, "keyboard button reaches the expected PS2 control");
    }
    keys.fill(0); keys[SDL_SCANCODE_LSHIFT] = 1; keys[SDL_SCANCODE_RSHIFT] = 1; pad = sample();
    check(pad.leftTrigger == 1.0f && pad.rightTrigger == 1.0f, "shoulder keys supply trigger values");
    keys.fill(0); keys[SDL_SCANCODE_SPACE] = 1; keys[SDL_SCANCODE_W] = 1; pad = sample();
    check((pad.pressedButtons & HostPadButtonCross) && pad.leftY == 1.0f,
          "movement and confirm can be held together");
    keys.fill(0); keys[SDL_SCANCODE_ESCAPE] = 1;
    check(!sample().pressedButtons, "Escape does not cancel gameplay while reserved for host menu");
    check(rrv::host::keyboardPadState(keys.data(), false).pressedButtons == HostPadButtonCircle,
          "Escape retains fallback cancel without a host menu");
    keys.fill(0); pad = sample();
    check(!pad.pressedButtons && !pad.leftX && !pad.leftY && !pad.rightX && !pad.rightY &&
          !pad.leftTrigger && !pad.rightTrigger, "release clears all axes, triggers and buttons");
    pad = rrv::host::keyboardPadState(keys.data(), true, true);
    check(pad.pressedButtons == HostPadButtonCross, "left mouse button supplies Cross");
    keys[SDL_SCANCODE_SPACE] = 1;
    pad = rrv::host::keyboardPadState(keys.data(), true, true);
    check(pad.pressedButtons == HostPadButtonCross, "Space and left mouse combine as one Cross press");
    pad = rrv::host::keyboardPadState(keys.data(), true, false);
    check(pad.pressedButtons == HostPadButtonCross, "mouse release preserves held Space");
    keys.fill(0); pad = rrv::host::keyboardPadState(keys.data(), true, false);
    check(!pad.pressedButtons, "releasing mouse and Space clears Cross");
    check(!rrv::host::keyboardPadState(nullptr, true).connected, "missing key state is disconnected");
    std::printf("keyboard pad: %u checks passed\n", checks);
}
