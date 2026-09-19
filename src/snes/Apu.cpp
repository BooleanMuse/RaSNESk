// ===========================================================================
// An SPC-700 with a sound chip bolted to it, and nothing else.
//
// bsnes's SPC-700 is a class with four things left abstract -- read, write,
// idle, and "am I synchronising" -- because the chip does not know what is
// on its bus. Here what is on its bus is 64K of RAM, the boot ROM, three
// timers and an S-DSP: the sound half of a Super Nintendo, exactly, and
// nothing of the other half.
//
// The clocks: the SPC-700 runs at 1.024 MHz and the DSP finishes a sample
// every thirty-two of its clocks, which is one DSP clock per CPU cycle and
// 32040 samples a second. bsnes counts both in half-clocks, and so does the
// wait-state table, which is why everything below is in units of two.
// ===========================================================================
#include "Apu.hpp"
#include "Sdsp.hpp"
#include "../Path.hpp"

#include <emulator/emulator.hpp>
#include <processor/spc700/spc700.hpp>
using namespace nall;

#include <cstdio>
#include <cstring>

namespace racksnes {

namespace {

// The 64 bytes every SNES starts from: the loader the CPU talks to over the
// four ports. bsnes ships it, and this is the same array its own targets use.
#include <target-libretro/resources.hpp>

} // namespace

// ===========================================================================

struct Apu::Impl : Processor::SPC700 {

    Sdsp      dsp;
    uint8_t   memory[AramSize] = {};
    bool      has = false;
    std::string name;

    ChipFrame pending;
    bool      ready = false;

    VoiceState voice[VoiceCount] = {};

    // --- the register page at $00f0 --------------------------------------
    struct {
        uint8_t apu[4] = {};        // what the console wrote to us
        uint8_t cpu[4] = {};        // what we wrote back to the console
        uint8_t aux[2] = {};

        bool    timersDisable = false;
        bool    ramWritable   = true;
        bool    ramDisable    = false;
        bool    timersEnable  = true;
        int     externalWait  = 0;
        int     internalWait  = 0;
        bool    iplEnable     = true;
        uint8_t dspAddr       = 0;
    } io;

    // Three of them: two at 8 kHz and one at 64 kHz, each a chain of four
    // stages where only the last is visible to the program.
    template <int Frequency>
    struct Timer {
        int     stage0 = 0;
        int     stage1 = 0;
        int     stage2 = 0;
        int     stage3 = 0;     // four bits, and reading it clears it
        bool    line   = false;
        bool    enable = false;
        uint8_t target = 0;

        void step(int clocks, bool enabled)
        {
            stage0 += clocks;
            if(stage0 < Frequency) return;
            stage0 -= Frequency;
            stage1 ^= 1;
            pulse(enabled);
        }

        void pulse(bool enabled)
        {
            bool level = stage1 && enabled;
            if(!(line && !level)) { line = level; return; }   // only 1 -> 0
            line = level;

            if(!enable) return;
            if(++stage2 != target) return;

            stage2 = 0;
            stage3 = (stage3 + 1) & 15;
        }
    };

    Timer<128> timer0, timer1;
    Timer<16>  timer2;

    int dspUnits = 0;

    Impl() { reset(); }

    // --- what the SPC-700 needs of us -------------------------------------
    auto synchronizing() const -> bool override { return false; }

    auto idle() -> void override { wait(-1, false); }

    auto read(uint16 address) -> uint8 override
    {
        // A read of the four ports holds the bus either side of itself. One
        // game depends on it and the chip's own test suite depends on the
        // ordinary case not doing it.
        if((address & 0xfffc) == 0x00f4)
        {
            wait(address, true);
            uint8 data = readRam(address);
            if((address & 0xfff0) == 0x00f0) data = readIo(address);
            wait(address, true);
            return data;
        }

        wait(address, false);
        uint8 data = readRam(address);
        if((address & 0xfff0) == 0x00f0) data = readIo(address);
        return data;
    }

    auto write(uint16 address, uint8 data) -> void override
    {
        wait(address, false);
        writeRam(address, data);     // an IO write lands in the RAM under it too
        if((address & 0xfff0) == 0x00f0) writeIo(address, data);
    }

    auto readDisassembler(uint16 address) -> uint8 override
    {
        if((address & 0xfff0) == 0x00f0) return 0x00;
        return readRam(address);
    }

    // --- the bus ----------------------------------------------------------
    uint8_t readRam(uint16_t address)
    {
        if(address >= 0xffc0 && io.iplEnable) return iplrom[address & 0x3f];
        if(io.ramDisable) return 0x5a;
        return memory[address];
    }

    void writeRam(uint16_t address, uint8_t data)
    {
        if(io.ramWritable && !io.ramDisable) memory[address] = data;
    }

