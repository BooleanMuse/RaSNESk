// ===========================================================================
// SNES -- the console.
//
// A Super Nintendo on a panel, and its sound chip taken apart on the way out.
//
// Two things here are worth knowing before reading:
//
// * There is one console per process, because bsnes is built out of global
//   singletons. A second SNES module says so on its screen rather than
//   quietly sharing a CPU with the first.
//
// * Everything that touches the console happens in process(), on the engine
//   thread. bsnes schedules its chips as cooperative threads and a thread is
//   resumed by switching to a stack; building those stacks on the thread that
//   ran the file dialog would build them on a thread that never runs them
//   again. So the buttons set a request and process() does the work.
// ===========================================================================
#include "../plugin.hpp"
#include "../widgets/Widgets.hpp"
#include "../snes/Console.hpp"
#include "../snes/Bank.hpp"
#include "../snes/Sdsp.hpp"
#include "../Gamepad.hpp"
#include "../Path.hpp"

#include <osdialog.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

namespace racksnes {

namespace L = layout::snes;

// The panel's gamepad row and the console's button order are the same list.
// If one ever moves, this stops the build rather than the game.
static_assert(ButtonCount == 12, "the gamepad has twelve buttons");

struct SnesModule : Module {

    enum ParamId {
        RateParam, VolumeParam,
        LoadParam, PrevParam, NextParam, RunParam, ResetParam, RipParam,
        ParamCount
    };

    enum InputId {
        PadInput,                                    // twelve, one to a button
        Pad2Input = PadInput + 12,
        ClockInput, RateCvInput, ResetInput, PortInput, WriteInput,
        InputCount
    };

    enum OutputId {
        MixLOutput, MixROutput,
        VoiceOutput,                                 // eight, one to a voice
        PitchOutput = VoiceOutput + VoiceCount,
        GateOutput, LevelOutput, SrcnOutput,
        EchoLOutput, EchoROutput,
        OutputCount
    };

    enum LightId { RunLight, RipLight, LightCount };

    Console console;
    bool    tried = false;          // have we asked for the console yet
    bool    mine  = false;

    // The two letterboxes an expander writes into. In Rack it is the module
    // that *reads* a message that has to provide the buffers; a neighbour
    // writes into these and asks for a flip. Without them, a BENDER placed
    // beside this module writes through a null pointer and takes Rack with
    // it -- which is not a thing the module browser or a probe can see,
    // because it needs two modules and one of them next to the other.
    BendMessage fromLeft[2];
    BendMessage fromRight[2];

    // --- what the buttons ask for, done later on the right thread ---------
    std::atomic<bool> wantLoad{false};
    std::atomic<int>  wantStep{0};
    std::atomic<bool> wantReset{false};
    std::atomic<bool> wantRip{false};
    std::string       pendingPath;
    std::string       folder;
    std::string       trouble;

    // --- what the panel reads ---------------------------------------------
    std::string  cartName;
    std::string  cartRegion;
    uint32_t     ripSerial = 0;
    float        ripLight = 0.f;

    // --- running ----------------------------------------------------------
    bool  running = true;
    bool  strobe  = false;
    bool  overscan = false;
    bool  embed   = false;          // carry the cartridge inside the patch

    // How much of the machine to pay for. The PPU does not render a frame it
    // is not going to show, so skipping frames makes the whole console
    // cheaper -- and a patch that wants the music and not the picture can
    // have most of that back.
    int   frameSkip = 0;
    int   appliedSkip = -1;
    int   bufferDepth = 2048;       // of the chip's own samples: 64 ms
    int   appliedDepth = -1;

    // Only tools/probe sets this. It runs the module the way Rack runs it but
    // as fast as it can rather than in real time, so it would outrun a
    // machine that is only trying to keep up with a clock and then report the
    // silence as a fault.
    bool  offline = false;
    bool  appliedOffline = false;

    // Whether the machine has been failing to have samples ready lately.
    bool     behind = false;
    uint32_t lastMissed = 0;
    float    behindFor = 0.f;
    bool  fastDsp = true;
    bool  appliedFast = false;

    std::vector<ChipFrame> block;
    int   blockFill = 0;
    int   blockRead = 0;

    dsp::SchmittTrigger clockTrigger, writeTrigger, resetTrigger;
    dsp::BooleanTrigger loadTrigger, prevTrigger, nextTrigger, ripTrigger, runTrigger;

    // --- a real gamepad ---------------------------------------------------
    // Read on the drawing thread, because GLFW allows it nowhere else, and
    // handed over as one word: twelve bits, one to a button.
    std::atomic<uint16_t> padBits{0};
    std::atomic<bool>     padSeen{false};
    std::atomic<int>      learning{-1};        // which button is being taught
    Mapping               padMap = Mapping::standard();
    int                   padIndex = -1;       // -1 is whichever is plugged in
    bool                  padOn = true;
    std::string           padName;

    // And a keyboard, for when there is no pad. Off by default: while it is
    // on it takes its keys wherever the mouse is, which is right for playing
    // a game and wrong for everything else Rack does.
    std::atomic<uint16_t> keyBits{0};
    std::atomic<int>      learningKey{-1};
    KeyMap                keyMap = KeyMap::standard();
    bool                  keyOn = false;

