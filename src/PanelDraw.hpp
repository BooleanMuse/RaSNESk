// ===========================================================================
// The four panels, drawn.
//
// Nothing in here knows about Rack. That is on purpose: the modules draw
// these functions, and so does tools/mockup, which renders every panel to a
// PNG with no window and no Rack at all. A label that lands on top of a
// socket lands there in both, which is the only way to find out before
// installing.
//
// Everything is in millimetres. `Ink::scale` is how many pixels one of them
// is: mm2px(1) inside Rack, whatever the mockup fancies outside it.
//
// The look is the machine's own: the warm grey of the console's body, the
// cream of a Mario Paint window, sockets sunk into the panel and buttons
// raised out of it, and every word set in the 5x7 font in Pixel.hpp. There is
// no typeface from the desktop anywhere on these panels.
// ===========================================================================
#pragma once

#include <nanovg.h>

#include "Layout.hpp"
#include "Pixel.hpp"

#include <cstdio>
#include <cstring>

namespace racksnes {

// The machine, as a set of colours: the grey of its body, the cream of a
// window, the purple of its buttons, and the four the Super Famicom put on
// its face buttons.
namespace sfc {
inline NVGcolor of(unsigned v) { return nvgRGB((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff); }

inline NVGcolor body()   { return of(0xdedbd2); }   // the Super Famicom's own
inline NVGcolor bodyDim(){ return of(0xb9b6ad); }   // the darker grey of its lid
inline NVGcolor deck()   { return of(0xa8a59d); }
inline NVGcolor shade()  { return of(0x8d8981); }
inline NVGcolor edge()   { return of(0x55524c); }
inline NVGcolor white()  { return of(0xf2f0ea); }
inline NVGcolor paper()  { return of(0xf6f1de); }   // a Mario Paint window
inline NVGcolor ink()    { return of(0x2f2e2b); }
inline NVGcolor grey()   { return of(0x6f6b64); }
inline NVGcolor silver() { return of(0x9a968d); }
inline NVGcolor bezel()  { return of(0x232327); }   // around the television
inline NVGcolor screen() { return of(0x101014); }

inline NVGcolor purple() { return of(0x6b5ba8); }   // the buttons on the lid
inline NVGcolor lilac()  { return of(0xa99bdc); }

inline NVGcolor red()    { return of(0xc4453c); }   // A
inline NVGcolor yellow() { return of(0xdca41c); }   // B
inline NVGcolor green()  { return of(0x3f9a4a); }   // Y
inline NVGcolor blue()   { return of(0x2f6ec4); }   // X
inline NVGcolor violet() { return of(0x7a5fb8); }
} // namespace sfc

namespace panel {

struct Ink {
    NVGcontext* vg    = nullptr;
    int         font  = -1;     // unused: the letters are drawn, not typeset
    float       scale = 1.f;    // pixels per millimetre
};

// Cap heights, in millimetres. Below about 2.4 a pixel of the font stops
// being a whole screen pixel at Rack's own zoom and the letters break up.
constexpr float TitleSize   = 7.0f;
constexpr float SectionSize = 3.2f;
constexpr float LabelSize   = 2.5f;
constexpr float SmallSize   = 2.4f;
constexpr float TinySize    = 2.4f;

inline pixel::Pen pen(const Ink& k) { return pixel::Pen { k.vg, k.scale }; }

// Same shape of call as a nanovg text: y is the baseline, and the alignment
// flags are nanovg's, so that every call site reads the way it always did.
inline void text(const Ink& k, float x, float y, const char* s,
                 float size, int align, NVGcolor color)
{
    if(!k.vg || !s || !*s) return;

    const float w = pixel::textWidth(s, size);

    float left = x;
    if(align & NVG_ALIGN_CENTER) left = x - w / 2;
    if(align & NVG_ALIGN_RIGHT)  left = x - w;

    float top = y - size;
    if(align & NVG_ALIGN_MIDDLE) top = y - size / 2;
    if(align & NVG_ALIGN_TOP)    top = y;

    pixel::textAt(pen(k), left, top, s, size, color);
}

inline void rect(const Ink& k, float x, float y, float w, float h,
                 NVGcolor color, float radius = 1.f)
{
    if(!k.vg) return;

    nvgBeginPath(k.vg);
    nvgRoundedRect(k.vg, x * k.scale, y * k.scale, w * k.scale, h * k.scale, radius * k.scale);
    nvgFillColor(k.vg, color);
    nvgFill(k.vg);
}

// A window, raised out of the panel or sunk into it, with the two-tone edge
// every menu on the machine was drawn with.
inline void window(const Ink& k, float x, float y, float w, float h,
                   NVGcolor face, bool raised = false, float thick = 0.6f)
{
    pixel::bevel(pen(k), x, y, w, h, face, sfc::white(), sfc::shade(), raised, thick);
}

inline void background(const Ink& k, float w)
{
    if(!k.vg) return;

    nvgBeginPath(k.vg);
    nvgRect(k.vg, 0, 0, w * k.scale, layout::PanelH * k.scale);
    nvgFillColor(k.vg, sfc::body());
    nvgFill(k.vg);

    // The seam down each flank where the two halves of the case meet.
    const NVGcolor seam = sfc::of(0xc7c4bb);
    rect(k, 1.6f, 3.0f, 0.7f, layout::PanelH - 6.0f, seam, 0.35f);
    rect(k, w - 2.3f, 3.0f, 0.7f, layout::PanelH - 6.0f, seam, 0.35f);
}

// The face of a Super Famicom controller, as a maker's mark: a dark disc with
// the two rounded beds and four buttons in a diamond -- blue at the top, red
// to the right, yellow at the bottom, green to the left. Nothing else on that
// machine is as recognisable from across a room.
inline void mark(const Ink& k, float cx, float cy, float r = 5.0f, bool lit = true)
{
    if(!k.vg) return;

    const float s = k.scale;

    nvgBeginPath(k.vg);
    nvgCircle(k.vg, cx * s, cy * s, r * s);
    nvgFillColor(k.vg, sfc::of(0x6e6b64));
    nvgFill(k.vg);

    // the two beds the buttons sit in, on the controller's own diagonal
    nvgSave(k.vg);
    nvgTranslate(k.vg, cx * s, cy * s);
    nvgRotate(k.vg, -0.7853981f);

    for(int i = -1; i <= 1; i += 2)
    {
        nvgBeginPath(k.vg);
        nvgRoundedRect(k.vg, -r * 0.66f * s, (i * 0.30f - 0.16f) * r * s,
                       r * 1.32f * s, r * 0.32f * s, r * 0.16f * s);
        nvgFillColor(k.vg, sfc::of(0xc9c6be));
        nvgFill(k.vg);
    }

    nvgRestore(k.vg);

    const float d = r * 0.47f;
    const float br = r * 0.235f;
    const NVGcolor dead = sfc::of(0x8d8a83);
    const NVGcolor four[4] = {
        lit ? sfc::blue()   : dead, lit ? sfc::red()    : dead,
        lit ? sfc::yellow() : dead, lit ? sfc::green()  : dead };
    const float ox[4] = { 0.f,  1.f, 0.f, -1.f };
    const float oy[4] = { -1.f, 0.f, 1.f,  0.f };

    for(int i = 0; i < 4; ++i)
    {
        nvgBeginPath(k.vg);
        nvgCircle(k.vg, (cx + ox[i] * d) * s, (cy + oy[i] * d) * s, br * s);
        nvgFillColor(k.vg, four[i]);
        nvgFill(k.vg);
    }
}

// The lid of the machine: a darker deck across the top of a panel, with the
// ribs the real one has cut into it, and the name sitting on it.
inline void deck(const Ink& k, float x, float y, float w, float h)
{
    rect(k, x, y, w, h, sfc::deck(), 1.6f);
    rect(k, x + 0.5f, y + 0.5f, w - 1.0f, h - 1.4f, sfc::bodyDim(), 1.4f);

    for(float rx = x + w - 4.0f; rx > x + w - 16.0f; rx -= 2.4f)
        rect(k, rx, y + h * 0.30f, 1.0f, h * 0.42f, sfc::deck(), 0.3f);
}

inline void header(const Ink& k, float x, float baseline, const char* title,
                   NVGcolor tint, float size = TitleSize)
{
    text(k, x, baseline, title, size, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE, tint);
}

// Each module's mascot, instead of a sentence saying what the module is. A
// panel that has to be read is a panel nobody reads, and the Satellaview put
// a character on everything it had no room to explain.
inline void mascot(const Ink& k, float x, float y, int which, float size)
{
    static const NVGcolor palette[8] = {
        sfc::of(0x2f2e2b),   // 0 the outline
        sfc::of(0xdedbd2),   // 1 plastic
        sfc::of(0xa8a59d),   // 2 its shadow
        sfc::of(0x3a3a42),   // 3 a hole, or an eye
        sfc::of(0xc4453c),   // 4 red
        sfc::of(0x2f6ec4),   // 5 blue
        sfc::of(0x3f9a4a),   // 6 green
        sfc::of(0xdca41c),   // 7 yellow
    };

    pixel::spriteAt(pen(k), x, y, pixel::mascot(which), pixel::SpriteW,
                    pixel::SpriteW, size, palette, 8);
}

// ---------------------------------------------------------------------------
// The jacks, once, so that a socket and its label cannot disagree. The
// modules build their ports off these lists too.
// ---------------------------------------------------------------------------

struct Jack { const char* label; NVGcolor (*tint)(); };

// The gamepad's own order, which is also the order Button is declared in and
// the order InputId follows. Snes.cpp static_asserts the first.
inline const char* const* padLabels()
{
    static const char* labels[12] = {
        "UP", "DOWN", "LEFT", "RIGHT", "B", "A", "Y", "X", "L", "R", "SEL", "START",
    };
    return labels;
}

// Where each of the twelve sockets goes, in the shape of the machine's own
// controller. The module places its ports off this and nothing else.
struct PadSeat { float x, y; NVGcolor (*tint)(); };

inline const PadSeat* padSeats()
{
    namespace S = layout::snes;

    static const PadSeat seats[12] = {
        { S::PadCrossX,              S::PadCrossY - S::PadSpace, sfc::grey   },  // up
        { S::PadCrossX,              S::PadCrossY + S::PadSpace, sfc::grey   },  // down
        { S::PadCrossX - S::PadSpace, S::PadCrossY,              sfc::grey   },  // left
        { S::PadCrossX + S::PadSpace, S::PadCrossY,              sfc::grey   },  // right

        { S::FaceX,                  S::FaceY + S::FaceSpace,    sfc::yellow },  // B
        { S::FaceX + S::FaceSpace,   S::FaceY,                   sfc::red    },  // A
        { S::FaceX - S::FaceSpace,   S::FaceY,                   sfc::green  },  // Y
        { S::FaceX,                  S::FaceY - S::FaceSpace,    sfc::blue   },  // X

        { S::ShoulderLX,             S::ShoulderY,               sfc::grey   },  // L
        { S::ShoulderRX,             S::ShoulderY,               sfc::grey   },  // R
        { S::SelectX,                S::MiddleY,                 sfc::grey   },  // select
        { S::StartX,                 S::MiddleY,                 sfc::grey   },  // start
    };
    return seats;
}

// The six that are not buttons.
struct Jack2 { const char* label; float x, y; NVGcolor (*tint)(); };

inline const Jack2* snesUtility()
{
    namespace S = layout::snes;

    static const Jack2 jacks[6] = {
        { "PAD 2", S::UtilX1, S::UtilY1, sfc::purple },
        { "CLOCK", S::UtilX2, S::UtilY1, sfc::purple },
        { "RATE",  S::UtilX1, S::UtilY2, sfc::purple },
        { "RESET", S::UtilX2, S::UtilY2, sfc::purple },
        { "PORT",  S::UtilX1, S::UtilY3, sfc::violet },
        { "WRITE", S::UtilX2, S::UtilY3, sfc::violet },
    };
    return jacks;
}

// And everything that comes out: two rows of eight.
inline const Jack* snesOut()
{
    static const Jack jacks[16] = {
        { "MIX L", sfc::ink    }, { "MIX R", sfc::ink    },
        { "V1",    sfc::red    }, { "V2",    sfc::red    },
        { "V3",    sfc::red    }, { "V4",    sfc::red    },
        { "V5",    sfc::red    }, { "V6",    sfc::red    },
        { "V7",    sfc::red    }, { "V8",    sfc::red    },
        { "V/OCT", sfc::green  }, { "GATE",  sfc::green  },
        { "LEVEL", sfc::green  }, { "SRCN",  sfc::green  },
        { "ECHO L", sfc::blue  }, { "ECHO R", sfc::blue  },
    };
    return jacks;
}

inline const int* snesButtonIcons()
{
    static const int icons[layout::snes::ButtonCount] = {
        pixel::IconCart, pixel::IconPrev, pixel::IconNext,
        pixel::IconPlay, pixel::IconLoop, pixel::IconChip,
    };
    return icons;
}

inline const char* const* snesButtonNames()
{
    static const char* names[layout::snes::ButtonCount] =
        { "LOAD", "PREV", "NEXT", "RUN", "RESET", "RIP" };
    return names;
}

// ---------------------------------------------------------------------------
// The controller, drawn around its sockets
// ---------------------------------------------------------------------------
inline void controller(const Ink& k)
{
    namespace S = layout::snes;
    if(!k.vg) return;

    const float s = k.scale;

    // the two shoulders, along the top of the thing
    for(float x : { S::ShoulderLX, S::ShoulderRX })
        rect(k, x - 9.0f, S::ShoulderY - 5.4f, 18.0f, 10.8f, sfc::deck(), 5.4f);

    // The letter goes on the shoulder itself, the way it is moulded into the
    // real one -- there is nothing above these but the voice meters.
    text(k, S::ShoulderLX - 6.4f, S::ShoulderY + 1.2f, "L", SmallSize,
         NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, sfc::ink());
    text(k, S::ShoulderRX + 6.4f, S::ShoulderY + 1.2f, "R", SmallSize,
         NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, sfc::ink());

    // the cross, as one shape rather than four
    const float arm = S::PadSpace + 5.0f;
    const float wid = 11.2f;

    rect(k, S::PadCrossX - arm, S::PadCrossY - wid / 2, arm * 2, wid, sfc::of(0x63605a), 1.6f);
    rect(k, S::PadCrossX - wid / 2, S::PadCrossY - arm, wid, arm * 2, sfc::of(0x63605a), 1.6f);

    // the disc the four buttons sit on, and the two beds across it
    nvgBeginPath(k.vg);
    nvgCircle(k.vg, S::FaceX * s, S::FaceY * s, S::FaceDisc * s);
    nvgFillColor(k.vg, sfc::of(0x77746d));
    nvgFill(k.vg);

    nvgSave(k.vg);
    nvgTranslate(k.vg, S::FaceX * s, S::FaceY * s);
    nvgRotate(k.vg, -0.7853981f);

    for(int i = -1; i <= 1; i += 2)
    {
        nvgBeginPath(k.vg);
        // Short enough to stay inside the disc: the bed's far corner is
        // half its length and half its width from the centre, and that has
        // to come out under the radius or the light grey pokes through the
        // dark and the whole thing stops looking moulded.
        nvgRoundedRect(k.vg, -13.2f * s, (i * 7.6f - 4.3f) * s,
                       26.4f * s, 8.6f * s, 4.3f * s);
        nvgFillColor(k.vg, sfc::of(0xc9c6be));
        nvgFill(k.vg);
    }

    nvgRestore(k.vg);

    // The letters, outside the disc at the four corners the machine put them
    // on: X above and right, A to the right, B below and left, Y to the left.
    const float out = S::FaceDisc + 3.2f;

    text(k, S::FaceX + out * 0.72f, S::FaceY - out * 0.72f + 1.2f, "X",
         SmallSize, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, sfc::shade());
    text(k, S::FaceX + out, S::FaceY + 1.2f, "A",
         SmallSize, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, sfc::shade());
    text(k, S::FaceX - out * 0.72f, S::FaceY + out * 0.72f + 1.2f, "B",
         SmallSize, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, sfc::shade());
    text(k, S::FaceX - out, S::FaceY + 1.2f, "Y",
         SmallSize, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, sfc::shade());

    // select and start, on their own slant
    for(float x : { S::SelectX, S::StartX })
        rect(k, x - 5.6f, S::MiddleY - 5.2f, 11.2f, 10.4f, sfc::deck(), 5.2f);

    // Above the pills, not below: below is where the output row's labels
    // have to start, and there is one gap and two things that want it.
    text(k, S::SelectX, S::MiddleY - 7.0f, "SEL", TinySize,
         NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, sfc::shade());
    text(k, S::StartX, S::MiddleY - 7.0f, "START", TinySize,
         NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, sfc::shade());
}

// ---------------------------------------------------------------------------
// SNES
// ---------------------------------------------------------------------------
inline void paintSnes(const Ink& k)
{
    namespace S = layout::snes;

    background(k, S::PanelW);

    // The television: a dark bezel with the picture sunk into it.
    rect(k, S::ScreenX - S::Bezel, S::ScreenY - S::Bezel,
            S::ScreenW + S::Bezel * 2, S::ScreenH + S::Bezel * 2, sfc::bezel(), 1.6f);
    rect(k, S::ScreenX, S::ScreenY, S::ScreenW, S::ScreenH, sfc::screen(), 0.f);

    // The toolbar under it, the shape a Mario Paint screen has.
    window(k, S::ScreenX - S::Bezel, S::BarY, S::ScreenW + S::Bezel * 2, S::BarH,
           sfc::bodyDim(), true);

    // The cartridge's name goes in a window of its own, in the toolbar.
    window(k, S::PlateX, S::BarY + 1.2f, S::PlateW - 1.8f, S::BarH - 2.4f, sfc::paper());

    // --- the console body, to the right of the television ------------------
    deck(k, S::ColX - 1.0f, layout::DeckY, S::ColW + 2.0f, layout::DeckH);
    mascot(k, S::ColX + 1.0f, layout::MascotY, pixel::MascotConsole, layout::MascotSize);
    header(k, S::ColX + 13.5f, layout::NameY, "RaSNESk", sfc::ink(), 7.4f);
    // Drawn dark: a widget lights the four buttons when a real pad is
    // plugged in, which is the whole of what this panel says about that.
    mark(k, S::ColR - 6.0f, 7.8f, 5.0f, false);

    // the eight voices
    window(k, S::ColX, S::MeterY - 1.4f, S::MeterW, S::MeterH + 2.4f, sfc::paper());
    // No heading over the eight rows: the window says V1 to V8 down its own
    // left edge, and a title here would sit under a word eight millimetres
    // tall and read as part of it.
    text(k, S::KnobNameX, S::RateY, "RATE", LabelSize,
         NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, sfc::ink());
    text(k, S::KnobNameX, S::VolY, "VOLUME", LabelSize,
         NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, sfc::ink());
    controller(k);

    for(int i = 0; i < 6; ++i)
        text(k, snesUtility()[i].x, snesUtility()[i].y - S::LabelDrop,
             snesUtility()[i].label, LabelSize,
             NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, snesUtility()[i].tint());

    for(int i = 0; i < 16; ++i)
    {
        const float y = (i < S::OutCols ? S::OutRow1 : S::OutRow2) - S::LabelDrop;
        text(k, S::outX(i % S::OutCols), y, snesOut()[i].label, LabelSize,
             NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, snesOut()[i].tint());
    }
}

// ---------------------------------------------------------------------------
// SAMPLER
// ---------------------------------------------------------------------------
inline const int* samplerButtonIcons()
{
    static const int icons[4] = {
        pixel::IconDisk, pixel::IconArrowDown, pixel::IconPrev, pixel::IconNext };
    return icons;
}

inline const char* const* samplerKnobs1()
{
    static const char* labels[6] = { "SAMPLE", "TUNE", "FINE", "PAN", "LEVEL", "ECHO" };
    return labels;
}

inline const char* const* samplerKnobs2()
{
    static const char* labels[6] = { "ATTACK", "DECAY", "SUSTN", "RELSE", "FEEDBK", "TIME" };
    return labels;
}

inline const Jack* samplerIn()
{
    static const Jack jacks[6] = {
        { "V/OCT",  sfc::green  }, { "GATE",   sfc::green  },
        { "SAMPLE", sfc::purple }, { "LEVEL",  sfc::purple },
        { "PAN",    sfc::purple }, { "TUNE",   sfc::purple },
    };
    return jacks;
}

inline const Jack* samplerOut()
{
    static const Jack jacks[6] = {
        { "MIX L",  sfc::ink  }, { "MIX R",  sfc::ink  },
        { "VOICES", sfc::red  }, { "DRY",    sfc::red  },
        { "ECHO L", sfc::blue }, { "ECHO R", sfc::blue },
    };
    return jacks;
}

inline void paintSampler(const Ink& k)
{
    namespace S = layout::sampler;

    background(k, S::PanelW);

    deck(k, S::Left - 1.0f, layout::DeckY, S::PanelW - 2 * S::Left + 2.0f, layout::DeckH);
    mascot(k, S::Left + 0.5f, layout::MascotY, pixel::MascotSampler, layout::MascotSize);
    header(k, S::Left + 13.0f, layout::NameY, "SAMPLER", sfc::ink(), 5.4f);
    mark(k, S::PanelW - S::Left - 6.0f, 7.8f, 5.0f);

    // The instrument, in a window of its own.
    window(k, S::Left, S::ScopeY, S::Right - S::Left, S::ScopeH, sfc::paper());

    for(int i = 0; i < 6; ++i)
    {
        text(k, S::colX(i), S::KnobRow1 + 9.5f, samplerKnobs1()[i], SmallSize,
             NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, sfc::ink());
        text(k, S::colX(i), S::KnobRow2 + 9.0f, samplerKnobs2()[i], SmallSize,
             NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, sfc::ink());
    }

    for(int i = 0; i < 6; ++i)
    {
        text(k, S::colX(i), S::InRow - S::LabelDrop, samplerIn()[i].label, LabelSize,
             NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, samplerIn()[i].tint());
        text(k, S::colX(i), S::OutRow - S::LabelDrop, samplerOut()[i].label, LabelSize,
             NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, samplerOut()[i].tint());
    }
}

// ---------------------------------------------------------------------------
// APU
// ---------------------------------------------------------------------------
inline const int* apuButtonIcons()
{
    static const int icons[3] = { pixel::IconDisk, pixel::IconArrowDown, pixel::IconStop };
    return icons;
}

inline const Jack* apuOut()
{
    static const Jack jacks[5] = {
        { "MIX L",  sfc::ink   }, { "MIX R", sfc::ink   },
        { "VOICES", sfc::red   }, { "V/OCT", sfc::green },
        { "GATE",   sfc::green },
    };
    return jacks;
}

inline void paintApu(const Ink& k)
{
    namespace A = layout::apu;

    background(k, A::PanelW);

    deck(k, A::Left - 1.0f, layout::DeckY, A::PanelW - 2 * A::Left + 2.0f, layout::DeckH);
    mascot(k, A::Left + 0.5f, layout::MascotY, pixel::MascotChip, layout::MascotSize);
    header(k, A::Left + 13.0f, layout::NameY, "APU", sfc::ink(), 5.4f);
    mark(k, A::PanelW - A::Left - 6.0f, 7.8f, 5.0f);

    window(k, A::Left, A::PlateY, A::Right - A::Left, A::PlateH, sfc::paper());

    text(k, A::Left, A::PortKnob - 7.0f, "PORTS", SmallSize,
         NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE, sfc::shade());

    for(int i = 0; i < 4; ++i)
    {
        char buf[8];
        std::snprintf(buf, sizeof buf, "%d", i);
        text(k, A::colX(i), A::PortLabel, buf, SectionSize,
             NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, sfc::red());
    }

    text(k, A::colX(4), A::PortKnob - 7.0f, "SONG", SmallSize,
         NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, sfc::yellow());
    text(k, A::colX(4), A::PortLabel, "WRITE", SmallSize,
         NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, sfc::red());

    rect(k, A::Left, A::RuleY, A::Right - A::Left, 0.5f, sfc::shade(), 0.25f);

    text(k, A::colX(0), A::WriteY - 7.5f, "RATE", SmallSize,
         NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, sfc::ink());
    text(k, A::colX(1), A::WriteY - 7.5f, "VOLUME", SmallSize,
         NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, sfc::ink());
    mark(k, (A::colX(3) + A::colX(4)) / 2, A::WriteY, 6.4f);

    for(int i = 0; i < 5; ++i)
        text(k, A::colX(i), A::OutRow - A::LabelDrop, apuOut()[i].label, LabelSize,
             NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE, apuOut()[i].tint());
}

// ---------------------------------------------------------------------------
// BENDER
// ---------------------------------------------------------------------------
struct BendSection { const char* name; const char* cue; NVGcolor (*tint)(); };

inline const BendSection* bendSections()
{
    static const BendSection sections[layout::bender::SectionCount] = {
        { "PALETTE",   "CV",     sfc::red    },
        { "TRANSPOSE", "1V/OCT", sfc::green  },
        { "WARP",      "CV",     sfc::blue   },
        { "ECHO",      "CV",     sfc::violet },
        { "GLITCH",    "CV",     sfc::yellow },
        { "FREEZE",    "GATE",   sfc::purple },
    };
    return sections;
}

inline void paintBender(const Ink& k)
{
    namespace B = layout::bender;

    background(k, B::PanelW);

    deck(k, 2.0f, layout::DeckY, B::PanelW - 4.0f, layout::DeckH);
    mascot(k, 3.0f, layout::MascotY, pixel::MascotGlitch, layout::MascotSize);
    header(k, 15.0f, layout::NameY, "BENDER", sfc::ink(), 4.8f);

    for(int i = 0; i < B::SectionCount; ++i)
    {
        const BendSection& s = bendSections()[i];

        rect(k, 2.5f, B::ruleY(i), B::PanelW - 5.f, 0.5f, sfc::shade(), 0.25f);

        text(k, 3.5f, B::ruleY(i) + 4.6f, s.name, SectionSize,
             NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE, s.tint());

        // The two sections without a chooser say what their socket wants; the
        // others have no room, because the chooser is where that would go.
        if(i == 1 || i == 3 || i == 5)
            text(k, B::PanelW - 3.5f, B::ruleY(i) + 4.4f, s.cue, TinySize,
                 NVG_ALIGN_RIGHT | NVG_ALIGN_BASELINE, sfc::shade());
    }

    mark(k, B::PanelW - 8.5f, 7.8f, 4.6f);
}

}} // namespace racksnes::panel