    uint8_t readIo(uint16_t address)
    {
        switch(address)
        {
        case 0xf0: case 0xf1: return 0x00;          // write-only
        case 0xf2: return io.dspAddr;
        case 0xf3: return dsp.read(io.dspAddr & 0x7f);
        case 0xf4: case 0xf5: case 0xf6: case 0xf7: return io.apu[address - 0xf4];
        case 0xf8: return io.aux[0];
        case 0xf9: return io.aux[1];
        case 0xfa: case 0xfb: case 0xfc: return 0x00;
        case 0xfd: { uint8_t v = (uint8_t)timer0.stage3; timer0.stage3 = 0; return v; }
        case 0xfe: { uint8_t v = (uint8_t)timer1.stage3; timer1.stage3 = 0; return v; }
        case 0xff: { uint8_t v = (uint8_t)timer2.stage3; timer2.stage3 = 0; return v; }
        }
        return 0x00;
    }

    void writeIo(uint16_t address, uint8_t data)
    {
        switch(address)
        {
        case 0xf0:                                   // TEST
            if(r.p.p) break;                         // only while the P flag is clear
            io.timersDisable = data >> 0 & 1;
            io.ramWritable   = data >> 1 & 1;
            io.ramDisable    = data >> 2 & 1;
            io.timersEnable  = data >> 3 & 1;
            io.externalWait  = data >> 4 & 3;
            io.internalWait  = data >> 6 & 3;
            timer0.pulse(timersOn());
            timer1.pulse(timersOn());
            timer2.pulse(timersOn());
            break;

        case 0xf1:                                   // CONTROL
            if(!timer0.enable && (data & 0x01)) { timer0.stage2 = 0; timer0.stage3 = 0; }
            if(!timer1.enable && (data & 0x02)) { timer1.stage2 = 0; timer1.stage3 = 0; }
            if(!timer2.enable && (data & 0x04)) { timer2.stage2 = 0; timer2.stage3 = 0; }
            timer0.enable = data & 0x01;
            timer1.enable = data & 0x02;
            timer2.enable = data & 0x04;

            if(data & 0x10) { io.apu[0] = 0; io.apu[1] = 0; }
            if(data & 0x20) { io.apu[2] = 0; io.apu[3] = 0; }

            io.iplEnable = (data & 0x80) != 0;
            break;

        case 0xf2: io.dspAddr = data; break;
        case 0xf3: if(!(io.dspAddr & 0x80)) dsp.write(io.dspAddr & 0x7f, data); break;

        case 0xf4: case 0xf5: case 0xf6: case 0xf7:
            io.cpu[address - 0xf4] = data;
            break;

        case 0xf8: io.aux[0] = data; break;
        case 0xf9: io.aux[1] = data; break;
        case 0xfa: timer0.target = data; break;
        case 0xfb: timer1.target = data; break;
        case 0xfc: timer2.target = data; break;
        default: break;                              // $fd-$ff are read-only
        }
    }

    bool timersOn() const { return io.timersEnable && !io.timersDisable; }

    // --- time ---------------------------------------------------------------
    void wait(int address, bool half)
    {
        // The chip divides its clock by {2,4,8,16}; on the last two the CPU
        // really takes 10 and 20, which is a hardware fault nobody relies on
        // but which bsnes reproduces, and the timers do not share.
        static const int cycleWait[4] = { 2, 4, 10, 20 };
        static const int timerWait[4] = { 2, 4,  8, 16 };

        int ws = io.externalWait;
        if(address < 0)                              ws = io.internalWait;
        else if((address & 0xfff0) == 0x00f0)        ws = io.internalWait;
        else if(address >= 0xffc0 && io.iplEnable)   ws = io.internalWait;

        step(cycleWait[ws] >> (half ? 1 : 0));
        stepTimers(timerWait[ws] >> (half ? 1 : 0));
    }

    void step(int units)
    {
        dspUnits += units;
        while(dspUnits >= 2)
        {
            dspUnits -= 2;
            dsp.step(1);
            if(dsp.takeSample(pending)) { ready = true; readVoices(); }
        }
    }

    void stepTimers(int clocks)
    {
        const bool on = timersOn();
        timer0.step(clocks, on);
        timer1.step(clocks, on);
        timer2.step(clocks, on);
    }

    void readVoices()
    {
        const uint8_t keyed = dsp.keyed();
        for(int v = 0; v < VoiceCount; ++v)
        {
            voice[v].level = dsp.envelope(v) * (1.f / 127.f);
            voice[v].srcn  = dsp.read(dspreg::voice(v, dspreg::Srcn));
            voice[v].keyed = (keyed >> v) & 1;

            if(voice[v].keyed)
            {
                const int pitch = dsp.read(dspreg::voice(v, dspreg::PitchL))
                                | dsp.read(dspreg::voice(v, dspreg::PitchH)) << 8;
                voice[v].volts = pitchToVolts(pitch & 0x3fff);
            }
        }
    }

    // --- running ------------------------------------------------------------
    void runOne(ChipFrame& out)
    {
        int guard = 0;

        while(!ready && guard++ < 4096)
        {
            // SLEEP and STOP halt the processor but not the chip beside it.
            if(r.wait || r.stop) { wait(-1, false); continue; }
            instruction();
        }

        out   = pending;
        ready = false;
    }

