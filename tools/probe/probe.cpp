// ===========================================================================
// Builds every module and its panel the way Rack does, with no Rack running.
//
//     ./build.sh probe [cartridge.sfc]
//
// Rack aborts on an uncaught exception and prints a stack trace of libRack's
// internals, which says where it was but not what was thrown. This does the
// same two calls Rack's module browser makes -- createModule() then
// createModuleWidget(module) -- with nothing else in the way, so the
// exception can be caught and read. It also makes the check
// RackWidget::addModule makes, which is the one that happens *after* a module
// has built, loaded and drawn perfectly in the browser, and which takes the
// whole program with it when it fails.
//
// Given a cartridge it goes further: it runs the SNES module the way Rack
// runs it, one process() per sample, and writes what came out of the jacks.
// ===========================================================================
#include <rack.hpp>

#include "../../src/plugin.hpp"
#include "../../src/Gamepad.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <thread>
#include <exception>
#include <vector>

using namespace rack;

extern "C" void init(Plugin* p);

static int failures = 0;

static void probe(Model* model, const char* name)
{
    std::printf("\n== %s ==\n", name);

    engine::Module* module = nullptr;

    try {
        module = model->createModule();
        std::printf("   createModule  ok: %d params / %d in / %d out / %d lights\n",
                    (int)module->params.size(), (int)module->inputs.size(),
                    (int)module->outputs.size(), (int)module->lights.size());
    }
    catch(const std::exception& e) {
        std::printf("   !! createModule threw: %s\n", e.what()); ++failures; return;
    }

    app::ModuleWidget* mw = nullptr;

    try {
        mw = model->createModuleWidget(module);
        std::printf("   createModuleWidget ok: %.1f x %.1f px, %d children\n",
                    mw->box.size.x, mw->box.size.y, (int)mw->children.size());
    }
    catch(const std::exception& e) {
        std::printf("   !! createModuleWidget threw: %s\n", e.what()); ++failures; return;
    }

    // The check RackWidget::addModule makes before it will accept a module.
    // Copied from Rack because it is the one that kills the program: it
    // compares the panel's size against the grid exactly, and a panel built
    // out of the 128.5 mm of a physical Eurorack rather than Rack's 380 px
    // misses it and aborts.
    {
        bool ok = true;

        if(mw->box.size.y != RACK_GRID_HEIGHT)
        {
            std::printf("   !! height is %.6f px and Rack accepts only %g exactly\n",
                        mw->box.size.y, RACK_GRID_HEIGHT);
            ok = false;
        }
        if(std::fmod(mw->box.size.x, RACK_GRID_WIDTH) != 0.f)
        {
            std::printf("   !! width is not a whole number of HP: %.6f px\n", mw->box.size.x);
            ok = false;
        }

        if(!ok) { ++failures; return; }
        std::printf("   geometry ok: %g HP, Rack will take it\n",
                    mw->box.size.x / RACK_GRID_WIDTH);
    }

    try {
        engine::Module::SampleRateChangeEvent e;
        e.sampleRate = 44100.f;
        e.sampleTime = 1.f / 44100.f;
        module->onSampleRateChange(e);
        std::printf("   onSampleRateChange ok\n");
    }
    catch(const std::exception& e) {
        std::printf("   !! onSampleRateChange threw: %s\n", e.what()); ++failures;
    }

    try {
        json_t* j = module->dataToJson();
        char* dump = j ? json_dumps(j, JSON_COMPACT) : nullptr;
        std::printf("   dataToJson ok: %d bytes\n", (int)(dump ? std::strlen(dump) : 0));
        if(j) { module->dataFromJson(j); json_decref(j); }
        free(dump);
        std::printf("   dataFromJson ok\n");
    }
    catch(const std::exception& e) {
        std::printf("   !! toJson threw: %s\n", e.what()); ++failures;
    }

    // onReset() is not tested here: Module::onReset resets every parameter
    // through the Engine, and there is no Engine outside Rack.

    try {
        engine::Module::ProcessArgs args;
        args.sampleRate = 44100.f;
        args.sampleTime = 1.f / args.sampleRate;
        for(int i = 0; i < 4000; ++i) { args.frame = i; module->process(args); }
        std::printf("   process x4000 ok\n");
    }
    catch(const std::exception& e) {
        std::printf("   !! process threw: %s\n", e.what()); ++failures;
    }

    // There is one console per process, and a SNES module holds it for as
    // long as it exists. Leaving this one alive would leave the machine taken
    // and every later test silent -- which is exactly what it would do in a
    // patch with two of them. The widget is left where it is: destroying a
    // ModuleWidget outside Rack goes looking for a scene and a history that
    // do not exist here.
    delete module;
}

