// ===========================================================================
// A cartridge's instruments.
//
// Every SNES game builds its own sampler. The cartridge uploads a sound
// driver and a pile of BRR samples into the sound chip's 64K at boot, writes
// the page number of a directory into the DIR register, and from then on a
// "note" is an index into that directory plus a pitch. Take a copy of those
// 64K and you have taken the instruments: nothing outside the sound chip is
// needed to play them again.
//
// That is what a Bank is. It travels in a patch file, it can be read out of a
// running console or out of an .spc, and it is what the SAMPLER plays.
// ===========================================================================
#pragma once

#include "SnesCore.hpp"

namespace racksnes {

// One entry of the directory that turned out to be a real sample.
struct BrrSample {
    int  index  = 0;      // its SRCN: what a voice writes to play it
    int  start  = 0;      // where its first block is in the 64K
    int  loop   = 0;      // where the chip jumps back to, if it loops
    int  blocks = 0;      // nine bytes each, sixteen samples each
    bool loops  = false;

    int  frames() const { return blocks * 16; }
    // At the pitch the chip calls 1:1, which is the note every sound driver
    // treats as this sample's root.
    float seconds() const { return frames() / (float)ApuRate; }
};

class Bank {
public:
    Bank();

    // --- filling it ------------------------------------------------------
    // A snapshot straight out of a running console, plus the DIR register.
    void adopt(const uint8_t aram[AramSize], int dirPage, const std::string& name);

    // An .spc: 64K of RAM, 128 DSP registers and the state of a CPU, which is
    // a song frozen mid-performance. We take the RAM and the registers.
    bool loadSpc(const std::string& path, std::string& error);

    // A .brr or a raw dump. The 64K is filled with it and a directory of one
    // entry is invented, so that a sample from anywhere can be played by the
    // same chip.
    bool loadRaw(const std::string& path, std::string& error);

    void clear();

    // --- reading it ------------------------------------------------------
    bool        valid() const { return present; }
    const char* name()  const { return title.c_str(); }
    int         dirPage() const { return dir; }
    uint32_t    serial() const { return stamp; }

    const uint8_t* ram() const { return memory; }
    uint8_t*       ram()       { return memory; }
    const uint8_t* dspRegs() const { return regs; }

    const std::vector<BrrSample>& samples() const { return found; }

    // Where in the list a given SRCN sits, or -1. The list is the entries
    // that turned out to be real, not all 256 of them.
    int slotOf(int srcn) const;
    // The nth real sample's SRCN, wrapping. What a knob turns into.
    int srcnAt(int slot) const;

    // The sample as sound, at the chip's own rate, for drawing and for
    // anything that wants to look at it rather than play it.
    void decode(int srcn, std::vector<float>& out, int limitFrames = 0) const;

    // --- what a patch file carries ---------------------------------------
    std::string encode() const;                 // base64 of the 64K, packed
    bool        decodeFrom(const std::string&);

private:
    void scan();

    bool                   present = false;
    std::string            title;
    int                    dir = 0;
    uint32_t               stamp = 0;
    uint8_t                memory[AramSize] = {};
    uint8_t                regs[DspRegCount] = {};
    std::vector<BrrSample> found;
};

// ---------------------------------------------------------------------------
// One shelf, for the whole plugin.
//
// A SAMPLER does not have to be next to the console to play what the console
// ripped -- expanders are a geometry, and this is not about geometry. The
// console publishes here when RIP is pressed; anything that wants the
// cartridge's instruments takes a copy.
// ---------------------------------------------------------------------------
namespace shelf {

void     publish(const Bank& bank);
bool     take(Bank& into);       // false if nothing has been published
uint32_t serial();               // changes when something new arrives
std::string label();             // what the last thing published called itself

} // namespace shelf

} // namespace racksnes
