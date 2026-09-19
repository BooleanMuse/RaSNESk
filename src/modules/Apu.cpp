// ===========================================================================
// APU -- the sound chip with the console taken away.
//
// An SPC-700, its 64K and an S-DSP. That is the whole of what makes a Super
// Nintendo sing, and it is exactly what an .spc file is: somebody stopped a
// game mid-song, wrote down the sound chip and threw the rest of the console
// away, and the music carried on. It carries on here too.
//
// The four ports are the only wire the console's processor ever had to the
// sound chip. Writing them from a rack is writing them the way the game's own
// code did -- which is how a cartridge's sound driver can be asked for a
// different song by somebody who is not the cartridge.
// ===========================================================================
#include "../plugin.hpp"
#include "../widgets/Widgets.hpp"
#include "../snes/Apu.hpp"
#include "../snes/Bank.hpp"
#include "../snes/Sdsp.hpp"
#include "../Path.hpp"

#include <osdialog.h>

#include <atomic>
#include <string>

namespace racksnes {

namespace L = layout::apu;

struct ApuModule : Module {

    enum ParamId {
        PortParam,                     // four of them
        SongParam = PortParam + 4,
        RateParam, VolumeParam,
        LoadParam, TakeParam, ResetParam,
        ParamCount
    };

    enum InputId {
        PortInput,                     // four of them
        WriteInput = PortInput + 4,
        InputCount
    };

    enum OutputId {
        MixLOutput, MixROutput, VoicesOutput, PitchOutput, GateOutput,
        OutputCount
    };

    enum LightId { TakeLight, LightCount };

    Apu  apu;
    Bank bank;
    uint32_t followed = 0;
    bool     autoFollow = true;

    std::atomic<bool> wantLoad{false};
    std::atomic<bool> wantTake{false};
    std::atomic<bool> wantReset{false};

    std::string trouble;
    std::string what;

    dsp::SchmittTrigger writeTrigger;
    dsp::BooleanTrigger loadTrigger, takeTrigger, resetTrigger;

    uint8_t lastPort[4] = { 0, 0, 0, 0 };
    int     lastSong = -1;
    bool    writeOnChange = true;

    Resampler<4 + 2 * VoiceCount + VoiceCount> resample;
    ChipFrame pending;
    float takeLight = 0.f;

    ApuModule()
    {
        config(ParamCount, InputCount, OutputCount, LightCount);

        for(int p = 0; p < 4; ++p)
        {
            configParam(PortParam + p, 0.f, 255.f, 0.f,
                        "Port " + std::to_string(p));
            getParamQuantity(PortParam + p)->snapEnabled = true;
            configInput(PortInput + p, "Port " + std::to_string(p));
        }

        configParam(SongParam, 0.f, 255.f, 0.f, "Song");
        getParamQuantity(SongParam)->snapEnabled = true;

        configParam(RateParam, -2.f, 2.f, 0.f, "Rate", " V");
        configParam(VolumeParam, 0.f, 2.f, 1.f, "Volume", "x");

        configButton(LoadParam,  "Load an .spc");
        configButton(TakeParam,  "Take whatever a SNES last ripped");
        configButton(ResetParam, "Reset the chip");

        configInput(WriteInput, "Write the ports");

        configOutput(MixLOutput,  "Mix left");
        configOutput(MixROutput,  "Mix right");
        configOutput(VoicesOutput,"Voices (polyphonic, eight)");
        configOutput(PitchOutput, "Pitch (polyphonic, eight)");
        configOutput(GateOutput,  "Gate (polyphonic, eight)");
    }

    void takeFromShelf()
    {
        Bank fresh;
        if(!shelf::take(fresh)) { trouble = "nothing has been ripped yet"; return; }

        bank = fresh;
        followed = shelf::serial();
        what = bank.name();
        trouble.clear();

        // A bank is a cartridge's 64K without a processor state to go with
        // it. The driver is in there; it gets started the way the console
        // starts it, from the boot ROM.
        apu.loadBank(bank);
        takeLight = 1.f;
    }

