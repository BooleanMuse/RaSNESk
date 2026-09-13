# ============================================================================
# A Super Nintendo for VCV Rack.
#
#   ./build.sh              -> plugin.so       (fetches what it needs)
#   ./build.sh install      -> installs it into your Rack
#   ./build.sh test         -> the console's own tests: no Rack, no window
#   ./build.sh dist         -> the .vcvplugin package
#
# Or, by hand:  RACK_DIR=/path/to/Rack-SDK make
# ============================================================================
RACK_DIR ?= third_party/Rack-SDK

# NOT -Isrc. Every Rack plugin's own header is called plugin.hpp, and so is
# one of the SDK's: with src on the include path ahead of the SDK, rack.hpp's
# own #include <plugin.hpp> finds ours instead, and the build depends on which
# file happened to be included first. Sources here reach each other by
# relative path.
#
# bsnes's headers are worse still -- it reaches its own code as <sfc/sfc.hpp>
# from a root that would have to be on the include path, and nall comes with
# it -- and none of them appear here at all. They belong to Makefile.core,
# which builds the console into a static library with its own flags and its
# own include path.

# The SDK pins -std=c++11 in compile.mk. EXTRA_CXXFLAGS lands after it on the
# command line and the last -std wins; it is the extension point compile.mk
# leaves open for exactly this.
EXTRA_CXXFLAGS += -std=c++17

SOURCES += $(wildcard src/*.cpp)
SOURCES += $(wildcard src/modules/*.cpp)

# The console itself, already compiled. An archive in OBJECTS lands after the
# plugin's own objects, which is the order a static library needs.
SNESCORE := build/core/libsnescore.a
OBJECTS  += $(SNESCORE)

DISTRIBUTABLES += $(wildcard LICENSE*)

include $(RACK_DIR)/plugin.mk

# Always ask the console's own make: it knows whether anything changed, and it
# is the only place that knows how to build it.
.PHONY: snescore
snescore:
	@$(MAKE) --no-print-directory -f Makefile.core

$(SNESCORE): snescore
	@:
