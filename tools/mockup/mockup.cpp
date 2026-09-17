// ===========================================================================
// Renders every panel to a PNG, with no window, no display server and no Rack.
//
//     ./build.sh mockup     ->  build/mockup/snes.png, sampler.png, ...
//
// Reading coordinates does not tell you that a label has landed on a socket or
// that two knobs are a millimetre apart. This draws the very same
// panel::paint* the modules draw, then puts every knob, socket and button
// on top at its true diameter, so an overlap is seen rather than deduced.
//
// NanoVG and GLEW both come out of libRack, which already contains them; the
// OpenGL context comes out of EGL.
// ===========================================================================
#include <EGL/egl.h>
#include <GL/glew.h>

#include <nanovg.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include <stb_image.h>
#include <stb_image_write.h>
#pragma GCC diagnostic pop

#include "../../src/PanelDraw.hpp"
#include "../../src/snes/SnesCore.hpp"
#include "../../src/Pixel.hpp"

// Declared rather than included: nanovg_gl.h wants a GL loader set up in a
// particular order, and these two are all that is needed out of it.
extern "C" NVGcontext* nvgCreateGL2(int flags);
extern "C" void nvgDeleteGL2(NVGcontext* ctx);
#define NVG_ANTIALIAS 1
#define NVG_STENCIL_STROKES 2

using namespace racksnes;

static const float Scale = 5.f;     // pixels per millimetre, for a readable picture

// The diameters Rack really draws its components at, in millimetres. A socket
// that fits on paper and not on the panel is the whole thing this catches.
static const float JackD    = 23.7f / 2.953f;
static const float KnobD    = 30.0f / 2.953f;
static const float BigKnobD = 36.0f / 2.953f;
static const float SmallKnobD = 24.0f / 2.953f;
static const float TrimD    = 17.5f / 2.953f;
static const float ButtonD  = 17.5f / 2.953f;
static const float LightD    = 8.0f / 2.953f;

// ---------------------------------------------------------------------------

struct Headless {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;

    bool start(int w, int h)
    {
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display == EGL_NO_DISPLAY) { std::fprintf(stderr, "no EGL display\n"); return false; }
        if (!eglInitialize(display, NULL, NULL)) { std::fprintf(stderr, "eglInitialize failed\n"); return false; }

        // Desktop GL, not GLES: the nanovg inside libRack is the GL2 backend.
        if (!eglBindAPI(EGL_OPENGL_API)) { std::fprintf(stderr, "no desktop GL through EGL\n"); return false; }

        const EGLint attribs[] = {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 24,
            // NanoVG strokes and clips with the stencil buffer; without one
            // the rounded corners come out as solid blocks.
            EGL_STENCIL_SIZE, 8,
            EGL_NONE,
        };

        EGLConfig config;
        EGLint count = 0;
        if (!eglChooseConfig(display, attribs, &config, 1, &count) || count < 1)
        { std::fprintf(stderr, "no usable EGL config\n"); return false; }

        const EGLint surfaceAttribs[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
        surface = eglCreatePbufferSurface(display, config, surfaceAttribs);
        if (surface == EGL_NO_SURFACE) { std::fprintf(stderr, "no %dx%d pbuffer\n", w, h); return false; }

        context = eglCreateContext(display, config, EGL_NO_CONTEXT, NULL);
        if (context == EGL_NO_CONTEXT) { std::fprintf(stderr, "no GL context\n"); return false; }
        if (!eglMakeCurrent(display, surface, surface, context))
        { std::fprintf(stderr, "eglMakeCurrent failed\n"); return false; }

        glewExperimental = GL_TRUE;
        GLenum status = glewInit();

        // GLEW insists on probing GLX even when the context came from EGL, and
        // says NO_GLX_DISPLAY when there is no X display to probe. Everything
        // it actually loaded is fine; only its GLX afterthought failed.
        if (status != GLEW_OK && status != GLEW_ERROR_NO_GLX_DISPLAY)
        { std::fprintf(stderr, "glewInit failed: %s\n", glewGetErrorString(status)); return false; }

        glGetError();   // GLEW's own probing leaves one behind
        return true;
    }

