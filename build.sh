#!/usr/bin/env bash
# ============================================================================
# Builds the Super Nintendo for VCV Rack in one go.
#
#   ./build.sh                 -> plugin.so
#   ./build.sh install         -> also installs it into your Rack (restart it)
#   ./build.sh test   [rom]    -> the console, with no Rack and no window
#   ./build.sh sampler [rom]   -> rips a cartridge's instruments and plays them
#   ./build.sh apu    [rom]    -> the sound chip alone, and an .spc ripped
#   ./build.sh bench  [rom]    -> what the console costs, in processor time
#   ./build.sh probe  [rom]    -> the two calls Rack makes, and the jacks, no Rack
#   ./build.sh mockup          -> every panel as a PNG, without opening Rack
#   ./build.sh win             -> the Windows plugin, cross-compiled from here
#   ./build.sh dist            -> the .vcvplugin package for sharing
#   ./build.sh clean
#
# The Rack SDK, bsnes's source and one stb header are fetched on first use and
# nothing else is needed. This project never asks you to go and install
# something by hand.
# ============================================================================
set -e
cd "$(dirname "$0")"

SDK_VER="${SDK_VER:-2.6.6}"

SDK_DIR="${RACK_DIR:-third_party/Rack-SDK}"
BSNES_DIR="third_party/bsnes-src"

# The commit the patch in patches/ was made against. Pinned, because the patch
# reaches into the sound chip, and "whatever master is today" is not a thing a
# patch can be made against.
BSNES_COMMIT="7d5aa1e656b9171524d01b1b22917197d8121cb4"

# Whatever cartridge the tests are pointed at when nothing is named. A SNES
# module with no cartridge is a television with nothing on.
ROM_DEFAULT="${SNES_ROM:-}"

case "$(uname -s)" in
    Linux)  SDK_OS=lin-x64 ;;
    Darwin) SDK_OS=$([ "$(uname -m)" = "arm64" ] && echo mac-arm64 || echo mac-x64) ;;
    *)      SDK_OS=win-x64 ;;
esac

need() {
    command -v "$1" >/dev/null || { echo "ERROR: '$1' is missing and the build needs it."; exit 1; }
}

JOBS=$(nproc 2>/dev/null || echo 4)

# plugin.mk reads these out of plugin.json with jq. Passing them on the command
# line overrides that assignment outright, so jq never has to exist.
SLUG=$(python3 -c 'import json;print(json.load(open("plugin.json"))["slug"])')
VERSION=$(python3 -c 'import json;print(json.load(open("plugin.json"))["version"])')
MAKEVARS=(RACK_DIR="$SDK_DIR" SLUG="$SLUG" VERSION="$VERSION")

# Cross-compiling: ./build.sh win  builds the Windows plugin from here, with
# whatever mingw-w64 is installed. The SDK for the target has to be fetched
# too, which fetch_sdk does off SDK_OS.
CROSS=()
if [ -n "${CROSS_PREFIX:-}" ]; then
    CROSS=(CC="${CROSS_PREFIX}gcc" CXX="${CROSS_PREFIX}g++" AR="${CROSS_PREFIX}ar"
           STRIP="${CROSS_PREFIX}strip" OBJCOPY="${CROSS_PREFIX}objcopy")
fi

CORE_INC=(-I"$BSNES_DIR/bsnes" -I"$BSNES_DIR" -Isrc)

# ---------------------------------------------------------------------------
# What has to be on disk before anything can be built
# ---------------------------------------------------------------------------
fetch_sdk() {
    [ -f "$SDK_DIR/plugin.mk" ] && return 0

    echo "-- Downloading the Rack SDK $SDK_VER ($SDK_OS)..."
    need curl; need unzip
    mkdir -p third_party
    curl -L --fail -o third_party/sdk.zip \
        "https://vcvrack.com/downloads/Rack-SDK-$SDK_VER-$SDK_OS.zip"
    unzip -q -o third_party/sdk.zip -d third_party
    rm -f third_party/sdk.zip

    # The zip always unpacks as Rack-SDK; a second one for another target has
    # to be told apart from the first.
    if [ "$SDK_DIR" != "third_party/Rack-SDK" ]; then
        rm -rf "$SDK_DIR"
        mv third_party/Rack-SDK "$SDK_DIR"
    fi
}

fetch_stb() {
    [ -f third_party/stb_image_write.h ] && return 0
    echo "-- Fetching stb_image_write.h..."
    need curl
    curl -sL --fail -o third_party/stb_image_write.h \
        "https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h"
}

# bsnes is a git submodule and the Makefile owns both fetching and patching
# it, so that `make dist` on its own is the whole build -- which is what the
# library's toolchain runs, and it never runs this script.
fetch_bsnes() {
    need git
    [ -f "$BSNES_DIR/bsnes/sfc/sfc.hpp" ] && return 0
    echo "-- Fetching bsnes..."
    git submodule update --init --recursive "$BSNES_DIR"
}

prepare() {
    fetch_sdk
    fetch_bsnes
}

# One library per target, because this cross-compiles.
CORE="build/core-$SDK_OS/libsnescore.a"

core() {
    need g++
    echo "-- Building the console..."
    make -f Makefile.core -j"$JOBS" RACK_DIR="$SDK_DIR" "${CROSS[@]}"
}

# One test program, built the same way each time.
build_test() {
    mkdir -p build/test
    g++ -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter \
        -Wno-missing-field-initializers -Ithird_party -Isrc \
        -o "build/test/$1" "tests/$1.cpp" "$CORE" -lm
}

# ---------------------------------------------------------------------------

case "${1:-all}" in

