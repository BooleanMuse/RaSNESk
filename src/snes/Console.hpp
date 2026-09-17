// ===========================================================================
// The Super Nintendo, behind a door, on a thread of its own.
//
// bsnes is built out of global singletons -- `cpu`, `smp`, `dsp`, `ppu`,
// `cartridge`, `system` are one each, for the process -- so there is exactly
// one console available no matter how many modules ask. claim() is how a
// module finds out whether it is the one that got it; a second SNES module in
// the same patch says so on its screen instead of quietly sharing a CPU with
// the first.
//
// The machine runs on a thread of its own and fills a ring. Two reasons, and
// the second is the one that matters:
//
// * bsnes schedules its chips as cooperative threads (libco) and a thread is
//   resumed by switching to a stack, so it has to be entered from one thread
//   and one thread only. A thread of its own is that, exactly.
//
// * An emulator on the audio thread has to finish inside every single block,
//   and a whole Super Nintendo cannot promise that on every machine. On its
//   own thread it only has to keep up on average, and the ring absorbs the
//   difference -- which is the whole distance between a console that stutters
//   and one that does not.
//
// So: run() only ever drains a ring, and everything else here is a message to
// the thread that owns the machine. Nothing of bsnes appears in this header;
// Console.cpp is the only file in the project that sees both sides.
// ===========================================================================
#pragma once

#include "SnesCore.hpp"

namespace racksnes {

class Console {
public:
    Console();
    ~Console();

    // Only one console exists. The first module to ask gets it; everyone else
    // gets false and should say so rather than pretend.
    bool claim();
    void release();
    bool claimed() const { return owner; }

    // --- the cartridge ----------------------------------------------------
    // Safe from any thread: the work happens on the machine's own, and this
    // waits for it. A .sfc, .smc, .swc, .fig or .bin, with or without a
    // copier header.
    bool load(const std::string& path, std::string& error);
    bool loadMemory(const std::vector<uint8_t>& rom, const std::string& name,
                    std::string& error);
    void unload();

    bool        loaded() const;
    std::string title() const;          // what the cartridge's header calls itself
    std::string region() const;         // "NTSC" or "PAL"

    void power();                       // cold
    void reset();                       // the button on the front

    // --- running ----------------------------------------------------------
    // Whether the machine is running at all. The ring keeps draining either
    // way, so stopping is silent rather than stuck.
    void setRunning(bool on);

    // Fill `count` frames at the rack's sample rate, from the ring. `speed`
    // is a multiplier on the machine's own crystal: 1 is sixty frames a
    // second, 0.5 is half of everything at once -- picture, music and pitch
    // -- because the sound is read out of the same buffer the machine fills.
    //
    // Audio thread only, and it never waits for the machine.
    void run(ChipFrame* out, int count, double rackRate, double speed);

    // How much of the ring is standing full, 0 to 1, and how many samples the
    // machine has failed to have ready. A panel can show the first; the
    // second is the only honest measure of whether this is working.
    float    buffer() const;
    uint32_t underruns() const;

    // How much sound to keep ahead, in samples of the chip's own 32 kHz. More
    // is steadier and further behind your thumb.
    void setBufferDepth(int samples);

    // Whether run() may wait for the machine when the ring is empty.
    //
    // An audio thread must never wait, so it holds the last frame and counts
    // the miss. Everything that is not an audio thread -- the tests, the
    // probe, the benchmark -- pulls audio as fast as it can and would
    // otherwise outrun a machine that is only trying to keep up with real
    // time, and hear silence for its trouble.
    void setWaitForMachine(bool on);

    // Hold the picture but keep the machine running at full speed. The other
    // reading of a clock, for when what you want is a strobe.
    void setStrobe(bool on);
    void strobeAdvance();               // let exactly one frame through

    // How many frames to draw. 0 is all of them; 1 is every other; a large
    // number is effectively none. The PPU does not render a frame it is not
    // going to show, so this is the one setting that makes the whole machine
    // cheaper, and for a patch that only wants the music it is most of it.
    void setFrameSkip(int skip);

    // The accurate sound chip runs one of its clocks at a time and the fast
    // one runs a whole sample; both finish the sample in the same place, so
    // the eight voices come out either way. A couple of cartridges need the
    // accurate one and the console turns it on for them by name.
    void setFastDsp(bool on);

    // The two renderers. The fast one caches each line's state as the frame
    // goes by and rasterises the whole frame in one go; the cycle-based one
    // draws as it goes, and costs about twice as much.
    void setFastPpu(bool on);

    // How many of the chip's samples to ask the machine for in one go.
    void setChunk(int samples);

    // --- the picture ------------------------------------------------------
    // RGBA, ScreenW x ScreenH, or null before the first frame. Safe to read
    // from the drawing thread: a frame is only published whole, and only once
    // the sound that belongs with it has been played, so the two do not drift
    // apart by the depth of the buffer.
    const uint8_t* frame() const;
    uint32_t       frameSerial() const;

    // --- the gamepad ------------------------------------------------------
    void setButton(int player, int button, bool pressed);

    // --- the chip ---------------------------------------------------------
    const VoiceState* voices() const;    // VoiceCount of them

    // The 64K the sound driver and all of the cartridge's samples live in.
    // Taking a copy of this is what "importing a cartridge's instruments"
    // actually is: the cartridge uploaded them here at boot and nothing
    // outside this RAM is needed to play them again.
    //
    // Asks the machine for a copy and waits for it, so: not the audio thread.
    bool snapshotAram(uint8_t out[AramSize], uint8_t regs[DspRegCount]);

    // The whole sound chip as an .spc: 64K, the DSP's registers and where the
    // SPC-700 had got to. That is all an .spc has ever been, and it is why a
    // song ripped from a game outlives the game -- the sound chip was never
    // listening to the console in the first place, only to four bytes of it.
    bool exportSpc(std::vector<uint8_t>& out);

    // Reaching in. Both are posted to the machine and applied there, so they
    // are safe from the audio thread and land within a chunk.
    void writeDspReg(uint8_t address, uint8_t data);
    void writeApuPort(int port, uint8_t value);

    // The circuit bending, as one message a block. The machine applies it
    // itself, because reaching into a running cartridge is something that has
    // to happen where the cartridge is running.
    void bend(const BendMessage& message);

    // What was loaded, so a patch can carry the cartridge with it.
    const std::vector<uint8_t>& romImage() const;
    const std::string&          romName() const;

private:
    bool owner = false;

    struct Impl;
    Impl* impl = nullptr;
};

} // namespace racksnes