    // The clock, measured rather than counted: what matters is how fast it is
    // going, because that is the machine's crystal.
    float clockPeriod = 0.f;
    float sinceClock  = 0.f;
    bool  clocked     = false;

    SnesModule()
    {
        config(ParamCount, InputCount, OutputCount, LightCount);

        configParam(RateParam, -3.f, 3.f, 0.f, "Rate", " V");
        configParam(VolumeParam, 0.f, 2.f, 1.f, "Volume", "x");

        configButton(LoadParam,  "Load a cartridge");
        configButton(PrevParam,  "The one before it in the folder");
        configButton(NextParam,  "The one after it");
        configButton(RunParam,   "Run");
        configButton(ResetParam, "Reset");
        configButton(RipParam,   "Take the sound chip's memory");

        for(int i = 0; i < 12; ++i)
            configInput(PadInput + i, panel::padLabels()[i]);

        configInput(Pad2Input,   "Gamepad 2 (polyphonic, one channel a button)");
        configInput(ClockInput,  "Clock -- the machine's crystal");
        configInput(RateCvInput, "Rate");
        configInput(ResetInput,  "Reset");
        configInput(PortInput,   "The four ports (polyphonic)");
        configInput(WriteInput,  "Write the ports");

        configOutput(MixLOutput, "Mix left");
        configOutput(MixROutput, "Mix right");

        for(int v = 0; v < VoiceCount; ++v)
        {
            std::string n = "Voice " + std::to_string(v + 1);
            configOutput(VoiceOutput + v, n);
        }

        configOutput(PitchOutput, "Pitch (polyphonic, eight voices)");
        configOutput(GateOutput,  "Gate (polyphonic, eight voices)");
        configOutput(LevelOutput, "Level (polyphonic, eight voices)");
        configOutput(SrcnOutput,  "Sample number (polyphonic, eight voices)");
        configOutput(EchoLOutput, "Echo return left");
        configOutput(EchoROutput, "Echo return right");

        block.resize(64);

        leftExpander.producerMessage  = &fromLeft[0];
        leftExpander.consumerMessage  = &fromLeft[1];
        rightExpander.producerMessage = &fromRight[0];
        rightExpander.consumerMessage = &fromRight[1];
    }

    // -----------------------------------------------------------------------

    // All four of these wait for the machine's own thread. The widget calls
    // them from step(); nothing here may be called from process().
    void serveRequests()
    {
        if(!mine) return;

        if(!wantReload.empty())
        {
            std::string path;
            path.swap(wantReload);
            doLoad(path);
        }

        if(wantSpc.exchange(false))  saveSpc();
        if(wantLoad.exchange(false)) askForCartridge();
        if(int by = wantStep.exchange(0)) step(by);
        if(wantRip.exchange(false))  rip();
    }

    void onReset(const ResetEvent& e) override
    {
        Module::onReset(e);
        wantReset.store(true);
    }

    void acquire()
    {
        if(tried) return;
        tried = true;
        mine  = console.claim();

        if(!mine)
            trouble = "another SNES module already has the console.\n"
                      "bsnes keeps one of everything, so there is one machine.";
    }

    // --- the cartridge -----------------------------------------------------
    void doLoad(const std::string& path)
    {
        std::string error;
        if(console.load(path, error))
        {
            cartName   = console.title();
            cartRegion = console.region();
            trouble.clear();
            console.setRunning(running);

            folder = folderOf(path);
            pendingPath = path;
            running = true;
        }
        else
        {
            trouble = error;
        }
    }

    // The rest of the folder the cartridge came from, in order, so that < and
    // > walk a shelf rather than a single file.
    void step(int by)
    {
        if(folder.empty() || pendingPath.empty()) return;

        std::vector<std::string> shelf;
        for(const std::string& name : system::getEntries(folder))
        {
            const std::string ext = string::lowercase(system::getExtension(name));
            if(ext == ".sfc" || ext == ".smc" || ext == ".swc" || ext == ".fig")
                shelf.push_back(name);
        }

        if(shelf.empty()) return;
        std::sort(shelf.begin(), shelf.end());

        int at = 0;
        for(size_t i = 0; i < shelf.size(); ++i)
            if(shelf[i] == pendingPath) { at = (int)i; break; }

        at = (at + by) % (int)shelf.size();
        if(at < 0) at += (int)shelf.size();

        doLoad(shelf[(size_t)at]);
    }

    // Take the sound chip's 64K and put it on the shelf: the driver the
    // cartridge uploaded, and every instrument it brought with it.
    //
    // Waits on the machine's thread, and the shelf it publishes to has a lock
    // of its own, so: the drawing thread, never the audio one.
    void rip()
    {
        if(!console.loaded()) return;

        uint8_t aram[AramSize];
        uint8_t regs[DspRegCount];
        if(!console.snapshotAram(aram, regs)) return;

        Bank bank;
        bank.adopt(aram, regs[dspreg::Dir], cartName.empty() ? "cartridge" : cartName);
        shelf::publish(bank);

        ripSerial = shelf::serial();
        ripLight  = 1.f;
    }

    // -----------------------------------------------------------------------