    void stop()
    {
        if (display == EGL_NO_DISPLAY) return;
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
        eglTerminate(display);
    }
};

static int loadFont(NVGcontext* vg)
{
    // The very font the panel uses inside Rack, then the usual fallbacks.
    const char* candidates[] = {
        "/home/vironlap/Documents/Software/Rack2Free/res/fonts/ShareTechMono-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    };

    if(const char* env = std::getenv("MOCKUP_FONT"))
    {
        int f = nvgCreateFont(vg, "ui", env);
        if(f >= 0) return f;
    }

    for(const char* path : candidates)
    {
        int f = nvgCreateFont(vg, "ui", path);
        if(f >= 0) return f;
    }

    std::fprintf(stderr, "warning: no font found, the mockup will have no labels\n");
    return -1;
}

// ---------------------------------------------------------------------------

static void outline(NVGcontext* vg, float x, float y, float w, float h, NVGcolor c)
{
    nvgBeginPath(vg);
    nvgRect(vg, x * Scale, y * Scale, w * Scale, h * Scale);
    nvgStrokeColor(vg, c);
    nvgStrokeWidth(vg, 1.f);
    nvgStroke(vg);
}

// ---------------------------------------------------------------------------

static void bevelButton(NVGcontext* vg, float mmX, float mmY, float mm,
                        int icon, NVGcolor tint)
{
    pixel::Pen pen { vg, Scale };
    pixel::bevel(pen, mmX - mm / 2, mmY - mm / 2, mm, mm,
                 sfc::paper(), sfc::white(), sfc::shade(), true, 0.55f);
    pixel::iconAt(pen, mmX - mm / 2 + mm * 0.11f, mmY - mm / 2 + mm * 0.11f,
                  icon, mm * 0.78f, sfc::ink(), tint);
}

static void knob(NVGcontext* vg, float mmX, float mmY, float mm)
{
    const float x = mmX * Scale, y = mmY * Scale, r = mm * 0.5f * Scale;

    nvgBeginPath(vg); nvgCircle(vg, x, y, r);
    nvgFillColor(vg, sfc::shade()); nvgFill(vg);

    nvgBeginPath(vg); nvgCircle(vg, x - 0.2f * Scale, y - 0.2f * Scale, r - 0.3f * Scale);
    nvgFillColor(vg, sfc::white()); nvgFill(vg);

    nvgBeginPath(vg); nvgCircle(vg, x, y, r - 0.55f * Scale);
    nvgFillColor(vg, sfc::bodyDim()); nvgFill(vg);

    nvgBeginPath(vg);
    nvgRect(vg, x - 0.45f * Scale, y - r + 0.9f * Scale, 0.9f * Scale, r * 0.55f);
    nvgFillColor(vg, sfc::ink()); nvgFill(vg);
}

static void port(NVGcontext* vg, float mmX, float mmY, NVGcolor rim = sfc::bodyDim())
{
    const float x = mmX * Scale, y = mmY * Scale, r = 4.1f * Scale;

    nvgBeginPath(vg); nvgCircle(vg, x, y, r);
    nvgFillColor(vg, sfc::white()); nvgFill(vg);
    nvgBeginPath(vg); nvgCircle(vg, x, y, r - 0.25f * Scale);
    nvgFillColor(vg, sfc::shade()); nvgFill(vg);
    nvgBeginPath(vg); nvgCircle(vg, x - 0.15f * Scale, y - 0.15f * Scale, r - 0.35f * Scale);
    nvgFillColor(vg, rim); nvgFill(vg);
    nvgBeginPath(vg); nvgCircle(vg, x, y, r * 0.52f);
    nvgFillColor(vg, sfc::bezel()); nvgFill(vg);
    nvgBeginPath(vg); nvgCircle(vg, x, y, r * 0.34f);
    nvgFillColor(vg, nvgRGB(0x0c, 0x0c, 0x0e)); nvgFill(vg);
}

static void snesScrew(NVGcontext* vg, float mmX, float mmY)
{
    const float d = 15.f / 2.953f;
    const float x = (mmX + d / 2) * Scale, y = (mmY + d / 2) * Scale, r = d * 0.31f * Scale;

    nvgBeginPath(vg); nvgCircle(vg, x, y, r);
    nvgFillColor(vg, sfc::shade()); nvgFill(vg);
    nvgBeginPath(vg); nvgCircle(vg, x, y, r * 0.84f);
    nvgFillColor(vg, sfc::bodyDim()); nvgFill(vg);
    nvgBeginPath(vg); nvgRect(vg, x - r * 0.7f, y - 0.22f * Scale, r * 1.4f, 0.44f * Scale);
    nvgFillColor(vg, sfc::shade()); nvgFill(vg);
}

static void screws(NVGcontext* vg, float panelW, bool four)
{
    const float d = 15.f / 2.953f;
    snesScrew(vg, d, 0.f);
    if(four) snesScrew(vg, panelW - 2.f * d, 0.f);
    if(four) snesScrew(vg, d, layout::PanelH - d);
    snesScrew(vg, panelW - 2.f * d, layout::PanelH - d);
}

// ---------------------------------------------------------------------------

static void drawSnes(NVGcontext* vg, int font, int screenImage)
{
    namespace S = layout::snes;

    panel::Ink ink { vg, font, Scale };
    pixel::Pen pen { vg, Scale };
    panel::paintSnes(ink);

    // The real thing the console's own test left behind, cropped to 224 rows
    // the way the module crops it.
    if(screenImage >= 0)
    {
        const float h  = S::ScreenH * (float)ScreenH / (float)ScreenVisible;
        const float oy = S::ScreenY - h * (float)ScreenCrop / (float)ScreenH;

        NVGpaint paint = nvgImagePattern(vg, S::ScreenX * Scale, oy * Scale,
                                         S::ScreenW * Scale, h * Scale, 0, screenImage, 1.f);
        nvgSave(vg);
        nvgScissor(vg, S::ScreenX * Scale, S::ScreenY * Scale,
                   S::ScreenW * Scale, S::ScreenH * Scale);
        nvgBeginPath(vg);
        nvgRect(vg, S::ScreenX * Scale, S::ScreenY * Scale,
                S::ScreenW * Scale, S::ScreenH * Scale);
        nvgFillPaint(vg, paint);
        nvgFill(vg);
        nvgRestore(vg);
    }

    // the nameplate's live text, which the module draws over the paint
    pixel::textAt(pen, S::PlateX + 2.6f, S::BarY + 2.7f, "MARIOPAINT", 2.6f, sfc::ink());
    {
        const char* state = "NTSC RUN";
        const float w = pixel::textWidth(state, 2.4f);
        pixel::textAt(pen, S::PlateX + S::PlateW - 3.2f - w, S::BarY + 2.9f, state,
                      2.4f, sfc::shade());
    }

    // and the voice monitor's, so the column is not judged empty
    for(int i = 0; i < 8; ++i)
    {
        const float y = S::meterY(i);
        char buf[8];
        std::snprintf(buf, sizeof buf, "V%d", i + 1);
        pixel::textAt(pen, S::ColX + 1.5f, y + S::MeterPitch / 2 - 1.2f, buf, 2.4f, sfc::shade());

        const float level = (i * 37 % 100) / 100.f;
        const float barX = S::ColX + 8.f, barW = S::MeterW - 8.f - 28.f;

        nvgBeginPath(vg);
        nvgRect(vg, barX * Scale, (y + S::MeterPitch * 0.24f) * Scale,
                barW * Scale, S::MeterPitch * 0.52f * Scale);
        nvgFillColor(vg, sfc::bodyDim()); nvgFill(vg);

        nvgBeginPath(vg);
        nvgRect(vg, barX * Scale, (y + S::MeterPitch * 0.24f) * Scale,
                barW * level * Scale, S::MeterPitch * 0.52f * Scale);
        nvgFillColor(vg, sfc::red()); nvgFill(vg);

        pixel::textAt(pen, S::ColX + S::MeterW - 24.f, y + S::MeterPitch / 2 - 1.2f,
                      " 13", 2.4f, sfc::ink());
        pixel::textAt(pen, S::ColX + S::MeterW - 9.f, y + S::MeterPitch / 2 - 1.2f,
                      "A#3", 2.4f, sfc::green());
    }

    const NVGcolor tint[S::ButtonCount] = {
        sfc::red(), sfc::blue(), sfc::blue(), sfc::green(), sfc::yellow(), sfc::violet() };

    for(int i = 0; i < S::ButtonCount; ++i)
        bevelButton(vg, S::buttonX(i), S::ButtonY, S::ButtonD,
                    panel::snesButtonIcons()[i], tint[i]);

    knob(vg, S::RateX, S::RateY, 11.f);
    knob(vg, S::VolX,  S::VolY,  11.f);

    for(int i = 0; i < 12; ++i)
        port(vg, panel::padSeats()[i].x, panel::padSeats()[i].y,
             panel::padSeats()[i].tint());

    for(int i = 0; i < 6; ++i)
        port(vg, panel::snesUtility()[i].x, panel::snesUtility()[i].y);

    for(int i = 0; i < 16; ++i)
        port(vg, S::outX(i % S::OutCols), i < S::OutCols ? S::OutRow1 : S::OutRow2);

    // the pad light, which the module draws over the panel
    panel::mark(ink, S::ColR - 6.0f, 7.8f, 5.0f, true);

    screws(vg, S::PanelW, true);
    outline(vg, 0, 0, S::PanelW, layout::PanelH, nvgRGBA(0xa0, 0, 0, 130));
}

static void drawSampler(NVGcontext* vg, int font)
{
    namespace S = layout::sampler;

    panel::Ink ink { vg, font, Scale };
    pixel::Pen pen { vg, Scale };
    panel::paintSampler(ink);

    pixel::textAt(pen, S::Left + 2.4f, S::ScopeY + 1.6f, "SUPER MARIO WORLD", 2.6f, sfc::ink());
    pixel::textAt(pen, S::Left + 2.4f, S::ScopeY + 5.4f, "11 OF 20  SRCN 13  0.607S  LOOPS",
                  2.4f, sfc::shade());

    nvgBeginPath(vg);
    for(int x = 0; x < 300; ++x)
    {
        const float t = x / 300.f;
        const float a = std::sin(t * 43.f) * std::sin(t * 6.f) * 0.8f;
        const float px = (S::Left + 1.f + t * (S::Right - S::Left - 2.f)) * Scale;
        const float py = (S::ScopeY + 9.f + (S::ScopeH - 10.f) * (0.5f - a * 0.46f)) * Scale;
        if(x == 0) nvgMoveTo(vg, px, py); else nvgLineTo(vg, px, py);
    }
    nvgStrokeColor(vg, sfc::red()); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);

    const NVGcolor tint[4] = { sfc::red(), sfc::violet(), sfc::blue(), sfc::blue() };
    for(int i = 0; i < 4; ++i)
        bevelButton(vg, S::colX(i + 1), S::ButtonY, 7.6f,
                    panel::samplerButtonIcons()[i], tint[i]);

    for(int i = 0; i < 6; ++i)
    {
        knob(vg, S::colX(i), S::KnobRow1, 11.f);
        knob(vg, S::colX(i), S::KnobRow2, 9.f);
        port(vg, S::colX(i), S::InRow);
        port(vg, S::colX(i), S::OutRow);
    }

    auto chooser = [&](float x, const char* label) {
        pixel::bevel(pen, x, S::ChoiceY, S::ChoiceW, S::ChoiceH,
                     sfc::paper(), sfc::white(), sfc::shade(), false, 0.5f);
        const float w = pixel::textWidth(label, 2.4f);
        pixel::textAt(pen, x + S::ChoiceW / 2 - w / 2, S::ChoiceY + S::ChoiceH / 2 - 1.2f,
                      label, 2.4f, sfc::ink());
    };
    chooser(S::Left + 1.f, "AS AUTHORED");
    chooser(S::Right - S::ChoiceW - 1.f, "GAUSSIAN");

    screws(vg, S::PanelW, false);
    outline(vg, 0, 0, S::PanelW, layout::PanelH, nvgRGBA(0xa0, 0, 0, 130));
}

