# ============================================================================
# RaSNESk -- a Super Nintendo for VCV Rack.
#
#   make                 the plugin for this machine
#   make install         and put it in your Rack
#   make dist            the .vcvplugin package
#
# This is the whole build: `make dist` with RACK_DIR pointing at a Rack SDK is
# all the library's toolchain runs, and all it needs. It fetches nothing from
# the network -- bsnes is a submodule -- and it cross-compiles wherever the
# SDK does, which is Linux, Windows and both flavours of macOS.
#
# ./build.sh is a friendlier wrapper around the same thing, with the tests and
# the panel mockups; nothing here depends on it.
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

DISTRIBUTABLES += $(wildcard LICENSE*) README.md

# --- the console, already compiled ------------------------------------------
# One library per target, because this cross-compiles and two architectures'
# objects must not meet.
#
# This has to be added to OBJECTS *before* plugin.mk, and the reason is worth
# writing down: make expands a rule's prerequisites when it reads the rule, so
# anything appended to OBJECTS afterwards still reaches the link command --
# which expands late -- but is never built, because it was not a prerequisite
# of anything. It links against whatever happens to be lying there, which on
# a machine that has built before is the right file and on a fresh checkout is
# no file at all. arch.mk is included early for ARCH_NAME and again by
# compile.mk, which does it no harm.
include $(RACK_DIR)/arch.mk

SNESCORE := build/core-$(ARCH_NAME)/libsnescore.a
OBJECTS  += $(SNESCORE)

include $(RACK_DIR)/plugin.mk

# --- Windows: carry the runtime, do not borrow it ---------------------------
# The SDK asks for -static-libstdc++ and stops there, which leaves a DLL that
# needs libgcc_s_seh-1.dll and libwinpthread-1.dll sitting next to Rack. This
# console runs its machine on a std::thread, so winpthread is ours to bring;
# --whole-archive is what pulls in its initialisers rather than just the
# symbols that happen to be referenced.
ifdef ARCH_WIN
	LDFLAGS += -static-libgcc
	LDFLAGS += -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive
endif

# bsnes is a submodule, and our one change to it is a patch applied here
# rather than a fork: it is eighty lines in two files, and a fork would be a
# thing to maintain. The stamp is what stops it being applied twice.
BSNES_DIR   := third_party/bsnes-src
BSNES_STAMP := $(BSNES_DIR)/.rasnesk-patched

$(BSNES_DIR)/bsnes/sfc/sfc.hpp:
	@echo "-- Fetching bsnes..."
	git submodule update --init --recursive $(BSNES_DIR)

# Not --depth 1: the submodule is pinned to a commit, and a shallow fetch
# brings back whatever the branch tip is today, which is not it.
$(BSNES_STAMP): patches/bsnes-rack.patch $(BSNES_DIR)/bsnes/sfc/sfc.hpp
	@cd $(BSNES_DIR) && \
	if patch -p1 -R --dry-run --silent < ../../patches/bsnes-rack.patch >/dev/null 2>&1; then \
		echo "-- bsnes is already patched"; \
	else \
		echo "-- Patching bsnes: the eight voices, kept apart..."; \
		patch -p1 --silent < ../../patches/bsnes-rack.patch; \
	fi
	@touch $@

# Always ask the console's own make: it knows whether anything changed, and it
# is the only place that knows how to build it.
.PHONY: snescore
snescore: $(BSNES_STAMP)
	@$(MAKE) --no-print-directory -f Makefile.core RACK_DIR="$(RACK_DIR)"

$(SNESCORE): snescore
	@:


# The SDK's own clean takes build/ with it, and the console's library lives
# in there, so there is nothing else to remove.
#
# One thing worth knowing if you build for more than one platform in the same
# checkout: the SDK compiles into build/ without an architecture in the path,
# so `make clean` between targets is not optional. The workflow in
# .github/workflows builds each one in a checkout of its own and never has to
# think about it.
