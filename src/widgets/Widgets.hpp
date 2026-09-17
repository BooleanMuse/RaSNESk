// ===========================================================================
// The Rack side of the four panels: the pieces that need a module, a mouse or
// a texture. The painting itself lives in ../PanelDraw.hpp, which knows
// nothing about Rack so that the mockup can draw the same panel.
//
// Every component here is drawn rather than borrowed. Rack's own knobs and
// sockets are fine and belong to a different machine; these are the grey
// plastic, the sunk sockets and the picture-on-a-button of a Super Nintendo
// and of the one program on it that was a program rather than a game.
// ===========================================================================
#pragma once

#include "../plugin.hpp"
#include "../Layout.hpp"
#include "../PanelDraw.hpp"
#include "../Pixel.hpp"

#include <functional>
#include <string>
#include <vector>

namespace racksnes {

// An Ink for drawing on a Rack panel: one millimetre's worth of pixels.
inline panel::Ink panelInk(NVGcontext* vg)
{
    panel::Ink ink;
    ink.vg    = vg;
    ink.scale = mm2px(1.f);
    return ink;
}

inline pixel::Pen panelPen(NVGcontext* vg) { return pixel::Pen { vg, mm2px(1.f) }; }

// ---------------------------------------------------------------------------
// The panel itself.
//
// Inside a framebuffer, because a panel set in a font that is drawn rather
// than typeset is a few thousand little rectangles, and there is no reason to
// send them to the card sixty times a second when none of them ever move.
// ---------------------------------------------------------------------------
struct PaintedPanel : widget::FramebufferWidget {

    struct Face : widget::Widget {
        std::function<void(const panel::Ink&)> painter;

        void draw(const DrawArgs& args) override
        {
            if(painter) painter(panelInk(args.vg));
        }
    };

    Face* face;

    PaintedPanel(float w, std::function<void(const panel::Ink&)> painter)
    {
        box.size = Vec(w, RACK_GRID_HEIGHT);

        face = new Face;
        face->box.size = box.size;
        face->painter  = painter;
        addChild(face);
    }
};

// ---------------------------------------------------------------------------
// A socket, sunk into the panel
// ---------------------------------------------------------------------------
struct SnesPort : app::PortWidget {

    // The rim takes the colour of the button it stands for, on the four that
    // had one. Everything else keeps the plastic's own grey.
    NVGcolor rim = sfc::bodyDim();

    SnesPort() { box.size = mm2px(Vec(8.2f, 8.2f)); }

    void draw(const DrawArgs& args) override
    {
        const float r = box.size.x / 2;

        // the rim, lit from the top left the way every window on the machine is
        nvgBeginPath(args.vg);
        nvgCircle(args.vg, r, r, r);
        nvgFillColor(args.vg, sfc::white());
        nvgFill(args.vg);

        nvgBeginPath(args.vg);
        nvgCircle(args.vg, r, r, r - mm2px(0.25f));
        nvgFillColor(args.vg, sfc::shade());
        nvgFill(args.vg);

        nvgBeginPath(args.vg);
        nvgCircle(args.vg, r - mm2px(0.15f), r - mm2px(0.15f), r - mm2px(0.35f));
        nvgFillColor(args.vg, rim);
        nvgFill(args.vg);

        // the hole
        nvgBeginPath(args.vg);
        nvgCircle(args.vg, r, r, r * 0.52f);
        nvgFillColor(args.vg, sfc::bezel());
        nvgFill(args.vg);

        nvgBeginPath(args.vg);
        nvgCircle(args.vg, r, r, r * 0.34f);
        nvgFillColor(args.vg, nvgRGB(0x0c, 0x0c, 0x0e));
        nvgFill(args.vg);

        PortWidget::draw(args);
    }
};

// ---------------------------------------------------------------------------
// A knob: grey plastic with a notch cut in it
// ---------------------------------------------------------------------------
struct SnesKnob : app::Knob {

    NVGcolor cap = sfc::bodyDim();
    NVGcolor notch = sfc::ink();

