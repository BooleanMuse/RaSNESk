// ===========================================================================
// The sound chip with the console taken away.
//
//   ./build.sh apu "/path/to/a.sfc"
//
// Checks the boot ROM handshake -- the sixty-four bytes every SNES starts
// from, which write $AA and $BB to two of the four ports and then wait --
// and then rips a song out of a running cartridge as an .spc and keeps
// playing it after the console is gone.
// ===========================================================================
#include "../src/snes/Console.hpp"
#include "../src/snes/Apu.hpp"
#include "../src/snes/Bank.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <cstring>
#include <string>
#include <vector>

using namespace racksnes;

static int failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "  ok  " : " FAIL ", what);
    if(!ok) ++failures;
}

static void writeWav(const std::string& path, const std::vector<float>& s, int ch, int rate)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if(!f) return;
    const uint32_t frames = (uint32_t)(s.size() / ch), bytes = frames * ch * 2;
    auto u32 = [&](uint32_t v){ std::fwrite(&v,4,1,f); };
    auto u16 = [&](uint16_t v){ std::fwrite(&v,2,1,f); };
    std::fwrite("RIFF",1,4,f); u32(36+bytes); std::fwrite("WAVE",1,4,f);
    std::fwrite("fmt ",1,4,f); u32(16); u16(1); u16((uint16_t)ch);
    u32((uint32_t)rate); u32((uint32_t)(rate*ch*2)); u16((uint16_t)(ch*2)); u16(16);
    std::fwrite("data",1,4,f); u32(bytes);
    for(float v : s){ if(v>1)v=1; if(v<-1)v=-1; u16((uint16_t)(int16_t)(v*32767.f)); }
    std::fclose(f);
}

int main(int argc, char** argv)
{
    // --- 1. the boot ROM, with nothing loaded at all ----------------------
    {
        Apu apu;
        Bank empty;

        uint8_t blank[AramSize] = {};
        empty.adopt(blank, 0, "nothing");
        apu.loadBank(empty);

        ChipFrame f;
        for(int i = 0; i < 2000; ++i) apu.run(f);

        const uint8_t p0 = apu.readPort(0), p1 = apu.readPort(1);
        std::printf("        the boot ROM says $%02x $%02x on the ports\n", p0, p1);
        check(p0 == 0xaa && p1 == 0xbb,
              "the SPC-700 ran its boot ROM and said hello on the ports");
    }

    if(argc < 2) { std::printf("\n(no cartridge given; stopping here)\n"); return failures ? 1 : 0; }

    // --- 2. rip a song out of a running console ---------------------------
    const double boot = argc > 2 ? std::atof(argv[2]) : 20.0;

    Console console;
    if(!console.claim()) { std::printf("no console\n"); return 1; }

    // Not an audio thread: this pulls audio as fast as it can, so it waits
    // for the machine rather than outrunning it and calling the silence a
    // result.
    console.setWaitForMachine(true);

    std::string error;
    if(!console.load(argv[1], error)) { std::printf("%s\n", error.c_str()); return 1; }

    // Wait for the cartridge to actually be playing something before taking
    // it. A game sitting on a menu is silent, and an .spc of silence plays
    // back perfectly as silence -- which looks exactly like a broken .spc if
    // nobody checked what was there to take.
    std::vector<ChipFrame> block(256);
    int   sounding = 0;
    bool  caught = false;

    for(int b = 0; b < (int)(boot * 48000 / 256) && !caught; ++b)
    {
        int frame = (int)((double)b * 256 / 48000 * 60.0);
        console.setButton(0, ButtonStart, (frame % 60) < 8);
        console.run(block.data(), 256, 48000.0, 1.0);

        const VoiceState* v = console.voices();
        int keyed = 0;
        for(int i = 0; i < VoiceCount; ++i) if(v[i].keyed && v[i].level > 0.05f) ++keyed;

        sounding = std::max(sounding, keyed);

        // Three voices at once is a piece of music rather than a menu blip.
        if(keyed >= 3) caught = true;
    }

    console.setButton(0, ButtonStart, false);

    if(!caught)
    {
        std::printf("        the cartridge never got to anything worth taking in %.0f s\n"
                    "        (the most it played at once was %d voices)\n", boot, sounding);
        std::printf("\n  nothing to test -- give it longer, or a cartridge that "
                    "starts playing sooner\n");
        return 0;
    }

    std::printf("        caught it with %d voices going\n", sounding);

    std::vector<uint8_t> spc;
    check(console.exportSpc(spc), "the console exported an .spc");
    check(spc.size() == 0x10200, "and it is the right size");

    // Not system("mkdir -p ..."): system() on Windows runs cmd.exe, which
    // has no -p and answers "The syntax of the command is incorrect" --
    // and then every file written here silently is not.
    std::filesystem::create_directories("build/test/out");
    FILE* f = std::fopen("build/test/out/ripped.spc", "wb");
    if(f) { std::fwrite(spc.data(), 1, spc.size(), f); std::fclose(f); }

    // --- 3. play it with the console switched off -------------------------
    Apu apu;
    if(!apu.loadSpc("build/test/out/ripped.spc", error))
    { std::printf("        %s\n", error.c_str()); check(false, "the .spc loads"); return 1; }

    check(true, "the .spc loads back into a chip of its own");
    std::printf("        \"%s\"\n", apu.title().c_str());

    std::vector<float> song;
    long long keyed[VoiceCount] = {};
    float peak = 0.f;

    const int seconds = 10;
    ChipFrame frame;
    for(int i = 0; i < (int)(ApuRate * seconds); ++i)
    {
        apu.run(frame);
        song.push_back(frame.main[0]);
        song.push_back(frame.main[1]);
        if(std::fabs(frame.main[0]) > peak) peak = std::fabs(frame.main[0]);

        const VoiceState* v = apu.voices();
        for(int n = 0; n < VoiceCount; ++n) if(v[n].keyed) ++keyed[n];
    }

    int sounded = 0;
    for(int n = 0; n < VoiceCount; ++n) if(keyed[n] > ApuRate / 10) ++sounded;

    std::printf("        %d seconds, peak %.3f, %d voices still playing\n",
                seconds, peak, sounded);
    for(int n = 0; n < VoiceCount; ++n)
        std::printf("        V%d %6.2f s\n", n + 1, (double)keyed[n] / ApuRate);

    check(peak > 0.02f, "the music carried on without the console");
    check(sounded >= 2, "and it is still the whole arrangement");

    writeWav("build/test/out/spc.wav", song, 2, (int)ApuRate);

    std::printf("\n%s\n", failures ? "  SOME CHECKS FAILED" : "  all checks passed");
    return failures ? 1 : 0;
}
