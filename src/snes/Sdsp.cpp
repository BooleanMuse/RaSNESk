// ===========================================================================
// blargg's SPC_DSP, compiled a second time so that it can be owned.
// ===========================================================================

// Everything blargg_common.h and SPC_DSP.cpp reach for, included out here
// where a system header belongs. Inside the namespace their include guards
// make them no-ops, which is the only way to wrap a C++ header in one.
#include <stddef.h>
#include <stdlib.h>
#include <assert.h>
#include <limits.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <new>
#if defined(__linux__)
  #include <endian.h>
#endif

#include "Sdsp.hpp"

namespace racksnes {
namespace chip {

// bsnes's one addition to blargg's DSP reads a switch off a global that
// belongs to the emulator. Standing alone, we own that switch.
struct Hacks { struct { struct { bool cubic = false; } dsp; } hacks; };
static Hacks configuration;

#include <sfc/dsp/SPC_DSP.h>
#include <sfc/dsp/SPC_DSP.cpp>

} // namespace chip

struct Sdsp::Impl {
    chip::SPC_DSP dsp;
    uint8_t       ram [AramSize] = {};
    uint8_t       echo[AramSize] = {};   // the echo lives away from the samples
    short         out[4] = {};
};

Sdsp::Sdsp() : impl(new Impl)
{
    impl->dsp.init(impl->ram, impl->echo);
    reset();
}

Sdsp::~Sdsp() { delete impl; }

void Sdsp::setCubic(bool on) { chip::configuration.hacks.dsp.cubic = on; }

void Sdsp::load(const uint8_t aram[AramSize])
{
    memcpy(impl->ram, aram, AramSize);
}

void Sdsp::reset()
{
    memset(impl->echo, 0, AramSize);
    impl->dsp.reset();
    impl->dsp.set_output(impl->out, 4);

    // The chip comes up muted with its echo writes disabled, which is right
    // for a console whose driver is about to set everything. Here there is no
    // driver, so: sound on, echo writes off until somebody asks for them.
    impl->dsp.write(dspreg::Flg,   0x20);
    impl->dsp.write(dspreg::MVolL, 0x7f);
    impl->dsp.write(dspreg::MVolR, 0x7f);
}

void Sdsp::write(uint8_t address, uint8_t value)
{
    impl->dsp.write(address & 0x7f, value);
}

uint8_t Sdsp::read(uint8_t address) const
{
    return impl->dsp.read(address & 0x7f);
}

uint8_t* Sdsp::ram() { return impl->ram; }

void Sdsp::run(ChipFrame& out)
{
    step(32);                              // thirty-two clocks is one sample
    takeSample(out);
}

void Sdsp::step(int clocks)
{
    impl->dsp.run(clocks);
}

bool Sdsp::takeSample(ChipFrame& out)
{
    if(impl->dsp.sample_count() < 2) return false;
    impl->dsp.set_output(impl->out, 4);

    const chip::SPC_DSP::rack_taps_t& t = impl->dsp.rack_taps();
    constexpr float Scale = 1.f / 32768.f;

    out.main[0] = t.main[0] * Scale;
    out.main[1] = t.main[1] * Scale;
    out.echo[0] = t.echo[0] * Scale;
    out.echo[1] = t.echo[1] * Scale;

    for(int v = 0; v < VoiceCount; ++v)
    {
        out.voice[v][0] = t.voice[v][0] * Scale;
        out.voice[v][1] = t.voice[v][1] * Scale;
        out.dry[v]      = t.dry[v]      * Scale;
    }

    return true;
}

uint8_t Sdsp::keyed() const { return impl->dsp.rack_taps().keyed; }

uint8_t Sdsp::envelope(int voice) const
{
    if(voice < 0 || voice >= VoiceCount) return 0;
    return impl->dsp.rack_taps().env[voice];
}

} // namespace racksnes