test)
    prepare; core; fetch_stb
    echo "-- Building the console's tests..."
    build_test test_console
    exec ./build/test/test_console "${2:-$ROM_DEFAULT}" "${3:-20}"
    ;;

sampler)
    prepare; core
    echo "-- Building the sampler's tests..."
    build_test test_sampler
    exec ./build/test/test_sampler "${2:-$ROM_DEFAULT}" "${3:-20}"
    ;;

apu)
    prepare; core
    echo "-- Building the chip's tests..."
    build_test test_apu
    exec ./build/test/test_apu "${2:-$ROM_DEFAULT}" "${3:-20}"
    ;;

bench)
    prepare; core
    echo "-- Building the benchmark..."
    build_test bench
    # Pinned to one core and measured in processor time rather than wall
    # clock: a laptop with a browser open will hand a benchmark somebody
    # else's twenty milliseconds and call it ours.
    exec taskset -c 2 ./build/test/bench "${2:-$ROM_DEFAULT}" "${3:-8}" "${4:-0}" "${5:-1}"
    ;;

probe)
    prepare; core
    # Rack refuses a module whose panel is not exactly its grid, and it
    # refuses it by throwing, which ends the program. That check happens when
    # the module is dropped into the rack -- long after it has built, loaded
    # and drawn perfectly in the module browser. This does the same two calls
    # the browser makes, and the same check RackWidget::addModule makes, with
    # nothing else in the way.
    echo "-- Building the probe..."
    mkdir -p build
    g++ -std=c++17 -O0 -g -Wall -Wextra -Wno-unused-parameter \
        -I"$SDK_DIR/include" -I"$SDK_DIR/dep/include" \
        -o build/probe tools/probe/probe.cpp \
        src/plugin.cpp src/modules/*.cpp \
        "$CORE" \
        -L"$SDK_DIR" -lRack -Wl,-rpath,"$(cd "$SDK_DIR" && pwd)"
    exec ./build/probe "${RACK_SYSTEM_DIR:-/home/vironlap/Documents/Software/Rack2Free}" \
                       "${2:-$ROM_DEFAULT}"
    ;;

mockup)
    prepare; fetch_stb
    need g++
    echo "-- Drawing every panel..."
    mkdir -p build/mockup
    g++ -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
        -Ithird_party -I"$SDK_DIR/dep/include" -I"$SDK_DIR/include" -Isrc \
        -o build/mockup/mockup tools/mockup/mockup.cpp \
        -L"$SDK_DIR" -lRack -lEGL -lGL -Wl,-rpath,"$(cd "$SDK_DIR" && pwd)"
    exec ./build/mockup/mockup
    ;;

clean)
    make "${MAKEVARS[@]}" clean || true
    rm -rf build plugin.so dist
    echo "-- Cleaned. third_party/ is left alone; ./build.sh clean-all takes that too."
    exit 0
    ;;

clean-all)
    make "${MAKEVARS[@]}" clean || true
    rm -rf build plugin.so dist third_party
    exit 0
    ;;

install)
    prepare
    make "${MAKEVARS[@]}" -j"$JOBS"

    # Rack loads an unpacked plugin folder just as happily as a .vcvplugin,
    # and this way the install needs neither zstd nor jq on the machine doing
    # it.
    DEST="${RACK_USER_DIR:-$HOME/.local/share/Rack2}/plugins-$SDK_OS/$SLUG"
    case "$SDK_OS" in
        mac-*) DEST="${RACK_USER_DIR:-$HOME/Library/Application Support/Rack2}/plugins-$SDK_OS/$SLUG" ;;
    esac

    mkdir -p "$DEST"
    cp plugin.so plugin.json "$DEST"/
    [ -f LICENSE.txt ] && cp LICENSE.txt "$DEST"/

    echo ""
    echo "-- Installed into $DEST"
    echo "   Restart Rack and look for RaSNESk under Viron Labs."
    exit 0
    ;;

win)
    # The Windows plugin, from here. Needs mingw-w64; everything else is
    # fetched. The two builds share build/, and the SDK compiles into it
    # without an architecture in the path, so this cleans first.
    need x86_64-w64-mingw32-g++
    make clean >/dev/null 2>&1 || true
    SDK_OS=win-x64
    SDK_DIR="third_party/Rack-SDK-win-x64"
    CORE="build/core-$SDK_OS/libsnescore.a"
    CROSS_PREFIX=x86_64-w64-mingw32-
    CROSS=(CC="${CROSS_PREFIX}gcc" CXX="${CROSS_PREFIX}g++" AR="${CROSS_PREFIX}ar"
           STRIP="${CROSS_PREFIX}strip" OBJCOPY="${CROSS_PREFIX}objcopy")
    MAKEVARS=(RACK_DIR="$SDK_DIR" SLUG="$SLUG" VERSION="$VERSION")
    prepare
    need zstd
    make "${MAKEVARS[@]}" "${CROSS[@]}" -j"$JOBS" dist
    ls -la dist/*.vcvplugin
    echo ""
    echo "-- Built on Linux, for Windows. Now run ./build.sh clean before"
    echo "   building for this machine again: the SDK shares build/ between"
    echo "   architectures."
    exit 0
    ;;

dist)
    prepare
    need zstd
    make "${MAKEVARS[@]}" "${CROSS[@]}" -j"$JOBS" dist
    ls -la dist/*.vcvplugin
    exit 0
    ;;

all|"")
    prepare
    make "${MAKEVARS[@]}" "${CROSS[@]}" -j"$JOBS"
    echo ""
    echo "-- Built. ./build.sh install puts it in your Rack."
    exit 0
    ;;

*)
    echo "usage: ./build.sh [all|install|win|test|sampler|apu|bench|probe|mockup|dist|clean|clean-all] [rom]"
    exit 1
    ;;
esac
