// ===========================================================================
// A sound chip of one's own.
//
// bsnes has exactly one of everything, so the console's S-DSP belongs to the
// console and to the game running on it. This is a second one -- the same
// code, blargg's SPC_DSP, compiled again into a namespace of ours -- with no
// SPC-700 above it and nobody else writing its registers. You give it a
// cartridge's 64K and then you are the sound driver.
//
// One difference from the chip in the console, and it is deliberate: the echo
// buffer is somewhere else. On real hardware the echo is written back into
// the same 64K the samples live in, and a sound driver carefully leaves room
// for it. Here the samples are the whole point and there is no driver to
// leave room, so the echo gets a 64K of its own and can never eat an
// instrument. (bsnes has the same switch, for older save states.)
// ===========================================================================
#pragma once

#include "SnesCore.hpp"

namespace racksnes {

class Sdsp {
public:
    Sdsp();
    ~Sdsp();

    Sdsp(const Sdsp&) = delete;
    Sdsp& operator=(const Sdsp&) = delete;

    // The cartridge's 64K. Copied: what happens here does not reach back.
    void load(const uint8_t aram[AramSize]);
    void reset();

    // You are the sound driver: these are the chip's own 128 registers, and
    // the addresses are the ones every SNES sound programmer knows.
    void    write(uint8_t address, uint8_t value);
    uint8_t read(uint8_t address) const;

    // Exactly one sample, at 32040 Hz, taken apart the same way the console's
    // chip is.
    void run(ChipFrame& out);

    // The same thing a clock at a time, for a caller that has a CPU to keep
    // in step with it. Thirty-two clocks make a sample; step() never makes
    // more than one, so a caller stepping one at a time cannot miss any.
    void step(int clocks);
    bool takeSample(ChipFrame& out);

    // Live, for a bend that wants to write into the samples themselves.
    uint8_t* ram();

    // Which voices are sounding, so that stealing can prefer a silent one.
    uint8_t  keyed() const;
    uint8_t  envelope(int voice) const;

    // bsnes's alternative to the chip's own gaussian interpolation: cleaner,
    // and not what a SNES sounds like. It is one switch for the whole
    // process, so it is set per block by whoever runs next -- which is fine,
    // because everything runs on one thread.
    static void setCubic(bool on);

private:
    struct Impl;
    Impl* impl;
};

// The register map, by name, because "write(0x5d, page)" reads like nothing.
namespace dspreg {
constexpr uint8_t VolL   = 0x00;   // + voice * 0x10
constexpr uint8_t VolR   = 0x01;
constexpr uint8_t PitchL = 0x02;
constexpr uint8_t PitchH = 0x03;
constexpr uint8_t Srcn   = 0x04;
constexpr uint8_t Adsr0  = 0x05;
constexpr uint8_t Adsr1  = 0x06;
constexpr uint8_t Gain   = 0x07;
constexpr uint8_t Envx   = 0x08;
constexpr uint8_t Outx   = 0x09;

constexpr uint8_t MVolL  = 0x0c;
constexpr uint8_t MVolR  = 0x1c;
constexpr uint8_t EVolL  = 0x2c;
constexpr uint8_t EVolR  = 0x3c;
constexpr uint8_t Kon    = 0x4c;
constexpr uint8_t Koff   = 0x5c;
constexpr uint8_t Flg    = 0x6c;
constexpr uint8_t Endx   = 0x7c;

constexpr uint8_t Efb    = 0x0d;
constexpr uint8_t Pmon   = 0x2d;
constexpr uint8_t Non    = 0x3d;
constexpr uint8_t Eon    = 0x4d;
constexpr uint8_t Dir    = 0x5d;
constexpr uint8_t Esa    = 0x6d;
constexpr uint8_t Edl    = 0x7d;

constexpr uint8_t Fir    = 0x0f;   // + tap * 0x10, eight of them

inline uint8_t voice(int v, uint8_t reg) { return (uint8_t)(v * 0x10 + reg); }
} // namespace dspreg

} // namespace racksnes