    void process(const ProcessArgs& args) override
    {
        acquire();

        // --- the buttons, now that we are on the right thread --------------
        if(loadTrigger.process(params[LoadParam].getValue() > 0.f)) wantLoad.store(true);
        if(prevTrigger.process(params[PrevParam].getValue() > 0.f)) wantStep.store(-1);
        if(nextTrigger.process(params[NextParam].getValue() > 0.f)) wantStep.store(+1);
        if(ripTrigger.process(params[RipParam].getValue()  > 0.f)) wantRip.store(true);
        if(runTrigger.process(params[RunParam].getValue() > 0.f))
        {
            running = !running;
            if(mine) console.setRunning(running);
        }

        if(resetTrigger.process(params[ResetParam].getValue() > 0.f
                                || inputs[ResetInput].getVoltage() > 1.f))
            wantReset.store(true);

        if(mine && appliedSkip != frameSkip)
        {
            appliedSkip = frameSkip;
            console.setFrameSkip(frameSkip);
        }

        if(mine && appliedOffline != offline)
        {
            appliedOffline = offline;
            console.setWaitForMachine(offline);
        }

        if(mine && appliedDepth != bufferDepth)
        {
            appliedDepth = bufferDepth;
            console.setBufferDepth(bufferDepth);
        }

        if(mine && appliedFast != fastDsp)
        {
            appliedFast = fastDsp;
            console.setFastDsp(fastDsp);
        }

        // Loading a cartridge, taking a copy of the sound chip, writing an
        // .spc: all of those wait on the machine's own thread, and the audio
        // thread waits for nothing. They happen in the widget's step().
        if(mine && wantReset.exchange(false) && console.loaded()) console.reset();

        // Once a second: has the machine missed anything since last time?
        behindFor += args.sampleTime;
        if(behindFor > 1.f)
        {
            behindFor = 0.f;
            const uint32_t missed = mine ? console.underruns() : 0;
            behind = missed > lastMissed + 64;
            lastMissed = missed;
        }

        lights[RunLight].setBrightness(running && console.loaded() ? 1.f : 0.f);
        ripLight = std::max(0.f, ripLight - args.sampleTime * 2.f);
        lights[RipLight].setBrightness(ripLight);

        // --- the gamepad ---------------------------------------------------
        readGamepad();

        // --- the ports, which is how a game is asked for a song ------------
        if(writeTrigger.process(inputs[WriteInput].getVoltage()) && console.loaded())
        {
            const int n = std::min(4, std::max(1, inputs[PortInput].getChannels()));
            for(int p = 0; p < n; ++p)
            {
                float v = clamp(inputs[PortInput].getVoltage(p), 0.f, 10.f);
                console.writeApuPort(p, (uint8_t)(v * 25.5f));
            }
        }

        // --- the crystal ---------------------------------------------------
        const double speed = measureSpeed(args);

        // --- and the sound -------------------------------------------------
        ChipFrame frame;

        if(mine && console.loaded())
        {
            if(blockRead >= blockFill)
            {
                console.setStrobe(strobe);
                console.run(block.data(), (int)block.size(), args.sampleRate,
                            strobe ? 1.0 : speed);
                blockFill = (int)block.size();
                blockRead = 0;
            }

            frame = block[blockRead++];
        }

        publish(frame);
        forwardBend();
    }

    void readGamepad()
    {
        if(!mine) return;

        // Whatever the pad and the keyboard are saying, as of the last frame
        // that was drawn. Either can press a button and neither can unpress
        // one the other is holding.
        const uint16_t pad = (padOn ? padBits.load(std::memory_order_relaxed) : 0)
                           | (keyOn ? keyBits.load(std::memory_order_relaxed) : 0);

        for(int b = 0; b < 12; ++b)
        {
            Input& in = inputs[PadInput + b];

            // A polyphonic cable presses the button for any channel that is
            // high, so a chord of triggers works on one jack.
            bool down = (pad >> b) & 1;

            for(int c = 0; c < in.getChannels() && !down; ++c)
                if(in.getVoltage(c) > 1.f) down = true;

            console.setButton(0, b, down);
        }

        Input& pad2 = inputs[Pad2Input];
        for(int b = 0; b < 12; ++b)
            console.setButton(1, b, b < pad2.getChannels() && pad2.getVoltage(b) > 1.f);
    }

    // CLOCK is the crystal, not a trigger for a frame. What matters is the
    // rate: a clock at half of sixty a second runs the whole machine at half
    // speed, and because the sound comes out of the same buffer the machine
    // fills, it sounds like a tape at half speed. That is one setting, not a
    // special mode.
    double measureSpeed(const ProcessArgs& args)
    {
        const float rate = params[RateParam].getValue()
                         + inputs[RateCvInput].getVoltage();

        double speed = std::pow(2.0, (double)clamp(rate, -6.f, 6.f));

        if(inputs[ClockInput].isConnected())
        {
            sinceClock += args.sampleTime;

            if(clockTrigger.process(inputs[ClockInput].getVoltage(), 0.1f, 1.f))
            {
                if(sinceClock > 1e-5f && sinceClock < 4.f)
                {
                    clockPeriod = sinceClock;
                    clocked = true;
                }
                sinceClock = 0.f;
            }

            // A clock that stopped should stop the machine, not leave it
            // running at whatever it was doing when the cable was pulled.
            if(clocked && sinceClock > clockPeriod * 4.f) clocked = false;

            const double nominal = console.region() == "PAL" ? 50.0069 : 60.0988;
            speed *= clocked ? (1.0 / clockPeriod) / nominal : 0.0;
        }

        return speed < 0.0 ? 0.0 : (speed > 8.0 ? 8.0 : speed);
    }

