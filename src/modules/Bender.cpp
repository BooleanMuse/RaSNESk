// ===========================================================================
// BENDER -- the circuit bending.
//
// An expander that goes either side of a SNES and reaches inside a running
// cartridge without its cooperation. Everything here is a write into a chip
// the game is using: the palette on its way to the screen, the pitch register
// of a voice the sound driver just set, a bit of the sound chip's memory that
// belonged to an instrument.
//
// The module itself does almost nothing. It resolves its knobs against its
// jacks once a sample and hands the answer to whichever SNES is beside it;
// the bending happens over there, where the chips are.
// ===========================================================================
#include "../plugin.hpp"
#include "../widgets/Widgets.hpp"

namespace racksnes {

namespace L = layout::bender;

struct BenderModule : Module {

    enum ParamId {
        PaletteParam, TransposeParam, WarpParam, EchoParam, GlitchParam, FreezeParam,
        PaletteModeParam, WarpModeParam, GlitchRegionParam,
        ParamCount
    };

    enum InputId {
        PaletteInput, TransposeInput, WarpInput, EchoInput, GlitchInput, FreezeInput,
        InputCount
    };

    enum OutputId { OutputCount };
    enum LightId  { AttachedLight, LightCount };

    // Rack gives an expander two buffers and swaps them; which side we are on
    // decides which of the neighbour's pairs we write into.
    BendMessage leftMessages[2];
    BendMessage rightMessages[2];

    BenderModule()
    {
        config(ParamCount, InputCount, OutputCount, LightCount);

        configParam(PaletteParam,   0.f, 1.f, 0.f, "Palette", "%", 0.f, 100.f);
        configParam(TransposeParam, -2.f, 2.f, 0.f, "Transpose", " octaves");
        configParam(WarpParam,      0.f, 1.f, 0.f, "Warp", "%", 0.f, 100.f);
        configParam(EchoParam,      0.f, 1.f, 0.f, "Echo feedback", "%", 0.f, 100.f);
        configParam(GlitchParam,    0.f, 1.f, 0.f, "Glitch", "%", 0.f, 100.f);
        configButton(FreezeParam,   "Freeze the sound chip's memory");

        configSwitch(PaletteModeParam, 0.f, 3.f, 0.f, "Palette",
                     { "ROTATE", "SHIFT", "INVERT", "DRAIN" });
        configSwitch(WarpModeParam, 0.f, 2.f, 0.f, "Warp",
                     { "PITCH MOD", "NOISE", "BOTH" });
        configSwitch(GlitchRegionParam, 0.f, 3.f, 0.f, "Glitch",
                     { "TILES", "PALETTE", "INSTRUMENTS", "WORK RAM" });

        configInput(PaletteInput,   "Palette");
        configInput(TransposeInput, "Transpose (a volt an octave)");
        configInput(WarpInput,      "Warp");
        configInput(EchoInput,      "Echo feedback");
        configInput(GlitchInput,    "Glitch");
        configInput(FreezeInput,    "Freeze");

        leftExpander.producerMessage  = &leftMessages[0];
        leftExpander.consumerMessage  = &leftMessages[1];
        rightExpander.producerMessage = &rightMessages[0];
        rightExpander.consumerMessage = &rightMessages[1];
    }

    float knobPlusCv(int param, int input, float low, float high)
    {
        float v = params[param].getValue();
        if(inputs[input].isConnected()) v += inputs[input].getVoltage() / 10.f * (high - low);
        return clamp(v, low, high);
    }

