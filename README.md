# RaSNESk — a Super Nintendo Emulator for VCV Rack

Every SNES game builds its own sampler for VCV Rack voltage control. 
<img width="933" height="446" alt="Screenshot_20260913_163145" src="https://github.com/user-attachments/assets/566c22c6-2fa6-4824-8bc0-4e74f28d2342" />


The console's sound chip is a computer of its own — an SPC-700, 64K of its own
RAM and an eight-voice sample player — and the cartridge has no direct access
to it at all. At boot the game uploads a sound driver and a pile of BRR
samples into that 64K, writes the page of a sample directory into one
register, and from then on it can only speak to the chip through four bytes.
Every game brings a different driver and a different set of instruments. That
is why each one sounds like itself, and it is why an `.spc` keeps playing
after the console is switched off: the sound chip was never listening to the
console in the first place.

---

## Getting it

```bash
git clone https://github.com/BooleanMuse/RaSNESk
cd RaSNESk
./build.sh install
```

That is the whole thing. The Rack SDK is fetched on first use and the console
comes with the clone -- bsnes is in this repository, under
`third_party/bsnes-src`, with our one patch already applied -- so there is no
submodule to remember and nothing else to install by hand. Restart Rack and
look for **RaSNESk** under Viron Labs.

Or take a `.vcvplugin` for your platform off the
[releases page](https://github.com/BooleanMuse/RaSNESk/releases) and drop it
in Rack's plugins folder.

## Building it

For the library, and for anybody who would rather not use the script:

```bash
make dist RACK_DIR=/path/to/Rack-SDK
```

That is the entire build. It fetches nothing, it needs no script, and it
cross-compiles wherever the SDK does. The console's own static library is
built by `Makefile.core` as part of it, with the same architecture flags the
SDK compiles the plugin with, into `build/core-<arch>` so that two targets
cannot meet.

| | |
|---|---|
| **Linux** | gcc or clang, make, and `jq` |
| **macOS** | the Xcode command line tools, plus `brew install jq zstd` |
| **Windows** | [MSYS2](https://www.msys2.org/)'s **MINGW64** shell, with `pacman -S make patch jq zip unzip zstd mingw-w64-x86_64-gcc` |
| **Windows, from Linux** | `./build.sh win` -- needs mingw-w64, fetches its own SDK |

`jq` is the SDK's requirement rather than ours: its `plugin.mk` reads the slug
and the version out of `plugin.json` with it. `./build.sh` passes both on the
command line instead, so the script works on a machine that does not have it.

Two things to know if you build on Windows:

* Build inside the **MINGW64** shell, not the MSYS one and not `cmd`. The
  plugin is a MinGW DLL and the SDK's makefiles are written for that shell.
* The repository carries a `.gitattributes` that checks every file out with
  Unix line endings, on Windows too. That is not cosmetic: GNU make does not
  strip a carriage return, so a `Makefile` checked out as CRLF looks for a
  directory whose name ends in a carriage return, and `patch` will not match
  a CRLF patch against LF sources. If you cloned this before that file existed, `git rm -r
  --cached . && git reset --hard` puts it right.

And one if you build for more than one platform in the same working copy: the
SDK compiles into `build/` *without* an architecture in the path, so
`make clean` between targets is not optional. `.github/workflows/build.yml`
gives each platform a checkout of its own and never has to think about it.


---

## RaSNESk — the console
<img width="956" height="468" alt="Screenshot_20260913_162215" src="https://github.com/user-attachments/assets/c919dc91-cf67-4033-bd9b-45ea72f15406" />


Drop a `.sfc`, `.smc`, `.swc` or `.fig` on the cartridge button and it
runs on the panel.


| Jack | |
|---|---|
| **MIX L / MIX R** | the console's own stereo out |
| **V1 – V8** | one chip voice each, dry: its own envelope, none of the game's panning, and none of the other seven |
| **V/OCT, GATE, LEVEL, SRCN** | polyphonic, eight channels, read straight off the sound registers. A closed gate keeps its pitch, the way a synthesiser does; SRCN says which instrument each voice has hold of |
| **ECHO L / ECHO R** | the echo bus on its own, through the chip's eight-tap filter |
| **the twelve gamepad jacks** | one gate to a button. A polyphonic cable presses the button for any channel that is high, so a chord of triggers works on one jack |
| **PAD 2** | the second player, as one polyphonic cable of twelve |
| **PORT / WRITE** | four channels of one cable into the four letterboxes, written on a trigger. This is what the game's own code does to ask its sound driver for a song |


---

## SAMPLER — the cartridge's instruments
<img width="427" height="468" alt="Screenshot_20260913_162309" src="https://github.com/user-attachments/assets/9e02b922-cb6f-4d4e-9642-31a05c465eb9" />

**TAKE** picks up whatever a SNES last ripped; **LOAD** reads an `.spc` or a
raw `.brr`. Either way what arrives is 64K of sound-chip memory, and the
module finds the instruments in it by walking the sample directory: an entry
is real if its BRR chain ends properly and, when it says it loops, loops to a
block inside itself. Nothing that was left uninitialised passes that.


- **V/OCT** and **GATE** are polyphonic. **SAMPLE** is too — a polyphonic
  cable there picks a different instrument per note.
- **ATTACK, DECAY, SUSTAIN, RELEASE** are the chip's own envelope: four bits,
  three bits, three bits and five. They step rather than slide because that is
  what the hardware can be told.
- **GAUSSIAN / CUBIC** — the chip interpolates between sample points with a
  four-tap gaussian, and that dullness is half of what a SNES sounds like.
  Cubic is cleaner and wrong.
- **AS AUTHORED / FORCE LOOP / ONE SHOT** — looping is one bit in the last
  block of a sample's BRR, and the 64K is ours: a drum can be made to hold,
  and a string can be made to stop.
- The echo is the chip's, with its feedback and its delay. On real hardware
  the echo buffer is written back into the same 64K the samples live in and a
  driver has to leave room for it. Here it has a 64K of its own, so it can
  never eat an instrument.

---

## APU — the chip SPC-700
<img width="427" height="468" alt="Screenshot_20260913_162405" src="https://github.com/user-attachments/assets/11530e77-8afa-42ea-ae53-c6172cce0031" />

An SPC-700, its 64K and an S-DSP, with the console taken away — which
is exactly what an `.spc` file is. **LOAD** plays one; **TAKE** picks up a
song straight off the SNES module, so the music keeps going with the console
switched off and the CPU free.

The **four ports** are the only wire the console's processor ever had to the
sound chip. Turn a knob or patch a cable and you are writing them the way the
game's code did. **SONG** is the convenience: most drivers in the Nintendo
family take a track number on port 0 and start playing it. It is a convention,
not a standard — the four knobs are the general case.

**RATE** is that chip's crystal at a volt to the octave: the whole
arrangement, transposed, tempo and all.

---

## BENDER — the circuit bending
<img width="427" height="468" alt="Screenshot_20260913_162456" src="https://github.com/user-attachments/assets/15bea705-bfa5-4894-8cd6-b1fd208d2d29" />

An expander writes into buffers its neighbour
has to have provided, so the console provides them: without that, a BENDER put
beside a SNES writes through a null pointer and takes Rack with it. `./build.sh
probe` now puts the two side by side, because one module on its own cannot
show that. It reaches inside a running cartridge without
its cooperation.

| | |
|---|---|
| **PALETTE** | rotate, shift, invert or drain the 256 colours on their way to the screen — written into the PPU's own table, so what changes is what the cartridge drew with |
| **TRANSPOSE** | a volt to the octave on every voice the sound chip is playing, by multiplying the pitch register the driver just set |
| **WARP** | force the pitch-modulation and noise bits on, a voice at a time. No game turns on more than a couple; this turns on all of them |
| **ECHO** | feedback past where the hardware would be driven |
| **GLITCH** | random bytes into the tiles, the palette, the instruments, or the whole of the working RAM |
| **FREEZE** | holds the sound chip's 64K and hands the same one back every sample. The driver keeps running and keeps finding the world it was in |

---



## Licence

GPLv3, because bsnes is. See LICENSE.txt.