    void publish(const ChipFrame& f)
    {
        const float gain = 5.f * params[VolumeParam].getValue();

        outputs[MixLOutput].setVoltage(f.main[0] * gain);
        outputs[MixROutput].setVoltage(f.main[1] * gain);
        outputs[EchoLOutput].setVoltage(f.echo[0] * gain);
        outputs[EchoROutput].setVoltage(f.echo[1] * gain);

        for(int v = 0; v < VoiceCount; ++v)
            outputs[VoiceOutput + v].setVoltage(f.dry[v] * gain);

        const VoiceState* state = mine ? console.voices() : nullptr;

        for(Output* o : { &outputs[PitchOutput], &outputs[GateOutput],
                          &outputs[LevelOutput], &outputs[SrcnOutput] })
            o->setChannels(VoiceCount);

        for(int v = 0; v < VoiceCount; ++v)
        {
            const VoiceState s = state ? state[v] : VoiceState();

            outputs[PitchOutput].setVoltage(s.volts, v);
            outputs[GateOutput ].setVoltage(s.keyed ? 10.f : 0.f, v);
            outputs[LevelOutput].setVoltage(s.level * 10.f, v);
            outputs[SrcnOutput ].setVoltage(s.srcn * (10.f / 255.f), v);
        }
    }

    // -----------------------------------------------------------------------
    // BENDER, either side
    //
    // The message is only forwarded. Reaching into a running cartridge's
    // palette or its sound chip's memory has to happen on the thread the
    // machine runs on, and that thread belongs to Console.
    // -----------------------------------------------------------------------
    void forwardBend()
    {
        if(!mine) return;

        const BendMessage* bend = nullptr;

        if(leftExpander.module && leftExpander.module->model == modelBender)
            bend = (const BendMessage*)leftExpander.consumerMessage;

        if((!bend || !bend->present)
        && rightExpander.module && rightExpander.module->model == modelBender)
            bend = (const BendMessage*)rightExpander.consumerMessage;

        static const BendMessage nothing;
        console.bend(bend && bend->present ? *bend : nothing);
    }

    void askForCartridge()
    {
        // The dialog is modal and this is the engine thread, but Rack's own
        // modules do exactly this and the alternative -- loading on the UI
        // thread -- would build bsnes's chip stacks on a thread that never
        // runs them.
        char* dir = folder.empty() ? NULL : strdup(folder.c_str());
        osdialog_filters* filters =
            osdialog_filters_parse("Super Nintendo cartridge:sfc,smc,swc,fig,bin");

        char* path = osdialog_file(OSDIALOG_OPEN, dir, NULL, filters);

        osdialog_filters_free(filters);
        free(dir);

        if(!path) return;

        doLoad(normalizeSeparators(path));
        free(path);
    }

    json_t* dataToJson() override
    {
        json_t* root = json_object();

        json_object_set_new(root, "cartridge", json_string(pendingPath.c_str()));
        json_object_set_new(root, "running",   json_boolean(running));
        json_object_set_new(root, "strobe",    json_boolean(strobe));
        json_object_set_new(root, "overscan",  json_boolean(overscan));
        json_object_set_new(root, "embed",     json_boolean(embed));
        json_object_set_new(root, "frameSkip", json_integer(frameSkip));
        json_object_set_new(root, "fastDsp",   json_boolean(fastDsp));
        json_object_set_new(root, "buffer",    json_integer(bufferDepth));

        json_object_set_new(root, "gamepad",   json_boolean(padOn));
        json_object_set_new(root, "gamepadIndex", json_integer(padIndex));

        json_t* map = json_array();
        for(int i = 0; i < ButtonCount; ++i)
        {
            json_t* one = json_object();
            json_object_set_new(one, "kind",  json_integer(padMap.to[i].kind));
            json_object_set_new(one, "index", json_integer(padMap.to[i].index));
            json_object_set_new(one, "bit",   json_integer(padMap.to[i].bit));
            json_array_append_new(map, one);
        }
        json_object_set_new(root, "gamepadMap", map);

        json_object_set_new(root, "keyboard", json_boolean(keyOn));
        json_t* keys = json_array();
        for(int i = 0; i < ButtonCount; ++i)
            json_array_append_new(keys, json_integer(keyMap.to[i]));
        json_object_set_new(root, "keyboardMap", keys);

        // A cartridge is megabytes and a patch file is text, so the cartridge
        // travels only when asked. What always travels is where it was.
        if(embed && !console.romImage().empty())
        {
            const std::vector<uint8_t>& rom = console.romImage();
            json_object_set_new(root, "rom",
                json_stringn((const char*)rom.data(), rom.size()));
        }

        return root;
    }