    void process(const ProcessArgs& args) override
    {
        BendMessage m;
        m.present = true;

        m.paletteMode   = (int)params[PaletteModeParam].getValue();
        m.paletteAmount = knobPlusCv(PaletteParam, PaletteInput, 0.f, 1.f);

        m.transpose = clamp(params[TransposeParam].getValue()
                          + inputs[TransposeInput].getVoltage(), -4.f, 4.f);

        m.warpMode   = (int)params[WarpModeParam].getValue();
        m.warpAmount = knobPlusCv(WarpParam, WarpInput, 0.f, 1.f);

        m.echoFeedback = knobPlusCv(EchoParam, EchoInput, 0.f, 1.f);

        m.glitchRegion = (int)params[GlitchRegionParam].getValue();
        m.glitchRate   = knobPlusCv(GlitchParam, GlitchInput, 0.f, 1.f);

        m.freezeHeld = params[FreezeParam].getValue() > 0.f
                    || inputs[FreezeInput].getVoltage() > 1.f;

        bool attached = false;

        // Whichever side the console is on gets the same message. Only the
        // side that has a SNES is written, so a Bender between two of
        // something else does nothing to either.
        if(leftExpander.module && leftExpander.module->model == modelSnes)
        {
            *(BendMessage*)leftExpander.module->rightExpander.producerMessage = m;
            leftExpander.module->rightExpander.requestMessageFlip();
            attached = true;
        }

        if(rightExpander.module && rightExpander.module->model == modelSnes)
        {
            *(BendMessage*)rightExpander.module->leftExpander.producerMessage = m;
            rightExpander.module->leftExpander.requestMessageFlip();
            attached = true;
        }

        lights[AttachedLight].setBrightness(attached ? 1.f : 0.f);
    }
};

// ===========================================================================

struct BenderWidget : ModuleWidget {

    BenderWidget(BenderModule* module)
    {
        setModule(module);
        box.size = Vec(RACK_GRID_WIDTH * L::Hp, RACK_GRID_HEIGHT);

        addChild(new PaintedPanel(box.size.x, panel::paintBender));

        addChild(createWidget<SnesScrew>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<SnesScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH,
                                               RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        const int knobs[L::SectionCount] = {
            BenderModule::PaletteParam, BenderModule::TransposeParam,
            BenderModule::WarpParam,    BenderModule::EchoParam,
            BenderModule::GlitchParam,  BenderModule::FreezeParam,
        };
        const int jacks[L::SectionCount] = {
            BenderModule::PaletteInput, BenderModule::TransposeInput,
            BenderModule::WarpInput,    BenderModule::EchoInput,
            BenderModule::GlitchInput,  BenderModule::FreezeInput,
        };

        for(int i = 0; i < L::SectionCount; ++i)
        {
            if(i == 5)
                addParam(buttonAt(module, knobs[i], L::KnobX, L::rowY(i),
                                  pixel::IconStop, sfc::purple(), 8.4f));
            else
                addParam(knobAt(module, knobs[i], L::KnobX, L::rowY(i), 11.0f));

            addInput(createInputCentered<SnesPort>(
                mm2px(Vec(L::JackX, L::rowY(i))), module, jacks[i]));
        }

        // The three sections that choose as well as turn. The chooser sits on
        // the section's own rule, to the right of its name, because down
        // beside the socket there is no room before the next section starts.
        addChoice(module, 0, BenderModule::PaletteModeParam, "ROTATE", sfc::red());
        addChoice(module, 2, BenderModule::WarpModeParam, "PITCH MOD", sfc::blue());
        addChoice(module, 4, BenderModule::GlitchRegionParam, "TILES", sfc::yellow());
    }

    void addChoice(BenderModule* module, int section, int param,
                   const std::string& placeholder, NVGcolor tint)
    {
        ChoiceParam* choice = new ChoiceParam(L::ChoiceW, L::ChoiceH, placeholder);
        choice->box.pos = mm2px(Vec(L::PanelW - 3.0f - L::ChoiceW, L::ruleY(section) + 0.8f));
        choice->module  = module;
        choice->paramId = param;
        choice->tint    = tint;
        choice->initParamQuantity();
        addParam(choice);
    }
};

} // namespace racksnes

Model* modelBender = createModel<racksnes::BenderModule, racksnes::BenderWidget>("Bender");
