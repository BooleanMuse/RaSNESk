#!/usr/bin/env bash
# ============================================================================
# Builds the Super Nintendo for VCV Rack in one go.
#
#   ./build.sh                 -> the plugin for this machine
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
# The Rack SDK and one stb header are fetched on first use; bsnes is already
# in this repository. Nothing else is needed -- not even jq, which the SDK's
# own makefile wants and which this script works around.
#
#   Linux    gcc or clang, and make
#   macOS    the Xcode command line tools, plus `brew install zstd` for `dist`
#   Windows  MSYS2's MINGW64 shell, with
#            `pacman -S make patch zip unzip zstd mingw-w64-x86_64-gcc`
# ============================================================================
set -e
cd "$(dirname "$0")"

# Pinned to the oldest Rack this is meant to run in rather than the newest
# SDK there is: a plugin built against an SDK newer than somebody's Rack can
# reach for a symbol their Rack does not have, and one built against an older
# SDK runs in every 2.x after it.
SDK_VER="${SDK_VER:-2.6.4}"

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
    *)      SDK_OS=win-x64 ;;   # MSYS2 says MINGW64_NT-..., Cygwin says CYGWIN_NT-...
esac

# What the plugin is called once it is built, which is not the same word on
# the three platforms and is what `install` has to copy.
case "$SDK_OS" in
    lin-x64) TARGET=plugin.so ;;
    mac-*)   TARGET=plugin.dylib ;;
    win-x64) TARGET=plugin.dll ;;
esac

need() {
    command -v "$1" >/dev/null || { echo "ERROR: '$1' is missing and the build needs it."; exit 1; }
}

JOBS=$(nproc 2>/dev/null || echo 4)

# plugin.mk reads these out of plugin.json with jq. Passing them on the command
# line overrides that assignment outright, so jq never has to exist -- which
# matters on a fresh MSYS2, where it does not. Three ways of reading two
# strings out of a file, in the order of how much you can trust them.
manifest() {
    if command -v jq >/dev/null; then
        jq -r ".$1" plugin.json
    elif command -v python3 >/dev/null; then
        python3 -c "import json;print(json.load(open('plugin.json'))['$1'])"
    else
        # Last resort: the two fields are one per line and quoted, and this
        # file is written by hand rather than generated, so a line is enough.
        grep -m1 "\"$1\"" plugin.json | cut -d'"' -f4
    fi
}

SLUG=$(manifest slug)
VERSION=$(manifest version)
[ -n "$SLUG" ] && [ -n "$VERSION" ] || { echo "ERROR: could not read slug and version out of plugin.json"; exit 1; }
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

# bsnes is not fetched: the whole of it is in this repository, under
# third_party/bsnes-src, already carrying patches/bsnes-rack.patch. That is
# what lets `make dist` on its own be the entire build, which is what the
# library's toolchain runs -- it never runs this script, and it never has a
# network.
check_bsnes() {
    [ -f "$BSNES_DIR/bsnes/sfc/sfc.hpp" ] \
        && [ -f "$BSNES_DIR/libco/libco.h" ] \
        && [ -d "$BSNES_DIR/nall" ] && return 0

    echo "ERROR: the console's source is not here."
    echo ""
    echo "  $BSNES_DIR should hold bsnes entire -- bsnes/, nall/ and libco/"
    echo "  at least. If it is missing or short, the clone did not finish."
    exit 1
}