    void dataFromJson(json_t* root) override
    {
        if(json_t* v = json_object_get(root, "running"))  running  = json_boolean_value(v);
        if(json_t* v = json_object_get(root, "strobe"))   strobe   = json_boolean_value(v);
        if(json_t* v = json_object_get(root, "overscan")) overscan = json_boolean_value(v);
        if(json_t* v = json_object_get(root, "embed"))    embed    = json_boolean_value(v);
        if(json_t* v = json_object_get(root, "frameSkip")) frameSkip = (int)json_integer_value(v);
        if(json_t* v = json_object_get(root, "fastDsp"))  fastDsp  = json_boolean_value(v);
        if(json_t* v = json_object_get(root, "buffer"))   bufferDepth = (int)json_integer_value(v);

        if(json_t* v = json_object_get(root, "cartridge"))
            pendingPath = json_string_value(v);

        if(json_t* v = json_object_get(root, "gamepad")) padOn = json_boolean_value(v);
        if(json_t* v = json_object_get(root, "gamepadIndex"))
            padIndex = (int)json_integer_value(v);

        if(json_t* v = json_object_get(root, "keyboard")) keyOn = json_boolean_value(v);

        if(json_t* keys = json_object_get(root, "keyboardMap"))
            for(int i = 0; i < ButtonCount && i < (int)json_array_size(keys); ++i)
                keyMap.to[i] = (int)json_integer_value(json_array_get(keys, i));

        if(json_t* map = json_object_get(root, "gamepadMap"))
        {
            for(int i = 0; i < ButtonCount && i < (int)json_array_size(map); ++i)
            {
                json_t* one = json_array_get(map, i);
                if(!one) continue;

                Binding b;
                if(json_t* v = json_object_get(one, "kind"))  b.kind  = (int)json_integer_value(v);
                if(json_t* v = json_object_get(one, "index")) b.index = (int)json_integer_value(v);
                if(json_t* v = json_object_get(one, "bit"))   b.bit   = (int)json_integer_value(v);
                padMap.to[i] = b;
            }
        }

        // Loading has to happen where bsnes can be touched, and that is not
        // here: dataFromJson runs when the patch is read.
        if(!pendingPath.empty()) wantReload = pendingPath;
    }

    int bufferChoice() const
    {
        if(bufferDepth <= 512)  return 0;
        if(bufferDepth <= 1024) return 1;
        if(bufferDepth <= 2048) return 2;
        return 3;
    }

    // Which of the four the menu should tick.
    int pictureChoice() const
    {
        if(frameSkip <= 0)  return 0;
        if(frameSkip == 1)  return 1;
        if(frameSkip <= 3)  return 2;
        return 3;
    }

    std::string       wantReload;
    std::atomic<bool> wantSpc{false};

    // A song, as the file every SNES music player reads. The console is not
    // needed to play one back: an .spc is the sound chip's 64K and its
    // registers, and the sound chip was never listening to the console in the
    // first place -- only to four bytes of it.
    void saveSpc()
    {
        if(!console.loaded()) return;

        std::vector<uint8_t> spc;
        if(!console.exportSpc(spc)) { trouble = "there was no song to take"; return; }

        std::string suggested = asFilename(cartName.empty() ? "song" : cartName);
        suggested += ".spc";

        osdialog_filters* filters = osdialog_filters_parse("SPC:spc");
        char* dir = folder.empty() ? NULL : strdup(folder.c_str());
        char* path = osdialog_file(OSDIALOG_SAVE, dir, suggested.c_str(), filters);
        osdialog_filters_free(filters);
        free(dir);

        if(!path) return;

        if(FILE* f = openBinary(normalizeSeparators(path), "wb"))
        {
            std::fwrite(spc.data(), 1, spc.size(), f);
            std::fclose(f);
        }
        free(path);
    }
};

// ===========================================================================

struct SnesWidget : ModuleWidget {

    SnesModule* snes = nullptr;

