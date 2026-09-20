// ===========================================================================
// Reading a cartridge's instruments out of 64K of sound-chip RAM.
// ===========================================================================
#include "Bank.hpp"
#include "../Path.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>

namespace racksnes {

namespace {

// BRR: nine bytes to a block. A header byte -- four bits of shift, two of
// filter, one "loop", one "end" -- and then sixteen four-bit samples.
constexpr int BrrBlock = 9;

// Is this a chain of BRR blocks, or is it whatever else happened to be at
// that address? Walk it until something says it ends, and refuse anything
// that runs off the end of memory or goes on implausibly long. A shift of 13
// or more is undefined on the real chip and effectively never authored.
int chainLength(const uint8_t* ram, int start, bool& loops)
{
    if(start < 0 || start + BrrBlock > AramSize) return 0;

    int addr   = start;
    int blocks = 0;
    loops      = false;

    // 64K / 9 is the most blocks that could possibly exist.
    while(blocks < AramSize / BrrBlock)
    {
        if(addr + BrrBlock > AramSize) return 0;

        const uint8_t header = ram[addr];
        if((header >> 4) > 12) return 0;            // an impossible shift

        ++blocks;
        addr += BrrBlock;

        if(header & 0x01)                            // end
        {
            loops = (header & 0x02) != 0;
            return blocks;
        }
    }

    return 0;
}

uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

const char* Base64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int base64Value(char c)
{
    if(c >= 'A' && c <= 'Z') return c - 'A';
    if(c >= 'a' && c <= 'z') return c - 'a' + 26;
    if(c >= '0' && c <= '9') return c - '0' + 52;
    if(c == '+') return 62;
    if(c == '/') return 63;
    return -1;
}

} // namespace

// ===========================================================================

Bank::Bank() { clear(); }

void Bank::clear()
{
    present = false;
    title.clear();
    dir = 0;
    std::memset(memory, 0, sizeof memory);
    std::memset(regs, 0, sizeof regs);
    found.clear();
    ++stamp;
}

void Bank::adopt(const uint8_t aram[AramSize], int dirPage, const std::string& what)
{
    std::memcpy(memory, aram, AramSize);
    dir     = dirPage & 0xff;
    title   = what;
    present = true;
    ++stamp;
    scan();
}

void Bank::scan()
{
    found.clear();

    const int base = dir * 0x100;
    if(base + 1024 > AramSize) return;

    for(int i = 0; i < 256; ++i)
    {
        const uint8_t* entry = memory + base + i * 4;

        const int start = le16(entry);
        const int loop  = le16(entry + 2);


        // A directory is 1K of pointers and most games fill a fraction of it.
        // Zero is not a sample, and neither is a pointer into the directory
        // itself.
        if(start == 0) continue;
        if(start >= base && start < base + 1024) continue;

        bool loops = false;
        const int blocks = chainLength(memory, start, loops);
        if(blocks < 1) continue;

        // A directory is 1K of pointers whatever the game does with it, and
        // the entries a game never filled in still point somewhere -- often
        // at something that is nine-byte-shaped enough to walk. The test that
        // separates them without guessing is the second pointer: every entry
        // has two, and in a real one they belong together.
        //
        // A looping sample loops to a block inside itself. A sample that does
        // not loop still has the field, and what games write in it is the
        // address just past the end -- EarthBound and Super Mario World both
        // do -- or, less often, the start again, or nothing at all. An entry
        // that was never filled in has two unrelated numbers, and the odds of
        // the second landing on a block boundary of whatever the first points
        // at are about one in nine times the size of the RAM.
        {
            const int end = start + blocks * BrrBlock;
            const bool spare = !loops && (loop == 0 || loop == start);

            // A pair that says nothing (zero, or the start again) proves
            // nothing either, so those have to clear a length instead: four
            // blocks is two milliseconds, and nothing musical is shorter.
            if(spare && blocks < 4) continue;

            if(!spare)
            {
                if(loop < start || loop > end)     continue;
                if((loop - start) % BrrBlock != 0) continue;
                if(loops && loop == end)           continue;   // nowhere to go
            }
        }

        // And a lone block that does not loop is a directory entry, not an
        // instrument: half a millisecond of sound that nothing plays twice.
        if(blocks == 1 && !loops) continue;

        BrrSample s;
        s.index  = i;
        s.start  = start;
        s.loop   = loop;
        s.blocks = blocks;
        s.loops  = loops;
        found.push_back(s);
    }

    // One last pass, for the thing no single entry can show.
    //
    // A sound driver that leaves a slot unused often fills it in with
    // something rather than nothing, and what it fills it in with is the same
    // pair of addresses over and over: Mega Man X has three slots pointing at
    // $1111, Chrono Trigger nine at $8af2, and both are a couple of
    // milliseconds long.
    //
    // Repetition on its own proves nothing -- a driver keeps its envelopes in
    // its own table, so several instruments sharing one sample at different
    // attacks is ordinary and Kirby Super Star does it a dozen times. It is
    // repetition *and* being too short to be a sound that gives it away.
    std::vector<BrrSample> kept;
    kept.reserve(found.size());

    for(const BrrSample& s : found)
    {
        int same = 0;
        for(const BrrSample& other : found)
            if(other.start == s.start && other.loop == s.loop) ++same;

        if(same > 2 && s.blocks < 8) continue;
        kept.push_back(s);
    }

    found.swap(kept);
}

int Bank::slotOf(int srcn) const
{
    for(size_t i = 0; i < found.size(); ++i)
        if(found[i].index == srcn) return (int)i;
    return -1;
}

int Bank::srcnAt(int slot) const
{
    if(found.empty()) return 0;
    int n = (int)found.size();
    slot = ((slot % n) + n) % n;
    return found[slot].index;
}

// ---------------------------------------------------------------------------
// BRR, decoded the way the chip decodes it.
//
// Four filters, each a two-tap IIR over the last two samples it produced.
// The arithmetic is the chip's -- integers, shifted, clamped -- because a
// waveform drawn from floating point would not be the waveform you hear.
// ---------------------------------------------------------------------------
void Bank::decode(int srcn, std::vector<float>& out, int limitFrames) const
{
    out.clear();

    const int slot = slotOf(srcn);
    if(slot < 0) return;

    const BrrSample& s = found[(size_t)slot];

    int p1 = 0, p2 = 0;
    int addr = s.start;

    const int want = limitFrames > 0 ? limitFrames : s.frames();
    out.reserve((size_t)want);

    for(int b = 0; b < s.blocks && (int)out.size() < want; ++b, addr += BrrBlock)
    {
        if(addr + BrrBlock > AramSize) break;

        const uint8_t header = memory[addr];
        const int shift  = header >> 4;
        const int filter = (header >> 2) & 3;

        for(int n = 0; n < 16 && (int)out.size() < want; ++n)
        {
            int nibble = memory[addr + 1 + n / 2];
            nibble = (n & 1) ? (nibble & 0x0f) : (nibble >> 4);
            if(nibble > 7) nibble -= 16;

            int sample;
            if(shift <= 12) sample = (nibble << shift) >> 1;
            else            sample = (nibble >> 3) << 11;   // the chip's own quirk

            switch(filter)
            {
            case 0: break;
            case 1: sample += p1 + ((-p1) >> 4); break;
            case 2: sample += p1 * 2 + ((-p1 * 3) >> 5) - p2 + (p2 >> 4); break;
            case 3: sample += p1 * 2 + ((-p1 * 13) >> 6) - p2 + ((p2 * 3) >> 4); break;
            }

            if(sample >  0x7fff) sample =  0x7fff;
            if(sample < -0x8000) sample = -0x8000;

            sample = (int16_t)(sample * 2);   // the chip keeps 15 bits and doubles

            p2 = p1;
            p1 = sample >> 1;

            out.push_back(sample / 32768.f);
        }
    }
}

// ---------------------------------------------------------------------------
// An .spc: a song frozen mid-performance, which is 64K of RAM and the chip's
// registers and nothing else. 0x100 of header, 0x10000 of RAM, 0x80 of DSP.
// ---------------------------------------------------------------------------
bool Bank::loadSpc(const std::string& path, std::string& error)
{
    FILE* f = openBinary(path, "rb");
    if(!f) { error = "could not read that file"; return false; }

    std::vector<uint8_t> blob;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);

