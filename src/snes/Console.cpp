// ===========================================================================
// The Super Nintendo, wired to a rack, on a thread of its own.
//
// This is the only file in the project that sees bsnes. Everything above it
// talks to Console.hpp, which mentions neither bsnes nor nall nor Rack.
//
// Three things here are worth knowing before reading:
//
// * bsnes is a set of global singletons. There is one `cpu`, one `dsp`, one
//   `cartridge`. So there is one console, and Console::claim() is how a
//   module finds out whether it is the one that has it.
//
// * bsnes schedules its chips as cooperative threads (libco), and a thread is
//   resumed by switching to a stack. Everything that touches the machine must
//   therefore happen on one thread, and here that is `worker` -- not the
//   audio thread, not the drawing thread. Everything else in this file is a
//   message to it.
//
// * The machine runs ahead and fills a ring; run() only drains it. An
//   emulator on the audio thread has to finish inside every single block and
//   a whole Super Nintendo cannot promise that. On its own thread it only has
//   to keep up on average.
// ===========================================================================
#include "Console.hpp"
#include "../Path.hpp"

#include <emulator/emulator.hpp>
#include <sfc/sfc.hpp>
#include <nall/directory.hpp>
#include <nall/decode/zip.hpp>
#include <nall/hash/sha256.hpp>
using namespace nall;

#include <heuristics/heuristics.hpp>
#include <heuristics/heuristics.cpp>
#include <heuristics/super-famicom.cpp>
#include <heuristics/game-boy.cpp>
#include <heuristics/bs-memory.cpp>

// The boot ROM of the sound chip and bsnes's board database, as bytes. bsnes
// ships them for exactly this reason: a host with no files on disk.
#include <target-libretro/resources.hpp>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace racksnes {

namespace {

// Only one console, because bsnes has only one of everything.
std::atomic<bool> consoleTaken{false};

// A file that is a buffer we own, so that a cartridge's save RAM is written
// where we can find it again. nall's own memory file takes a copy, which is
// right for a ROM and wrong for a battery.
struct BufferFile : nall::vfs::file {
    uint8_t*  data = nullptr;
    uintmax   bytes = 0;
    uintmax   pos = 0;

    auto size()   const -> uintmax override { return bytes; }
    auto offset() const -> uintmax override { return pos; }

    auto seek(intmax offset, index mode) -> void override
    {
        if(mode == index::absolute) pos = (uintmax)offset;
        if(mode == index::relative) pos += (uintmax)offset;
    }

    auto read() -> uint8_t override
    {
        if(pos >= bytes) return 0x00;
        return data[pos++];
    }

    auto write(uint8_t value) -> void override
    {
        if(pos >= bytes) return;
        data[pos++] = value;
    }
};

} // namespace

// ===========================================================================

struct Console::Impl : Emulator::Platform {

    Emulator::Interface* emulator = nullptr;

    // --- the cartridge, as bsnes wants to be handed it --------------------
    std::vector<uint8_t> image;        // exactly what was on disk, header removed
    std::string          name;         // the file's own name, for the panel
    std::string          cartTitle;
    std::string          cartRegion;

    nall::string         manifest;
    Markup::Node         document;
    vector<uint8_t>      program, data, expansion, firmware;
    std::vector<uint8_t> battery;      // save RAM, whatever size the board has

    std::atomic<bool>    isLoaded{false};
    std::atomic<bool>    running{true};

    // --- the picture ------------------------------------------------------
    // Eight buffers, each stamped with how much sound had been made when it
    // was drawn. The machine fills them ahead of time; the consumer publishes
    // one once it has played that far, so a frame is never seen before the
    // music that goes with it.
    static constexpr int VideoSlots = 8;

    uint8_t               video[VideoSlots][ScreenW * ScreenH * 4] = {};
    uint64_t              videoAt[VideoSlots] = {};
    int                   drawing = 0;
    std::atomic<int>      videoReady{-1};      // newest the machine has drawn
    std::atomic<int>      shown{-1};           // what the panel may read
    int                   showing = -1;
    std::atomic<uint32_t> serial{0};
    uint32_t              palette[32768] = {};

    bool                  strobe = false;
    int                   strobeWanted = 0;
    int                   frameSkip = 0;
    bool                  fastDsp = true;
    bool                  needsExactDsp = false;
    bool                  fastPpu = true;
    bool                  needsExactPpu = false;

    // --- the ring ---------------------------------------------------------
    // One writer, one reader, no lock. The machine pushes at `head` and the
    // audio thread takes at `tail`; neither ever waits for the other, which
    // is the point of the whole arrangement.
    static constexpr int RingSize = 32768;     // a second of the chip's own time

    ChipFrame             ring[RingSize];
    std::atomic<int>      head{0};
    std::atomic<int>      tail{0};
    std::atomic<int>      wanted{2048};        // how full to keep it
    std::atomic<bool>     mayWait{false};      // may run() wait for the machine
    std::atomic<uint32_t> missed{0};           // samples the machine did not have