    void reset()
    {
        SPC700::power();

        io = {};
        timer0 = {};
        timer1 = {};
        timer2 = {};
        dspUnits = 0;
        ready = false;

        dsp.load(memory);
        dsp.reset();

        // Where the boot ROM says to start.
        r.pc.byte.l = iplrom[62];
        r.pc.byte.h = iplrom[63];
    }
};

// ===========================================================================

Apu::Apu() : impl(new Impl) {}
Apu::~Apu() { delete impl; }

bool Apu::loadSpc(const std::string& path, std::string& error)
{
    FILE* f = openBinary(path, "rb");
    if(!f) { error = "could not read that file"; return false; }

    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);

    if(size < 0x10180) { std::fclose(f); error = "too small to be an .spc"; return false; }

    std::vector<uint8_t> blob((size_t)size);
    size_t got = std::fread(blob.data(), 1, blob.size(), f);
    std::fclose(f);
    if(got != blob.size()) { error = "that file ended early"; return false; }

    if(std::memcmp(blob.data(), "SNES-SPC700 Sound File Data", 27) != 0)
    { error = "that is not an .spc"; return false; }

    impl->reset();

    std::memcpy(impl->memory, blob.data() + 0x100, AramSize);
    impl->dsp.load(impl->memory);

    // The chip's registers as they were, in the order the file has them.
    for(int i = 0; i < DspRegCount; ++i)
        impl->dsp.write((uint8_t)i, blob[0x10100 + i]);

    // And the processor, mid-instruction.
    impl->r.pc.w   = (uint16_t)(blob[0x25] | blob[0x26] << 8);
    impl->r.ya.byte.l = blob[0x27];          // A
    impl->r.x      = blob[0x28];
    impl->r.ya.byte.h = blob[0x29];          // Y
    impl->r.p      = blob[0x2a];
    impl->r.s      = blob[0x2b];

    // The register page travels inside the 64K, so the state of the timers
    // and of the boot ROM switch has to be read back out of it.
    impl->io.dspAddr = impl->memory[0xf2];

    // $f4-$f7 in a dump are what the sound driver last wrote *out* to the
    // console. What the console last wrote *in* is in a latch that is not
    // memory and that no .spc has ever carried. Every player makes the same
    // guess, and it is the right one: a driver echoes the command it is
    // working on back to the same port, so the value in RAM is very likely
    // the command it thinks it has. Handing it back its own last command is
    // what keeps the song going; handing it a zero is, to most drivers, the
    // order to stop.
    for(int i = 0; i < 4; ++i)
    {
        impl->io.cpu[i] = impl->memory[0xf4 + i];
        impl->io.apu[i] = impl->memory[0xf4 + i];
    }
    impl->timer0.target = impl->memory[0xfa];
    impl->timer1.target = impl->memory[0xfb];
    impl->timer2.target = impl->memory[0xfc];

    // $f1 is where the timers and the boot ROM are switched, so it has to be
    // put back -- but two of its bits are not state at all: bits 4 and 5
    // clear the port latches when they are written, once. They land in the
    // dumped RAM like everything else, and writing that byte back would
    // throw away the command the driver is in the middle of following. The
    // whole song stops, quietly, and every other symptom looks like a broken
    // processor.
    impl->writeIo(0xf1, (uint8_t)(impl->memory[0xf1] & 0xcf));

    char header[33] = {};
    std::memcpy(header, blob.data() + 0x2e, 32);
    for(int i = 31; i >= 0 && (header[i] == ' ' || header[i] == 0); --i) header[i] = 0;
    impl->name = header[0] ? header : "spc";

    impl->has = true;
    return true;
}

void Apu::loadBank(const Bank& bank)
{
    if(!bank.valid()) return;

    std::memcpy(impl->memory, bank.ram(), AramSize);
    impl->reset();

    for(int i = 0; i < DspRegCount; ++i)
        impl->dsp.write((uint8_t)i, bank.dspRegs()[i]);

    impl->name = bank.name();
    impl->has  = true;
}

void Apu::reset()     { impl->reset(); }
bool Apu::loaded() const { return impl->has; }

void Apu::run(ChipFrame& out)
{
    if(!impl->has) { out = ChipFrame(); return; }
    impl->runOne(out);
}

void Apu::writePort(int port, uint8_t value)
{
    if(port < 0 || port > 3) return;
    impl->io.apu[port] = value;
}

uint8_t Apu::readPort(int port) const
{
    if(port < 0 || port > 3) return 0;
    return impl->io.cpu[port];
}

void    Apu::writeDsp(uint8_t address, uint8_t value) { impl->dsp.write(address, value); }
uint8_t Apu::readDsp(uint8_t address) const           { return impl->dsp.read(address); }

uint8_t*       Apu::ram()       { return impl->memory; }
const uint8_t* Apu::ram() const { return impl->memory; }

const VoiceState* Apu::voices() const { return impl->voice; }
uint16_t          Apu::programCounter() const { return impl->r.pc.w; }
std::string       Apu::title()  const { return impl->name; }

} // namespace racksnes
