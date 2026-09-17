// ===========================================================================
// The point of the whole thing, without Rack.
//
//   ./build.sh sampler "/path/to/a.sfc"
//
// Boots the cartridge long enough for it to upload its sound driver and its
// samples, takes the 64K, finds the instruments in it, and then plays a tune
// with them on a second sound chip that the game knows nothing about.
// ===========================================================================
#include "../src/snes/Console.hpp"
#include "../src/snes/Bank.hpp"
#include "../src/snes/Sdsp.hpp"

#include <cmath>
#include <cstdio>
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
    if(argc < 2) { std::printf("usage: test_sampler <cartridge.sfc> [seconds-to-boot]\n"); return 2; }

    const double boot = argc > 2 ? std::atof(argv[2]) : 20.0;

    // --- 1. let the cartridge load its instruments -------------------------
    Console console;
    if(!console.claim()) { std::printf("no console\n"); return 1; }

    // Not an audio thread: this pulls audio as fast as it can, so it waits
    // for the machine rather than outrunning it and calling the silence a
    // result.
    console.setWaitForMachine(true);

    std::string error;
    if(!console.load(argv[1], error)) { std::printf("%s\n", error.c_str()); return 1; }

    std::vector<ChipFrame> block(256);
    const int blocks = (int)(boot * 48000 / 256);
    for(int b = 0; b < blocks; ++b)
    {
        int frame = (int)((double)b * 256 / 48000 * 60.0);
        console.setButton(0, ButtonStart, (frame % 60) < 8);
        console.run(block.data(), 256, 48000.0, 1.0);
    }

    uint8_t aram[AramSize], regs[DspRegCount];
    console.snapshotAram(aram, regs);

    // --- 2. what is in there ----------------------------------------------
    Bank bank;
    bank.adopt(aram, regs[dspreg::Dir], console.title());

    check(bank.valid(), "the bank took");
    check(!bank.samples().empty(), "the cartridge's directory has instruments in it");

    std::printf("\n        \"%s\": directory at $%04x, %d instruments\n\n",
                bank.name(), bank.dirPage() * 0x100, (int)bank.samples().size());
    std::printf("        srcn   start    loop  blocks      length\n");

    for(const BrrSample& s : bank.samples())
        std::printf("        %4d   $%04x   $%04x   %5d   %6.3f s%s\n",
                    s.index, s.start, s.loop, s.blocks, s.seconds(),
                    s.loops ? "  loops" : "");

    if(bank.samples().empty()) return 1;

    // The instrument with the most to it: the longest looping one, or just
    // the longest. That is nearly always a melodic instrument rather than a
    // drum, which makes the scale below worth listening to.
    const BrrSample* pick = &bank.samples()[0];
    for(const BrrSample& s : bank.samples())
        if(s.loops == pick->loops ? s.blocks > pick->blocks : s.loops) pick = &s;

    std::printf("\n        playing srcn %d\n", pick->index);

    // --- 3. does it decode to a waveform ----------------------------------
    std::vector<float> wave;
    bank.decode(pick->index, wave, 4096);
    float peak = 0.f;
    for(float v : wave) if(std::fabs(v) > peak) peak = std::fabs(v);
    check(peak > 0.01f, "the BRR decoder produced a waveform");

    // --- 4. play it, on a chip the game does not have ----------------------
    Sdsp chip;
    chip.load(bank.ram());
    chip.reset();
    chip.write(dspreg::Dir, (uint8_t)bank.dirPage());

    // A scale, because a scale is the shortest thing that proves both that
    // the sample plays and that the pitch register means what it should.
    const int scale[8] = { 0, 2, 4, 5, 7, 9, 11, 12 };
    const double rate = ApuRate;
    const int noteFrames = (int)(rate * 0.35);

    std::vector<float> song;
    long long sounded = 0;

    for(int n = 0; n < 8; ++n)
    {
        const int v = n % VoiceCount;
        const float volts = scale[n] / 12.f;
        const int pitch = voltsToPitch(volts);

        chip.write(dspreg::voice(v, dspreg::Srcn),   (uint8_t)pick->index);
        chip.write(dspreg::voice(v, dspreg::PitchL), (uint8_t)(pitch & 0xff));
        chip.write(dspreg::voice(v, dspreg::PitchH), (uint8_t)(pitch >> 8));
        chip.write(dspreg::voice(v, dspreg::VolL),   0x60);
        chip.write(dspreg::voice(v, dspreg::VolR),   0x60);
        // Attack fast, decay slow, sustain high, release slow: an organ, so
        // that what you hear is the sample and not an envelope.
        chip.write(dspreg::voice(v, dspreg::Adsr0),  0x8f);
        chip.write(dspreg::voice(v, dspreg::Adsr1),  0xe0);
        chip.write(dspreg::Kon, (uint8_t)(1 << v));

        ChipFrame f;
        for(int i = 0; i < noteFrames; ++i)
        {
            chip.run(f);
            song.push_back(f.main[0]);
            song.push_back(f.main[1]);
            if(std::fabs(f.dry[v]) > 0.005f) ++sounded;
        }

        chip.write(dspreg::Koff, (uint8_t)(1 << v));
    }

    float songPeak = 0.f;
    for(float v : song) if(std::fabs(v) > songPeak) songPeak = std::fabs(v);

    std::printf("        the scale sounded for %.2f s of %.2f s, peak %.3f\n",
                (double)sounded / rate, song.size() / 2.0 / rate, songPeak);

    check(songPeak > 0.02f, "the cartridge's sample played on our own chip");
    check(sounded > noteFrames * 4, "every note in the scale sounded");

    system("mkdir -p build/test/out");
    writeWav("build/test/out/scale.wav", song, 2, (int)rate);
    writeWav("build/test/out/waveform.wav", wave, 1, (int)rate);

    // --- 5. and it survives a patch file ----------------------------------
    std::string packed = bank.encode();
    Bank again;
    check(again.decodeFrom(packed), "a bank packs and unpacks");
    check(again.samples().size() == bank.samples().size(),
          "and comes back with the same instruments");
    std::printf("        packed to %d bytes of text for the patch file\n", (int)packed.size());

    std::printf("\n%s\n", failures ? "  SOME CHECKS FAILED" : "  all checks passed");
    return failures ? 1 : 0;
}