    void process(const ProcessArgs& args) override
    {
        if(loadTrigger.process(params[LoadParam].getValue() > 0.f))   wantLoad.store(true);
        if(takeTrigger.process(params[TakeParam].getValue() > 0.f))   wantTake.store(true);
        if(resetTrigger.process(params[ResetParam].getValue() > 0.f)) wantReset.store(true);

        if(wantLoad.exchange(false))  askForSpc();
        if(wantTake.exchange(false))  takeFromShelf();
        if(wantReset.exchange(false)) apu.reset();

        if(autoFollow && shelf::serial() != followed && shelf::serial() != 0)
            takeFromShelf();

        takeLight = std::max(0.f, takeLight - args.sampleTime * 2.f);
        lights[TakeLight].setBrightness(takeLight);

        writePorts();

        // --- the crystal ---------------------------------------------------
        const double speed = std::pow(2.0, (double)clamp(params[RateParam].getValue(), -4.f, 4.f));
        const double step  = ApuRate * speed / args.sampleRate;

        constexpr int N = 4 + 2 * VoiceCount + VoiceCount;
        float flat[N];

        resample.advance(step);
        while(resample.hungry())
        {
            apu.run(pending);

            int k = 0;
            flat[k++] = pending.main[0]; flat[k++] = pending.main[1];
            flat[k++] = pending.echo[0]; flat[k++] = pending.echo[1];
            for(int v = 0; v < VoiceCount; ++v)
            {
                flat[k++] = pending.voice[v][0];
                flat[k++] = pending.voice[v][1];
            }
            for(int v = 0; v < VoiceCount; ++v) flat[k++] = pending.dry[v];

            resample.push(flat);
        }

        resample.read(flat);

        const float gain = 5.f * params[VolumeParam].getValue();

        outputs[MixLOutput].setVoltage(flat[0] * gain);
        outputs[MixROutput].setVoltage(flat[1] * gain);

        outputs[VoicesOutput].setChannels(VoiceCount);
        outputs[PitchOutput].setChannels(VoiceCount);
        outputs[GateOutput].setChannels(VoiceCount);

        const VoiceState* v = apu.voices();

        for(int i = 0; i < VoiceCount; ++i)
        {
            outputs[VoicesOutput].setVoltage(flat[4 + VoiceCount * 2 + i] * gain, i);
            outputs[PitchOutput].setVoltage(v[i].volts, i);
            outputs[GateOutput].setVoltage(v[i].keyed ? 10.f : 0.f, i);
        }
    }

    void writePorts()
    {
        uint8_t want[4];
        for(int p = 0; p < 4; ++p)
        {
            float v = params[PortParam + p].getValue();
            if(inputs[PortInput + p].isConnected())
                v = clamp(inputs[PortInput + p].getVoltage(), 0.f, 10.f) * 25.5f;
            want[p] = (uint8_t)clamp((int)(v + 0.5f), 0, 255);
        }

        const int song = (int)params[SongParam].getValue();
        const bool trig = writeTrigger.process(inputs[WriteInput].getVoltage(), 0.1f, 1.f);

        // SONG is the convenience: most sound drivers in the Nintendo family
        // take a track number on port 0 and start playing it. It is a
        // convention, not a standard -- the four knobs are the general case,
        // and a game whose driver wants something else wants it on those.
        if(song != lastSong)
        {
            lastSong = song;
            apu.writePort(0, (uint8_t)song);
            for(int p = 0; p < 4; ++p) lastPort[p] = want[p];
            return;
        }

        if(trig || writeOnChange)
        {
            for(int p = 0; p < 4; ++p)
            {
                if(!trig && want[p] == lastPort[p]) continue;
                lastPort[p] = want[p];
                apu.writePort(p, want[p]);
            }
        }
    }

    void askForSpc()
    {
        osdialog_filters* filters = osdialog_filters_parse("A song:spc");
        char* path = osdialog_file(OSDIALOG_OPEN, NULL, NULL, filters);
        osdialog_filters_free(filters);

        if(!path) return;

        std::string p = normalizeSeparators(path);
        free(path);

        std::string error;
        if(apu.loadSpc(p, error))
        {
            bank.loadSpc(p, error);
            what = apu.title();
            trouble.clear();
        }
        else trouble = error;
    }

    json_t* dataToJson() override
    {
        json_t* root = json_object();
        json_object_set_new(root, "follow", json_boolean(autoFollow));
        json_object_set_new(root, "writeOnChange", json_boolean(writeOnChange));

        if(bank.valid())
        {
            json_object_set_new(root, "bank", json_string(bank.encode().c_str()));
            json_object_set_new(root, "name", json_string(what.c_str()));
        }

        return root;
    }

    void dataFromJson(json_t* root) override
    {
        if(json_t* v = json_object_get(root, "follow")) autoFollow = json_boolean_value(v);
        if(json_t* v = json_object_get(root, "writeOnChange")) writeOnChange = json_boolean_value(v);
        if(json_t* v = json_object_get(root, "name")) what = json_string_value(v);

        if(json_t* v = json_object_get(root, "bank"))
        {
            if(bank.decodeFrom(json_string_value(v)))
            {
                followed = shelf::serial();
                apu.loadBank(bank);
            }
        }
    }
};

// ===========================================================================

struct ApuWidget : ModuleWidget {

