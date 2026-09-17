// ===========================================================================
// What the console, the chip and the rack all agree on.
//
// Nothing in this header knows about bsnes, and nothing in it knows about
// Rack. bsnes reaches its own code through an include path rooted at its
// source tree and brings nall with it -- a library that redefines `uint`,
// writes every function with a trailing return type, and ends one of its
// headers with `#undef double`. A Rack plugin that saw any of that would be
// a plugin whose build depended on which header came first.
//
// So: three files under src/snes/ see bsnes and are built by Makefile.core
// with bsnes's own flags. Everything else in the plugin sees only this.
// ===========================================================================
#pragma once

#include <cmath>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace racksnes {

// The S-DSP: eight voices, 32 kHz, 64K of RAM it shares with its own CPU.
constexpr int      VoiceCount  = 8;
constexpr int      AramSize    = 0x10000;
constexpr int      DspRegCount = 128;

// 32040 Hz exactly: the APU's 24.576 MHz crystal divided by 768. Everything
// the chip makes happens at this rate, and the pitch of a sample is a ratio
// against it, which is why a cartridge's music transposes when you change it.
constexpr double   ApuRate     = 32040.0;

// The picture, as the PPU hands it over: 256 across, 240 down. Most games
// draw 224 of those rows and leave the eight at each end to the overscan.
constexpr int      ScreenW     = 256;
constexpr int      ScreenH     = 240;
constexpr int      ScreenCrop  = 8;          // rows hidden at each end by default
constexpr int      ScreenVisible = ScreenH - 2 * ScreenCrop;

// The gamepad, in the order the SNES shifts its buttons out.
enum Button : int {
    ButtonUp = 0, ButtonDown, ButtonLeft, ButtonRight,
    ButtonB, ButtonA, ButtonY, ButtonX,
    ButtonL, ButtonR, ButtonSelect, ButtonStart,
    ButtonCount
};

// ---------------------------------------------------------------------------
// One sample of the chip, taken apart.
//
// Everything is normalised so that the chip's full scale is 1.0, and nothing
// else has been done to it: `main` is what the console's own output jack
// would carry, `voice[i]` is what one voice put into that sum after its
// envelope and its panning, and `dry[i]` is the same voice before the panning
// -- one voice, alone, the way a synthesiser voice is alone.
// ---------------------------------------------------------------------------
struct ChipFrame {
    float main [2] = {};
    float echo [2] = {};
    float voice[VoiceCount][2] = {};
    float dry  [VoiceCount] = {};

    // What each voice was doing when this sample was made. It travels with
    // the sample rather than beside it because the machine runs ahead of what
    // you are hearing: a gate that arrived when the note was made instead of
    // when it is heard would be a gate playing the rest of the patch several
    // frames early.
    uint8_t  env  [VoiceCount] = {};   // 0..127, as ENVX reads it
    uint8_t  srcn [VoiceCount] = {};
    uint16_t pitch[VoiceCount] = {};   // 14 bits, 0x1000 is 1:1
    uint8_t  keyed = 0;                // a bit a voice
};

// The floats of a ChipFrame, in the order the resampler sees them.
constexpr int ChipFloats = 4 + 2 * VoiceCount + VoiceCount;

inline void flatten(const ChipFrame& f, float* out)
{
    int k = 0;
    out[k++] = f.main[0]; out[k++] = f.main[1];
    out[k++] = f.echo[0]; out[k++] = f.echo[1];
    for(int v = 0; v < VoiceCount; ++v) { out[k++] = f.voice[v][0]; out[k++] = f.voice[v][1]; }
    for(int v = 0; v < VoiceCount; ++v) out[k++] = f.dry[v];
}

inline void unflatten(const float* in, ChipFrame& f)
{
    int k = 0;
    f.main[0] = in[k++]; f.main[1] = in[k++];
    f.echo[0] = in[k++]; f.echo[1] = in[k++];
    for(int v = 0; v < VoiceCount; ++v) { f.voice[v][0] = in[k++]; f.voice[v][1] = in[k++]; }
    for(int v = 0; v < VoiceCount; ++v) f.dry[v] = in[k++];
}