    uint64_t              produced = 0;        // total ever pushed
    std::atomic<uint64_t> consumed{0};         // total ever taken

    int                   chunk = 16;
    int                   ringWant = 0;

    Resampler<ChipFloats> resample;
    ChipFrame             held;                // the last frame, for an underrun

    // --- the thread that owns the machine ---------------------------------
    std::thread             worker;
    std::atomic<bool>       quit{false};
    std::mutex              wake;
    std::condition_variable wakeUp;

    // --- what the audio and drawing threads ask of it ---------------------
    // Rare things -- loading, resetting, taking a copy of the sound chip --
    // go through a lock and wait. Frequent ones -- a register poke, the
    // bending -- do not, because one of the threads that sends them must
    // never wait for anything.
    enum class Ask { None, Load, Unload, Power, Reset, Snapshot, Spc };

    std::mutex              askLock;
    std::condition_variable askDone;
    std::atomic<bool>       asking{false};     // so the loop need not take the lock
    Ask                     ask = Ask::None;
    std::string             askPath;
    std::vector<uint8_t>    askRom;
    std::string             askName;
    bool                    askOk = false;
    std::string             askError;
    uint8_t                 askAram[AramSize] = {};
    uint8_t                 askRegs[DspRegCount] = {};
    std::vector<uint8_t>    askSpc;

    // A poke is two bytes and a kind; a ring of them is all the audio thread
    // ever needs to reach the machine with.
    struct Poke { uint8_t kind, a, b; };       // 0 dsp, 1 port
    static constexpr int PokeSize = 256;

    Poke                  pokes[PokeSize];
    std::atomic<int>      pokeHead{0};
    std::atomic<int>      pokeTail{0};

    // The bending, as a sequence lock: the writer bumps a counter either side
    // of the copy, and a reader that sees the same even number twice knows it
    // read a whole one.
    std::atomic<uint32_t> bendSeq{0};
    BendMessage           bendSlot;
    BendMessage           bending;

    // --- the gamepad ------------------------------------------------------
    std::atomic<bool>     held12[2][ButtonCount] = {};

    // --- the bending's own state, on the machine's side -------------------
    float                 glitchPhase = 0.f;
    bool                  frozen = false;
    std::vector<uint8_t>  freezeRam;

    VoiceState            voiceState[VoiceCount] = {};

    Impl()
    {
        buildPalette();
        worker = std::thread(&Impl::serve, this);
    }

    ~Impl()
    {
        quit.store(true);
        wakeUp.notify_all();
        if(worker.joinable()) worker.join();
    }

    // -----------------------------------------------------------------------
    // Emulator::Platform
    // -----------------------------------------------------------------------
    auto path(uint id) -> nall::string override { return ""; }

    auto open(uint id, nall::string filename, vfs::file::mode mode, bool required)
        -> shared_pointer<vfs::file> override
    {
        shared_pointer<vfs::file> result;

        if(filename == "ipl.rom" && mode == vfs::file::mode::read)
            return vfs::memory::file::open(iplrom, sizeof iplrom);

        if(filename == "boards.bml" && mode == vfs::file::mode::read)
            return vfs::memory::file::open(Boards, sizeof Boards);

        if(id != 1) return result;   // Super Famicom is path id 1

        if(filename == "manifest.bml" && mode == vfs::file::mode::read)
            return vfs::memory::file::open(manifest.data<uint8_t>(), manifest.size());

        if(filename == "program.rom" && mode == vfs::file::mode::read)
            return vfs::memory::file::open(program.data(), program.size());

        if(filename == "data.rom" && mode == vfs::file::mode::read)
            return vfs::memory::file::open(data.data(), data.size());

        if(filename == "expansion.rom" && mode == vfs::file::mode::read)
            return vfs::memory::file::open(expansion.data(), expansion.size());

        // Everything else a board asks for by name is a coprocessor's
        // firmware, and those travel appended to the ROM.
        if(filename.endsWith(".rom") && mode == vfs::file::mode::read)
            return vfs::memory::file::open(firmware.data(), firmware.size());

        // The battery. Handed over as itself rather than as a copy, so that a
        // game which saves has somewhere to save to.
        if(filename.endsWith(".ram"))
        {
            // bsnes asks for the file before it says how big the board's RAM
            // is, so give it the whole of what the heuristics allowed for.
            if(battery.empty()) battery.resize(512 * 1024, 0xff);

            auto file = shared_pointer<BufferFile>{new BufferFile};
            file->data  = battery.data();
            file->bytes = battery.size();
            return file;
        }

        return result;
    }

    auto load(uint id, nall::string name, nall::string type, vector<nall::string> options)
        -> Emulator::Platform::Load override
    {
        if(id == 1) return {id, cartRegion.c_str()};
        return {id, options(0)};
    }

