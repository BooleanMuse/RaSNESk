// ===========================================================================
// SAMPLER -- a cartridge's instruments, on a keyboard.
//
// Every SNES game builds its own sampler. The cartridge uploads a sound
// driver and a pile of BRR samples into the sound chip's 64K when it boots,
// writes the page of a directory into one register, and from then on a note
// is an index into that directory and a pitch. Take the 64K and you have
// taken the instruments.
//
// This module is those 64K and a sound chip of its own -- the same code the
// console runs, compiled again so that it can be owned -- with nobody else
// writing its registers. You are the sound driver.
//
// Eight notes at once, because the chip has eight voices. Not seven, not
// nine: eight, and stealing one means taking it off whatever had it.
// ===========================================================================
#include "../plugin.hpp"
#include "../widgets/Widgets.hpp"
#include "../snes/Bank.hpp"
#include "../snes/Sdsp.hpp"

#include <osdialog.h>

#include <atomic>
#include <mutex>
#include <cstring>
#include <string>
#include <vector>

namespace racksnes {

namespace L = layout::sampler;

struct SamplerModule : Module {

    enum ParamId {
        SampleParam, TuneParam, FineParam, PanParam, LevelParam, EchoParam,
        AttackParam, DecayParam, SustainParam, ReleaseParam, FeedbackParam, TimeParam,
        LoopParam, InterpParam,
        LoadParam, TakeParam, PrevParam, NextParam,
        ParamCount
    };

    enum InputId {
        PitchInput, GateInput, SampleInput, LevelInput, PanInput, TuneInput,
        InputCount
    };

    enum OutputId {
        MixLOutput, MixROutput, VoicesOutput, DryOutput, EchoLOutput, EchoROutput,
        OutputCount
    };

    enum LightId { TakeLight, LightCount };

    Bank bank;
    Sdsp chip;

    // The panel draws the waveform of whichever instrument the knob is on,
    // which means the drawing thread reads the bank while this one may be
    // replacing it. A Bank owns a vector; swapping it under a reader is a
    // crash rather than a glitch.
    std::mutex bankLock;

    uint32_t followed = 0;          // which publication of the shelf we have
    bool     autoFollow = true;

    // --- the eight voices, and what has hold of each -----------------------
    struct Voice {
        bool  held   = false;
        int   srcn   = -1;
        float volts  = 0.f;
    };
    Voice voice[VoiceCount];

    dsp::SchmittTrigger gateTrigger[VoiceCount];
    dsp::BooleanTrigger loadTrigger, takeTrigger, prevTrigger, nextTrigger;

    std::atomic<bool> wantLoad{false};
    std::atomic<bool> wantTake{false};

    std::string trouble;

    // The waveform on the panel, decoded once when the choice changes rather
    // than every frame.
    std::vector<float> shown;
    int      shownSrcn = -1;
    uint32_t shownStamp = 0;
    float    shownLoop = -1.f;

    Resampler<4 + 2 * VoiceCount + VoiceCount> resample;
    ChipFrame pendingIn;

    int   lastLoopMode = -1;
    float takeLight = 0.f;