static void drawApu(NVGcontext* vg, int font)
{
    namespace A = layout::apu;

    panel::Ink ink { vg, font, Scale };
    pixel::Pen pen { vg, Scale };
    panel::paintApu(ink);

    pixel::textAt(pen, A::Left + 2.4f, A::PlateY + 1.6f, "SUPER MARIO WORLD", 2.6f, sfc::ink());
    pixel::textAt(pen, A::Left + 2.4f, A::PlateY + 5.4f, "PC $055D  PORTS 03 00 00 00",
                  2.4f, sfc::shade());

    for(int i = 0; i < 8; ++i)
    {
        const float y = A::PlateY + 9.4f + 3.0f * i;
        char buf[8];
        std::snprintf(buf, sizeof buf, "V%d", i + 1);
        pixel::textAt(pen, A::Left + 2.4f, y, buf, 2.4f, sfc::shade());

        nvgBeginPath(vg);
        nvgRect(vg, (A::Left + 9.f) * Scale, (y + 0.5f) * Scale,
                (A::Right - A::Left - 38.f) * ((i * 29 % 100) / 100.f) * Scale, 1.7f * Scale);
        nvgFillColor(vg, sfc::red()); nvgFill(vg);
    }

    const NVGcolor tint[3] = { sfc::red(), sfc::violet(), sfc::yellow() };
    for(int i = 0; i < 3; ++i)
        bevelButton(vg, A::colX(i + 1), A::ButtonY, 7.6f, panel::apuButtonIcons()[i], tint[i]);

    for(int p = 0; p < 4; ++p)
    {
        knob(vg, A::colX(p), A::PortKnob, 9.f);
        port(vg, A::colX(p), A::PortJack);
    }

    knob(vg, A::colX(4), A::PortKnob, 11.f);
    port(vg, A::colX(4), A::PortJack);
    knob(vg, A::colX(0), A::WriteY, 11.f);
    knob(vg, A::colX(1), A::WriteY, 11.f);

    for(int i = 0; i < 5; ++i) port(vg, A::colX(i), A::OutRow);

    screws(vg, A::PanelW, false);
    outline(vg, 0, 0, A::PanelW, layout::PanelH, nvgRGBA(0xa0, 0, 0, 130));
}