// What each voice is doing, for the jacks that carry it as control rather
// than as sound. Updated every sample; read once a block.
struct VoiceState {
    float    volts   = 0.f;   // pitch as a volt per octave, against C4
    float    level   = 0.f;   // envelope, 0..1
    uint8_t  srcn    = 0;     // which sample in the directory
    bool     keyed   = false; // its envelope is not silent
};

// ---------------------------------------------------------------------------
// The bending, as a message.
//
// It lives here rather than in the plugin because the machine is what applies
// it: reaching into a running cartridge's palette or its sound chip's memory
// is something that has to happen on the thread the emulator runs on, and
// that thread belongs to Console.
// ---------------------------------------------------------------------------
struct BendMessage {
    bool  present       = false;

    int   paletteMode   = 0;     // rotate / shift / invert / drain
    float paletteAmount = 0.f;

    float transpose     = 0.f;   // volts, applied to every voice's pitch

    int   warpMode      = 0;     // which voices modulate / go to noise
    float warpAmount    = 0.f;

    float echoFeedback  = 0.f;   // past where the hardware would go

    int   glitchRegion  = 0;     // tiles / palette / samples / all of it
    float glitchRate    = 0.f;

    bool  freezeHeld    = false;
};

// ---------------------------------------------------------------------------
// A cubic resampler. The chip runs at 32040 Hz and the rack does not, and the
// ratio between them is also where the speed control lives: ask for more
// input per output and the whole machine -- picture, music and pitch together
// -- runs slow, the way a tape does.
// ---------------------------------------------------------------------------
template <int N>
struct Resampler {
    float  h[4][N] = {};
    double phase   = 1.0;   // >= 1 means "I need another input frame"
    int    primed  = 0;

    void reset()
    {
        for (int i = 0; i < 4; ++i)
            for (int c = 0; c < N; ++c) h[i][c] = 0.f;
        phase  = 1.0;
        primed = 0;
    }

    bool hungry() const { return phase >= 1.0 || primed < 4; }

    void push(const float* in)
    {
        for (int c = 0; c < N; ++c)
        {
            h[0][c] = h[1][c];
            h[1][c] = h[2][c];
            h[2][c] = h[3][c];
            h[3][c] = in[c];
        }

        if (primed < 4) ++primed;
        else            phase -= 1.0;
    }

    // Catmull-Rom through h[1] and h[2]. Called only when hungry() is false.
    void read(float* out)
    {
        const float t = (float)phase;

        for (int c = 0; c < N; ++c)
        {
            const float a = h[0][c], b = h[1][c], d = h[2][c], e = h[3][c];
            const float c0 = b;
            const float c1 = 0.5f * (d - a);
            const float c2 = a - 2.5f * b + 2.f * d - 0.5f * e;
            const float c3 = 0.5f * (e - a) + 1.5f * (b - d);
            out[c] = ((c3 * t + c2) * t + c1) * t + c0;
        }
    }

    void advance(double step) { phase += step; }
};

// ---------------------------------------------------------------------------
// The pitch register, both ways.
//
// A voice's PITCH is how far through its sample the chip steps each of its
// 32040 ticks, as a 14-bit number where 0x1000 is one sample per tick. A BRR
// sample has no notion of what note it is: 0x1000 is whatever pitch it was
// recorded at. By the convention every SNES sound driver uses, that is the
// note the sampler calls its root.
// ---------------------------------------------------------------------------
inline float pitchToVolts(int pitch)
{
    if (pitch <= 0) return -10.f;
    return (float)std::log2((double)pitch / 4096.0);
}

inline int voltsToPitch(float volts)
{
    int pitch = (int)(4096.0 * std::exp2((double)volts) + 0.5);
    if (pitch < 0)      pitch = 0;
    if (pitch > 0x3fff) pitch = 0x3fff;   // the register is fourteen bits: the
    return pitch;                         // chip cannot play higher than 4x
}

} // namespace racksnes