    ApuWidget(ApuModule* module)
    {
        setModule(module);
        box.size = Vec(RACK_GRID_WIDTH * L::Hp, RACK_GRID_HEIGHT);

        addChild(new PaintedPanel(box.size.x, panel::paintApu));

        addChild(createWidget<SnesScrew>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<SnesScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH,
                                               RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        addPlate(module);

        const int buttons[3] = { ApuModule::LoadParam, ApuModule::TakeParam,
                                 ApuModule::ResetParam };
        const NVGcolor tint[3] = { sfc::red(), sfc::violet(), sfc::yellow() };

        for(int i = 0; i < 3; ++i)
            addParam(buttonAt(module, buttons[i], L::colX(i + 1), L::ButtonY,
                              panel::apuButtonIcons()[i], tint[i]));

        for(int p = 0; p < 4; ++p)
        {
            addParam(knobAt(module, ApuModule::PortParam + p, L::colX(p), L::PortKnob, 9.0f));
            addInput(createInputCentered<SnesPort>(
                mm2px(Vec(L::colX(p), L::PortJack)), module, ApuModule::PortInput + p));
        }

        addParam(knobAt(module, ApuModule::SongParam, L::colX(4), L::PortKnob, 11.0f));
        addInput(createInputCentered<SnesPort>(
            mm2px(Vec(L::colX(4), L::PortJack)), module, ApuModule::WriteInput));

        addParam(knobAt(module, ApuModule::RateParam,   L::colX(0), L::WriteY, 11.0f));
        addParam(knobAt(module, ApuModule::VolumeParam, L::colX(1), L::WriteY, 11.0f));

        const int outs[5] = { ApuModule::MixLOutput, ApuModule::MixROutput,
                              ApuModule::VoicesOutput, ApuModule::PitchOutput,
                              ApuModule::GateOutput };
        for(int i = 0; i < 5; ++i)
            addOutput(createOutputCentered<SnesPort>(
                mm2px(Vec(L::colX(i), L::OutRow)), module, outs[i]));
    }

    void addPlate(ApuModule* module)
    {
        TextPlate* plate = new TextPlate;
        plate->box.pos  = mm2px(Vec(L::Left + 1.f, L::PlateY + 0.5f));
        plate->box.size = mm2px(Vec(L::Right - L::Left - 2.f, 9.0f));

        plate->lines = [module]() {
            std::vector<TextPlate::Line> lines;

            if(!module || !module->apu.loaded())
            {
                lines.push_back({ "nothing loaded", 2.4f, sfc::grey(), NVG_ALIGN_LEFT });
                lines.push_back({ "LOAD an .spc, or TAKE from a SNES",
                                  1.7f, sfc::grey(), NVG_ALIGN_LEFT });
                return lines;
            }

            lines.push_back({ module->what, 2.4f, sfc::white(), NVG_ALIGN_LEFT });

            char buf[64];
            std::snprintf(buf, sizeof buf, "pc $%04x   ports %02x %02x %02x %02x",
                          module->apu.programCounter(),
                          module->apu.readPort(0), module->apu.readPort(1),
                          module->apu.readPort(2), module->apu.readPort(3));
            lines.push_back({ buf, 1.7f, sfc::silver(), NVG_ALIGN_LEFT });

            return lines;
        };

        addChild(plate);

        VoiceMeters* meters = new VoiceMeters;
        meters->box.pos  = mm2px(Vec(L::Left + 1.f, L::PlateY + 9.0f));
        meters->box.size = mm2px(Vec(L::Right - L::Left - 2.f, 24.0f));
        meters->rowPitch = 3.0f;
        meters->source   = [module]() -> const VoiceState* {
            return module ? module->apu.voices() : nullptr;
        };
        addChild(meters);
    }

    void appendContextMenu(Menu* menu) override
    {
        ApuModule* m = dynamic_cast<ApuModule*>(module);
        if(!m) return;

        menu->addChild(new MenuSeparator);
        menu->addChild(createBoolPtrMenuItem("Follow whatever a SNES rips", "", &m->autoFollow));
        menu->addChild(createBoolPtrMenuItem("Write a port as soon as it changes", "",
                                             &m->writeOnChange));
        menu->addChild(createMenuLabel("   otherwise they go together, on WRITE"));
    }
};

} // namespace racksnes

Model* modelApu = createModel<racksnes::ApuModule, racksnes::ApuWidget>("Apu");