static void drawBender(NVGcontext* vg, int font)
{
    namespace B = layout::bender;

    panel::Ink ink { vg, font, Scale };
    pixel::Pen pen { vg, Scale };
    panel::paintBender(ink);

    const char* choices[3] = { "ROTATE", "PITCH MOD", "TILES" };
    const int at[3] = { 0, 2, 4 };

    for(int i = 0; i < 3; ++i)
    {
        const float x = B::PanelW - 3.0f - B::ChoiceW;
        const float y = B::ruleY(at[i]) + 0.8f;
        pixel::bevel(pen, x, y, B::ChoiceW, B::ChoiceH,
                     sfc::paper(), sfc::white(), sfc::shade(), false, 0.5f);
        const float w = pixel::textWidth(choices[i], 2.4f);
        pixel::textAt(pen, x + B::ChoiceW / 2 - w / 2, y + B::ChoiceH / 2 - 1.2f,
                      choices[i], 2.4f, sfc::ink());
    }

    for(int i = 0; i < B::SectionCount; ++i)
    {
        if(i == 5) bevelButton(vg, B::KnobX, B::rowY(i), 8.4f, pixel::IconStop, sfc::purple());
        else       knob(vg, B::KnobX, B::rowY(i), 11.f);
        port(vg, B::JackX, B::rowY(i));
    }

    screws(vg, B::PanelW, false);
    outline(vg, 0, 0, B::PanelW, layout::PanelH, nvgRGBA(0xa0, 0, 0, 130));
}