// ---------------------------------------------------------------------------

static void writeWav(const char* path, const std::vector<float>& samples, int channels)
{
    std::vector<int16_t> pcm(samples.size());
    for(size_t i = 0; i < samples.size(); ++i)
        // Five volts is full scale, which is what Rack calls line level, so
        // the file plays back at the loudness the patch would have.
        pcm[i] = (int16_t)math::clamp(samples[i] * 6553.4f, -32767.f, 32767.f);

    FILE* f = std::fopen(path, "wb");
    if(!f) return;

    const uint32_t dataBytes = (uint32_t)pcm.size() * 2;
    const uint32_t byteRate = 44100u * (uint32_t)channels * 2;
    const uint32_t riffSize = 36 + dataBytes, fmtSize = 16, sr = 44100;
    const uint16_t pcmTag = 1, ch = (uint16_t)channels, bits = 16;
    const uint16_t blockAlign = (uint16_t)(channels * 2);

    std::fwrite("RIFF", 1, 4, f); std::fwrite(&riffSize, 4, 1, f);
    std::fwrite("WAVEfmt ", 1, 8, f); std::fwrite(&fmtSize, 4, 1, f);
    std::fwrite(&pcmTag, 2, 1, f); std::fwrite(&ch, 2, 1, f);
    std::fwrite(&sr, 4, 1, f); std::fwrite(&byteRate, 4, 1, f);
    std::fwrite(&blockAlign, 2, 1, f); std::fwrite(&bits, 2, 1, f);
    std::fwrite("data", 1, 4, f); std::fwrite(&dataBytes, 4, 1, f);
    std::fwrite(pcm.data(), 1, dataBytes, f);
    std::fclose(f);
}