    auto videoFrame(const uint16* source, uint pitch, uint width, uint height, uint scale)
        -> void override
    {
        if(strobe && strobeWanted <= 0) return;
        if(strobeWanted > 0) --strobeWanted;

        pitch >>= 1;   // bsnes counts it in bytes, and these are 16-bit

        // Hires is 512 across and interlace is 480 down; both get taken back
        // to the 256x240 the panel is built for, by dropping every other one.
        // Nothing here pretends to deinterlace: a game in hires shows the odd
        // columns, which is what a television at this size would show anyway.
        const uint xstep = width  >= 512 ? 2 : 1;
        const uint ystep = height >= 480 ? 2 : 1;

        uint8_t* dest = video[drawing];

        for(int y = 0; y < ScreenH; ++y)
        {
            uint sy = (uint)y * ystep;
            if(sy >= height) { std::memset(dest + y * ScreenW * 4, 0, ScreenW * 4); continue; }

            const uint16* line = source + sy * pitch;
            uint32_t* out = (uint32_t*)(dest + y * ScreenW * 4);

            for(int x = 0; x < ScreenW; ++x)
            {
                uint sx = (uint)x * xstep;
                out[x] = sx < width ? palette[line[sx] & 0x7fff] : palette[0];
            }
        }

        // Stamped with how much sound had been made when it was drawn. The
        // consumer shows it when it has played that much, so the picture and
        // the music stay together however far ahead the machine is running.
        videoAt[drawing] = produced;
        videoReady.store(drawing, std::memory_order_release);
        drawing = (drawing + 1) % VideoSlots;
    }

    auto audioFrame(const double*, uint) -> void override
    {
        // Deliberately empty. The sound is taken from the chip itself, one
        // voice at a time, in dspSample() below: by the time it reaches here
        // it is a sum, and a sum is the one thing this module must not be
        // given. (The cost is that a coprocessor with its own audio -- MSU-1,
        // a Super Game Boy -- is not in the mix.)
    }

    auto inputPoll(uint port, uint device, uint id) -> int16 override
    {
        if(port > 1 || id >= (uint)ButtonCount) return 0;
        return held12[port][id].load(std::memory_order_relaxed) ? 1 : 0;
    }

    // -----------------------------------------------------------------------
    // One sample of the chip, from our patch to SPC_DSP
    // -----------------------------------------------------------------------
    static void dspSink(void* context, const SuperFamicom::SPC_DSP::rack_taps_t& t)
    {
        ((Impl*)context)->dspSample(t);
    }

    void dspSample(const SuperFamicom::SPC_DSP::rack_taps_t& t)
    {
        constexpr float Scale = 1.f / 32768.f;

        const int h = head.load(std::memory_order_relaxed);
        ChipFrame& f = ring[h];

        f.main[0] = t.main[0] * Scale;
        f.main[1] = t.main[1] * Scale;
        f.echo[0] = t.echo[0] * Scale;
        f.echo[1] = t.echo[1] * Scale;

        for(int v = 0; v < VoiceCount; ++v)
        {
            f.voice[v][0] = t.voice[v][0] * Scale;
            f.voice[v][1] = t.voice[v][1] * Scale;
            f.dry[v]      = t.dry[v]      * Scale;

            f.env[v]   = t.env[v];
            f.srcn[v]  = t.srcn[v];
            f.pitch[v] = (uint16_t)t.pitch[v];
        }

        f.keyed = t.keyed;

        head.store((h + 1) % RingSize, std::memory_order_release);
        ++produced;

        // Enough for now, or nowhere left to put it: either way, stop here
        // and let the machine be resumed when there is room.
        if(fill() >= ringWant || fill() >= RingSize - 64)
            SuperFamicom::scheduler.leave(SuperFamicom::Scheduler::Event::Synchronized);
    }

    int fill() const
    {
        const int n = head.load(std::memory_order_relaxed)
                    - tail.load(std::memory_order_acquire);
        return n < 0 ? n + RingSize : n;
    }