    SamplerModule()
    {
        config(ParamCount, InputCount, OutputCount, LightCount);

        configParam(SampleParam, 0.f, 255.f, 0.f, "Sample");
        getParamQuantity(SampleParam)->snapEnabled = true;

        configParam(TuneParam, -24.f, 24.f, 0.f, "Tune", " semitones");
        getParamQuantity(TuneParam)->snapEnabled = true;
        configParam(FineParam, -1.f, 1.f, 0.f, "Fine", " semitones");
        configParam(PanParam, -1.f, 1.f, 0.f, "Pan");
        configParam(LevelParam, 0.f, 1.f, 0.75f, "Level");
        configParam(EchoParam, 0.f, 1.f, 0.f, "Echo");

        // These are the chip's own envelope, and the chip's own envelope is
        // four bits of attack, three of decay and five of sustain rate. A
        // knob that slid smoothly would be a knob that lied about what the
        // hardware can be told.
        configParam(AttackParam, 0.f, 15.f, 12.f, "Attack");
        configParam(DecayParam, 0.f, 7.f, 7.f, "Decay");
        configParam(SustainParam, 0.f, 7.f, 7.f, "Sustain level");
        configParam(ReleaseParam, 0.f, 31.f, 20.f, "Sustain rate");
        for(int p : { AttackParam, DecayParam, SustainParam, ReleaseParam })
            getParamQuantity(p)->snapEnabled = true;

        configParam(FeedbackParam, 0.f, 1.f, 0.f, "Echo feedback");
        configParam(TimeParam, 0.f, 15.f, 4.f, "Echo time", " x 16 ms");
        getParamQuantity(TimeParam)->snapEnabled = true;

        configSwitch(LoopParam, 0.f, 2.f, 0.f, "Looping",
                     { "AS AUTHORED", "FORCE LOOP", "ONE SHOT" });
        configSwitch(InterpParam, 0.f, 1.f, 0.f, "Interpolation",
                     { "GAUSSIAN", "CUBIC" });

        configButton(LoadParam, "Load an .spc or a .brr");
        configButton(TakeParam, "Take whatever a SNES last ripped");
        configButton(PrevParam, "The instrument before this one");
        configButton(NextParam, "The one after it");

        configInput(PitchInput,  "Pitch (polyphonic, up to eight notes)");
        configInput(GateInput,   "Gate (polyphonic)");
        configInput(SampleInput, "Sample (polyphonic: a note at a time)");
        configInput(LevelInput,  "Level (polyphonic)");
        configInput(PanInput,    "Pan (polyphonic)");
        configInput(TuneInput,   "Tune");

        configOutput(MixLOutput,  "Mix left");
        configOutput(MixROutput,  "Mix right");
        configOutput(VoicesOutput,"Voices (polyphonic, eight)");
        configOutput(DryOutput,   "Voices before their panning (polyphonic)");
        configOutput(EchoLOutput, "Echo left");
        configOutput(EchoROutput, "Echo right");
    }

    // -----------------------------------------------------------------------
    // The bank
    // -----------------------------------------------------------------------
    void adopt()
    {
        chip.load(bank.ram());
        chip.reset();
        chip.write(dspreg::Dir, (uint8_t)bank.dirPage());

        lastLoopMode = -1;
        shownSrcn = -1;
        for(Voice& v : voice) { v.held = false; v.srcn = -1; }
    }

    void takeFromShelf()
    {
        Bank fresh;
        if(!shelf::take(fresh)) { trouble = "nothing has been ripped yet"; return; }

        {
            std::lock_guard<std::mutex> guard(bankLock);
            bank = fresh;
        }

        followed = shelf::serial();
        trouble.clear();
        adopt();
        takeLight = 1.f;
    }

    // Looping is a bit in the last block of a sample's BRR, and the 64K is
    // ours: forcing every instrument to loop, or refusing to let any of them,
    // is one byte each.
    void applyLoopMode(int mode)
    {
        if(mode == lastLoopMode) return;
        lastLoopMode = mode;

        std::memcpy(chip.ram(), bank.ram(), AramSize);
        if(mode == 0) return;

        uint8_t* ram = chip.ram();

        for(const BrrSample& s : bank.samples())
        {
            const int last = s.start + (s.blocks - 1) * 9;
            if(last < 0 || last >= AramSize) continue;

            if(mode == 1) ram[last] |= 0x02;      // loop
            else          ram[last] &= (uint8_t)~0x02;
        }
    }

    // -----------------------------------------------------------------------

    int srcnFor(float knob, float cv)
    {
        if(bank.samples().empty()) return (int)clamp(knob, 0.f, 255.f);

        // The knob walks the instruments that are really there, not all 256
        // entries of a directory most of which a game never filled in.
        const int n = (int)bank.samples().size();
        int slot = (int)std::floor(knob * n / 256.f + cv * n / 10.f + 0.5f);
        return bank.srcnAt(slot);
    }

