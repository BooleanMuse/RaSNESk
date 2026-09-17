// ===========================================================================
// The sound chip on its own: an SPC-700, its 64K, and an S-DSP.
//
// This is the whole of what makes a SNES sing, with the console taken away.
// It is not a reduction -- it is what an .spc file is: somebody stopped a
// game mid-song, wrote down the sound chip's 64K and its registers, and threw
// the rest of the console away, and the music carried on.
//
// The four ports are the only thing the console's CPU ever had. Writing them
// from a rack is writing them the way the game's own code did, which is how a
// cartridge's sound driver can be asked for a different song by somebody who
// is not the cartridge.
// ===========================================================================
#pragma once

#include "SnesCore.hpp"
#include "Bank.hpp"

namespace racksnes {

class Apu {
public:
    Apu();
    ~Apu();

    Apu(const Apu&) = delete;
    Apu& operator=(const Apu&) = delete;

    // A song, frozen: 64K, the DSP's registers, and where the CPU was.
    bool loadSpc(const std::string& path, std::string& error);

    // A cartridge's RAM without a CPU state to go with it: the sound driver
    // is in there and gets started from the boot ROM, which is what happens
    // on a console at power-on.
    void loadBank(const Bank& bank);

    void reset();
    bool loaded() const;

    // One sample at 32040 Hz, taken apart.
    void run(ChipFrame& out);

    // The four letterboxes, from the console's side.
    void    writePort(int port, uint8_t value);
    uint8_t readPort(int port) const;

    // And the chip's own registers, for the modules that reach past the
    // driver entirely.
    void    writeDsp(uint8_t address, uint8_t value);
    uint8_t readDsp(uint8_t address) const;

    uint8_t* ram();
    const uint8_t* ram() const;

    const VoiceState* voices() const;
    std::string       title() const;

    // Where the processor is, for anything that needs to know whether the
    // sound driver is running or stuck.
    uint16_t          programCounter() const;

private:
    struct Impl;
    Impl* impl;
};

} // namespace racksnes