    void buildPalette()
    {
        // bsnes hands over fifteen-bit colour as an index. This is the same
        // curve its own targets use: a gamma of 1.5, nothing else.
        for(int color = 0; color < 32768; ++color)
        {
            int r = (color >> 10) & 31;
            int g = (color >>  5) & 31;
            int b = (color >>  0) & 31;

            r = r << 3 | r >> 2;
            g = g << 3 | g >> 2;
            b = b << 3 | b >> 2;

            auto gamma = [](int v) -> int {
                double x = v / 255.0;
                double y = std::pow(x, 1.0 / 1.5);
                int    o = (int)(y * 255.0 + 0.5);
                return o < 0 ? 0 : o > 255 ? 255 : o;
            };

            r = gamma(r); g = gamma(g); b = gamma(b);

            // nanovg wants R,G,B,A in memory order.
            palette[color] = (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | 0xff000000u;
        }
    }

    bool startEmulator()
    {
        if(!emulator) emulator = new SuperFamicom::Interface;

        // The fast sound chip: it runs thirty-two clocks in one go instead of
        // one at a time, and finishes the sample in the same place, so the
        // eight voices come out of it unchanged. Two cartridges below ask for
        // the accurate one by name.
        emulator->configure("Hacks/DSP/Fast", fastDsp);
        emulator->configure("Hacks/DSP/Cubic", false);
        emulator->configure("Hacks/PPU/Fast", fastPpu);
        emulator->configure("Hacks/PPU/NoSpriteLimit", false);
        emulator->configure("Hacks/Hotfixes", true);
        emulator->configure("Audio/Frequency", ApuRate);

        return true;
    }

    bool loadImage(std::string& error)
    {
        if(image.size() < 0x8000) { error = "that file is too small to be a cartridge"; return false; }

        // A copier header is 512 bytes on the front of an otherwise whole ROM.
        if((image.size() & 0x7fff) == 512)
            image.erase(image.begin(), image.begin() + 512);

        vector<uint8_t> rom;
        rom.resize(image.size());
        std::memcpy(rom.data(), image.data(), image.size());

        auto heuristics = Heuristics::SuperFamicom(rom, name.c_str());

        cartTitle  = (const char*)heuristics.title();
        cartRegion = (const char*)heuristics.videoRegion();
        manifest   = heuristics.manifest();

        if(!manifest) { error = "no board in bsnes's database matches this cartridge"; return false; }

        document = BML::unserialize(manifest);

        uint offset = 0;
        auto slice = [&](vector<uint8_t>& into, uint size) {
            into.reset();
            if(!size) return;
            into.resize(size);
            for(uint i = 0; i < size && offset + i < rom.size(); ++i) into[i] = rom[offset + i];
            offset += size;
        };

        slice(program,   heuristics.programRomSize());
        slice(data,      heuristics.dataRomSize());
        slice(expansion, heuristics.expansionRomSize());
        slice(firmware,  heuristics.firmwareRomSize());

        startEmulator();

        emulator->unload();
        if(!emulator->load()) { error = "bsnes would not load it"; return false; }

        // Per-game overrides, the handful that matter to the sound or to a
        // picture that would otherwise be wrong. bsnes's own target carries a
        // longer list; these are the ones about this module's subject.
        needsExactDsp = cartTitle == "KOUSHIEN_2" || cartTitle == "RENDERING RANGER R2";
        if(needsExactDsp) emulator->configure("Hacks/DSP/Fast", false);
        needsExactPpu = cartTitle == "AIR STRIKE PATROL" || cartTitle == "DESERT FIGHTER";
        if(needsExactPpu) emulator->configure("Hacks/PPU/Fast", false);

        emulator->connect(SuperFamicom::ID::Port::Controller1, SuperFamicom::ID::Device::Gamepad);
        emulator->connect(SuperFamicom::ID::Port::Controller2, SuperFamicom::ID::Device::Gamepad);

        emulator->power();

        // Our patch's delivery address, set after power() because power()
        // rebuilds the DSP.
        SuperFamicom::dsp.rackSink        = &Impl::dspSink;
        SuperFamicom::dsp.rackSinkContext = this;

        SuperFamicom::system.frameSkip = (uint)frameSkip;

        isLoaded.store(true);
        emptyRing();
        return true;
    }
    // -----------------------------------------------------------------------
    // The thread that owns the machine
    // -----------------------------------------------------------------------
    void serve()
    {
        // Every one of these has to happen here, on this thread, and never
        // again anywhere else.
        platform = this;
        startEmulator();

        while(!quit.load(std::memory_order_relaxed))
        {
            answer();
            applyPokes();
            applyBend();

            if(isLoaded.load(std::memory_order_relaxed)
            && running.load(std::memory_order_relaxed)
            && fill() < wanted.load(std::memory_order_relaxed))
            {
                // Fill the whole gap in one go. Off the audio thread there is
                // nothing to be gained by taking it in small bites, and the
                // machine is cheaper the less often it is stopped and started.
                ringWant = wanted.load(std::memory_order_relaxed);
                SuperFamicom::system.run();
                continue;
            }

            // Full, stopped, or empty-handed: sleep until somebody drains the
            // ring or asks for something. The timeout is short because the
            // wake-up is a courtesy, not a contract.
            std::unique_lock<std::mutex> lock(wake);
            wakeUp.wait_for(lock, std::chrono::milliseconds(2));
        }

        if(emulator)
        {
            emulator->unload();
            delete emulator;
            emulator = nullptr;
        }

        platform = nullptr;
    }

    // The things somebody waits for. The flag is checked first so that the
    // common case -- nobody asking -- costs an atomic load rather than a
    // mutex, a couple of thousand times a second.
    void answer()
    {
        if(!asking.load(std::memory_order_acquire)) return;

        std::unique_lock<std::mutex> lock(askLock);
        if(ask == Ask::None) { asking.store(false); return; }

        switch(ask)
        {
        case Ask::Load:
            image = askRom;
            name  = askName;
            battery.clear();
            askOk = loadImage(askError);
            if(!askOk) isLoaded.store(false);
            break;

        case Ask::Unload:
            if(isLoaded.load()) { emulator->unload(); isLoaded.store(false); }
            shown.store(-1);
            videoReady.store(-1);
            askOk = true;
            break;

        case Ask::Power:
            if(isLoaded.load())
            {
                emulator->power();
                SuperFamicom::dsp.rackSink        = &Impl::dspSink;
                SuperFamicom::dsp.rackSinkContext = this;
                emptyRing();
            }
            askOk = true;
            break;

        case Ask::Reset:
            if(isLoaded.load()) emulator->reset();
            askOk = true;
            break;

        case Ask::Snapshot:
            if(isLoaded.load())
            {
                std::memcpy(askAram, SuperFamicom::dsp.rackApuRam(), AramSize);
                for(int i = 0; i < DspRegCount; ++i)
                    askRegs[i] = SuperFamicom::dsp.read((uint8_t)i);
                askOk = true;
            }
            else askOk = false;
            break;

        case Ask::Spc:
            askOk = writeSpc(askSpc);
            break;

        default: break;
        }

        ask = Ask::None;
        asking.store(false, std::memory_order_release);
        lock.unlock();
        askDone.notify_all();
    }

    void emptyRing()
    {
        head.store(0); tail.store(0);
        produced = 0; consumed.store(0);
        resample.reset();
        held = ChipFrame();
    }

    // The pokes: two bytes each, and the audio thread never waits to send one.
    void applyPokes()
    {
        if(!isLoaded.load(std::memory_order_relaxed)) { pokeTail.store(pokeHead.load()); return; }

        int t = pokeTail.load(std::memory_order_relaxed);
        const int h = pokeHead.load(std::memory_order_acquire);

        while(t != h)
        {
            const Poke& p = pokes[t];

            if(p.kind == 0) SuperFamicom::dsp.write(p.a, p.b);
            else            SuperFamicom::smp.portWrite((uint)(p.a & 3), p.b);

            t = (t + 1) % PokeSize;
        }

        pokeTail.store(t, std::memory_order_release);
    }

    // -----------------------------------------------------------------------
    // The bending, applied where the cartridge is running
    // -----------------------------------------------------------------------
    void applyBend()
    {
        // A sequence lock: an odd count means the writer is mid-copy, and the
        // same even count either side means what was read was whole.
        const uint32_t a = bendSeq.load(std::memory_order_acquire);
        if(!(a & 1))
        {
            BendMessage fresh = bendSlot;
            if(bendSeq.load(std::memory_order_acquire) == a) bending = fresh;
        }

        if(!isLoaded.load(std::memory_order_relaxed)) return;

        if(!bending.present) { frozen = false; return; }

        bendPalette();
        bendVoices();
        bendGlitch();
        bendFreeze();
    }

    void bendPalette()
    {
        if(!SuperFamicom::system.fastPPU()) return;
        if(bending.paletteAmount <= 0.001f) return;

        uint16* cgram = SuperFamicom::ppufast.cgram;
        const int amount = (int)(bending.paletteAmount * 31.f);

        // The 256 colours on their way to the screen. Written back into the
        // PPU's own table, so what changes is what the cartridge drew with,
        // not a filter over the top of it.
        for(int i = 1; i < 256; ++i)
        {
            int r = cgram[i]        & 31;
            int g = (cgram[i] >> 5) & 31;
            int b = (cgram[i] >>10) & 31;

            switch(bending.paletteMode)
            {
            case 0: { int t = r; r = g; g = b; b = t; } break;          // rotate
            case 1: r = (r + amount) & 31; g = (g + amount / 2) & 31; break;
            case 2: r = 31 - r; g = 31 - g; b = 31 - b; break;          // invert
            default: {                                                  // drain
                int grey = (r + g + b) / 3;
                r += (grey - r) * amount / 31;
                g += (grey - g) * amount / 31;
                b += (grey - b) * amount / 31;
            } break;
            }

            cgram[i] = (uint16)(r | g << 5 | b << 10);
        }
    }

    void bendVoices()
    {
        // Transposing means multiplying every voice's pitch register, which
        // is the same arithmetic the chip does to play a sample at a note.
        if(std::fabs(bending.transpose) > 0.001f)
        {
            const double ratio = std::pow(2.0, (double)bending.transpose);

            for(int v = 0; v < VoiceCount; ++v)
            {
                const uint8_t lo = SuperFamicom::dsp.read((uint8_t)(v * 0x10 + 0x02));
                const uint8_t hi = SuperFamicom::dsp.read((uint8_t)(v * 0x10 + 0x03));

                int pitch = (lo | hi << 8) & 0x3fff;
                pitch = (int)(pitch * ratio);
                if(pitch > 0x3fff) pitch = 0x3fff;

                SuperFamicom::dsp.write((uint8_t)(v * 0x10 + 0x02), (uint8_t)(pitch & 0xff));
                SuperFamicom::dsp.write((uint8_t)(v * 0x10 + 0x03), (uint8_t)(pitch >> 8));
            }
        }

        // Pitch modulation makes each voice's pitch follow the one below it,
        // and noise replaces a voice's sample with the chip's noise source.
        // Both are one bit a voice, and no game turns on more than a couple.
        const int bits = (int)(bending.warpAmount * 8.f);
        uint8_t mask = 0;
        for(int i = 0; i < bits && i < 8; ++i) mask |= (uint8_t)(1 << (7 - i));

        if(bending.warpMode == 0 || bending.warpMode == 2)
            SuperFamicom::dsp.write(0x2d, mask & 0xfe);     // voice 0 cannot
        if(bending.warpMode == 1 || bending.warpMode == 2)
            SuperFamicom::dsp.write(0x3d, mask);

        if(bending.echoFeedback > 0.001f)
        {
            SuperFamicom::dsp.write(0x0d, (uint8_t)(int)(bending.echoFeedback * 127.f));
            SuperFamicom::dsp.write(0x4d, 0xff);            // every voice to the echo
            const uint8_t flg = SuperFamicom::dsp.read(0x6c);
            SuperFamicom::dsp.write(0x6c, (uint8_t)(flg & ~0x20));
        }
    }

    void bendGlitch()
    {
        if(bending.glitchRate <= 0.001f) return;

        // A chunk of the chip's own time has gone by since the last call.
        glitchPhase += (float)chunk / (float)ApuRate * bending.glitchRate * 4000.f;

        int writes = (int)glitchPhase;
        if(writes <= 0) return;
        glitchPhase -= writes;
        if(writes > 64) writes = 64;

        for(int i = 0; i < writes; ++i)
        {
            const uint8_t value = (uint8_t)(rand() & 0xff);

            switch(bending.glitchRegion)
            {
            case 0:                                          // the tiles
                if(SuperFamicom::system.fastPPU())
                    SuperFamicom::ppufast.vram[rand() % 32768] ^= (uint16)(value | value << 8);
                break;

            case 1:                                          // the palette
                if(SuperFamicom::system.fastPPU())
                    SuperFamicom::ppufast.cgram[rand() % 256] ^= (uint16)(value << 3);
                break;

            case 2:                                          // the instruments
                SuperFamicom::dsp.rackApuRam()[rand() % AramSize] ^= value;
                break;

            default:                                         // all the working RAM
                SuperFamicom::cpu.wram[rand() % (128 * 1024)] ^= value;
                break;
            }
        }
    }

    void bendFreeze()
    {
        if(!bending.freezeHeld) { frozen = false; return; }

        if(!frozen)
        {
            frozen = true;
            freezeRam.resize(AramSize);
            std::memcpy(freezeRam.data(), SuperFamicom::dsp.rackApuRam(), AramSize);
            return;
        }

        // Hand the same 64K back: the driver keeps running and keeps finding
        // the world it was in.
        std::memcpy(SuperFamicom::dsp.rackApuRam(), freezeRam.data(), AramSize);
    }

    // -----------------------------------------------------------------------

    bool writeSpc(std::vector<uint8_t>& out)
    {
        if(!isLoaded.load()) return false;

        // An .spc says where the SPC-700 was, and an address is only a place
        // to resume from if it is between two instructions. bsnes parks its
        // chips wherever the one that yielded happened to be, which for the
        // processor is usually halfway through an opcode -- the rest of that
        // opcode lives on its thread's stack, and no .spc has ever had room
        // for a stack. runToSave() walks every thread to a clean point.
        //
        // It runs the console to get there, and our own sink would stop it as
        // soon as the ring filled, so for the length of this the ring is told
        // it can never have enough.
        const int was = ringWant;
        ringWant = 0x7fffffff;
        SuperFamicom::system.runToSave();
        ringWant = was;

        out.assign(0x10200, 0);

        std::memcpy(out.data(), "SNES-SPC700 Sound File Data v0.30", 33);
        out[0x21] = 0x1a; out[0x22] = 0x1a; out[0x23] = 0x1a;
        out[0x24] = 30;

        const auto& r = SuperFamicom::smp.r;
        out[0x25] = r.pc.w & 0xff;
        out[0x26] = r.pc.w >> 8;
        out[0x27] = r.ya.byte.l;    // A
        out[0x28] = r.x;
        out[0x29] = r.ya.byte.h;    // Y
        out[0x2a] = r.p;
        out[0x2b] = r.s;

        std::string what = cartTitle;
        what.resize(32, ' ');
        std::memcpy(out.data() + 0x2e, what.data(), 32);
        std::memcpy(out.data() + 0x4e, what.data(), 32);

        std::memcpy(out.data() + 0x100, SuperFamicom::dsp.rackApuRam(), AramSize);
        for(int i = 0; i < DspRegCount; ++i)
            out[0x10100 + i] = SuperFamicom::dsp.read((uint8_t)i);

        return true;
    }

    // Ask the machine for something and wait for it. Never the audio thread.
    bool request(Ask what, std::string* error = nullptr)
    {
        std::unique_lock<std::mutex> lock(askLock);
        ask = what;
        askOk = false;
        askError.clear();
        asking.store(true, std::memory_order_release);

        wakeUp.notify_all();
        askDone.wait(lock, [&] { return ask == Ask::None; });

        if(error) *error = askError;
        return askOk;
    }

    void poke(uint8_t kind, uint8_t a, uint8_t b)
    {
        const int h = pokeHead.load(std::memory_order_relaxed);
        const int next = (h + 1) % PokeSize;

        // A full queue means the machine is a long way behind; dropping a
        // register poke is better than making the audio thread wait for it.
        if(next == pokeTail.load(std::memory_order_acquire)) return;

        pokes[h] = { kind, a, b };
        pokeHead.store(next, std::memory_order_release);
    }
};

// ===========================================================================

Console::Console() : impl(nullptr) {}

Console::~Console() { release(); }

bool Console::claim()
{
    if(owner) return true;

    bool expected = false;
    if(!consoleTaken.compare_exchange_strong(expected, true)) return false;

    owner = true;
    impl  = new Impl;
    return true;
}

void Console::release()
{
    if(!owner) return;

    delete impl;
    impl  = nullptr;
    owner = false;
    consoleTaken.store(false);
}

bool Console::load(const std::string& path, std::string& error)
{
    if(!impl) { error = "this module is not the one holding the console"; return false; }

    auto blob = nall::file::read(path.c_str());
    if(!blob) { error = "could not read that file"; return false; }

    std::vector<uint8_t> bytes(blob.data(), blob.data() + blob.size());

    const std::string leaf = leafOf(path);

    return loadMemory(bytes, leaf, error);
}

bool Console::loadMemory(const std::vector<uint8_t>& rom, const std::string& leaf,
                         std::string& error)
{
    if(!impl) { error = "this module is not the one holding the console"; return false; }

    {
        std::lock_guard<std::mutex> guard(impl->askLock);
        impl->askRom  = rom;
        impl->askName = leaf;
    }

    return impl->request(Impl::Ask::Load, &error);
}

void Console::unload() { if(impl) impl->request(Impl::Ask::Unload); }
void Console::power()  { if(impl) impl->request(Impl::Ask::Power); }
void Console::reset()  { if(impl) impl->request(Impl::Ask::Reset); }

bool        Console::loaded() const { return impl && impl->isLoaded.load(); }
std::string Console::title()  const { return impl ? impl->cartTitle  : std::string(); }
std::string Console::region() const { return impl ? impl->cartRegion : std::string(); }

void Console::setRunning(bool on)     { if(impl) impl->running.store(on); }
void Console::setStrobe(bool on)      { if(impl) impl->strobe = on; }
void Console::strobeAdvance()         { if(impl) ++impl->strobeWanted; }
void Console::setChunk(int samples)   { if(impl) impl->chunk = samples < 4 ? 4 : samples; }

void Console::setBufferDepth(int samples)
{
    if(!impl) return;
    const int n = samples < 256 ? 256 : (samples > Impl::RingSize / 2 ? Impl::RingSize / 2 : samples);
    impl->wanted.store(n);
}

float Console::buffer() const
{
    if(!impl) return 0.f;
    const int want = impl->wanted.load();
    return want > 0 ? (float)impl->fill() / (float)want : 0.f;
}

uint32_t Console::underruns() const { return impl ? impl->missed.load() : 0; }

void Console::setWaitForMachine(bool on) { if(impl) impl->mayWait.store(on); }

void Console::setFrameSkip(int skip)
{
    if(!impl) return;
    impl->frameSkip = skip < 0 ? 0 : skip;
    if(impl->isLoaded.load()) SuperFamicom::system.frameSkip = (uint)impl->frameSkip;
}

void Console::setFastDsp(bool on)
{
    if(!impl) return;
    impl->fastDsp = on;
    if(impl->emulator) impl->emulator->configure("Hacks/DSP/Fast", on && !impl->needsExactDsp);
}

void Console::setFastPpu(bool on)
{
    if(!impl) return;
    impl->fastPpu = on;
    if(impl->emulator) impl->emulator->configure("Hacks/PPU/Fast", on && !impl->needsExactPpu);
}

// ---------------------------------------------------------------------------
// Draining the ring. The audio thread's whole share of the work.
// ---------------------------------------------------------------------------
void Console::run(ChipFrame* out, int count, double rackRate, double speed)
{
    if(!impl || rackRate <= 0.0)
    {
        for(int i = 0; i < count; ++i) out[i] = ChipFrame();
        return;
    }

    Impl& m = *impl;

    // Input frames per output frame. This is the speed control: ask for more
    // of the chip's samples per sample of the rack's and the whole machine
    // runs faster, picture and pitch together, because the picture is shown
    // when the sound that goes with it is played.
    const double step = ApuRate * speed / rackRate;

    float flat[ChipFloats];
    uint64_t taken = 0;

    for(int i = 0; i < count; ++i)
    {
        m.resample.advance(step);

        while(m.resample.hungry())
        {
            const int t = m.tail.load(std::memory_order_relaxed);

            if(t == m.head.load(std::memory_order_acquire))
            {
                // Not an audio thread: wait for it rather than hear a hole.
                if(m.mayWait.load(std::memory_order_relaxed))
                {
                    m.wakeUp.notify_one();

                    for(int spin = 0; spin < 200000; ++spin)
                    {
                        if(t != m.head.load(std::memory_order_acquire)) break;
                        std::this_thread::yield();
                    }

                    if(t != m.head.load(std::memory_order_acquire)) continue;
                }

                // The machine did not have it ready. Hold the last frame
                // rather than click, and count it: an underrun is the only
                // honest measure of whether this is working.
                m.missed.fetch_add(1, std::memory_order_relaxed);
                flatten(m.held, flat);
                m.resample.push(flat);
                continue;
            }

            m.held = m.ring[t];
            m.tail.store((t + 1) % Impl::RingSize, std::memory_order_release);
            ++taken;

            flatten(m.held, flat);
            m.resample.push(flat);
        }

        m.resample.read(flat);
        unflatten(flat, out[i]);

        // The voices come out of the frame that is being heard, not the one
        // the machine has just made: the two are a buffer's depth apart.
        for(int v = 0; v < VoiceCount; ++v)
        {
            m.voiceState[v].level = m.held.env[v] * (1.f / 127.f);
            m.voiceState[v].srcn  = m.held.srcn[v];
            m.voiceState[v].keyed = (m.held.keyed >> v) & 1;

            // A closed gate keeps its pitch, the way a synthesiser does.
            if(m.voiceState[v].keyed)
                m.voiceState[v].volts = pitchToVolts(m.held.pitch[v]);
        }
    }

    if(taken)
    {
        m.consumed.fetch_add(taken, std::memory_order_release);

        // Show the newest frame the machine drew at a point we have now
        // played past.
        const uint64_t at = m.consumed.load(std::memory_order_relaxed);
        const int ready = m.videoReady.load(std::memory_order_acquire);

        if(ready >= 0 && ready != m.showing && m.videoAt[ready] <= at)
        {
            m.showing = ready;
            m.shown.store(ready, std::memory_order_release);
            m.serial.fetch_add(1, std::memory_order_release);
        }

        m.wakeUp.notify_one();
    }
}

const uint8_t* Console::frame() const
{
    if(!impl) return nullptr;
    const int which = impl->shown.load(std::memory_order_acquire);
    return which < 0 ? nullptr : impl->video[which];
}

uint32_t Console::frameSerial() const
{
    return impl ? impl->serial.load(std::memory_order_acquire) : 0;
}

void Console::setButton(int player, int button, bool pressed)
{
    if(!impl || player < 0 || player > 1 || button < 0 || button >= ButtonCount) return;
    impl->held12[player][button].store(pressed, std::memory_order_relaxed);
}

const VoiceState* Console::voices() const
{
    return impl ? impl->voiceState : nullptr;
}

bool Console::snapshotAram(uint8_t out[AramSize], uint8_t regs[DspRegCount])
{
    if(!impl) return false;
    if(!impl->request(Impl::Ask::Snapshot)) return false;

    std::lock_guard<std::mutex> guard(impl->askLock);
    std::memcpy(out, impl->askAram, AramSize);
    std::memcpy(regs, impl->askRegs, DspRegCount);
    return true;
}

bool Console::exportSpc(std::vector<uint8_t>& out)
{
    if(!impl) return false;
    if(!impl->request(Impl::Ask::Spc)) return false;

    std::lock_guard<std::mutex> guard(impl->askLock);
    out = impl->askSpc;
    return true;
}

void Console::writeDspReg(uint8_t address, uint8_t value) { if(impl) impl->poke(0, address, value); }
void Console::writeApuPort(int port, uint8_t value)
{
    if(impl && port >= 0 && port <= 3) impl->poke(1, (uint8_t)port, value);
}

void Console::bend(const BendMessage& message)
{
    if(!impl) return;

    impl->bendSeq.fetch_add(1, std::memory_order_release);   // odd: writing
    impl->bendSlot = message;
    impl->bendSeq.fetch_add(1, std::memory_order_release);   // even: whole
}

const std::vector<uint8_t>& Console::romImage() const
{
    static const std::vector<uint8_t> none;
    return impl ? impl->image : none;
}

const std::string& Console::romName() const
{
    static const std::string none;
    return impl ? impl->name : none;
}

} // namespace racksnes