// ---------------------------------------------------------------------------

static bool render(const char* path, float mmW,
                   void (*draw)(NVGcontext*, int, int), bool wantScreen)
{
    const int w = (int)(mmW * Scale + 0.5f);
    const int h = (int)(layout::PanelH * Scale + 0.5f);

    Headless gl;
    if(!gl.start(w, h)) return false;

    NVGcontext* vg = nvgCreateGL2(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
    if(!vg) { std::fprintf(stderr, "nvgCreateGL2 failed\n"); gl.stop(); return false; }

    const int font = loadFont(vg);

    int image = -1;
    if(wantScreen)
    {
        int iw = 0, ih = 0, ic = 0;
        if(unsigned char* px = stbi_load("build/test/out/frame.png", &iw, &ih, &ic, 4))
        {
            image = nvgCreateImageRGBA(vg, iw, ih, NVG_IMAGE_NEAREST, px);
            stbi_image_free(px);
        }
    }

    glViewport(0, 0, w, h);
    glClearColor(0.06f, 0.07f, 0.11f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    nvgBeginFrame(vg, (float)w, (float)h, 1.f);
    draw(vg, font, image);
    nvgEndFrame(vg);

    std::vector<unsigned char> pixels((size_t)w * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    // OpenGL hands back the bottom row first.
    std::vector<unsigned char> flipped((size_t)w * h * 4);
    for(int y = 0; y < h; ++y)
        std::memcpy(&flipped[(size_t)y * w * 4],
                    &pixels[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);

    stbi_write_png(path, w, h, 4, flipped.data(), w * 4);
    std::printf("-- wrote %s (%dx%d)\n", path, w, h);

    nvgDeleteGL2(vg);
    gl.stop();
    return true;
}

// A specimen sheet: every glyph the panels can draw and every icon, at the
// sizes they are drawn at. A font authored as pictures has to be looked at.
static void drawSpecimen(NVGcontext* vg, int, int)
{
    pixel::Pen pen { vg, Scale };
    panel::Ink ink { vg, -1, Scale };

    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, 200 * Scale, layout::PanelH * Scale);
    nvgFillColor(vg, sfc::body());
    nvgFill(vg);

    const char* lines[] = {
        "ABCDEFGHIJKLM",
        "NOPQRSTUVWXYZ",
        "0123456789",
        "!\"#$%&'()*+,-./",
        ":;<=>?@[\\]^_",
        "MIX L V/OCT ECHO R",
        "the quick brown fox",
    };

    float y = 4.f;
    for(const char* line : lines)
    {
        pixel::textAt(pen, 4.f, y, line, 4.0f, sfc::ink());
        y += 6.5f;
    }

    y += 2.f;
    const float sizes[] = { 2.4f, 2.8f, 3.5f, 5.0f, 7.0f };
    for(float size : sizes)
    {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.1fMM SAMPLE 013", size);
        pixel::textAt(pen, 4.f, y, buf, size, sfc::ink());
        y += size * 1.6f;
    }

    y += 3.f;
    for(int i = 0; i < pixel::MascotCount; ++i)
    {
        const float x = 4.f + i * 34.f;
        pixel::bevel(pen, x - 1.f, y - 1.f, 34.f, 34.f,
                     sfc::body(), sfc::white(), sfc::shade(), false, 0.7f);
        panel::mascot(ink, x + 0.5f, y + 0.5f, i, 32.f);
    }
    y += 38.f;

    for(int i = 0; i < pixel::IconCount; ++i)
    {
        const float x = 4.f + (i % 6) * 14.f;
        const float iy = y + (i / 6) * 14.f;
        pixel::bevel(pen, x - 1.f, iy - 1.f, 12.f, 12.f,
                     sfc::paper(), sfc::white(), sfc::shade(), true, 0.7f);
        pixel::iconAt(pen, x + 0.5f, iy + 0.5f, i, 9.f, sfc::ink(), sfc::red());
    }
}

int main()
{
    bool ok = true;

    ok &= render("build/mockup/specimen.png", 200.f,
                 [](NVGcontext* vg, int font, int) { drawSpecimen(vg, font, 0); }, false);

    ok &= render("build/mockup/snes.png", layout::snes::PanelW,
                 [](NVGcontext* vg, int font, int image) { drawSnes(vg, font, image); }, true);

    ok &= render("build/mockup/sampler.png", layout::sampler::PanelW,
                 [](NVGcontext* vg, int font, int) { drawSampler(vg, font); }, false);

    ok &= render("build/mockup/apu.png", layout::apu::PanelW,
                 [](NVGcontext* vg, int font, int) { drawApu(vg, font); }, false);

    ok &= render("build/mockup/bender.png", layout::bender::PanelW,
                 [](NVGcontext* vg, int font, int) { drawBender(vg, font); }, false);

    return ok ? 0 : 1;
}
