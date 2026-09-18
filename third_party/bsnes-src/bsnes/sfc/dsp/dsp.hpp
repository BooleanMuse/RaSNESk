#include "SPC_DSP.h"

struct DSP {
  shared_pointer<Emulator::Stream> stream;
  uint8 apuram[64 * 1024] = {};

  auto main() -> void;
  auto read(uint8 address) -> uint8;
  auto write(uint8 address, uint8 data) -> void;

  auto load() -> bool;
  auto power(bool reset) -> void;
  auto mute() -> bool;

  auto serialize(serializer&) -> void;

  int64 clock = 0;

#ifdef SNES_RACK_VOICE_TAPS
  // Where the host wants each sample's eight voices delivered. A plain
  // function pointer and a cookie rather than anything of nall's: the only
  // code that sets this is a Rack plugin, which does not get to see nall.
  using RackSink = void (*)(void* context, const SPC_DSP::rack_taps_t& taps);
  RackSink rackSink = nullptr;
  void*    rackSinkContext = nullptr;

  // The ARAM, for a host that wants to take the sound driver and the samples
  // the cartridge uploaded and go and do something else with them.
  auto rackApuRam() -> uint8* { return apuram; }
#endif

private:
  bool fastDSP = false;
  SPC_DSP spc_dsp;
  int16 samplebuffer[8192];

//unserialized:
  uint8 echoram[64 * 1024] = {};
};

extern DSP dsp;