    if(size < 0x10180) { std::fclose(f); error = "too small to be an .spc"; return false; }

    blob.resize((size_t)size);
    size_t got = std::fread(blob.data(), 1, blob.size(), f);
    std::fclose(f);
    if(got != blob.size()) { error = "that file ended early"; return false; }

    if(std::memcmp(blob.data(), "SNES-SPC700 Sound File Data", 27) != 0)
    { error = "that is not an .spc"; return false; }

    std::memcpy(memory, blob.data() + 0x100,   AramSize);
    std::memcpy(regs,   blob.data() + 0x10100, DspRegCount);

    // The title the ripper typed, if they typed one.
    char header[33] = {};
    std::memcpy(header, blob.data() + 0x2e, 32);
    for(int i = 31; i >= 0 && (header[i] == ' ' || header[i] == 0); --i) header[i] = 0;

    title = header[0] ? header : "spc";
    dir   = regs[0x5d];
    present = true;
    ++stamp;
    scan();
    return true;
}

bool Bank::loadRaw(const std::string& path, std::string& error)
{
    FILE* f = openBinary(path, "rb");
    if(!f) { error = "could not read that file"; return false; }

    std::memset(memory, 0, sizeof memory);

    // Leave the first page for the directory we are about to invent, and put
    // the sample where a sample can live.
    const int at = 0x0200;
    size_t got = std::fread(memory + at, 1, (size_t)(AramSize - at), f);
    std::fclose(f);

    if(got < (size_t)BrrBlock) { error = "too small to be a BRR sample"; return false; }

    // If it does not already end, make it end where the data does.
    bool ends = false;
    for(size_t o = 0; o + BrrBlock <= got; o += BrrBlock)
        if(memory[at + o] & 1) { ends = true; break; }

    if(!ends)
    {
        size_t last = (got / BrrBlock - 1) * BrrBlock;
        memory[at + last] |= 0x03;    // end, and loop
    }

    dir = 0x01;
    memory[0x100] = at & 0xff; memory[0x101] = at >> 8;
    memory[0x102] = at & 0xff; memory[0x103] = at >> 8;

    title = leafOf(path);
    present = true;
    ++stamp;
    scan();
    return true;
}