    SnesWidget(SnesModule* module)
    {
        setModule(module);
        snes = module;

        // Off RACK_GRID_WIDTH and RACK_GRID_HEIGHT, never off millimetres:
        // RackWidget::addModule compares the panel's height against the grid
        // exactly and throws if it misses, and 380 px is 128.693 mm, not the
        // 128.5 mm of a physical Eurorack panel.
        box.size = Vec(RACK_GRID_WIDTH * L::Hp, RACK_GRID_HEIGHT);

        addChild(new PaintedPanel(box.size.x, panel::paintSnes));

        addChild(createWidget<SnesScrew>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<SnesScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<SnesScrew>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<SnesScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH,
                                             RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        addScreen(module);
        addToolbar(module);
        addMeters(module);
        addKnobs(module);
        addJacks(module);

        // The controller's face on the deck lights up when a real one is
        // plugged in. That is everything this panel says about gamepads.
        MarkLight* light = new MarkLight;
        light->box.pos  = mm2px(Vec(L::ColR - 11.0f, 2.8f));
        light->box.size = mm2px(Vec(10.0f, 10.0f));
        light->radius   = 5.0f;
        light->live     = [module]() {
            if(!module) return false;
            return (module->padOn && module->padSeen.load(std::memory_order_relaxed))
                || module->keyOn;
        };
        light->learning = [module]() {
            return module && (module->learning.load() >= 0
                           || module->learningKey.load() >= 0);
        };
        // Clicking the controller's face is how you get at the mapping. It is
        // also in the right-click menu, but a panel with no words on it has to
        // put the thing where the hand looks for it.
        light->menu = [this, module](Menu* m) {
            if(!module) return;
            m->addChild(createSubmenuItem("Gamepad",
                module->padSeen.load() ? module->padName : std::string("none found"),
                [this, module](Menu* sub) { gamepadMenu(sub, module); }));
            m->addChild(createSubmenuItem("Keyboard", module->keyOn ? "on" : "off",
                [this, module](Menu* sub) { keyboardMenu(sub, module); }));
        };
        addChild(light);
    }

    // -----------------------------------------------------------------------
    // The gamepad, read where GLFW allows it to be read
    //
    // GLFW's joystick functions may only be called from the thread that
    // initialised it, which in Rack is this one. So the pad is polled once a
    // frame here and the answer goes to the module as a single word; nothing
    // ever reads a joystick from the audio thread.
    // -----------------------------------------------------------------------
    PadState padNow, padThen;
    int      padFound = -1;

    void step() override
    {
        ModuleWidget::step();
        if(!snes) return;

        pollPad();
        pollKeys();

        // The things that wait for the machine's own thread happen here, on
        // the thread that is allowed to wait.
        snes->serveRequests();
    }

    // The keyboard, read the same way and from the same thread.
    //
    // Nothing is read while a text field has the focus: a module that steals
    // the letters out of a search box is a module nobody keeps installed.
    void pollKeys()
    {
        if(!snes->keyOn)
        {
            snes->keyBits.store(0, std::memory_order_relaxed);
            return;
        }

        GLFWwindow* window = APP->window ? APP->window->win : nullptr;

        if(dynamic_cast<ui::TextField*>(APP->event->getSelectedWidget()))
        {
            snes->keyBits.store(0, std::memory_order_relaxed);
            return;
        }

        const int teaching = snes->learningKey.load(std::memory_order_relaxed);

        if(teaching >= 0 && teaching < ButtonCount)
        {
            const int key = firstKeyDown(window);
            if(key != GLFW_KEY_UNKNOWN)
            {
                // Escape means "none", so a button can be unbound.
                snes->keyMap.to[teaching] = key == GLFW_KEY_ESCAPE ? GLFW_KEY_UNKNOWN : key;
                snes->learningKey.store(-1, std::memory_order_relaxed);
            }

            snes->keyBits.store(0, std::memory_order_relaxed);
            return;
        }

        snes->keyBits.store(squashKeys(window, snes->keyMap), std::memory_order_relaxed);
    }

    void pollPad()
    {
        padThen = padNow;

        // A fixed choice, or else the first one that is plugged in.
        int which = snes->padIndex;

        if(which < 0)
        {
            which = -1;
            for(int j = 0; j <= GLFW_JOYSTICK_LAST; ++j)
                if(glfwJoystickPresent(j)) { which = j; break; }
        }

        padFound = which;

        if(which < 0 || !readPad(which, padNow))
        {
            snes->padSeen.store(false, std::memory_order_relaxed);
            snes->padBits.store(0, std::memory_order_relaxed);
            snes->padName.clear();
            return;
        }

        snes->padSeen.store(true, std::memory_order_relaxed);
        snes->padName = padNow.name;

        const int teaching = snes->learning.load(std::memory_order_relaxed);

        if(teaching >= 0 && teaching < ButtonCount)
        {
            // Whatever is pressed next becomes that button, and the learn
            // ends there. Nothing is written until something is pressed, so
            // changing your mind means pressing something else.
            Binding caught = firstPressed(padThen, padNow);
            if(caught.kind != Binding::None)
            {
                snes->padMap.to[teaching] = caught;
                snes->learning.store(-1, std::memory_order_relaxed);
            }

            snes->padBits.store(0, std::memory_order_relaxed);
            return;
        }

        snes->padBits.store(squash(padNow, snes->padMap), std::memory_order_relaxed);
    }

    void addScreen(SnesModule* module)
    {
        ScreenWidget* screen = new ScreenWidget;
        screen->box.pos  = mm2px(Vec(L::ScreenX, L::ScreenY));
        screen->box.size = mm2px(Vec(L::ScreenW, L::ScreenH));

        screen->source   = [module]() -> const uint8_t* {
            return module ? module->console.frame() : nullptr;
        };
        screen->overscan = [module]() { return module && module->overscan; };
        screen->message  = [module]() -> std::string {
            if(!module) return "SUPER NINTENDO";
            if(!module->trouble.empty()) return module->trouble;
            return "PRESS THE CARTRIDGE BUTTON AND PICK A .SFC";
        };

        addChild(screen);
    }

    // The row of pictures under the screen, which is the shape a Mario Paint
    // screen has, and the cartridge's name in a window beside them.
    void addToolbar(SnesModule* module)
    {
        const int param[L::ButtonCount] = {
            SnesModule::LoadParam, SnesModule::PrevParam, SnesModule::NextParam,
            SnesModule::RunParam,  SnesModule::ResetParam, SnesModule::RipParam,
        };
        const NVGcolor tint[L::ButtonCount] = {
            sfc::red(), sfc::blue(), sfc::blue(), sfc::green(), sfc::yellow(), sfc::violet(),
        };

        for(int i = 0; i < L::ButtonCount; ++i)
        {
            addParam(buttonAt(module, param[i], L::buttonX(i), L::ButtonY,
                              panel::snesButtonIcons()[i], tint[i], L::ButtonD));
        }

        // One line, because the toolbar is eight millimetres tall: the
        // cartridge's own name on the left, what it is doing on the right.
        TextPlate* name = new TextPlate;
        name->box.pos  = mm2px(Vec(L::PlateX + 1.4f, L::BarY + 2.7f));
        name->box.size = mm2px(Vec(L::PlateW * 0.62f, 3.0f));
        name->lines = [module]() {
            std::vector<TextPlate::Line> lines;
            lines.push_back({ module && !module->cartName.empty()
                                  ? module->cartName : std::string("NO CARTRIDGE"),
                              2.6f, sfc::ink(), NVG_ALIGN_LEFT });
            return lines;
        };
        addChild(name);

        TextPlate* state = new TextPlate;
        state->box.pos  = mm2px(Vec(L::PlateX + L::PlateW * 0.62f, L::BarY + 2.9f));
        state->box.size = mm2px(Vec(L::PlateW * 0.38f - 3.2f, 3.0f));
        state->lines = [module]() {
            std::vector<TextPlate::Line> lines;

            // Short, because the window is a centimetre wide: the region and
            // one word. Whether it has been ripped is the RIP button lighting
            // up, not more text in here.
            std::string what;
            if(module && module->console.loaded())
            {
                what = module->cartRegion;

                // BEHIND, not RUN: the machine is not keeping up with the
                // clock, and saying so is more use than a number nobody will
                // go looking for. It means this processor, this cartridge --
                // try the Picture setting, or look at your power governor.
                if(module->behind)      what += " BEHIND";
                else if(module->running) what += " RUN";
                else                     what += " STOP";
            }

            lines.push_back({ what, 2.4f, sfc::shade(), NVG_ALIGN_RIGHT });
            return lines;
        };
        addChild(state);
    }

    void addMeters(SnesModule* module)
    {
        VoiceMeters* meters = new VoiceMeters;
        meters->box.pos  = mm2px(Vec(L::ColX, L::MeterY));
        // MeterW, not ColW: the window is narrower than the column, because
        // RATE and VOLUME stand beside it. A widget the width of the column
        // right-aligns its two text columns underneath them.
        meters->box.size = mm2px(Vec(L::MeterW, L::MeterH));
        meters->rowPitch = L::MeterPitch;
        meters->source   = [module]() -> const VoiceState* {
            return module && module->mine ? module->console.voices() : nullptr;
        };
        addChild(meters);
    }

    void addKnobs(SnesModule* module)
    {
        addParam(knobAt(module, SnesModule::RateParam,   L::RateX, L::RateY, 11.0f));
        addParam(knobAt(module, SnesModule::VolumeParam, L::VolX,  L::VolY,  11.0f));
    }

    void addJacks(SnesModule* module)
    {
        // The twelve buttons go where the machine's own controller puts
        // them, with each socket's rim in the colour of the button it is.
        for(int i = 0; i < 12; ++i)
        {
            const panel::PadSeat& seat = panel::padSeats()[i];

            SnesPort* port = createInputCentered<SnesPort>(
                mm2px(Vec(seat.x, seat.y)), module, SnesModule::PadInput + i);
            port->rim = seat.tint();
            addInput(port);
        }

        for(int i = 0; i < 6; ++i)
            addInput(createInputCentered<SnesPort>(
                mm2px(Vec(panel::snesUtility()[i].x, panel::snesUtility()[i].y)),
                module, SnesModule::Pad2Input + i));

        for(int i = 0; i < 16; ++i)
            addOutput(createOutputCentered<SnesPort>(
                mm2px(Vec(L::outX(i % L::OutCols),
                          i < L::OutCols ? L::OutRow1 : L::OutRow2)),
                module, SnesModule::MixLOutput + i));
    }

    void appendContextMenu(Menu* menu) override
    {
        SnesModule* m = dynamic_cast<SnesModule*>(module);
        if(!m) return;

        menu->addChild(new MenuSeparator);

        menu->addChild(createBoolPtrMenuItem("Show the overscan", "", &m->overscan));

        // The one setting that makes the machine itself cheaper: the PPU does
        // not render a frame nobody is going to see.
        static const char* how[4] = { "Every frame", "Every other frame",
                                      "One in four", "None -- sound only" };
        static const int   skips[4] = { 0, 1, 3, 999 };

        menu->addChild(createSubmenuItem("Picture", how[m->pictureChoice()],
            [m](Menu* sub) {
                for(int i = 0; i < 4; ++i)
                    sub->addChild(createCheckMenuItem(how[i], "",
                        [m, i]() { return m->pictureChoice() == i; },
                        [m, i]() { m->frameSkip = skips[i]; }));
            }));

        menu->addChild(createBoolPtrMenuItem("Strobe", "", &m->strobe));
        menu->addChild(createMenuLabel("   the machine keeps running at sixty;"));
        menu->addChild(createMenuLabel("   the picture is what the clock holds"));

        // How far ahead the machine is allowed to run. Deeper survives more,
        // and puts more time between your thumb and the sound.
        static const char* deep[4] = { "Short -- 16 ms", "Medium -- 32 ms",
                                       "Long -- 64 ms", "Longest -- 128 ms" };
        static const int   depths[4] = { 512, 1024, 2048, 4096 };

        menu->addChild(createSubmenuItem("Buffer", deep[m->bufferChoice()],
            [m](Menu* sub) {
                for(int i = 0; i < 4; ++i)
                    sub->addChild(createCheckMenuItem(deep[i], "",
                        [m, i]() { return m->bufferChoice() == i; },
                        [m, i]() { m->bufferDepth = depths[i]; }));
            }));
        menu->addChild(createMenuLabel(string::f("   %u samples were not ready",
                                                 m->console.underruns())));

        menu->addChild(createBoolPtrMenuItem("The fast sound chip", "", &m->fastDsp));
        menu->addChild(createMenuLabel("   same eight voices, a third less work;"));
        menu->addChild(createMenuLabel("   two cartridges ask for the other one"));

        menu->addChild(new MenuSeparator);
        menu->addChild(createBoolPtrMenuItem("Carry the cartridge in the patch", "", &m->embed));

        menu->addChild(createMenuItem("Save the song as an .spc", "", [m]() {
            m->wantSpc.store(true);
        }));

        menu->addChild(new MenuSeparator);
        menu->addChild(createSubmenuItem("Gamepad",
            m->padSeen.load() ? m->padName : std::string("none found"),
            [this, m](Menu* sub) { gamepadMenu(sub, m); }));

        menu->addChild(createSubmenuItem("Keyboard", m->keyOn ? "on" : "off",
            [this, m](Menu* sub) { keyboardMenu(sub, m); }));
    }

    void keyboardMenu(Menu* menu, SnesModule* m)
    {
        menu->addChild(createBoolPtrMenuItem("Play with the keyboard", "", &m->keyOn));
        menu->addChild(createMenuLabel("   while this is on, these keys go to the game"));

        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel("Click a button, then press a key"));

        for(int i = 0; i < ButtonCount; ++i)
        {
            const bool waiting = m->learningKey.load() == i;
            const std::string right = waiting ? "press a key..." : keyName(m->keyMap.to[i]);

            menu->addChild(createMenuItem(panel::padLabels()[i], right, [m, i]() {
                m->learningKey.store(m->learningKey.load() == i ? -1 : i);
            }));
        }

        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuItem("Back to the usual layout", "", [m]() {
            m->keyMap = KeyMap::standard();
            m->learningKey.store(-1);
        }));
    }