    SnesKnob(float mm = 10.0f)
    {
        box.size = mm2px(Vec(mm, mm));
        minAngle = -0.83f * M_PI;
        maxAngle =  0.83f * M_PI;
    }

    void draw(const DrawArgs& args) override
    {
        const float r = box.size.x / 2;

        nvgBeginPath(args.vg);
        nvgCircle(args.vg, r, r, r);
        nvgFillColor(args.vg, sfc::shade());
        nvgFill(args.vg);

        nvgBeginPath(args.vg);
        nvgCircle(args.vg, r - mm2px(0.2f), r - mm2px(0.2f), r - mm2px(0.3f));
        nvgFillColor(args.vg, sfc::white());
        nvgFill(args.vg);

        nvgBeginPath(args.vg);
        nvgCircle(args.vg, r, r, r - mm2px(0.55f));
        nvgFillColor(args.vg, cap);
        nvgFill(args.vg);

        engine::ParamQuantity* pq = getParamQuantity();
        const float v = pq ? pq->getScaledValue() : 0.5f;
        const float a = minAngle + (maxAngle - minAngle) * v;

        nvgSave(args.vg);
        nvgTranslate(args.vg, r, r);
        nvgRotate(args.vg, a);

        nvgBeginPath(args.vg);
        nvgRect(args.vg, -mm2px(0.45f), -r + mm2px(0.9f), mm2px(0.9f), r * 0.55f);
        nvgFillColor(args.vg, notch);
        nvgFill(args.vg);

        nvgRestore(args.vg);

        Knob::draw(args);
    }
};

// ---------------------------------------------------------------------------
// A button with a picture on it, which is what a Mario Paint toolbar is
// ---------------------------------------------------------------------------
struct IconButton : app::Switch {

    int      icon = 0;
    NVGcolor tint = sfc::red();
    float    mm   = 7.6f;

    IconButton(int which = 0, NVGcolor colour = sfc::red(), float size = 7.6f)
        : icon(which), tint(colour), mm(size)
    {
        box.size = mm2px(Vec(size, size));
        momentary = true;
    }

    void draw(const DrawArgs& args) override
    {
        engine::ParamQuantity* pq = getParamQuantity();
        const bool down = pq && pq->getValue() > 0.5f;

        pixel::Pen pen = panelPen(args.vg);
        nvgSave(args.vg);
        nvgTranslate(args.vg, 0, down ? mm2px(0.25f) : 0.f);

        pixel::bevel(pen, 0, 0, mm, mm, sfc::paper(), sfc::white(), sfc::shade(),
                     !down, 0.55f);
        pixel::iconAt(pen, mm * 0.11f, mm * 0.11f, icon, mm * 0.78f, sfc::ink(), tint);

        nvgRestore(args.vg);

        Switch::draw(args);
    }
};

// ---------------------------------------------------------------------------
// The screen
//
// The module hands over a buffer of RGBA it finished writing on the audio
// thread. Nothing here waits on that thread: the module keeps three buffers
// and only ever publishes a whole one.
// ---------------------------------------------------------------------------
struct ScreenWidget : widget::Widget {
    std::function<const uint8_t*()> source;     // the frame, or null
    std::function<std::string()>    message;    // what to say when there is none
    std::function<bool()>           overscan;   // show all 240 rows

    int         image   = -1;
    NVGcontext* imageVg = nullptr;

