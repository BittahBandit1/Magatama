#pragma once

// On-screen controller for iPhone, iPad and Android touchscreens (MAGATAMA_TOUCH_CONTROLS).
//
// A PS2 pad drawn over the game: the left stick (bottom left), the four face buttons (bottom right) with L1 and R1
// above them, L2/R2 (top corners), Select and Start (bottom centre) and a menu button (top centre, the Fukami in-game menu).
// The left control is a stick or a D-pad: a D-pad while the game's main menu is open (the game layer reports it,
// rrv_game_signals.h), a stick otherwise; the small button beside it switches by hand until the menu next opens
// or closes.
// Each finger belongs to what it first touched: the stick follows its finger anywhere on screen until it lifts,
// a finger on the face buttons slides between them. The stick also presses the D-pad direction it is pushed
// towards (past 60%), so menus that read only the D-pad follow it too.
//
// The layout is in units of the screen height, anchored to the left and right edges, so it fits any screen shape
// (4:3 and 13" iPads, 11" iPads and the mini, phones). It hides by itself while a physical controller is connected
// and comes back when the last one disconnects; the menu's Input > Touch controls (MAGATAMA_TOUCH_CONTROLS) turns
// it off altogether.
//
// Threads: finger events and pad reads come from the SDL main thread, drawing from the GS owner thread (the
// bridge's overlay callback); one mutex covers both.

#include "runtime/host_pad.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>

namespace rrv::host {

class TouchPad
{
public:
    // One draw call for the bridge (rrv_pcsx2_gs_bridge_draw_shape). Points are fractions of the screen width (x)
    // and height (y); radii, sizes and thickness are fractions of the screen height. rgba is 0xRRGGBBAA.
    enum Shape : int
    {
        FilledCircle = 0, CircleOutline = 1, FilledRect = 2, Line = 3, Text = 4, TriangleOutline = 5, RectOutline = 6
    };
    using DrawFn = void (*)(int shape, float x0, float y0, float x1, float y1, uint32_t rgba, float thickness,
                            const char *text);

    explicit TouchPad(bool enabled) : m_enabled(enabled) {}

    // The menu setting (Input > Touch controls).
    void setEnabled(bool enabled)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_enabled = enabled;
        releaseLocked();
    }

