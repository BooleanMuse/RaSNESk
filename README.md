# RaSNESk — a Super Nintendo Emulator for VCV Rack

Every SNES game builds its own sampler.

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

## RaSNESk — the console
<img width="956" height="468" alt="Screenshot_20260913_162215" src="https://github.com/user-attachments/assets/c919dc91-cf67-4033-bd9b-45ea72f15406" />


Drop a `.sfc`, `.smc`, `.swc` or `.fig` on the cartridge button and it
runs on the panel at sixty frames a second.

**Play it with a gamepad.** Any pad Rack can see: GLFW ships mappings for
several hundred of them, so a controller it recognises works with no setup at
all, and the right-click menu will teach it one it does not. **Or with the
keyboard** — the arrows, Z and X, Enter for start, and every key remappable
the same way. Both are in the right-click menu; the keyboard is off until you
ask for it, because while it is on those keys go to the game.

The controller's face on the console's lid lights up when a real pad is
plugged in, and clicking it opens the mapping — every one of the twelve
buttons, for the pad and for the keyboard. Click a button, press the one you
want, and the face blinks until you do. That is everything the panel says
about gamepads.

The twelve button jacks are laid out as the controller itself: a cross, a
diamond of four on its grey disc in the colours the machine used, two pills in
the middle and two shoulders on top. A row of twelve sockets labelled UP DOWN
LEFT RIGHT would carry the same signals and tell you nothing.

The picture is 384 x 336 — three panel pixels to every two of the console's,
which is as large as a Super Nintendo gets on a 3U panel: twice size would be
152 mm down and a panel is 128.7. `<` and `>`
walk the rest of the folder it came from. Coprocessors and all: SuperFX, SA-1,
the DSP chips, the Super Game Boy.

**CLOCK is the crystal.** Patch it and the whole machine — picture, music and
pitch together — runs at whatever rate you give it, from a crawl to past
double. That is one setting, not a special mode: the sound comes out of the
same buffer the machine fills, read at the speed the machine is running, so a
slow clock is a slow tape and sounds like one. The right-click menu offers the
other reading of a clock, **Strobe**, where the machine keeps running at sixty
and the picture is what holds.

**RATE** is a volt to the octave on that crystal: 0 is sixty frames a second,
+1 is a hundred and twenty, −1 is thirty. It multiplies whatever CLOCK is
doing, and works on its own when nothing is patched.

| Jack | |
|---|---|
| **MIX L / MIX R** | the console's own stereo out |
| **V1 – V8** | one chip voice each, dry: its own envelope, none of the game's panning, and none of the other seven |
| **V/OCT, GATE, LEVEL, SRCN** | polyphonic, eight channels, read straight off the sound registers. A closed gate keeps its pitch, the way a synthesiser does; SRCN says which instrument each voice has hold of |
| **ECHO L / ECHO R** | the echo bus on its own, through the chip's eight-tap filter |
| **the twelve gamepad jacks** | one gate to a button. A polyphonic cable presses the button for any channel that is high, so a chord of triggers works on one jack |
| **PAD 2** | the second player, as one polyphonic cable of twelve |
| **PORT / WRITE** | four channels of one cable into the four letterboxes, written on a trigger. This is what the game's own code does to ask its sound driver for a song |

The six buttons under the picture are a cartridge, two arrows, run, reset and
a sound chip — a toolbar with pictures on it rather than words, which is the
shape a Mario Paint screen has and the reason the panels are set in a 5x7 font
of their own rather than in a typeface off the desktop. Each module wears a
mascot on its lid instead of a sentence saying what it is.

**RIP** takes the sound chip's 64K — the driver the cartridge uploaded, and
every instrument it brought with it — and puts it where the other three
modules can reach it. The right-click menu will also write it out as an `.spc`.

Eight rows down the right-hand side show what each voice is doing: its
envelope, which sample it has, and what note that works out to.

There is **one console per process**, because bsnes keeps one of everything. A
second SNES module says so on its screen rather than quietly sharing a CPU
with the first.

---

## SAMPLER — the cartridge's instruments
<img width="427" height="468" alt="Screenshot_20260913_162309" src="https://github.com/user-attachments/assets/9e02b922-cb6f-4d4e-9642-31a05c465eb9" />

**TAKE** picks up whatever a SNES last ripped; **LOAD** reads an `.spc` or a
raw `.brr`. Either way what arrives is 64K of sound-chip memory, and the
module finds the instruments in it by walking the sample directory: an entry
is real if its BRR chain ends properly and, when it says it loops, loops to a
block inside itself. Nothing that was left uninitialised passes that.

A SNES instrument has no name — it is a number in a table — so the panel draws
its waveform, with a line where the loop starts.

Then it plays them, on a sound chip of its own: the same code the console
runs, compiled again so that it can be owned, with nobody else writing its
registers. **Eight notes at once, because the chip has eight voices.**

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