    void process(const ProcessArgs& args) override
    {
        if(loadTrigger.process(params[LoadParam].getValue() > 0.f)) wantLoad.store(true);
        if(takeTrigger.process(params[TakeParam].getValue() > 0.f)) wantTake.store(true);

        if(prevTrigger.process(params[PrevParam].getValue() > 0.f))
            nudge(-1);
        if(nextTrigger.process(params[NextParam].getValue() > 0.f))
            nudge(+1);

        if(wantLoad.exchange(false)) askForFile();
        if(wantTake.exchange(false)) takeFromShelf();

        if(autoFollow && shelf::serial() != followed && shelf::serial() != 0)
            takeFromShelf();

        takeLight = std::max(0.f, takeLight - args.sampleTime * 2.f);
        lights[TakeLight].setBrightness(takeLight);

        applyLoopMode((int)params[LoopParam].getValue());
        Sdsp::setCubic(params[InterpParam].getValue() > 0.5f);

        playNotes();
        setEcho();

        // --- the chip, at its own rate, resampled to the rack's ------------
        const double step = ApuRate / args.sampleRate;
        constexpr int N = 4 + 2 * VoiceCount + VoiceCount;
        float flat[N];

        resample.advance(step);
        while(resample.hungry())
        {
            chip.run(pendingIn);

            int k = 0;
            flat[k++] = pendingIn.main[0]; flat[k++] = pendingIn.main[1];
            flat[k++] = pendingIn.echo[0]; flat[k++] = pendingIn.echo[1];
            for(int v = 0; v < VoiceCount; ++v)
            {
                flat[k++] = pendingIn.voice[v][0];
                flat[k++] = pendingIn.voice[v][1];
            }
            for(int v = 0; v < VoiceCount; ++v) flat[k++] = pendingIn.dry[v];

            resample.push(flat);
        }

        resample.read(flat);

        const float gain = 5.f;

        outputs[MixLOutput].setVoltage(flat[0] * gain);
        outputs[MixROutput].setVoltage(flat[1] * gain);
        outputs[EchoLOutput].setVoltage(flat[2] * gain);
        outputs[EchoROutput].setVoltage(flat[3] * gain);

        outputs[VoicesOutput].setChannels(VoiceCount);
        outputs[DryOutput].setChannels(VoiceCount);

        for(int v = 0; v < VoiceCount; ++v)
        {
            const float l = flat[4 + v * 2];
            const float r = flat[4 + v * 2 + 1];
            outputs[VoicesOutput].setVoltage((l + r) * gain, v);
            outputs[DryOutput].setVoltage(flat[4 + VoiceCount * 2 + v] * gain, v);
        }
    }

    void nudge(int by)
    {
        if(bank.samples().empty()) return;

        const int n = (int)bank.samples().size();
        const float per = 256.f / n;
        float now = params[SampleParam].getValue();
        int slot = (int)std::floor(now / per + 0.5f) + by;
        slot = ((slot % n) + n) % n;
        params[SampleParam].setValue(clamp(slot * per, 0.f, 255.f));
    }

