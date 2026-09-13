// ===========================================================================
// A gamepad, for playing the game with.
//
// Rack is built on GLFW and GLFW already knows about gamepads, including the
// standard layout for the several hundred it ships mappings for. So this is
// small: poll the pad, turn what it says into the twelve buttons a Super
// Nintendo had, and let anything be remapped onto anything.
//
// One rule, and it is GLFW's: joystick functions may only be called from the
// thread that initialised GLFW, which in Rack is the one that draws. So the
// reading happens in the widget's step() and the answer reaches the module
// through a word of memory, never the other way round.
// ===========================================================================
#pragma once

#include <GLFW/glfw3.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "snes/SnesCore.hpp"

namespace racksnes {

// Where one of the console's buttons gets its state from.
struct Binding {
    enum Kind : int { None = 0, Button, AxisLow, AxisHigh, HatBit };

    int kind  = None;
    int index = 0;      // which button, axis or hat
    int bit   = 0;      // for a hat: GLFW_HAT_UP and friends

    bool operator==(const Binding& o) const
    { return kind == o.kind && index == o.index && bit == o.bit; }
};

struct Mapping {
    Binding to[ButtonCount];

    // GLFW's standard layout, laid over a Super Nintendo's. The face buttons
    // are the interesting part: a SNES pad has B at the bottom and A on the
    // right, which is where GLFW's A and B are, so the letters differ and the
    // positions agree -- and it is the positions a thumb knows.
    static Mapping standard()
    {
        Mapping m;

        auto b = [](int i) { Binding x; x.kind = Binding::Button; x.index = i; return x; };

        m.to[ButtonUp]     = b(GLFW_GAMEPAD_BUTTON_DPAD_UP);
        m.to[ButtonDown]   = b(GLFW_GAMEPAD_BUTTON_DPAD_DOWN);
        m.to[ButtonLeft]   = b(GLFW_GAMEPAD_BUTTON_DPAD_LEFT);
        m.to[ButtonRight]  = b(GLFW_GAMEPAD_BUTTON_DPAD_RIGHT);
        m.to[ButtonB]      = b(GLFW_GAMEPAD_BUTTON_A);
        m.to[ButtonA]      = b(GLFW_GAMEPAD_BUTTON_B);
        m.to[ButtonY]      = b(GLFW_GAMEPAD_BUTTON_X);
        m.to[ButtonX]      = b(GLFW_GAMEPAD_BUTTON_Y);
        m.to[ButtonL]      = b(GLFW_GAMEPAD_BUTTON_LEFT_BUMPER);
        m.to[ButtonR]      = b(GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER);
        m.to[ButtonSelect] = b(GLFW_GAMEPAD_BUTTON_BACK);
        m.to[ButtonStart]  = b(GLFW_GAMEPAD_BUTTON_START);

        return m;
    }
};

// What one pad is doing, in the plainest possible terms.
struct PadState {
    bool  present = false;
    bool  standard = false;         // GLFW has a mapping for it
    int   buttonCount = 0;
    int   axisCount = 0;
    int   hatCount = 0;
    unsigned char button[32] = {};
    float axis[8] = {};
    unsigned char hat[4] = {};
    std::string name;
};

// ---------------------------------------------------------------------------
// Reading the pad. Call from the drawing thread and nowhere else.
// ---------------------------------------------------------------------------
inline bool readPad(int joystick, PadState& out)
{
    out = PadState();

    if(joystick < 0 || joystick > GLFW_JOYSTICK_LAST) return false;
    if(!glfwJoystickPresent(joystick)) return false;

    out.present = true;
    if(const char* n = glfwGetJoystickName(joystick)) out.name = n;

    // A pad GLFW recognises reports in a fixed layout, so a mapping made on
    // one machine means the same thing on another. One it does not recognise
    // reports whatever its firmware felt like, and has to be taught.
    GLFWgamepadstate gs;
    if(glfwJoystickIsGamepad(joystick) && glfwGetGamepadState(joystick, &gs))
    {
        out.standard = true;
        if(const char* n = glfwGetGamepadName(joystick)) out.name = n;

        out.buttonCount = GLFW_GAMEPAD_BUTTON_LAST + 1;
        for(int i = 0; i <= GLFW_GAMEPAD_BUTTON_LAST; ++i) out.button[i] = gs.buttons[i];

        out.axisCount = GLFW_GAMEPAD_AXIS_LAST + 1;
        for(int i = 0; i <= GLFW_GAMEPAD_AXIS_LAST; ++i) out.axis[i] = gs.axes[i];

        return true;
    }

    int n = 0;
    if(const unsigned char* b = glfwGetJoystickButtons(joystick, &n))
    {
        out.buttonCount = n > 32 ? 32 : n;
        for(int i = 0; i < out.buttonCount; ++i) out.button[i] = b[i];
    }

    if(const float* a = glfwGetJoystickAxes(joystick, &n))
    {
        out.axisCount = n > 8 ? 8 : n;
        for(int i = 0; i < out.axisCount; ++i) out.axis[i] = a[i];
    }

    if(const unsigned char* h = glfwGetJoystickHats(joystick, &n))
    {
        out.hatCount = n > 4 ? 4 : n;
        for(int i = 0; i < out.hatCount; ++i) out.hat[i] = h[i];
    }

    return true;
}

inline bool bindingHeld(const PadState& pad, const Binding& b)
{
    switch(b.kind)
    {
    case Binding::Button:
        return b.index < pad.buttonCount && pad.button[b.index];

    // A stick counts as pressed past halfway, which is well outside any
    // resting position a worn-out pad settles into.
    case Binding::AxisLow:
        return b.index < pad.axisCount && pad.axis[b.index] < -0.5f;
    case Binding::AxisHigh:
        return b.index < pad.axisCount && pad.axis[b.index] > 0.5f;

    case Binding::HatBit:
        return b.index < pad.hatCount && (pad.hat[b.index] & b.bit);

    default:
        return false;
    }
}

// The twelve buttons, packed into one word, so that the drawing thread can
// hand them to the audio thread without either waiting for the other.
inline uint16_t squash(const PadState& pad, const Mapping& map)
{
    uint16_t bits = 0;

    for(int i = 0; i < ButtonCount; ++i)
        if(bindingHeld(pad, map.to[i])) bits |= (uint16_t)(1 << i);

    // The left stick does what the D-pad does, always, because a pad whose
    // D-pad is mapped and whose stick is not is a pad half of people will
    // think is broken.
    if(pad.standard)
    {
        if(pad.axis[GLFW_GAMEPAD_AXIS_LEFT_Y] < -0.5f) bits |= 1 << ButtonUp;
        if(pad.axis[GLFW_GAMEPAD_AXIS_LEFT_Y] >  0.5f) bits |= 1 << ButtonDown;
        if(pad.axis[GLFW_GAMEPAD_AXIS_LEFT_X] < -0.5f) bits |= 1 << ButtonLeft;
        if(pad.axis[GLFW_GAMEPAD_AXIS_LEFT_X] >  0.5f) bits |= 1 << ButtonRight;
    }

    return bits;
}

// What the pad is doing right now, for a panel that has to show it and for
// the learn that has to catch it.
inline Binding firstPressed(const PadState& before, const PadState& now)
{
    Binding b;

    for(int i = 0; i < now.buttonCount; ++i)
        if(now.button[i] && !before.button[i])
        { b.kind = Binding::Button; b.index = i; return b; }

    for(int i = 0; i < now.axisCount; ++i)
    {
        if(now.axis[i] < -0.6f && before.axis[i] >= -0.6f)
        { b.kind = Binding::AxisLow; b.index = i; return b; }
        if(now.axis[i] >  0.6f && before.axis[i] <=  0.6f)
        { b.kind = Binding::AxisHigh; b.index = i; return b; }
    }

    for(int i = 0; i < now.hatCount; ++i)
    {
        const int fresh = now.hat[i] & ~before.hat[i];
        for(int bit : { GLFW_HAT_UP, GLFW_HAT_RIGHT, GLFW_HAT_DOWN, GLFW_HAT_LEFT })
            if(fresh & bit) { b.kind = Binding::HatBit; b.index = i; b.bit = bit; return b; }
    }

    return b;
}

inline std::string bindingName(const Binding& b)
{
    char buf[32];

    switch(b.kind)
    {
    case Binding::Button:   std::snprintf(buf, sizeof buf, "button %d", b.index); break;
    case Binding::AxisLow:  std::snprintf(buf, sizeof buf, "axis %d -", b.index); break;
    case Binding::AxisHigh: std::snprintf(buf, sizeof buf, "axis %d +", b.index); break;
    case Binding::HatBit:   std::snprintf(buf, sizeof buf, "hat %d %s", b.index,
                                b.bit == GLFW_HAT_UP    ? "up"   :
                                b.bit == GLFW_HAT_DOWN  ? "down" :
                                b.bit == GLFW_HAT_LEFT  ? "left" : "right"); break;
    default: return "-";
    }

    return buf;
}

// ===========================================================================
// A keyboard, for playing the game without a pad.
//
// Polled the same way and from the same thread, because GLFW says so. The
// default is the layout every emulator has used since the nineties: the
// arrows, Z and X for the buttons your thumb lives on, and Enter for start.
// ===========================================================================
struct KeyMap {
    int to[ButtonCount];

