// ===========================================================================
// The panels' own pixels.
//
// A Super Nintendo drew everything out of an 8x8 grid, and a panel that wants
// to look like one cannot borrow a typeface off the desktop. So: a 5x7 font,
// authored here as pictures rather than as hex, a sheet of little icons in
// the same grid, and the two-tone bevel that every menu on that machine was
// drawn with.
//
// Nothing here knows about Rack -- it is nanovg and millimetres, like the
// rest of PanelDraw -- so the mockup draws exactly what the module draws.
//
// One pixel of this font is `size / 7` millimetres. Below about 2.4 mm of
// cap height a pixel stops being a whole screen pixel at Rack's own zoom and
// the letters break up, which is the one rule the layout has to respect.
// ===========================================================================
#pragma once

#include <nanovg.h>

#include <cstring>

namespace racksnes {
namespace pixel {

// ---------------------------------------------------------------------------
// The font: ASCII 32 to 95, five across and seven down. Lower case is drawn
// as capitals, which is what nearly every game on the machine did.
// ---------------------------------------------------------------------------
constexpr int GlyphW = 5;
constexpr int GlyphH = 7;
constexpr int Advance = 6;      // one column of air between letters

// Lower case is drawn as a small capital rather than as a letter of its own:
// a 5x7 cell has no room for a descender, and a name like RaSNESk wants the
// difference to show. `smallCaps()` says how tall to draw it.
inline bool isLower(char c) { return c >= 'a' && c <= 'z'; }
constexpr float SmallCapRows = 5.f;      // out of the seven

inline const char* const* glyph(char c)
{
    static const char* const Font[64][GlyphH] = {
    /* space */ {".....", ".....", ".....", ".....", ".....", ".....", "....."},
    /* !  */ {"..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#.."},
    /* "  */ {".#.#.", ".#.#.", ".....", ".....", ".....", ".....", "....."},
    /* #  */ {".#.#.", ".#.#.", "#####", ".#.#.", "#####", ".#.#.", ".#.#."},
    /* $  */ {"..#..", ".####", "#.#..", ".###.", "..#.#", "####.", "..#.."},
    /* %  */ {"##..#", "##..#", "...#.", "..#..", ".#...", "#..##", "#..##"},
    /* &  */ {".##..", "#..#.", "#.#..", ".#...", "#.#.#", "#..#.", ".##.#"},
    /* '  */ {"..#..", "..#..", ".....", ".....", ".....", ".....", "....."},
    /* (  */ {"...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#."},
    /* )  */ {".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#..."},
    /* *  */ {".....", "#.#.#", ".###.", "#####", ".###.", "#.#.#", "....."},
    /* +  */ {".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."},
    /* ,  */ {".....", ".....", ".....", ".....", ".....", "..#..", ".#..."},
    /* -  */ {".....", ".....", ".....", "#####", ".....", ".....", "....."},
    /* .  */ {".....", ".....", ".....", ".....", ".....", ".....", "..#.."},
    /* /  */ {"....#", "....#", "...#.", "..#..", ".#...", "#....", "#...."},
    /* 0  */ {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."},
    /* 1  */ {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."},
    /* 2  */ {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"},
    /* 3  */ {"#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###."},
    /* 4  */ {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."},
    /* 5  */ {"#####", "#....", "####.", "....#", "....#", "#...#", ".###."},
    /* 6  */ {"..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."},
    /* 7  */ {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."},
    /* 8  */ {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."},
    /* 9  */ {".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."},
    /* :  */ {".....", "..#..", ".....", ".....", "..#..", ".....", "....."},
    /* ;  */ {".....", "..#..", ".....", ".....", "..#..", "..#..", ".#..."},
    /* <  */ {"...#.", "..#..", ".#...", "#....", ".#...", "..#..", "...#."},
    /* =  */ {".....", ".....", "#####", ".....", "#####", ".....", "....."},
    /* >  */ {".#...", "..#..", "...#.", "....#", "...#.", "..#..", ".#..."},
    /* ?  */ {".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.."},
    /* @  */ {".###.", "#...#", "....#", "#.###", "#.#.#", "#.###", ".####"},
    /* A  */ {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"},
    /* B  */ {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."},
    /* C  */ {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."},
    /* D  */ {"###..", "#..#.", "#...#", "#...#", "#...#", "#..#.", "###.."},
    /* E  */ {"#####", "#....", "#....", "####.", "#....", "#....", "#####"},
    /* F  */ {"#####", "#....", "#....", "####.", "#....", "#....", "#...."},
    /* G  */ {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".###."},
    /* H  */ {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"},
    /* I  */ {".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."},
    /* J  */ {"....#", "....#", "....#", "....#", "#...#", "#...#", ".###."},
    /* K  */ {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"},
    /* L  */ {"#....", "#....", "#....", "#....", "#....", "#....", "#####"},
    /* M  */ {"#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"},
    /* N  */ {"#...#", "##..#", "#.#.#", "#.#.#", "#..##", "#...#", "#...#"},
    /* O  */ {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."},
    /* P  */ {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."},
    /* Q  */ {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"},
    /* R  */ {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"},
    /* S  */ {".###.", "#...#", "#....", ".###.", "....#", "#...#", ".###."},
    /* T  */ {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."},
    /* U  */ {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."},
    /* V  */ {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."},
    /* W  */ {"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#"},
    /* X  */ {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"},
    /* Y  */ {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."},
    /* Z  */ {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"},
    /* [  */ {"..###", "..#..", "..#..", "..#..", "..#..", "..#..", "..###"},
    /* \  */ {"#....", "#....", ".#...", "..#..", "...#.", "....#", "....#"},
    /* ]  */ {"###..", "..#..", "..#..", "..#..", "..#..", "..#..", "###.."},
    /* ^  */ {"..#..", ".#.#.", "#...#", ".....", ".....", ".....", "....."},
    /* _  */ {".....", ".....", ".....", ".....", ".....", ".....", "#####"},
    };

    int i = (unsigned char)c;
    if(i >= 'a' && i <= 'z') i -= 32;      // capitals, the way the machine did
    if(i < 32 || i > 95) i = 32;           // anything else is a space
    return Font[i - 32];
}

// ---------------------------------------------------------------------------
// Little pictures, nine by nine, in the same grid.
//
// Two colours and a hole: '#' is the ink, 'o' the second colour, '.' nothing.
// A Super Nintendo's menus were drawn like this and so is a Mario Paint
// toolbar, which is where the idea of putting a picture on a button instead of
// a word comes from.
// ---------------------------------------------------------------------------
constexpr int IconW = 9;

enum Icon {
    IconCart = 0, IconPrev, IconNext, IconPlay, IconStop, IconChip,
    IconDisk, IconArrowDown, IconNote, IconWave, IconLoop, IconBrush,
    IconCount
};

inline const char* const* icon(int which)
{
    static const char* const Sheet[IconCount][IconW] = {
    // a cartridge, label and all
    { ".#######.", ".#ooooo#.", ".#ooooo#.", ".#######.", ".#.....#.",
      ".##...##.", ".#.#.#.#.", ".#.#.#.#.", ".#######." },
    // back
    { "....##...", "...###...", "..####...", ".#####...", "..####...",
      "...###...", "....##...", ".........", "........." },
    // on
    { "...##....", "...###...", "...####..", "...#####.", "...####..",
      "...###...", "...##....", ".........", "........." },
    // play
    { "..#......", "..##.....", "..#o#....", "..#oo#...", "..#ooo#..",
      "..#oo#...", "..#o#....", "..##.....", "..#......" },
    // stop
    { ".........", ".#######.", ".#ooooo#.", ".#ooooo#.", ".#ooooo#.",
      ".#ooooo#.", ".#ooooo#.", ".#######.", "........." },
    // a chip with its legs out: the sound chip, and what RIP takes
    { "..#...#..", ".#######.", "##ooooo##", ".#ooooo#.", "##ooooo##",
      ".#ooooo#.", "##ooooo##", ".#######.", "..#...#.." },
    // a disk
    { ".#######.", ".#ooooo#.", ".#o...o#.", ".#o...o#.", ".#ooooo#.",
      ".##...##.", ".#.###.#.", ".#.###.#.", ".#######." },
    // take: an arrow into a tray
    { "....#....", "....#....", "....#....", ".#..#..#.", ".##.#.##.",
      "..#####..", "...###...", ".#######.", ".#######." },
    // a note
    { "....###..", "....#oo#.", "....###..", "....#....", "....#....",
      "..###....", ".####....", ".####....", "..###...." },
    // a waveform
    { ".........", "...#.....", "..#.#..#.", ".#...#.#.", "#.....#..",
      ".........", ".........", ".........", "........." },
    // a loop
    { ".........", "..#####..", ".#.....#.", "#.......#", "#.......#",
      "#.......#", ".#.....#.", "..##.##..", "...#.#..." },
    // a brush, for Mario Paint's sake
    { ".......##", "......##.", ".....##..", "....##...", "...###...",
      "..#####..", "..#ooo#..", "..#ooo#..", "..#####.." },
    };

    if(which < 0 || which >= IconCount) which = 0;
    return Sheet[which];
}

// ---------------------------------------------------------------------------
// The mascots
//
// Sixteen by sixteen, in the handful of colours a Super Famicom tile had.
// Every module gets one instead of a sentence explaining itself: the
// Satellaview put a character on everything it had no room to describe, and
// a panel that has to be read is a panel nobody reads.
//
// '.' is nothing; the digits index the palette below.
// ---------------------------------------------------------------------------
constexpr int SpriteW = 16;

enum Mascot { MascotConsole = 0, MascotSampler, MascotChip, MascotGlitch, MascotCount };

// 0 outline  1 body  2 shade  3 white  4 red  5 blue  6 green  7 yellow
inline const char* const* mascot(int which)
{
    static const char* const Sheet[MascotCount][SpriteW] = {

    // A console with a cartridge in its head and its four colours on its chest
    { "................",
      ".....000000.....",
      ".....022220.....",
      ".....023320.....",
      ".....022220.....",
      "..000000000000..",
      "..011111111110..",
      "..013311113310..",
      "..013311113310..",
      "..011111111110..",
      "..011100001110..",
      "..011111111110..",
      "..014567111110..",
      "..022222222220..",
      "..000000000000..",
      "................" },

    // A note with a face, which is what a sound is once it has a name
    { ".......000000...",
      ".......077770...",
      ".......0770770..",
      ".......0770.770.",
      ".......0770..00.",
      ".......0770.....",
      ".......0770.....",
      ".......0770.....",
      ".......0770.....",
      "..000000770.....",
      ".0777777770.....",
      "07707707770.....",
      "07777777770.....",
      "07700077770.....",
      ".0777777770.....",
      "..00000000......" },

    // A chip with its legs out and its eyes lit
    { "................",
      "...0..0..0..0...",
      "...0..0..0..0...",
      ".00000000000000.",
      ".01111111111110.",
      "001333110133110.",
      ".01300110130010.",
      ".01333110133110.",
      ".01111111111110.",
      ".01144444441110.",
      ".01111111111110.",
      ".00000000000000.",
      "...0..0..0..0...",
      "...0..0..0..0...",
      "................",
      "................" },

    // Something that got into the machine and is enjoying itself
    { "................",
      "......0000......",
      "....00555500....",
      "...0555555550...",
      "..055555555550..",
      "..051155115550..",
      "..051155115550..",
      "..055555555550..",
      "..055444455550..",
      "..055555555550..",
      "..055555555550..",
      "..055555555550..",
      "..050550550550..",
      "..00.00.00.00...",
      "................",
      "................" },
    };

    if(which < 0 || which >= MascotCount) which = 0;
    return Sheet[which];
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

struct Pen {
    NVGcontext* vg = nullptr;
    float scale = 1.f;          // pixels per millimetre
};

// Every run of set pixels in a row becomes one rectangle, and the whole
// string becomes one path: a label is a few dozen rectangles, not a few
// hundred, and nanovg fills it in one go.
inline void runsOf(NVGcontext* vg, const char* const* rows, int cols, int height,
                   float x, float y, float px, char which)
{
    for(int r = 0; r < height; ++r)
    {
        int c = 0;
        while(c < cols)
        {
            if(rows[r][c] != which) { ++c; continue; }

            int start = c;
            while(c < cols && rows[r][c] == which) ++c;

            nvgRect(vg, x + start * px, y + r * px, (c - start) * px, px);
        }
    }
}

inline float textWidth(const char* s, float size)
{
    if(!s || !*s) return 0.f;

    const float px = size / GlyphH;
    float w = 0.f;

    for(const char* c = s; *c; ++c)
    {
        const float cell = isLower(*c) ? px * SmallCapRows / GlyphH : px;
        w += Advance * cell;
    }

    return w - px;
}

// x is the left edge, y the top of the cap height. Alignment is done by the
// caller passing the left edge it wants, which the helpers below work out.
inline void textAt(const Pen& pen, float x, float y, const char* s,
                   float size, NVGcolor color)
{
    if(!pen.vg || !s || !*s) return;

    const float px = size / GlyphH;

    nvgBeginPath(pen.vg);

    float at = x;
    for(const char* c = s; *c; ++c)
    {
        // A small capital is the same drawing at five sevenths, sitting on
        // the same baseline.
        const float cell = isLower(*c) ? px * SmallCapRows / GlyphH : px;
        const float drop = (GlyphH * px) - (GlyphH * cell);

        runsOf(pen.vg, glyph(*c), GlyphW, GlyphH, at * pen.scale, (y + drop) * pen.scale,
               cell * pen.scale, '#');
        at += Advance * cell;
    }

    nvgFillColor(pen.vg, color);
    nvgFill(pen.vg);
}

// A sprite in as many colours as it asks for: one pass of rectangles per
// colour, so a sixteen-square mascot is eight paths rather than 256.
inline void spriteAt(const Pen& pen, float x, float y, const char* const* rows,
                     int cols, int height, float size,
                     const NVGcolor* palette, int colours)
{
    if(!pen.vg) return;

    const float px = size / height;

    for(int c = 0; c < colours; ++c)
    {
        nvgBeginPath(pen.vg);
        runsOf(pen.vg, rows, cols, height, x * pen.scale, y * pen.scale,
               px * pen.scale, (char)('0' + c));
        nvgFillColor(pen.vg, palette[c]);
        nvgFill(pen.vg);
    }
}

inline void iconAt(const Pen& pen, float x, float y, int which, float size,
                   NVGcolor ink, NVGcolor fill)
{
    if(!pen.vg) return;

    const float px = size / IconW;
    const char* const* rows = icon(which);

    nvgBeginPath(pen.vg);
    runsOf(pen.vg, rows, IconW, IconW, x * pen.scale, y * pen.scale, px * pen.scale, 'o');
    nvgFillColor(pen.vg, fill);
    nvgFill(pen.vg);

    nvgBeginPath(pen.vg);
    runsOf(pen.vg, rows, IconW, IconW, x * pen.scale, y * pen.scale, px * pen.scale, '#');
    nvgFillColor(pen.vg, ink);
    nvgFill(pen.vg);
}

// The two-tone bevel every menu on the machine was drawn with: a light edge
// where the light comes from and a dark one opposite. `raised` is a button,
// `!raised` is a well for something to sit in.
inline void bevel(const Pen& pen, float x, float y, float w, float h,
                  NVGcolor face, NVGcolor light, NVGcolor dark,
                  bool raised = true, float thick = 0.5f)
{
    if(!pen.vg) return;

    const float s = pen.scale;

    nvgBeginPath(pen.vg);
    nvgRect(pen.vg, x * s, y * s, w * s, h * s);
    nvgFillColor(pen.vg, raised ? dark : light);
    nvgFill(pen.vg);

    nvgBeginPath(pen.vg);
    nvgRect(pen.vg, x * s, y * s, (w - thick) * s, (h - thick) * s);
    nvgFillColor(pen.vg, raised ? light : dark);
    nvgFill(pen.vg);

    nvgBeginPath(pen.vg);
    nvgRect(pen.vg, (x + thick) * s, (y + thick) * s,
            (w - thick * 2) * s, (h - thick * 2) * s);
    nvgFillColor(pen.vg, face);
    nvgFill(pen.vg);
}

}} // namespace racksnes::pixel