    // -----------------------------------------------------------------------
    // Playing
    // -----------------------------------------------------------------------
    void playNotes()
    {
        const int channels = std::max(inputs[GateInput].getChannels(),
                                      inputs[PitchInput].getChannels());

        const float knob   = params[SampleParam].getValue();
        const float tune   = params[TuneParam].getValue() + params[FineParam].getValue()
                           + inputs[TuneInput].getVoltage() * 12.f;
        const float level  = params[LevelParam].getValue();
        const float pan    = params[PanParam].getValue();

        const uint8_t adsr0 = (uint8_t)(0x80
                            | ((int)params[DecayParam].getValue()  & 7) << 4
                            | ((int)params[AttackParam].getValue() & 15));
        const uint8_t adsr1 = (uint8_t)(((int)params[SustainParam].getValue() & 7) << 5
                            | ((int)params[ReleaseParam].getValue() & 31));

        uint8_t kon = 0, koff = 0;

        for(int v = 0; v < VoiceCount; ++v)
        {
            const bool inRange = v < channels;

            const float gateV = inRange ? inputs[GateInput].getPolyVoltage(v) : 0.f;
            const bool  rise  = gateTrigger[v].process(gateV, 0.1f, 1.f);
            const bool  down  = gateV > 1.f;

            if(!down)
            {
                if(voice[v].held) { koff |= (uint8_t)(1 << v); voice[v].held = false; }
                continue;
            }

            const float cv    = inputs[SampleInput].getPolyVoltage(v);
            const int   srcn  = srcnFor(knob, cv);

            const float volts = (inRange ? inputs[PitchInput].getPolyVoltage(v) : 0.f)
                              + tune / 12.f;
            const int   pitch = voltsToPitch(volts);

            const float chanLevel = level
                * (inputs[LevelInput].isConnected()
                   ? clamp(inputs[LevelInput].getPolyVoltage(v) / 10.f, 0.f, 1.f) : 1.f);

            const float chanPan = clamp(pan + inputs[PanInput].getPolyVoltage(v) / 5.f,
                                        -1.f, 1.f);

            // Equal power either side, in the chip's own signed sevens.
            const int left  = (int)(chanLevel * 127.f * std::cos((chanPan + 1.f) * 0.25f * M_PI * 2.f));
            const int right = (int)(chanLevel * 127.f * std::sin((chanPan + 1.f) * 0.25f * M_PI * 2.f));

            chip.write(dspreg::voice(v, dspreg::VolL), (uint8_t)clamp(left,  0, 127));
            chip.write(dspreg::voice(v, dspreg::VolR), (uint8_t)clamp(right, 0, 127));
            chip.write(dspreg::voice(v, dspreg::PitchL), (uint8_t)(pitch & 0xff));
            chip.write(dspreg::voice(v, dspreg::PitchH), (uint8_t)(pitch >> 8));

            if(rise || voice[v].srcn != srcn)
            {
                chip.write(dspreg::voice(v, dspreg::Srcn), (uint8_t)srcn);
                voice[v].srcn = srcn;
            }

            chip.write(dspreg::voice(v, dspreg::Adsr0), adsr0);
            chip.write(dspreg::voice(v, dspreg::Adsr1), adsr1);

            if(rise) { kon |= (uint8_t)(1 << v); voice[v].held = true; }
            voice[v].volts = volts;
        }

        // KON and KOFF are edges to the chip, not states: write the bits that
        // changed this sample and then write nothing.
        chip.write(dspreg::Koff, koff);
        chip.write(dspreg::Kon,  kon);
    }

    void setEcho()
    {
        const float send = params[EchoParam].getValue();
        const int   fb   = (int)(params[FeedbackParam].getValue() * 100.f);
        const int   edl  = (int)params[TimeParam].getValue();

        chip.write(dspreg::EVolL, (uint8_t)(int)(send * 127.f));
        chip.write(dspreg::EVolR, (uint8_t)(int)(send * 127.f));
        chip.write(dspreg::Efb,   (uint8_t)fb);
        chip.write(dspreg::Edl,   (uint8_t)edl);
        chip.write(dspreg::Esa,   0x00);
        chip.write(dspreg::Eon,   send > 0.001f ? 0xff : 0x00);

        // A flat first tap and nothing after it: the echo most games use, and
        // the one that does not colour what goes into it.
        chip.write(dspreg::Fir, 0x7f);
        for(int t = 1; t < 8; ++t) chip.write((uint8_t)(t * 0x10 + dspreg::Fir), 0x00);

        // Bit 5 stops the chip writing its echo buffer at all. Our echo has a
        // 64K of its own -- away from the instruments, where on real hardware
        // it would be eating them -- so it can simply be on.
        chip.write(dspreg::Flg, 0x00);
    }

    // -----------------------------------------------------------------------

    void askForFile()
    {
        osdialog_filters* filters =
            osdialog_filters_parse("A song or a sample:spc,brr,bin");
        char* path = osdialog_file(OSDIALOG_OPEN, NULL, NULL, filters);
        osdialog_filters_free(filters);

        if(!path) return;

        std::string p = path;
        free(path);

        std::string error;
        const bool spc = string::lowercase(system::getExtension(p)) == ".spc";

        bool ok;
        {
            std::lock_guard<std::mutex> guard(bankLock);
            ok = spc ? bank.loadSpc(p, error) : bank.loadRaw(p, error);
        }

        if(ok) { trouble.clear(); adopt(); }
        else   trouble = error;
    }