    void drawLayer(const DrawArgs& args, int layer) override
    {
        // Layer 1 is the one that stays lit when the room lights go out,
        // which is the only sensible place for a screen.
        if(layer != 1) { Widget::drawLayer(args, layer); return; }

        const uint8_t* pixels = source ? source() : nullptr;

        if(!pixels)
        {
            drawTestCard(args);
            Widget::drawLayer(args, layer);
            return;
        }

        if(image < 0 || imageVg != args.vg)
        {
            // NEAREST, because every pixel of this is meant to be a square.
            image = nvgCreateImageRGBA(args.vg, ScreenW, ScreenH, NVG_IMAGE_NEAREST, pixels);
            imageVg = args.vg;
        }
        else nvgUpdateImage(args.vg, image, pixels);

        if(image >= 0)
        {
            // The box is 224 rows tall. The picture is 240, and the eight at
            // each end are the overscan: most cartridges draw nothing there
            // and a few draw rubbish, so they are outside the box unless
            // somebody asks for them.
            const bool all = overscan && overscan();

            const float h  = all ? box.size.y
                                 : box.size.y * (float)ScreenH / (float)ScreenVisible;
            const float oy = all ? 0.f : -h * (float)ScreenCrop / (float)ScreenH;

            nvgSave(args.vg);
            nvgScissor(args.vg, 0, 0, box.size.x, box.size.y);

            NVGpaint paint = nvgImagePattern(args.vg, 0, oy, box.size.x, h, 0, image, 1.f);
            nvgBeginPath(args.vg);
            nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
            nvgFillPaint(args.vg, paint);
            nvgFill(args.vg);

            nvgRestore(args.vg);
        }

        Widget::drawLayer(args, layer);
    }

    // Nothing loaded. A television with nothing on has never shown black.
    void drawTestCard(const DrawArgs& args)
    {
        nvgBeginPath(args.vg);
        nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
        nvgFillColor(args.vg, sfc::screen());
        nvgFill(args.vg);

        // Eight bars, which is what a television test card has and also how
        // many voices are waiting behind this one.
        static const unsigned Bars[8] = {
            0xf2f0ea, 0xdca41c, 0x3f9a4a, 0x2f6ec4,
            0x7a5fb8, 0xc4453c, 0x6f6b64, 0x232327,
        };

        const float w   = box.size.x / 8.f;
        const float top = box.size.y * 0.10f;
        const float h   = box.size.y * 0.55f;

        for(int i = 0; i < 8; ++i)
        {
            NVGcolor c = sfc::of(Bars[i]);
            c.a = 0.72f;

            nvgBeginPath(args.vg);
            nvgRect(args.vg, i * w, top, w + 0.5f, h);
            nvgFillColor(args.vg, c);
            nvgFill(args.vg);
        }

        std::string t = message ? message() : "";
        if(t.empty()) return;

        // Wrapped by hand, because the letters are drawn one at a time and
        // nanovg's text box knows nothing about them.
        pixel::Pen pen = panelPen(args.vg);
        const float size = 3.4f;
        const float perLine = box.size.x / mm2px(1.f) * 0.92f;

        std::vector<std::string> lines;
        std::string line;

        for(size_t i = 0; i <= t.size(); ++i)
        {
            const bool end = i == t.size();
            if(!end && t[i] != ' ' && t[i] != '\n') { line += t[i]; continue; }

            lines.push_back(line);
            if(!end && t[i] == '\n') { lines.push_back(""); }
            line.clear();
        }

        // put the words back together into lines that fit
        std::vector<std::string> wrapped;
        std::string current;

        for(const std::string& word : lines)
        {
            std::string candidate = current.empty() ? word : current + " " + word;
            if(pixel::textWidth(candidate.c_str(), size) > perLine && !current.empty())
            {
                wrapped.push_back(current);
                current = word;
            }
            else current = candidate;
        }
        if(!current.empty()) wrapped.push_back(current);

        float y = box.size.y / mm2px(1.f) * 0.74f;
        for(const std::string& l : wrapped)
        {
            const float w2 = pixel::textWidth(l.c_str(), size);
            pixel::textAt(pen, box.size.x / mm2px(1.f) / 2 - w2 / 2, y, l.c_str(),
                          size, sfc::white());
            y += size * 1.7f;
        }
    }
};

// ---------------------------------------------------------------------------
// A plate of text: a cartridge's name, a bank's, whatever the module wants to
// say about what it has got hold of.
// ---------------------------------------------------------------------------
struct TextPlate : widget::Widget {
    struct Line { std::string text; float size; NVGcolor color; int align; };

    std::function<std::vector<Line>()> lines;