    // Whether a physical controller is connected (checked every frame on the main thread): it hides the pad.
    void setControllerConnected(bool connected)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (connected == m_controller)
            return;
        m_controller = connected;
        releaseLocked();
    }

    // Screen shape (width / height), from the window size.
    void setAspect(float aspect)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (aspect > 0.5f && aspect < 4.0f)
            m_aspect = aspect;
    }

    // The game's main menu (rrv::host::fieldMenuOpen: 1 open, 0 closed, -1 unknown): a change puts the left control
    // back under automatic control (D-pad while open).
    void setMenuOpen(int open)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (open == m_menuOpen)
            return;
        m_menuOpen = open;
        m_manual = false;
    }

    // Shown and taking touches.
    bool visible() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return shownLocked();
    }

    // Finger events, x and y as fractions of the window (SDL's tfinger.x/y). down() returns true when the finger
    // pressed the menu button (the caller toggles the in-game menu).
    bool down(int64_t id, float x, float y)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!shownLocked())
            return false;
        Finger *slot = nullptr;
        for (Finger &finger : m_fingers)
            if (finger.active && finger.id == id)
                slot = &finger;
        if (!slot)
            for (Finger &finger : m_fingers)
                if (!finger.active)
                {
                    slot = &finger;
                    break;
                }
        if (!slot)
            return false;
        const float ux = x * m_aspect, uy = y;
        *slot = {};
        slot->id = id;
        slot->active = true;
        if (distance(ux, uy, menuX(), kMenuY) <= kMenuHit)
        {
            slot->owner = Owner::Menu;
            return true;
        }
        if (distance(ux, uy, kToggleX, kToggleY) <= kToggleHit)
        {
            slot->owner = Owner::Toggle;
            const bool dpad = dpadLocked();
            m_manual = true;
            m_manualDpad = !dpad;
            return false;
        }
        if (distance(ux, uy, kStickX, kStickY) <= kStickHit)
        {
            slot->owner = Owner::Stick;
            slot->stickX = ux;
            slot->stickY = uy;
            return false;
        }
        if (const uint32_t face = faceAt(ux, uy))
        {
            slot->owner = Owner::Face;
            slot->buttons = face;
            return false;
        }
        if (const uint32_t button = buttonAt(ux, uy))
        {
            slot->owner = Owner::Button;
            slot->buttons = button;
            return false;
        }
        slot->owner = Owner::None; // empty screen: nothing pressed
        return false;
    }

    void move(int64_t id, float x, float y)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!shownLocked())
            return;
        for (Finger &finger : m_fingers)
        {
            if (!finger.active || finger.id != id)
                continue;
            const float ux = x * m_aspect, uy = y;
            if (finger.owner == Owner::Stick)
            {
                finger.stickX = ux;
                finger.stickY = uy;
            }
            else if (finger.owner == Owner::Face)
                finger.buttons = faceAt(ux, uy); // slide between face buttons (or off them)
        }
    }

    void up(int64_t id)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (Finger &finger : m_fingers)
            if (finger.active && finger.id == id)
                finger = {};
    }

    // All fingers lifted (focus lost, menu opened).
    void releaseAll()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        releaseLocked();
    }

    // Port 0 with the on-screen pad added: its buttons combined, its stick replacing a centred physical stick.
    HostPadState apply(HostPadState state) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!shownLocked())
            return state;
        uint32_t buttons = 0u;
        float sx = 0.0f, sy = 0.0f;
        const bool dpad = dpadLocked();
        for (const Finger &finger : m_fingers)
        {
            if (!finger.active)
                continue;
            buttons |= finger.buttons;
            if (finger.owner == Owner::Stick)
            {
                if (dpad)
                    buttons |= dpadOf(finger);
                else
                    stickOf(finger, sx, sy);
            }
        }
        bool any = buttons != 0u || sx != 0.0f || sy != 0.0f;
        if (!any)
            return state;
        state.connected = true;
        state.pressedButtons |= buttons;
        if (sx != 0.0f || sy != 0.0f)
        {
            state.leftX = sx;
            state.leftY = sy;
            if (sx < -kDpadAt) state.pressedButtons |= HostPadButtonLeft;
            if (sx > kDpadAt) state.pressedButtons |= HostPadButtonRight;
            if (sy > kDpadAt) state.pressedButtons |= HostPadButtonUp;
            if (sy < -kDpadAt) state.pressedButtons |= HostPadButtonDown;
        }
        if (buttons & HostPadButtonL2) state.leftTrigger = 1.0f;
        if (buttons & HostPadButtonR2) state.rightTrigger = 1.0f;
        return state;
    }

    // Draws the pad (GS owner thread, inside the bridge's overlay frame).
    void draw(DrawFn fn) const
    {
        if (!fn)
            return;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!shownLocked())
            return;
        uint32_t pressed = 0u;
        float sx = 0.0f, sy = 0.0f;
        bool menuDown = false, toggleDown = false;
        const bool dpad = dpadLocked();
        for (const Finger &finger : m_fingers)
        {
            if (!finger.active)
                continue;
            pressed |= finger.buttons;
            if (finger.owner == Owner::Stick)
            {
                if (dpad)
                    pressed |= dpadOf(finger);
                else
                    stickOf(finger, sx, sy);
            }
            menuDown |= finger.owner == Owner::Menu;
            toggleDown |= finger.owner == Owner::Toggle;
        }
        const float a = m_aspect;
        auto X = [a](float ux) { return ux / a; };
        auto fill = [](bool on) { return on ? 0xFFFFFF88u : 0x20202066u; };
        constexpr uint32_t kEdge = 0xFFFFFFAAu, kGlyph = 0xFFFFFFDDu;

        if (dpad)
        {
            // D-pad: four arms around a centre square.
            constexpr float w = kDpadArm * 0.5f, r = kStickRadius;
            struct Arm { uint32_t button; float x0, y0, x1, y1; };
            const Arm arms[] = {
                {HostPadButtonUp, -w, -r, w, -w}, {HostPadButtonDown, -w, w, w, r},
                {HostPadButtonLeft, -r, -w, -w, w}, {HostPadButtonRight, w, -w, r, w},
            };
            fn(FilledRect, X(kStickX - w), kStickY - w, X(kStickX + w), kStickY + w, 0x20202066u, 0, nullptr);
            for (const Arm &arm : arms)
            {
                fn(FilledRect, X(kStickX + arm.x0), kStickY + arm.y0, X(kStickX + arm.x1), kStickY + arm.y1,
                   fill(pressed & arm.button), 0, nullptr);
                fn(RectOutline, X(kStickX + arm.x0), kStickY + arm.y0, X(kStickX + arm.x1), kStickY + arm.y1,
                   kEdge, 0.003f, nullptr);
            }
        }
        else
        {
            // Stick: base ring and knob.
            fn(FilledCircle, X(kStickX), kStickY, kStickRadius, 0, 0x20202055u, 0, nullptr);
            fn(CircleOutline, X(kStickX), kStickY, kStickRadius, 0, kEdge, 0.004f, nullptr);
            fn(FilledCircle, X(kStickX + sx * kStickRadius), kStickY - sy * kStickRadius, kKnobRadius, 0,
               (sx != 0.0f || sy != 0.0f) ? 0xFFFFFF99u : 0xFFFFFF55u, 0, nullptr);
        }
        // Stick/D-pad switch: shows what it switches to.
        fn(FilledCircle, X(kToggleX), kToggleY, kToggleRadius, 0, fill(toggleDown), 0, nullptr);
        fn(CircleOutline, X(kToggleX), kToggleY, kToggleRadius, 0, kEdge, 0.003f, nullptr);
        if (dpad)
            fn(CircleOutline, X(kToggleX), kToggleY, kToggleRadius * 0.45f, 0, kGlyph, 0.0035f, nullptr);
        else
        {
            const float g = kToggleRadius * 0.5f;
            fn(Line, X(kToggleX - g), kToggleY, X(kToggleX + g), kToggleY, kGlyph, 0.006f, nullptr);
            fn(Line, X(kToggleX), kToggleY - g, X(kToggleX), kToggleY + g, kGlyph, 0.006f, nullptr);
        }

        // Face buttons with their PlayStation symbols.
        for (const Face &face : kFaces)
        {
            const float cx = X(faceX() + face.dx), cy = kFaceY + face.dy;
            fn(FilledCircle, cx, cy, kFaceRadius, 0, fill(pressed & face.button), 0, nullptr);
            fn(CircleOutline, cx, cy, kFaceRadius, 0, kEdge, 0.003f, nullptr);
            const float g = kFaceRadius * 0.45f;
            switch (face.button)
            {
            case HostPadButtonTriangle:
                fn(TriangleOutline, cx, cy, g, 0, kGlyph, 0.004f, nullptr);
                break;
            case HostPadButtonCircle:
                fn(CircleOutline, cx, cy, g, 0, kGlyph, 0.004f, nullptr);
                break;
            case HostPadButtonCross:
                fn(Line, cx - X(g), cy - g, cx + X(g), cy + g, kGlyph, 0.004f, nullptr);
                fn(Line, cx - X(g), cy + g, cx + X(g), cy - g, kGlyph, 0.004f, nullptr);
                break;
            default: // square
                fn(RectOutline, cx - X(g * 0.85f), cy - g * 0.85f, cx + X(g * 0.85f), cy + g * 0.85f, kGlyph,
                   0.004f, nullptr);
                break;
            }
        }

        // Shoulders, Select/Start, menu.
        for (const Rect &r : rects())
        {
            fn(FilledRect, X(r.x0), r.y0, X(r.x1), r.y1, fill(pressed & r.button), 0, nullptr);
            fn(RectOutline, X(r.x0), r.y0, X(r.x1), r.y1, kEdge, 0.003f, nullptr);
            fn(Text, X((r.x0 + r.x1) * 0.5f), (r.y0 + r.y1) * 0.5f, r.text, 0, kGlyph, 0, r.label);
        }
        fn(FilledCircle, X(menuX()), kMenuY, kMenuRadius, 0, fill(menuDown), 0, nullptr);
        fn(CircleOutline, X(menuX()), kMenuY, kMenuRadius, 0, kEdge, 0.003f, nullptr);
        for (int i = -1; i <= 1; ++i) // three bars
        {
            const float y = kMenuY + i * kMenuRadius * 0.35f, w = kMenuRadius * 0.5f;
            fn(Line, X(menuX() - w), y, X(menuX() + w), y, kGlyph, 0.0035f, nullptr);
        }
    }

