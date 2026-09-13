#pragma once

#include <rack.hpp>

#include "Layout.hpp"
#include "snes/SnesCore.hpp"

using namespace rack;

extern Plugin* pluginInstance;

extern Model* modelSnes;
extern Model* modelSampler;
extern Model* modelApu;
extern Model* modelBender;

namespace racksnes {

// Only tools/probe calls this. It drives the console module the way Rack
// does but as fast as it can go rather than in real time, which would outrun
// a machine that is only trying to keep up with a clock; this lets the
// machine be waited for instead.
void snesRunOffline(engine::Module* module, bool on);

// And this: in Rack the widget's step() does the things that wait for the
// machine -- loading a cartridge, taking a copy of its sound chip. A probe
// has no widget, so it has to do that job itself.
void snesServe(engine::Module* module);
uint32_t snesUnderruns(engine::Module* module);

// What BENDER sends to the SNES beside it. The struct itself lives in
// snes/SnesCore.hpp, because the machine is what applies it and the machine
// has its own thread to apply it on.

// SNES -> BENDER, once a frame: enough for the expander to say whether it is
// attached to anything and what that thing is doing.
struct BendReply {
    bool     present = false;
    bool     running = false;
    uint8_t  keyed   = 0;
};

} // namespace racksnes