    void draw(const DrawArgs& args) override
    {
        if(!lines) { Widget::draw(args); return; }

        pixel::Pen pen = panelPen(args.vg);
        const float mm = 1.f / mm2px(1.f);

        float y = 1.0f;

        for(const Line& line : lines())
        {
            if(!line.text.empty())
            {
                const float w = pixel::textWidth(line.text.c_str(), line.size);

                float x = 1.4f;
                if(line.align & NVG_ALIGN_CENTER) x = box.size.x * mm / 2 - w / 2;
                if(line.align & NVG_ALIGN_RIGHT)  x = box.size.x * mm - 1.4f - w;

                pixel::textAt(pen, x, y, line.text.c_str(), line.size, line.color);
            }

            y += line.size * 1.5f;
        }

        Widget::draw(args);
    }
};

// ---------------------------------------------------------------------------
// The eight voices
//
// A row to a voice: a bar for its envelope, the number of the sample it is
// playing, and what note that works out to. This is the one thing the panel
// can show that the jacks cannot -- which of the eight is doing what, at a
// glance.
// ---------------------------------------------------------------------------
struct VoiceMeters : widget::Widget {
    std::function<const VoiceState*()> source;
    float rowPitch = 4.25f;
    float textSize = 2.4f;

    void draw(const DrawArgs& args) override
    {
        if(!source) { Widget::draw(args); return; }

        const VoiceState* v = source();

        pixel::Pen pen = panelPen(args.vg);
        const float mmScale = mm2px(1.f);
        const float wmm = box.size.x / mmScale;

        const float barX = 8.0f;
        const float barW = wmm - barX - 28.0f;

        for(int i = 0; i < VoiceCount; ++i)
        {
            const float y = i * rowPitch;
            char buf[32];

            std::snprintf(buf, sizeof buf, "V%d", i + 1);
            pixel::textAt(pen, 1.5f, y + rowPitch / 2 - textSize / 2, buf, textSize,
                          sfc::shade());

            // the well the bar sits in
            nvgBeginPath(args.vg);
            nvgRect(args.vg, barX * mmScale, (y + rowPitch * 0.24f) * mmScale,
                    barW * mmScale, rowPitch * 0.52f * mmScale);
            nvgFillColor(args.vg, sfc::bodyDim());
            nvgFill(args.vg);

            const float level = v ? clamp(v[i].level, 0.f, 1.f) : 0.f;

            if(level > 0.002f)
            {
                nvgBeginPath(args.vg);
                nvgRect(args.vg, barX * mmScale, (y + rowPitch * 0.24f) * mmScale,
                        barW * level * mmScale, rowPitch * 0.52f * mmScale);
                nvgFillColor(args.vg, v && v[i].keyed ? sfc::red() : sfc::silver());
                nvgFill(args.vg);
            }

            if(!v) continue;

            std::snprintf(buf, sizeof buf, "%3d", v[i].srcn);
            float w = pixel::textWidth(buf, textSize);
            pixel::textAt(pen, wmm - 17.0f - w, y + rowPitch / 2 - textSize / 2, buf,
                          textSize, v[i].keyed ? sfc::ink() : sfc::silver());

            // The pitch as a note rather than as a register: a sample has no
            // note of its own, so this is against the pitch the chip calls
            // 1:1, which is the note every sound driver treats as the root.
            static const char* names[12] =
                { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

            const int semis = (int)std::round(v[i].volts * 12.f);
            const int note  = ((semis % 12) + 12) % 12;
            const int oct   = 4 + (int)std::floor((semis + 0.5f) / 12.f);

            std::snprintf(buf, sizeof buf, "%s%d", names[note], oct);
            w = pixel::textWidth(buf, textSize);
            pixel::textAt(pen, wmm - 1.5f - w, y + rowPitch / 2 - textSize / 2, buf,
                          textSize, v[i].keyed ? sfc::green() : sfc::silver());
        }

        Widget::draw(args);
    }
};

// ---------------------------------------------------------------------------
// One BRR sample, drawn
//
// A SNES instrument has no name. It is a number in a table, and the only way
// to know what number 13 is before you play it is to look at its shape.
// ---------------------------------------------------------------------------
struct WaveWidget : widget::Widget {
    std::function<const std::vector<float>*()> source;
    std::function<float()> loopPoint;    // 0..1, or negative for none