private:
    enum class Owner { None, Stick, Face, Button, Menu, Toggle };
    struct Finger
    {
        int64_t id = 0;
        bool active = false;
        Owner owner = Owner::None;
        uint32_t buttons = 0u;
        float stickX = 0.0f, stickY = 0.0f; // finger position (height units) for the stick
    };
    struct Face { uint32_t button; float dx, dy; };
    struct Rect { uint32_t button; float x0, y0, x1, y1; const char *label; float text; };

    // Layout, in screen-height units (x measured from the left edge; right-side items from the right edge).
    static constexpr float kStickX = 0.25f, kStickY = 0.70f, kStickRadius = 0.13f, kKnobRadius = 0.06f;
    static constexpr float kStickHit = 0.24f; // where a finger can start the stick
    static constexpr float kFaceInset = 0.27f, kFaceY = 0.68f, kFaceSpread = 0.105f, kFaceRadius = 0.06f;
    static constexpr float kFaceHit = 0.085f;
    static constexpr float kMenuY = 0.07f, kMenuRadius = 0.045f, kMenuHit = 0.07f;
    static constexpr float kDpadAt = 0.6f;
    static constexpr float kDpadArm = 0.085f; // D-pad arm width
    static constexpr float kToggleX = kStickX + 0.17f, kToggleY = kStickY - 0.19f, kToggleRadius = 0.035f;
    static constexpr float kToggleHit = 0.055f;
    static constexpr std::array<Face, 4> kFaces{{
        {HostPadButtonTriangle, 0.0f, -kFaceSpread},
        {HostPadButtonCross, 0.0f, kFaceSpread},
        {HostPadButtonSquare, -kFaceSpread, 0.0f},
        {HostPadButtonCircle, kFaceSpread, 0.0f},
    }};

    bool shownLocked() const { return m_enabled && !m_controller; }
    bool dpadLocked() const { return m_manual ? m_manualDpad : m_menuOpen == 1; }

    // D-pad directions from where its finger is: eight ways (a diagonal presses both).
    static uint32_t dpadOf(const Finger &finger)
    {
        const float dx = finger.stickX - kStickX, dy = kStickY - finger.stickY, length = std::hypot(dx, dy);
        if (length < 0.03f)
            return 0u;
        constexpr float kSector = 0.38f; // cos 67.5 degrees
        uint32_t buttons = 0u;
        if (dx > kSector * length) buttons |= HostPadButtonRight;
        if (dx < -kSector * length) buttons |= HostPadButtonLeft;
        if (dy > kSector * length) buttons |= HostPadButtonUp;
        if (dy < -kSector * length) buttons |= HostPadButtonDown;
        return buttons;
    }
    void releaseLocked()
    {
        for (Finger &finger : m_fingers)
            finger = {};
    }

    static float distance(float x0, float y0, float x1, float y1) { return std::hypot(x1 - x0, y1 - y0); }
    float faceX() const { return m_aspect - kFaceInset; }
    float menuX() const { return m_aspect * 0.5f; }

    std::array<Rect, 6> rects() const
    {
        // L1/R1 sit side by side above the face buttons (the right thumb turns the camera without leaving them);
        // L2/R2 stay in the top corners.
        const float a = m_aspect, mid = a * 0.5f, fx = faceX();
        constexpr float top = kFaceY - kFaceSpread - kFaceRadius - 0.105f, bottom = top + 0.075f;
        return {{
            {HostPadButtonL1, fx - 0.20f, top, fx - 0.015f, bottom, "L1", 0.04f},
            {HostPadButtonL2, 0.04f, 0.27f, 0.24f, 0.35f, "L2", 0.04f},
            {HostPadButtonR1, fx + 0.015f, top, fx + 0.20f, bottom, "R1", 0.04f},
            {HostPadButtonR2, a - 0.24f, 0.27f, a - 0.04f, 0.35f, "R2", 0.04f},
            {HostPadButtonSelect, mid - 0.26f, 0.90f, mid - 0.06f, 0.96f, "SELECT", 0.032f},
            {HostPadButtonStart, mid + 0.06f, 0.90f, mid + 0.26f, 0.96f, "START", 0.032f},
        }};
    }

    uint32_t faceAt(float ux, float uy) const
    {
        uint32_t best = 0u;
        float bestDistance = kFaceHit;
        for (const Face &face : kFaces)
        {
            const float d = distance(ux, uy, faceX() + face.dx, kFaceY + face.dy);
            if (d <= bestDistance)
            {
                bestDistance = d;
                best = face.button;
            }
        }
        return best;
    }

    uint32_t buttonAt(float ux, float uy) const
    {
        constexpr float slop = 0.02f; // fingers are wider than the drawn edge
        for (const Rect &r : rects())
            if (ux >= r.x0 - slop && ux <= r.x1 + slop && uy >= r.y0 - slop && uy <= r.y1 + slop)
                return r.button;
        return 0u;
    }

    // Stick deflection (-1..1, positive up) from where its finger is.
    static void stickOf(const Finger &finger, float &x, float &y)
    {
        float dx = (finger.stickX - kStickX) / kStickRadius, dy = (kStickY - finger.stickY) / kStickRadius;
        const float length = std::hypot(dx, dy);
        if (length < 0.12f) // dead zone
            return;
        if (length > 1.0f)
        {
            dx /= length;
            dy /= length;
        }
        x = std::clamp(dx, -1.0f, 1.0f);
        y = std::clamp(dy, -1.0f, 1.0f);
    }

    mutable std::mutex m_mutex;
    bool m_enabled;
    bool m_controller = false;
    int m_menuOpen = -1;       // the game's main menu (setMenuOpen)
    bool m_manual = false;     // the switch was used since the menu last opened or closed
    bool m_manualDpad = false; // what it chose
    float m_aspect = 4.0f / 3.0f;
    std::array<Finger, 10> m_fingers{};
};

} // namespace rrv::host