    void gamepadMenu(Menu* menu, SnesModule* m)
    {
        menu->addChild(createBoolPtrMenuItem("Play with a gamepad", "", &m->padOn));

        // Which one. GLFW is asked here rather than kept in a list, because a
        // pad can be plugged in while the menu is open.
        menu->addChild(createSubmenuItem("Which pad", "", [m](Menu* sub) {
            sub->addChild(createCheckMenuItem("Whichever is plugged in", "",
                [m]() { return m->padIndex < 0; },
                [m]() { m->padIndex = -1; }));

            bool any = false;
            for(int j = 0; j <= GLFW_JOYSTICK_LAST; ++j)
            {
                if(!glfwJoystickPresent(j)) continue;
                any = true;

                std::string name = glfwJoystickIsGamepad(j) ? glfwGetGamepadName(j)
                                                            : glfwGetJoystickName(j);
                sub->addChild(createCheckMenuItem(name, string::f("%d", j),
                    [m, j]() { return m->padIndex == j; },
                    [m, j]() { m->padIndex = j; }));
            }

            if(!any) sub->addChild(createMenuLabel("nothing is plugged in"));
        }));

        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel("Click a button, then press one on the pad"));

        for(int i = 0; i < ButtonCount; ++i)
        {
            const bool waiting = m->learning.load() == i;
            const std::string right = waiting ? "press one..."
                                              : bindingName(m->padMap.to[i]);

            menu->addChild(createMenuItem(panel::padLabels()[i], right, [m, i]() {
                m->learning.store(m->learning.load() == i ? -1 : i);
            }));
        }

        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuItem("Back to the usual layout", "", [m]() {
            m->padMap = Mapping::standard();
            m->learning.store(-1);
        }));
    }
};

// Only tools/probe. See plugin.hpp.
void snesRunOffline(engine::Module* module, bool on)
{
    if(SnesModule* snes = dynamic_cast<SnesModule*>(module)) snes->offline = on;
}

void snesServe(engine::Module* module)
{
    if(SnesModule* snes = dynamic_cast<SnesModule*>(module)) snes->serveRequests();
}

uint32_t snesUnderruns(engine::Module* module)
{
    SnesModule* snes = dynamic_cast<SnesModule*>(module);
    return snes ? snes->console.underruns() : 0;
}

} // namespace racksnes

Model* modelSnes = createModel<racksnes::SnesModule, racksnes::SnesWidget>("Snes");