prepare() {
    fetch_sdk
    check_bsnes
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
    # taskset is Linux's; macOS and Windows have nothing equivalent to reach
    # for, so there the numbers are noisier and that is all.
    if command -v taskset >/dev/null; then
        exec taskset -c 2 ./build/test/bench "${2:-$ROM_DEFAULT}" "${3:-8}" "${4:-0}" "${5:-1}"
    fi
    exec ./build/test/bench "${2:-$ROM_DEFAULT}" "${3:-8}" "${4:-0}" "${5:-1}"
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
    # Its own directory: the probe writes build/probe/mix.wav, and on
    # Linux a file called build/probe and a directory called build/probe
    # cannot both exist.
    mkdir -p build/probe

    # The same three things the SDK's own compile.mk and plugin.mk do per
    # platform, because this links against libRack without going through
    # them: M_PI is not in <cmath> on Windows without the define, a Windows
    # DLL is found on PATH rather than by rpath, and macOS resolves Rack's
    # symbols at load time.
    PROBE_FLAGS=()
    PROBE_LINK=(-L"$SDK_DIR" -lRack)
    case "$SDK_OS" in
        win-x64)
            PROBE_FLAGS+=(-D_USE_MATH_DEFINES)
            PROBE_LINK+=(-static-libgcc
                         -Wl,-Bstatic,--whole-archive -lwinpthread
                         -Wl,--no-whole-archive -Wl,-Bdynamic)
            ;;
        mac-*)
            PROBE_LINK+=(-Wl,-rpath,"$(cd "$SDK_DIR" && pwd)")
            ;;
        *)
            PROBE_LINK+=(-Wl,-rpath,"$(cd "$SDK_DIR" && pwd)")
            ;;
    esac

    g++ -std=c++17 -O0 -g -Wall -Wextra -Wno-unused-parameter \
        "${PROBE_FLAGS[@]}" \
        -I"$SDK_DIR/include" -I"$SDK_DIR/dep/include" \
        -o build/probe/probe tools/probe/probe.cpp \
        src/plugin.cpp src/modules/*.cpp \
        "$CORE" \
        "${PROBE_LINK[@]}"

    # Where Rack itself is installed, for the fonts and the component SVGs the
    # panels ask for. Override with RACK_SYSTEM_DIR.
    case "$SDK_OS" in
        win-x64) RACK_SYS="${RACK_SYSTEM_DIR:-C:/Program Files/VCV/Rack2Free}"
                 # libRack.dll sits next to Rack.exe and is found on PATH.
                 PATH="$(cygpath -u "$RACK_SYS" 2>/dev/null || echo "$RACK_SYS"):$PATH"
                 export PATH ;;
        mac-*)   RACK_SYS="${RACK_SYSTEM_DIR:-/Applications/Rack2Free.app/Contents/Resources}" ;;
        *)       RACK_SYS="${RACK_SYSTEM_DIR:-/usr/share/Rack2}" ;;
    esac

    exec ./build/probe/probe "$RACK_SYS" "${2:-$ROM_DEFAULT}"
    ;;

mockup)
    # This one really is Linux-only: it opens an EGL context to draw into,
    # and EGL is not what macOS or Windows offer. Nothing else here needs it.
    if [ "$SDK_OS" != "lin-x64" ]; then
        echo "-- ./build.sh mockup draws the panels through EGL, which is Linux's."
        echo "   Everything else here builds on all three; this one does not."
        exit 1
    fi
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
    rm -rf build plugin.so plugin.dll plugin.dylib dist
    echo "-- Cleaned. third_party/ is left alone; ./build.sh clean-all takes the SDK too."
    exit 0
    ;;

clean-all)
    make "${MAKEVARS[@]}" clean || true
    rm -rf build plugin.so plugin.dll plugin.dylib dist
    # Not third_party entire: the console lives there and is ours to keep.
    rm -rf third_party/Rack-SDK third_party/Rack-SDK-win-x64 third_party/stb_image_write.h
    exit 0
    ;;

install)
    prepare
    make "${MAKEVARS[@]}" -j"$JOBS"

    # Rack loads an unpacked plugin folder just as happily as a .vcvplugin,
    # and this way the install needs neither zstd nor jq on the machine doing
    # it.
    # Where Rack keeps its user directory, which is a different place on each
    # of the three. RACK_USER_DIR overrides the lot, and is the answer if any
    # of this guesses wrong.
    if [ -n "${RACK_USER_DIR:-}" ]; then
        USER_DIR="$RACK_USER_DIR"
    else
        case "$SDK_OS" in
        mac-*)
            USER_DIR="$HOME/Library/Application Support/Rack2"
            ;;
        win-x64)
            # %LOCALAPPDATA%, as a path this shell can write to. A MINGW64
            # shell started the ordinary way has it; one started from
            # somewhere that scrubbed the environment does not, and then it
            # is worth asking Windows rather than installing into /Rack2,
            # which is C:\msys64\Rack2 and is nobody's Rack.
            win_local="${LOCALAPPDATA:-}"
            [ -n "$win_local" ] || \
                win_local=$(cmd //c echo %LOCALAPPDATA% 2>/dev/null | tr -d '\r')
            case "$win_local" in ''|'%LOCALAPPDATA%')
                echo "ERROR: cannot find %LOCALAPPDATA%. Set RACK_USER_DIR to Rack's"
                echo "       user folder -- the one with plugins-win-x64 in it."
                exit 1 ;;
            esac
            USER_DIR="$(cygpath -u "$win_local" 2>/dev/null || echo "$win_local")/Rack2"
            ;;
        *)
            USER_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/Rack2"
            ;;
        esac
    fi

    DEST="$USER_DIR/plugins-$SDK_OS/$SLUG"

    mkdir -p "$DEST"
    cp "$TARGET" plugin.json "$DEST"/
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