// The audio path, all the way out of the jacks.
//
// The module is driven the way Rack drives it, but as fast as this can go
// rather than in real time, so the console is told it may wait for its own
// machine instead of reporting the silence of having outrun it.
//
// The console's own tests prove the chip makes sound. They do not prove the
// module gets it out: between the two are a ring, a read head moving at
// whatever speed the clock asks for, and the interpolation between them.
static void listen(const char* cartridge)
{
    std::printf("\n== the audio path, with %s ==\n", cartridge);

    engine::Module* module = modelSnes->createModule();
    racksnes::snesRunOffline(module, true);

    engine::Module::SampleRateChangeEvent sr;
    sr.sampleRate = 44100.f;
    sr.sampleTime = 1.f / 44100.f;
    module->onSampleRateChange(sr);

    // What a saved patch hands the module.
    json_t* data = json_object();
    json_object_set_new(data, "cartridge", json_string(cartridge));
    json_object_set_new(data, "running", json_true());
    module->dataFromJson(data);
    json_decref(data);

    // In Rack the widget's step() does this; here there is no widget.
    racksnes::snesServe(module);

    engine::Module::ProcessArgs args;
    args.sampleRate = 44100.f;
    args.sampleTime = 1.f / args.sampleRate;

    const int seconds = 25;
    const int frames  = 44100 * seconds;

    std::vector<float> stereo;
    stereo.reserve((size_t)frames * 2);

    double peakMix = 0, sumMix = 0, peakVoice = 0;
    int keyedSamples = 0;

    for(int i = 0; i < frames; ++i)
    {
        args.frame = i;

        // In Rack the widget's step() serves what the module asked for, sixty
        // times a second. Here nothing else will.
        if(i % 735 == 0) racksnes::snesServe(module);

        // A tap on START now and then, to get past a title screen.
        const int frame60 = (int)((double)i / 44100.0 * 60.0);
        module->inputs[11].channels = 1;   // START is the twelfth gamepad jack
        module->inputs[11].setVoltage((frame60 % 60) < 8 ? 10.f : 0.f);

        module->process(args);

        const float l = module->outputs[0].getVoltage();
        const float r = module->outputs[1].getVoltage();
        const float v = module->outputs[2].getVoltage();

        stereo.push_back(l);
        stereo.push_back(r);

        peakMix   = std::fmax(peakMix, std::fabs((double)l));
        peakVoice = std::fmax(peakVoice, std::fabs((double)v));
        sumMix   += std::fabs((double)l) + std::fabs((double)r);

        // GATE is polyphonic, one channel a voice.
        for(int c = 0; c < 8; ++c)
            if(module->outputs[11].getVoltage(c) > 1.f) { ++keyedSamples; break; }
    }

    std::printf("   %d samples out of the jacks\n", frames);
    std::printf("   MIX   peak %.2f V, mean %.3f V\n", peakMix, sumMix / (frames * 2));
    std::printf("   V1    peak %.2f V\n", peakVoice);
    std::printf("   a voice was keyed for %.1f of %d seconds\n",
                (double)keyedSamples / 44100.0, seconds);

    system::createDirectories("build/probe");
    writeWav("build/probe/mix.wav", stereo, 2);
    std::printf("   wrote build/probe/mix.wav\n");

    if(peakMix < 0.05) { std::printf("   !! nothing came out of MIX\n"); ++failures; }
    if(keyedSamples < 44100) { std::printf("   !! almost nothing played\n"); ++failures; }

    // There is one console per process and this module is holding it. Leaving
    // it alive leaves every later test with no machine and nothing to
    // measure -- which is exactly what it would do to a second SNES module in
    // a patch, so it is worth the probe knowing about.
    delete module;
}

// ---------------------------------------------------------------------------
// The mapping, and the learn
//
// A button cannot be pressed from in here, so the pad is made up: two states,
// and what the learn is supposed to catch between them.
// ---------------------------------------------------------------------------
static void mapping()
{
    using namespace racksnes;

    std::printf("\n== the gamepad mapping ==\n");

    PadState before, now;
    before.present = now.present = true;
    before.standard = now.standard = true;
    before.buttonCount = now.buttonCount = GLFW_GAMEPAD_BUTTON_LAST + 1;
    before.axisCount = now.axisCount = GLFW_GAMEPAD_AXIS_LAST + 1;

    Mapping map = Mapping::standard();

    // The face buttons: a SNES pad has B at the bottom and A on the right,
    // which is where GLFW's A and B are. The letters differ; the thumb agrees.
    now.button[GLFW_GAMEPAD_BUTTON_A] = 1;
    uint16_t bits = squash(now, map);

    if(bits == (1 << ButtonB))
        std::printf("   the pad's bottom button is the console's B, ok\n");
    else { std::printf("   !! bottom button gave %04x\n", bits); ++failures; }

    // The left stick does what the D-pad does, without being mapped to it.
    now.button[GLFW_GAMEPAD_BUTTON_A] = 0;
    now.axis[GLFW_GAMEPAD_AXIS_LEFT_X] = -0.9f;
    bits = squash(now, map);

    if(bits & (1 << ButtonLeft)) std::printf("   the left stick presses LEFT, ok\n");
    else { std::printf("   !! the stick did nothing\n"); ++failures; }

    now.axis[GLFW_GAMEPAD_AXIS_LEFT_X] = 0.f;

    // The learn: whatever is pressed next becomes the button being taught.
    now.button[7] = 1;
    Binding caught = firstPressed(before, now);

    if(caught.kind == Binding::Button && caught.index == 7)
        std::printf("   the learn caught button 7 (\"%s\"), ok\n",
                    bindingName(caught).c_str());
    else { std::printf("   !! the learn caught nothing\n"); ++failures; }

    map.to[ButtonStart] = caught;
    if(squash(now, map) & (1 << ButtonStart))
        std::printf("   and what it caught is what START answers to, ok\n");
    else { std::printf("   !! the new binding does not work\n"); ++failures; }

    // A button already down is not a press: the learn waits for a fresh one.
    before = now;
    if(firstPressed(before, now).kind == Binding::None)
        std::printf("   a button already held is not caught, ok\n");
    else { std::printf("   !! the learn caught a held button\n"); ++failures; }

    // An axis pushed past two thirds counts, so a stick can be a button.
    now.axis[3] = 0.8f;
    Binding fromStick = firstPressed(before, now);

    if(fromStick.kind == Binding::AxisHigh && fromStick.index == 3)
        std::printf("   a stick can be learned too (\"%s\"), ok\n",
                    bindingName(fromStick).c_str());
    else { std::printf("   !! a stick was not caught\n"); ++failures; }

    KeyMap keys = KeyMap::standard();
    if(keys.to[ButtonB] == GLFW_KEY_Z && keyName(keys.to[ButtonStart]) == "enter")
        std::printf("   the keyboard starts on Z, X and enter, ok\n");
    else { std::printf("   !! the keyboard's layout is wrong\n"); ++failures; }
}

