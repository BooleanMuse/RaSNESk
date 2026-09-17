// ===========================================================================
// What the console costs, with nothing else in the way.
//
//   ./build.sh bench "/path/to/a.sfc" [seconds]
//
// Boots the cartridge and runs it for a fixed stretch of its own time with
// nothing patched and no buttons pressed, so that two builds can be compared
// without the difference being which title screen the emulator happened to
// reach. Reports the share of one core it would take to keep up.
// ===========================================================================
#include "../src/snes/Console.hpp"

#include <chrono>
#include <ctime>
#include <cstdio>
#include <string>
#include <vector>

using namespace racksnes;

// Processor time, not wall clock. A laptop with a browser open will hand a
// benchmark twenty milliseconds of somebody else's work and call it ours;
// this counts only what this process actually ran for, which is the number
// the question is about.
static double cpuSeconds()
{
    timespec t;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(int argc, char** argv)
{
    if(argc < 2) { std::printf("usage: bench <cartridge.sfc> [seconds]\n"); return 2; }

    const double seconds = argc > 2 ? std::atof(argv[2]) : 10.0;
    const double rate = 48000.0;

    const int  skip = argc > 3 ? std::atoi(argv[3]) : 0;
    const bool fast = argc > 4 ? std::atoi(argv[4]) != 0 : true;

    Console console;
    if(!console.claim()) { std::printf("no console\n"); return 1; }

    // Not an audio thread: this pulls audio as fast as it can, so it waits
    // for the machine rather than outrunning it and calling the silence a
    // result.
    console.setWaitForMachine(true);

    console.setFastDsp(fast);
    console.setFastPpu(argc > 7 ? std::atoi(argv[7]) != 0 : true);

    std::string error;
    if(!console.load(argv[1], error)) { std::printf("%s\n", error.c_str()); return 1; }

    console.setFrameSkip(skip);
    console.setChunk(argc > 8 ? std::atoi(argv[8]) : 16);

    // The module asks for sixty-four at a time, so the benchmark does too:
    // how often the emulator is entered is part of what is being measured.
    const int chunk = argc > 5 ? std::atoi(argv[5]) : 64;
    const double bootSeconds = argc > 6 ? std::atof(argv[6]) : 2.0;

    std::vector<ChipFrame> block((size_t)chunk);
    const int blocks = (int)(seconds * rate / chunk);

    // Boot, and press start now and then, so the cartridge gets somewhere
    // worth timing: a game sitting on its logo is not what it costs to play.
    {
        const int bootBlocks = (int)(bootSeconds * rate / chunk);
        for(int b = 0; b < bootBlocks; ++b)
        {
            const int frame = (int)((double)b * chunk / rate * 60.0);
            console.setButton(0, ButtonStart, (frame % 60) < 8);
            console.run(block.data(), chunk, rate, 1.0);
        }
        console.setButton(0, ButtonStart, false);
    }

    const double start = cpuSeconds();

    double peak = 0, worst = 0;
    int overOne = 0;

    for(int b = 0; b < blocks; ++b)
    {
        const double a = cpuSeconds();
        console.run(block.data(), chunk, rate, 1.0);
        const double took = cpuSeconds() - a;

        if(took > worst) worst = took;
        if(took > 0.001) ++overOne;

        for(const ChipFrame& f : block)
            if(std::fabs(f.main[0]) > peak) peak = std::fabs(f.main[0]);
    }

    const double took = cpuSeconds() - start;
    const double emulated = blocks * (double)chunk / rate;

    std::printf("%-24s skip %-3d dsp %-5s %6.1f%% of a core  worst call %6.3f ms"
                "  (%d of %d over 1 ms)  peak %.3f\n",
                console.title().c_str(), skip, (argc > 7 && !std::atoi(argv[7])) ? "exact"
                                              : (fast ? "fast" : "exactD"),
                100.0 * took / emulated, worst * 1000.0, overOne, blocks, peak);
    return 0;
}