    static KeyMap standard()
    {
        KeyMap m;

        m.to[ButtonUp]     = GLFW_KEY_UP;
        m.to[ButtonDown]   = GLFW_KEY_DOWN;
        m.to[ButtonLeft]   = GLFW_KEY_LEFT;
        m.to[ButtonRight]  = GLFW_KEY_RIGHT;
        m.to[ButtonB]      = GLFW_KEY_Z;
        m.to[ButtonA]      = GLFW_KEY_X;
        m.to[ButtonY]      = GLFW_KEY_A;
        m.to[ButtonX]      = GLFW_KEY_S;
        m.to[ButtonL]      = GLFW_KEY_Q;
        m.to[ButtonR]      = GLFW_KEY_W;
        m.to[ButtonSelect] = GLFW_KEY_RIGHT_SHIFT;
        m.to[ButtonStart]  = GLFW_KEY_ENTER;

        return m;
    }
};

// What a key is called. GLFW knows the printable ones and nothing else, so
// the rest are named here -- and they are the ones a keyboard player uses.
inline std::string keyName(int key)
{
    if(key == GLFW_KEY_UNKNOWN) return "-";

    switch(key)
    {
    case GLFW_KEY_SPACE:        return "space";
    case GLFW_KEY_ENTER:        return "enter";
    case GLFW_KEY_TAB:          return "tab";
    case GLFW_KEY_BACKSPACE:    return "backspace";
    case GLFW_KEY_ESCAPE:       return "escape";
    case GLFW_KEY_UP:           return "up";
    case GLFW_KEY_DOWN:         return "down";
    case GLFW_KEY_LEFT:         return "left";
    case GLFW_KEY_RIGHT:        return "right";
    case GLFW_KEY_LEFT_SHIFT:   return "left shift";
    case GLFW_KEY_RIGHT_SHIFT:  return "right shift";
    case GLFW_KEY_LEFT_CONTROL: return "left ctrl";
    case GLFW_KEY_RIGHT_CONTROL:return "right ctrl";
    case GLFW_KEY_LEFT_ALT:     return "left alt";
    case GLFW_KEY_RIGHT_ALT:    return "right alt";
    case GLFW_KEY_KP_ENTER:     return "keypad enter";
    default: break;
    }

    if(key >= GLFW_KEY_KP_0 && key <= GLFW_KEY_KP_9)
    {
        char buf[16];
        std::snprintf(buf, sizeof buf, "keypad %d", key - GLFW_KEY_KP_0);
        return buf;
    }

    if(key >= GLFW_KEY_F1 && key <= GLFW_KEY_F25)
    {
        char buf[8];
        std::snprintf(buf, sizeof buf, "f%d", key - GLFW_KEY_F1 + 1);
        return buf;
    }

    if(const char* n = glfwGetKeyName(key, 0)) return n;

    char buf[16];
    std::snprintf(buf, sizeof buf, "key %d", key);
    return buf;
}

// Every key a person might bind, which is every key that is not a modifier
// GLFW reports twice. Walked once a frame only while something is being
// taught, and never otherwise.
inline int firstKeyDown(GLFWwindow* window)
{
    if(!window) return GLFW_KEY_UNKNOWN;

    for(int k = GLFW_KEY_SPACE; k <= GLFW_KEY_LAST; ++k)
        if(glfwGetKey(window, k) == GLFW_PRESS) return k;

    return GLFW_KEY_UNKNOWN;
}

inline uint16_t squashKeys(GLFWwindow* window, const KeyMap& map)
{
    if(!window) return 0;

    uint16_t bits = 0;

    for(int i = 0; i < ButtonCount; ++i)
        if(map.to[i] != GLFW_KEY_UNKNOWN && glfwGetKey(window, map.to[i]) == GLFW_PRESS)
            bits |= (uint16_t)(1 << i);

    return bits;
}

} // namespace racksnes