// ---------------------------------------------------------------------------
// A BENDER beside a SNES
//
// An expander writes into buffers its neighbour has to have provided, and a
// neighbour that never provided them is a null pointer written through. That
// is not something one module on its own can show: it needs two, and one of
// them next to the other, which is why it survived every check above and took
// Rack with it the first time somebody put them side by side.
// ---------------------------------------------------------------------------
static void expanders()
{
    std::printf("\n== a BENDER beside a SNES ==\n");

    engine::Module* snes   = modelSnes->createModule();
    engine::Module* bender = modelBender->createModule();

    engine::Module::ProcessArgs args;
    args.sampleRate = 44100.f;
    args.sampleTime = 1.f / args.sampleRate;

    for(int side = 0; side < 2; ++side)
    {
        // Either side: the console takes a bend from its left or its right.
        snes->leftExpander.module   = side == 0 ? bender : nullptr;
        snes->rightExpander.module  = side == 1 ? bender : nullptr;
        bender->rightExpander.module = side == 0 ? snes : nullptr;
        bender->leftExpander.module  = side == 1 ? snes : nullptr;

        // Every bend on at once, which is the worst thing a patch can do.
        for(size_t p = 0; p < bender->params.size(); ++p)
            bender->params[p].setValue(bender->paramQuantities[p]->maxValue);

        try {
            for(int i = 0; i < 2000; ++i)
            {
                args.frame = i;
                bender->process(args);
                snes->rightExpander.messageFlipRequested = false;
                snes->leftExpander.messageFlipRequested  = false;
                std::swap(snes->leftExpander.producerMessage, snes->leftExpander.consumerMessage);
                std::swap(snes->rightExpander.producerMessage, snes->rightExpander.consumerMessage);
                snes->process(args);
            }
            std::printf("   %s of the console: 2000 samples with every bend at maximum, ok\n",
                        side == 0 ? "left " : "right");
        }
        catch(const std::exception& e) {
            std::printf("   !! threw: %s\n", e.what()); ++failures;
        }
    }

    delete snes;
    delete bender;
}

