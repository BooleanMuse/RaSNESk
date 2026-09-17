// ===========================================================================
// The console, without Rack and without a window.
//
//   ./build.sh test "/path/to/a.sfc"
//
// Boots the cartridge, runs it for a few seconds of its own time, and writes
// out what came through the jacks: build/test/mix.wav, one wav per chip voice,
// and the last frame it drew as a PNG. Everything this checks is something
// that cannot be checked from inside Rack without watching and listening.
// ===========================================================================
#include "../src/snes/Console.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

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

// A 16-bit wav, so that the result can be listened to rather than believed.
static void writeWav(const std::string& path, const std::vector<float>& samples,
                     int channels, int rate)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if(!f) return;

    const uint32_t frames = (uint32_t)(samples.size() / channels);
    const uint32_t dataBytes = frames * channels * 2;

    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };

    std::fwrite("RIFF", 1, 4, f); u32(36 + dataBytes); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); u32(16); u16(1); u16((uint16_t)channels);
    u32((uint32_t)rate); u32((uint32_t)(rate * channels * 2));
    u16((uint16_t)(channels * 2)); u16(16);
    std::fwrite("data", 1, 4, f); u32(dataBytes);

    for(float s : samples)
    {
        if(s >  1.f) s =  1.f;
        if(s < -1.f) s = -1.f;
        u16((uint16_t)(int16_t)(s * 32767.f));
    }

    std::fclose(f);
}

int main(int argc, char** argv)
{
    if(argc < 2)
    {
        std::printf("usage: test_console <cartridge.sfc> [seconds] [hold-start]\n");
        return 2;
    }

    const std::string path = argv[1];
    const double seconds = argc > 2 ? std::atof(argv[2]) : 6.0;
    const bool   tapStart = argc > 3 ? std::atoi(argv[3]) != 0 : true;
    const double rate = 48000.0;

    Console console;
    check(console.claim(), "the console can be claimed");

    // Not an audio thread: this pulls audio as fast as it can, so it waits
    // for the machine rather than outrunning it and calling the silence a
    // result.
    console.setWaitForMachine(true);

    Console second;
    check(!second.claim(), "a second module does not get the same console");

    std::string error;
    bool loaded = console.load(path, error);
    if(!loaded) std::printf("        %s\n", error.c_str());
    check(loaded, "the cartridge loads");
    if(!loaded) return 1;

    std::printf("        title  \"%s\"\n", console.title().c_str());
    std::printf("        region  %s\n",   console.region().c_str());

    const int blockSize = 256;
    const int blocks = (int)(seconds * rate / blockSize);

    std::vector<ChipFrame> block(blockSize);
    std::vector<float> mix, voices[VoiceCount];
    mix.reserve((size_t)(seconds * rate * 2));

    // How many samples each voice was actually sounding for, and how loud it
    // got: a cartridge whose music never started would pass every other
    // check in here and produce silence.
    long long voiceKeyed[VoiceCount] = {};
    float     voicePeak[VoiceCount]  = {};
    float     mixPeak = 0.f;
    uint8_t   seenSrcn[256] = {};

    for(int b = 0; b < blocks; ++b)
    {
        // A tap on START every second, to get past a title screen.
        if(tapStart)
        {
            int frame = (int)((double)b * blockSize / rate * 60.0);
            console.setButton(0, ButtonStart, (frame % 60) < 8);
        }

        console.run(block.data(), blockSize, rate, 1.0);

        for(const ChipFrame& f : block)
        {
            mix.push_back(f.main[0]);
            mix.push_back(f.main[1]);
            if(std::fabs(f.main[0]) > mixPeak) mixPeak = std::fabs(f.main[0]);

            for(int v = 0; v < VoiceCount; ++v)
            {
                voices[v].push_back(f.voice[v][0]);
                voices[v].push_back(f.voice[v][1]);
                float m = std::fabs(f.dry[v]);
                if(m > voicePeak[v]) voicePeak[v] = m;
            }
        }

        const VoiceState* vs = console.voices();
        for(int v = 0; v < VoiceCount; ++v)
        {
            if(vs[v].keyed) { voiceKeyed[v] += blockSize; seenSrcn[vs[v].srcn] = 1; }
        }
    }

    check(console.frame() != nullptr, "the PPU drew a frame");
    check(console.frameSerial() > (uint32_t)(seconds * 50), "it drew about sixty a second");
    check(mixPeak > 0.01f, "the chip made a sound");

    int sounded = 0, distinct = 0;
    for(int v = 0; v < VoiceCount; ++v) if(voiceKeyed[v] > 0) ++sounded;
    for(int i = 0; i < 256; ++i) if(seenSrcn[i]) ++distinct;

    check(sounded >= 2, "more than one voice played");

    std::printf("\n        voice   keyed     peak\n");
    for(int v = 0; v < VoiceCount; ++v)
        std::printf("        V%d    %7.2f s   %5.3f\n", v + 1,
                    (double)voiceKeyed[v] / rate, voicePeak[v]);
    std::printf("        %d distinct samples were played, %u frames drawn, mix peak %.3f\n",
                distinct, console.frameSerial(), mixPeak);

    // The sum of the voices should be the mix, near enough: the chip clamps
    // and applies its master volume to the sum, so this is a sanity check on
    // the taps being the same signal, not an identity.
    double sumErr = 0.0;
    for(size_t i = 0; i < mix.size(); ++i)
    {
        double s = 0.0;
        for(int v = 0; v < VoiceCount; ++v) s += voices[v][i];
        sumErr += std::fabs(s - mix[i]);
    }
    double meanErr = mix.empty() ? 1.0 : sumErr / mix.size();
    std::printf("        mean |sum of voices - mix| = %.5f\n", meanErr);
    check(meanErr < 0.05, "the eight voices add up to the mix");

    // --- what came out ----------------------------------------------------
    system("mkdir -p build/test/out");
    writeWav("build/test/out/mix.wav", mix, 2, (int)rate);
    for(int v = 0; v < VoiceCount; ++v)
    {
        char name[64];
        std::snprintf(name, sizeof name, "build/test/out/voice%d.wav", v + 1);
        writeWav(name, voices[v], 2, (int)rate);
    }

    if(const uint8_t* px = console.frame())
        stbi_write_png("build/test/out/frame.png", ScreenW, ScreenH, 4, px, ScreenW * 4);

    // --- the instruments the cartridge brought with it ---------------------
    uint8_t aram[AramSize];
    uint8_t regs[DspRegCount];
    console.snapshotAram(aram, regs);

    FILE* f = std::fopen("build/test/out/aram.bin", "wb");
    if(f) { std::fwrite(aram, 1, AramSize, f); std::fclose(f); }

    std::printf("        DIR is at $%04x, FLG $%02x, master volume %d/%d\n",
                regs[0x5d] << 8, regs[0x6c], (int8_t)regs[0x0c], (int8_t)regs[0x1c]);

    int nonZero = 0;
    for(int i = 0; i < AramSize; ++i) if(aram[i]) ++nonZero;
    check(nonZero > 4096, "the cartridge uploaded a sound driver and its samples");

    std::printf("\n%s\n", failures ? "  SOME CHECKS FAILED" : "  all checks passed");
    return failures ? 1 : 0;
}