    void draw(const DrawArgs& args) override
    {
        if(!source) { Widget::draw(args); return; }

        const std::vector<float>* wave = source();

        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, 0, box.size.y / 2);
        nvgLineTo(args.vg, box.size.x, box.size.y / 2);
        nvgStrokeColor(args.vg, sfc::silver());
        nvgStrokeWidth(args.vg, 1.f);
        nvgStroke(args.vg);

        if(wave && !wave->empty())
        {
            const int   n    = (int)wave->size();
            const float half = box.size.y * 0.46f;
            const int   cols = (int)box.size.x;

            nvgBeginPath(args.vg);

            for(int x = 0; x < cols; ++x)
            {
                // Every sample that falls in this column, not every nth one:
                // a BRR loop drawn by sampling is a BRR loop that lies.
                const int a = (int)((int64_t)x * n / cols);
                const int b = (int)((int64_t)(x + 1) * n / cols);

                float lo = 1.f, hi = -1.f;
                for(int i = a; i < b && i < n; ++i)
                {
                    const float v = (*wave)[i];
                    if(v < lo) lo = v;
                    if(v > hi) hi = v;
                }
                if(lo > hi) lo = hi = a < n ? (*wave)[a] : 0.f;

                nvgMoveTo(args.vg, x + 0.5f, box.size.y / 2 - hi * half);
                nvgLineTo(args.vg, x + 0.5f, box.size.y / 2 - lo * half + 0.6f);
            }

            nvgStrokeColor(args.vg, sfc::red());
            nvgStrokeWidth(args.vg, 1.f);
            nvgStroke(args.vg);
        }

        const float loop = loopPoint ? loopPoint() : -1.f;
        if(loop >= 0.f && loop <= 1.f)
        {
            nvgBeginPath(args.vg);
            nvgMoveTo(args.vg, loop * box.size.x, 0);
            nvgLineTo(args.vg, loop * box.size.x, box.size.y);
            nvgStrokeColor(args.vg, sfc::green());
            nvgStrokeWidth(args.vg, 1.f);
            nvgStroke(args.vg);
        }

        Widget::draw(args);
    }
};

// ---------------------------------------------------------------------------
// A parameter shown as the name of what it selects. Clicking moves it on;
// Rack's own right-click menu still offers the whole list.
// ---------------------------------------------------------------------------
struct ChoiceParam : app::ParamWidget {
    NVGcolor tint = sfc::ink();
    std::string placeholder;

    ChoiceParam(float wmm, float hmm, const std::string& empty = "")
        : placeholder(empty)
    {
        box.size = Vec(mm2px(wmm), mm2px(hmm));
    }

    std::string currentLabel()
    {
        engine::ParamQuantity* pq = getParamQuantity();
        if(!pq) return placeholder;

        engine::SwitchQuantity* sq = dynamic_cast<engine::SwitchQuantity*>(pq);
        if(sq)
        {
            int index = (int)std::round(pq->getValue());
            if(index >= 0 && index < (int)sq->labels.size()) return sq->labels[index];
        }

        return pq->getDisplayValueString();
    }

    void draw(const DrawArgs& args) override
    {
        pixel::Pen pen = panelPen(args.vg);
        const float mm = 1.f / mm2px(1.f);
        const float w = box.size.x * mm, h = box.size.y * mm;

        pixel::bevel(pen, 0, 0, w, h, sfc::paper(), sfc::white(), sfc::shade(), false, 0.5f);

        const std::string t = currentLabel();
        const float size = 2.4f;
        const float tw = pixel::textWidth(t.c_str(), size);

        pixel::textAt(pen, w / 2 - tw / 2, h / 2 - size / 2, t.c_str(), size, tint);
    }