    json_t* dataToJson() override
    {
        json_t* root = json_object();

        json_object_set_new(root, "follow", json_boolean(autoFollow));

        // The instruments travel inside the patch, packed: a bank is 64K and
        // mostly zeroes, and a patch that opened without its samples would be
        // a patch that opened silent.
        if(bank.valid())
        {
            json_object_set_new(root, "bank", json_string(bank.encode().c_str()));
            json_object_set_new(root, "name", json_string(bank.name()));
        }

        return root;
    }

    void dataFromJson(json_t* root) override
    {
        if(json_t* v = json_object_get(root, "follow")) autoFollow = json_boolean_value(v);

        if(json_t* v = json_object_get(root, "bank"))
        {
            bool ok;
            {
                std::lock_guard<std::mutex> guard(bankLock);
                ok = bank.decodeFrom(json_string_value(v));
            }

            if(ok) { followed = shelf::serial(); adopt(); }
        }
    }
};

// ===========================================================================

struct SamplerWidget : ModuleWidget {

    SamplerWidget(SamplerModule* module)
    {
        setModule(module);
        box.size = Vec(RACK_GRID_WIDTH * L::Hp, RACK_GRID_HEIGHT);

        addChild(new PaintedPanel(box.size.x, panel::paintSampler));

        addChild(createWidget<SnesScrew>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<SnesScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH,
                                               RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        addScope(module);

        const int buttons[4] = { SamplerModule::LoadParam, SamplerModule::TakeParam,
                                 SamplerModule::PrevParam, SamplerModule::NextParam };
        const NVGcolor tint[4] = { sfc::red(), sfc::violet(), sfc::blue(), sfc::blue() };

        for(int i = 0; i < 4; ++i)
            addParam(buttonAt(module, buttons[i], L::colX(i + 1), L::ButtonY,
                              panel::samplerButtonIcons()[i], tint[i]));

        const int row1[6] = { SamplerModule::SampleParam, SamplerModule::TuneParam,
                              SamplerModule::FineParam,   SamplerModule::PanParam,
                              SamplerModule::LevelParam,  SamplerModule::EchoParam };
        const int row2[6] = { SamplerModule::AttackParam,  SamplerModule::DecayParam,
                              SamplerModule::SustainParam, SamplerModule::ReleaseParam,
                              SamplerModule::FeedbackParam, SamplerModule::TimeParam };

        for(int i = 0; i < 6; ++i)
        {
            addParam(knobAt(module, row1[i], L::colX(i), L::KnobRow1, 11.0f));
            addParam(knobAt(module, row2[i], L::colX(i), L::KnobRow2,  9.0f));
        }

        ChoiceParam* loop = new ChoiceParam(L::ChoiceW, L::ChoiceH, "AS AUTHORED");
        loop->box.pos = mm2px(Vec(L::Left + 1.f, L::ChoiceY)) - loop->box.size.mult(0.f);
        loop->box.pos = mm2px(Vec(L::Left + 1.f, L::ChoiceY));
        loop->module = module;
        loop->paramId = SamplerModule::LoopParam;
        loop->tint = sfc::green();
        loop->initParamQuantity();
        addParam(loop);

        ChoiceParam* interp = new ChoiceParam(L::ChoiceW, L::ChoiceH, "GAUSSIAN");
        interp->box.pos = mm2px(Vec(L::Right - L::ChoiceW - 1.f, L::ChoiceY));
        interp->module = module;
        interp->paramId = SamplerModule::InterpParam;
        interp->tint = sfc::blue();
        interp->initParamQuantity();
        addParam(interp);

        const int ins[6] = { SamplerModule::PitchInput, SamplerModule::GateInput,
                             SamplerModule::SampleInput, SamplerModule::LevelInput,
                             SamplerModule::PanInput, SamplerModule::TuneInput };
        const int outs[6] = { SamplerModule::MixLOutput, SamplerModule::MixROutput,
                              SamplerModule::VoicesOutput, SamplerModule::DryOutput,
                              SamplerModule::EchoLOutput, SamplerModule::EchoROutput };

        for(int i = 0; i < 6; ++i)
        {
            addInput(createInputCentered<SnesPort>(
                mm2px(Vec(L::colX(i), L::InRow)), module, ins[i]));
            addOutput(createOutputCentered<SnesPort>(
                mm2px(Vec(L::colX(i), L::OutRow)), module, outs[i]));
        }
    }

    void addScope(SamplerModule* module)
    {
        // The shape of the instrument, and above it what it is.
        WaveWidget* wave = new WaveWidget;
        wave->box.pos  = mm2px(Vec(L::Left + 1.f, L::ScopeY + 8.0f));
        wave->box.size = mm2px(Vec(L::Right - L::Left - 2.f, L::ScopeH - 9.0f));

        wave->source = [module]() -> const std::vector<float>* {
            if(!module) return nullptr;

            std::lock_guard<std::mutex> guard(module->bankLock);

            const int srcn = module->srcnFor(module->params[SamplerModule::SampleParam].getValue(),
                                             0.f);

            if(srcn != module->shownSrcn || module->bank.serial() != module->shownStamp)
            {
                module->shownSrcn  = srcn;
                module->shownStamp = module->bank.serial();
                module->bank.decode(srcn, module->shown, 40000);

                module->shownLoop = -1.f;
                const int slot = module->bank.slotOf(srcn);
                if(slot >= 0)
                {
                    const BrrSample& s = module->bank.samples()[(size_t)slot];
                    if(s.loops && s.blocks > 0)
                        module->shownLoop = (float)(s.loop - s.start) / (float)(s.blocks * 9);
                }
            }

            return &module->shown;
        };

        wave->loopPoint = [module]() { return module ? module->shownLoop : -1.f; };
        addChild(wave);

        TextPlate* plate = new TextPlate;
        plate->box.pos  = mm2px(Vec(L::Left + 1.f, L::ScopeY + 0.5f));
        plate->box.size = mm2px(Vec(L::Right - L::Left - 2.f, 8.0f));

        plate->lines = [module]() {
            std::vector<TextPlate::Line> lines;

            if(module) module->bankLock.lock();
            struct Unlock {
                SamplerModule* m;
                ~Unlock() { if(m) m->bankLock.unlock(); }
            } unlock { module };

            if(!module || !module->bank.valid())
            {
                lines.push_back({ "no instruments yet", 2.4f, sfc::grey(), NVG_ALIGN_LEFT });
                lines.push_back({ "TAKE what a SNES ripped, or LOAD an .spc",
                                  1.7f, sfc::grey(), NVG_ALIGN_LEFT });
                return lines;
            }

            lines.push_back({ module->bank.name(), 2.4f, sfc::white(), NVG_ALIGN_LEFT });

            const int srcn = module->shownSrcn;
            const int slot = module->bank.slotOf(srcn);

            char buf[96];
            if(slot >= 0)
            {
                const BrrSample& s = module->bank.samples()[(size_t)slot];
                std::snprintf(buf, sizeof buf, "%d of %d   srcn %d   %.3f s%s",
                              slot + 1, (int)module->bank.samples().size(),
                              srcn, s.seconds(), s.loops ? "   loops" : "");
            }
            else std::snprintf(buf, sizeof buf, "srcn %d   nothing there", srcn);

            lines.push_back({ buf, 1.7f, sfc::silver(), NVG_ALIGN_LEFT });
            return lines;
        };

        addChild(plate);
    }

    void appendContextMenu(Menu* menu) override
    {
        SamplerModule* m = dynamic_cast<SamplerModule*>(module);
        if(!m) return;

        menu->addChild(new MenuSeparator);
        menu->addChild(createBoolPtrMenuItem("Follow whatever a SNES rips", "", &m->autoFollow));

        if(m->bank.valid())
        {
            menu->addChild(new MenuSeparator);
            char buf[64];
            std::snprintf(buf, sizeof buf, "%d instruments, directory at $%04x",
                          (int)m->bank.samples().size(), m->bank.dirPage() * 0x100);
            menu->addChild(createMenuLabel(buf));
        }
    }
};

} // namespace racksnes

Model* modelSampler = createModel<racksnes::SamplerModule, racksnes::SamplerWidget>("Sampler");