// ---------------------------------------------------------------------------
// What it costs
//
// Rack calls process() once a sample and gives the whole engine one block's
// worth of wall clock to finish: at 44.1 kHz with a block of 256 that is 5.8
// milliseconds for every module in the patch together. So two numbers matter
// and they are different questions. The mean says what share of a core this
// module takes, which is what Rack's own meter shows. The worst single call
// says whether it can blow the block -- and this module is spiky by
// construction, because the console is asked for sixty-four samples at a time
// and one call in sixty-four does all the work.
// ---------------------------------------------------------------------------
// Processor time, not wall clock: a laptop with a browser open will hand a
// benchmark twenty milliseconds of somebody else's work and call it ours, and
// at this scale that is all you would be measuring.
//
// Except on Windows, which does not keep a per-process CPU clock anything
// like this fine. MinGW's clock_gettime takes CLOCK_PROCESS_CPUTIME_ID and
// answers zero, and GetProcessTimes -- what tests/bench uses, where the
// measurement is seconds long -- moves in steps of about 16 ms, which is
// three audio blocks. So there it is the wall clock, at the resolution
// QueryPerformanceCounter gives, and the numbers carry whatever else the
// machine was doing. `windowsClock` below is what says so in the output.
#if defined(_WIN32)
static const bool windowsClock = true;
static double cpuSeconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
#else
static const bool windowsClock = false;
static double cpuSeconds()
{
    timespec t;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
#endif

struct Cost { double mean, worst, share; };

// Measured in real time, because that is the only way this means anything
// now. The machine runs on its own thread and the audio thread only drains a
// ring; running the audio side flat out would charge it for a machine that is
// only trying to keep up with a clock. So: process a block, then wait until
// the block's worth of wall time has actually gone by, and count only what
// the audio thread itself spent.
static Cost measure(engine::Module* module, int samples, float rate, int block = 256)
{
    engine::Module::ProcessArgs args;
    args.sampleRate = rate;
    args.sampleTime = 1.f / rate;

    const double blockSeconds = block / (double)rate;

    // A warm-up, in real time too, so the ring is standing full before
    // anything is counted.
    const auto begin = std::chrono::steady_clock::now();
    for(int b = 0; b < (int)(rate / block / 4); ++b)
    {
        for(int i = 0; i < block; ++i) module->process(args);
        std::this_thread::sleep_until(begin + std::chrono::duration<double>(
            (b + 1) * blockSeconds));
    }

    double total = 0, worst = 0;
    const auto start = std::chrono::steady_clock::now();
    const int blocks = samples / block;

    for(int b = 0; b < blocks; ++b)
    {
        const double a = cpuSeconds();
        for(int i = 0; i < block; ++i) { args.frame = b * block + i; module->process(args); }
        const double took = cpuSeconds() - a;

        total += took;
        worst = std::max(worst, took);

        std::this_thread::sleep_until(start + std::chrono::duration<double>(
            (b + 1) * blockSeconds));
    }

    Cost c;
    c.mean  = total / blocks;
    c.worst = worst;
    c.share = total / (blocks * blockSeconds);
    return c;
}

static void report(const char* what, const Cost& c, float rate)
{
    const double block = 256.0 / rate;

    std::printf("   %-22s %6.2f%% of the audio thread   worst block %6.3f ms"
                "  (%4.1f%% of its 5.8 ms)\n",
                what, c.share * 100.0, c.worst * 1000.0, c.worst / block * 100.0);

    if(c.share > 0.50) { std::printf("   !! that is half the audio thread\n"); ++failures; }
    if(c.worst > block) { std::printf("   !! one block did not finish in time\n"); ++failures; }
}

static void performance(const char* cartridge)
{
    std::printf("\n== what the audio thread pays, at 44.1 kHz ==\n");
    std::printf("   (the machine is on its own thread; this is only the "
                "draining)\n");
    if(windowsClock)
        std::printf("   (Windows: wall clock rather than processor time -- see cpuSeconds)\n");

    const float rate = 44100.f;
    const int   samples = (int)rate;      // one second of audio, timed

    // --- the console ------------------------------------------------------
    engine::Module* snes = modelSnes->createModule();
    racksnes::snesRunOffline(snes, true);

    engine::Module::SampleRateChangeEvent sr;
    sr.sampleRate = rate; sr.sampleTime = 1.f / rate;
    snes->onSampleRateChange(sr);

    if(cartridge && *cartridge)
    {
        json_t* data = json_object();
        json_object_set_new(data, "cartridge", json_string(cartridge));
        json_object_set_new(data, "running", json_true());
        snes->dataFromJson(data);
        json_decref(data);
        racksnes::snesServe(snes);
    }

    // Give the cartridge time to get to where it plays something: a game is
    // silent while it boots, and timing a silent console is timing nothing.
    {
        engine::Module::ProcessArgs args;
        args.sampleRate = rate; args.sampleTime = 1.f / rate;

        float peak = 0.f;

        for(int i = 0; i < (int)(rate * 14); ++i)
        {
            args.frame = i;
            if(i % 735 == 0) racksnes::snesServe(snes);
            snes->inputs[11].channels = 1;         // START, to get past a title
            snes->inputs[11].setVoltage(((int)(i / (rate / 60.f)) % 60) < 8 ? 10.f : 0.f);
            snes->process(args);

            peak = std::max(peak, std::fabs(snes->outputs[0].getVoltage()));
        }

        snes->inputs[11].setVoltage(0.f);

        if(peak < 0.02f)
        {
            std::printf("   !! the console made no sound -- nothing here is worth timing\n");
            ++failures;
            delete snes;
            return;
        }

        // From here it is measured as Rack would drive it: in real time, and
        // without the console waiting for its own machine.
        racksnes::snesRunOffline(snes, false);

        report("the console", measure(snes, samples, rate), rate);
        std::printf("      and the machine failed to have %u samples ready\n",
                    racksnes::snesUnderruns(snes));

        // And take its instruments, so the sampler below has some.
        snes->params[7].setValue(1.f);             // RIP
        snes->process(args);
        snes->params[7].setValue(0.f);
        snes->process(args);
        racksnes::snesServe(snes);
    }

    // --- the sampler, with all eight voices held --------------------------
    engine::Module* sampler = modelSampler->createModule();
    sampler->onSampleRateChange(sr);

    sampler->inputs[0].channels = 8;      // V/OCT
    sampler->inputs[1].channels = 8;      // GATE
    for(int c = 0; c < 8; ++c)
    {
        sampler->inputs[0].setVoltage(c / 12.f, c);
        sampler->inputs[1].setVoltage(10.f, c);
    }

    report("the sampler, 8 notes", measure(sampler, samples, rate), rate);

    // --- the chip on its own ----------------------------------------------
    engine::Module* apu = modelApu->createModule();
    apu->onSampleRateChange(sr);
    report("the chip alone", measure(apu, samples, rate), rate);

    // --- and the expander -------------------------------------------------
    engine::Module* bender = modelBender->createModule();
    bender->onSampleRateChange(sr);
    report("the bender", measure(bender, samples, rate), rate);

    // --- the console at double speed, which is the worst it can be asked --
    snes->params[0].setValue(1.f);        // RATE: one volt is twice as fast
    report("the console at 2x", measure(snes, samples, rate), rate);

    std::printf("      and now %u, which at twice speed on a slow machine is\n"
                "      the machine honestly not keeping up\n",
                racksnes::snesUnderruns(snes));

    delete snes;
    delete sampler;
    delete apu;
    delete bender;
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv)
{
    const char* systemDir = argc > 1 ? argv[1] : "/usr/share/Rack2";
    const char* cartridge = argc > 2 ? argv[2] : nullptr;

    asset::systemDir = systemDir;
    asset::userDir   = systemDir;

    Plugin* plugin = new Plugin;
    plugin->path = ".";
    init(plugin);

    std::printf("plugin \"%s\": %d models\n", plugin->slug.c_str(), (int)plugin->models.size());

    probe(modelSnes,    "SNES");
    probe(modelSampler, "SAMPLER");
    probe(modelApu,     "APU");
    probe(modelBender,  "BENDER");

    mapping();
    expanders();

    if(cartridge && *cartridge) listen(cartridge);
    if(cartridge && *cartridge) performance(cartridge);

    std::printf("\n%s\n", failures ? "  SOMETHING IS WRONG" : "  everything Rack checks, checks out");
    return failures ? 1 : 0;
}