// ---------------------------------------------------------------------------
// Travelling inside a patch file.
//
// 64K is a lot to put in a JSON string, so the run-length of a bank -- which
// is mostly zeroes in every game -- is packed before it is base64'd.
// ---------------------------------------------------------------------------
std::string Bank::encode() const
{
    if(!present) return "";

    std::vector<uint8_t> packed;
    packed.reserve(AramSize / 4);

    packed.push_back((uint8_t)dir);
    for(int i = 0; i < DspRegCount; ++i) packed.push_back(regs[i]);

    for(int i = 0; i < AramSize; )
    {
        const uint8_t v = memory[i];
        int run = 1;
        while(i + run < AramSize && memory[i + run] == v && run < 255) ++run;

        if(run >= 3 || v == 0xfe)
        {
            packed.push_back(0xfe);
            packed.push_back((uint8_t)run);
            packed.push_back(v);
        }
        else
        {
            for(int k = 0; k < run; ++k) packed.push_back(v);
        }
        i += run;
    }

    std::string out;
    out.reserve(packed.size() * 4 / 3 + 4);

    for(size_t i = 0; i < packed.size(); i += 3)
    {
        uint32_t v = (uint32_t)packed[i] << 16;
        if(i + 1 < packed.size()) v |= (uint32_t)packed[i + 1] << 8;
        if(i + 2 < packed.size()) v |= (uint32_t)packed[i + 2];

        out += Base64[(v >> 18) & 63];
        out += Base64[(v >> 12) & 63];
        out += (i + 1 < packed.size()) ? Base64[(v >> 6) & 63] : '=';
        out += (i + 2 < packed.size()) ? Base64[v & 63]        : '=';
    }

    return out;
}

bool Bank::decodeFrom(const std::string& text)
{
    if(text.empty()) return false;

    std::vector<uint8_t> packed;
    packed.reserve(text.size() * 3 / 4);

    uint32_t acc = 0;
    int have = 0;

    for(char c : text)
    {
        const int v = base64Value(c);
        if(v < 0) continue;

        acc = acc << 6 | (uint32_t)v;
        have += 6;

        if(have >= 8)
        {
            have -= 8;
            packed.push_back((uint8_t)(acc >> have));
        }
    }

    if(packed.size() < 1 + DspRegCount) return false;

    size_t p = 0;
    dir = packed[p++];
    for(int i = 0; i < DspRegCount; ++i) regs[i] = packed[p++];

    int at = 0;
    std::memset(memory, 0, sizeof memory);

    while(p < packed.size() && at < AramSize)
    {
        const uint8_t v = packed[p++];

        if(v == 0xfe && p + 1 < packed.size())
        {
            int run = packed[p++];
            const uint8_t value = packed[p++];
            while(run-- > 0 && at < AramSize) memory[at++] = value;
        }
        else
        {
            memory[at++] = v;
        }
    }

    present = true;
    ++stamp;
    scan();
    return true;
}

// ===========================================================================

namespace shelf {
namespace {
    std::mutex  lock;
    Bank        held;
    bool        anything = false;
    uint32_t    stamp = 0;
    std::string what;
}

void publish(const Bank& bank)
{
    std::lock_guard<std::mutex> guard(lock);
    held     = bank;
    anything = true;
    what     = bank.name();
    ++stamp;
}

bool take(Bank& into)
{
    std::lock_guard<std::mutex> guard(lock);
    if(!anything) return false;
    into = held;
    return true;
}

uint32_t    serial() { std::lock_guard<std::mutex> guard(lock); return stamp; }
std::string label()  { std::lock_guard<std::mutex> guard(lock); return what; }

} // namespace shelf

} // namespace racksnes