    void onButton(const event::Button& e) override
    {
        engine::ParamQuantity* pq = getParamQuantity();

        if(pq && e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT)
        {
            float next = pq->getValue() + 1.f;
            if(next > pq->getMaxValue()) next = pq->getMinValue();
            pq->setValue(next);
            e.consume(this);
            return;
        }

        ParamWidget::onButton(e);
    }
};

// The controller's face on the console's deck, lit when a real one is
// plugged in. The panel says nothing about gamepads; this is how it tells
// you it found yours.
struct MarkLight : widget::Widget {
    std::function<bool()>      live;
    std::function<bool()>      learning;
    std::function<void(Menu*)> menu;
    float radius = 5.4f;

    ui::Tooltip* tip = nullptr;

    ~MarkLight() { hideTip(); }

    void draw(const DrawArgs& args) override
    {
        const bool teaching = learning && learning();

        // While something is being taught the four buttons blink, which is
        // the panel's whole way of saying "press one".
        if(teaching && ((int)(system::getTime() * 3.0) & 1))
        { Widget::draw(args); return; }

        if(!teaching && (!live || !live())) { Widget::draw(args); return; }

        const float r = radius;
        const float d = r * 0.47f, br = r * 0.235f;

        const NVGcolor four[4] = { sfc::blue(), sfc::red(), sfc::yellow(), sfc::green() };
        const float ox[4] = { 0.f,  1.f, 0.f, -1.f };
        const float oy[4] = { -1.f, 0.f, 1.f,  0.f };

        for(int i = 0; i < 4; ++i)
        {
            nvgBeginPath(args.vg);
            nvgCircle(args.vg, mm2px(r + ox[i] * d), mm2px(r + oy[i] * d), mm2px(br));
            nvgFillColor(args.vg, four[i]);
            nvgFill(args.vg);
        }

        Widget::draw(args);
    }

    void onButton(const event::Button& e) override
    {
        if(menu && e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT)
        {
            hideTip();
            menu(createMenu());
            e.consume(this);
            return;
        }

        Widget::onButton(e);
    }

    void onEnter(const event::Enter& e) override
    {
        hideTip();
        tip = new ui::Tooltip;
        tip->text = "Gamepad and keyboard";
        APP->scene->addChild(tip);
    }

    void onLeave(const event::Leave& e) override { hideTip(); }

    void hideTip()
    {
        if(!tip) return;
        APP->scene->removeChild(tip);
        delete tip;
        tip = nullptr;
    }
};

// Two helpers, because every module places these the same way: by the centre
// of the component, in millimetres, which is how the layout is written.
inline SnesKnob* knobAt(Module* module, int param, float mmX, float mmY, float mm = 10.f)
{
    SnesKnob* k = new SnesKnob(mm);
    k->box.pos  = mm2px(Vec(mmX - mm / 2, mmY - mm / 2));
    k->module   = module;
    k->paramId  = param;
    k->initParamQuantity();
    return k;
}

inline IconButton* buttonAt(Module* module, int param, float mmX, float mmY,
                            int icon, NVGcolor tint, float mm = 7.6f)
{
    IconButton* b = new IconButton(icon, tint, mm);
    b->box.pos  = mm2px(Vec(mmX - mm / 2, mmY - mm / 2));
    b->module   = module;
    b->paramId  = param;
    b->initParamQuantity();
    return b;
}

// The screws the machine's own case had: a flat grey disc, not Rack's silver.
struct SnesScrew : widget::Widget {
    SnesScrew() { box.size = Vec(RACK_GRID_WIDTH, RACK_GRID_WIDTH); }

    void draw(const DrawArgs& args) override
    {
        const float r = box.size.x / 2;

        nvgBeginPath(args.vg);
        nvgCircle(args.vg, r, r, r * 0.62f);
        nvgFillColor(args.vg, sfc::shade());
        nvgFill(args.vg);

        nvgBeginPath(args.vg);
        nvgCircle(args.vg, r - mm2px(0.1f), r - mm2px(0.1f), r * 0.52f);
        nvgFillColor(args.vg, sfc::bodyDim());
        nvgFill(args.vg);

        nvgBeginPath(args.vg);
        nvgRect(args.vg, r - r * 0.36f, r - mm2px(0.22f), r * 0.72f, mm2px(0.44f));
        nvgFillColor(args.vg, sfc::shade());
        nvgFill(args.vg);
    }
};

} // namespace racksnes
