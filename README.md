# Relay

MIDI to CV for two Daisy-based Eurorack modules, from one engine:

- **Daisy Patch** — TRS MIDI in (and USB MIDI on the Seed's micro-USB) to
  CV OUT 1 (pitch, 1 V/oct), CV OUT 2 (assignable) and GATE OUT. One voice.
- **Hermetic Modular Alchemy Lab V2** — USB MIDI on the front USB-C to the
  eight CV-out jacks. One to four voices, with velocity, two assignable
  modulation outs, trigger, clock, run/reset depending on the voice count.

The engine (`core/relay.c`, C99, no hardware in it) does the parser, note
priority, legato, retrigger, glide, pitch bend, poly voice allocation and
stealing, the 24-ppqn clock divider and the transport. `test/` proves it on
the host. Each board directory is a thin panel: pots, buttons, jacks,
display or rings, and the DAC.

The name is a placeholder.

## Status

**Built and unit-tested, never run on hardware** (2026-09-11). Neither
firmware has been flashed; nothing has been heard or measured. See
"What is unproven" before trusting a jack.

## Layout

```
core/       relay.h, relay.c          the engine
test/       make -C test              native tests (ASan + UBSan), 126 checks
patch/      make -C patch             Daisy Patch firmware -> build/relay_patch.bin
alchemy/    make -C alchemy           Alchemy Lab firmware -> build/relay_alchemy.bin
tools/      hostlink-fs.mjs           card access over HostLink (Alchemy, HostLink mode only)
```

Dependencies are taken by path from `~/tim-os/daisy-sdk`: the Patch builds
against `daisy-sdk/libDaisy` (the tree the other Patch ports use); the
Alchemy build against `daisy-sdk/alchemy-sdk` and the libDaisy it vendors at
`vendor/libDaisy`. Those are different libDaisy commits; do not point both
Makefiles at one tree. Override `LIBDAISY_DIR` / `ALCHEMY_DIR` on the make
line to build elsewhere.

Both firmwares are `APP_TYPE = BOOT_SRAM` and carry the family's SD-card
module picker, so they can be switched to and from Belt, Smack, Mark and Meld
(Patch) or Smack, Mark, Belt (Alchemy) from the panel. Their `.bin` files
must live in the card's `modules/` (Patch) or `alchemy/` (Alchemy) folder,
never in the card root.

## The engine's rules

- **Channel** 0 = omni. Messages on other channels are ignored but noted
  (the Patch shows "MIDI on chNN, not NN"; the Lab flashes nothing).
- **One voice**: held notes form a stack; the sounding note is the last,
  lowest or highest held (PRIORITY). A note-off falls back to the next note
  legato with the gate held. A new note that does not win priority changes
  nothing.
- **Two to four voices**: a note takes the free voice released longest ago;
  with none free, the voice holding the oldest note is stolen and
  retriggered. The same note played again retriggers in place.
- **Retrigger** (off by default): on any new note into a sounding voice the
  gate drops for 2 ms and the trigger fires. Off = legato.
- **Glide**: constant-time, linear in pitch. "Legato" restricts it to
  overlapping notes. Bend is added after the glide, never glided.
- **Pitch** = (note − base note) / 12 volts, plus bend × range / 12. Base
  note options are 12, 24, 36, 48, 60 (labelled C-1..C3, Ableton naming,
  60 = C3). Default 36 → notes 36..96 span 0..5 V.
- **Clock**: every 24-ppqn tick is counted whether or not the transport is
  running. The clock output goes high on each Nth tick (1/32 = 3, 1/16 = 6,
  1/8 = 12, 1/4 = 24, 1/2 = 48, bar = 96) and low halfway; a 5 ms pulse for
  a 1-tick division. No ticks for 2 s → low. Start resets the divider and
  fires a 10 ms reset pulse; Start and Continue raise run, Stop drops it.
  Song Position Pointer sets the divider phase.
- **Controllers**: CC 120 and 123 release every voice; CC 121 zeroes bend,
  mod wheel and aftertouch; system reset (FF) releases everything and
  stops. Aftertouch is channel pressure or the last note's poly pressure,
  whichever is higher.
- **Changing channel or voice count releases every voice first** so a pot
  sweep can never leave a gate up.

## Daisy Patch

| Panel | Role |
|---|---|
| TRS MIDI IN | MIDI |
| Seed micro-USB | USB MIDI device (same engine; both inputs merge) |
| CV OUT 1 | pitch, 0..5 V, 1 V/oct from the base note |
| CV OUT 2 | assignable: Vel, Mod, CC n, AfterT, Bend (2.5 V centre), Trig, Clock, Run, Reset |
| GATE OUT | gate |
| knob 1 | octave transpose −2..+2 (five zones, with hysteresis) |
| knob 2 | glide, 0..2 s, square law |
| knobs 3, 4 / CV ins / gate ins | unused |
| encoder | turn = menu, push+turn = edit; push+turn on Mods = module picker |

Screen: channel and which input is alive (TRS / USB), the sounding note
with its volts and a gate box (a "!" means the pitch is outside 0..5 V
and clipped), velocity / mod / bend / octave, then a scrolling menu.

Menu: Chan, Prio, Retrig, Bend, Base, CV2, CV2 CC, Clock, Glide
(Always/Legato), CalScl, CalOfs, Test, Mods. Settings persist in QSPI
(offset 7 MB) two seconds after the last edit.

**Calibration.** From the Rev8 schematic (sheet 2, "DAC Outputs") each CV out
is a 33k/49k9 inverting stage into a 100k/100k inverting stage: gain 1.512,
non-inverting overall, 0..3.3 V in → "0 - 5V" out. `CalScl` is the voltage
at DAC full scale (default 4.990 V) and `CalOfs` the voltage at code 0
(default 0 mV). `Test` holds the base note, then +1..+4 octaves, so a meter
or a tuned oscillator can trim them: play the 0 V and 4 V test notes,
adjust CalScl until they are exactly four octaves apart, then CalOfs until
0 V is 0 V. The gate out is the same op-amp topology, so a high gate is
about +5 V.

## Alchemy Lab

PLAY page: CHANNEL (Omni, 1..16; the ring shows the channel as one lit
LED, amber above 13, and brightens on each note), VOICES (Mono / Duo / Tri /
Quad, with a green pip per gated voice), PRIORITY, GLIDE, BEND (0..24
semitones), CLOCK division.

SETUP page (hold B3): SRC A and SRC B (Vel / Mod / CC / AfterT / Bend), CC A
and CC B numbers, the 0 V note, and what J10 carries in Mono and Duo (Run or
Reset).

Settings (B2+B3 held two seconds): P1 brightness, P2 Retrigger, P3/P4 the
SDK's preset slot and action, P5 **USB port** (MIDI or HostLink, next
boot), P6 Glide (Always/Legato); page 1 is the SD firmware picker.

Buttons: B1 = all notes off (red flash). B2 = test note: the 0 V note, then
+1, +2, +3 octaves, then off (amber while it sounds; B2 is ignored while
B3 is held so the Settings chord cannot leave a note sounding). B2 is blue
on each clock pulse and steady magenta in HostLink mode.

Jack map by VOICES (all ±5 V jacks; unipolar things use 0..5 V):

| | J3 | J4 | J5 | J6 | J7 | J8 | J9 | J10 |
|---|---|---|---|---|---|---|---|---|
| Mono | pitch | vel | src A | src B | gate | trig | clock | run / reset |
| Duo | pitch 1 | pitch 2 | vel 1 | vel 2 | gate 1 | gate 2 | clock | run / reset |
| Tri | pitch 1 | pitch 2 | pitch 3 | src A | gate 1 | gate 2 | gate 3 | clock |
| Quad | pitch 1 | pitch 2 | pitch 3 | pitch 4 | gate 1 | gate 2 | gate 3 | gate 4 |

Pitch sits on J3..J6 because those jacks have per-jack DAC calibration in
the board's cal record (12-bit over 10 V = 2.4 mV, about three cents);
J9/J10 are 24-bit but nominal. Gates on J7/J8 are the STM DAC (<1 µs).
Everything is written from the 1 ms poll, MCP jacks staged and latched
once per poll when something changed.

**USB is one class per boot.** libDaisy's device stack presents CDC or MIDI
descriptors by a global switch, so this firmware cannot be a MIDI device
and a HostLink device at once. In the default MIDI mode there is no
HostLink: `make program-live` does not work, `hostlink-fs.mjs` does not
work, the web programmer sees nothing. Flash it via the SD picker of the
firmware that is running, or hold B3 at power-on and `make program-dfu`
(front USB-C). To reach presets or the card from a computer, set USB to
HostLink in Settings and power-cycle; set it back afterwards.

## What is unproven (2026-09-11)

Nothing has run on hardware. In rough order of "would surprise me":

1. **Alchemy: does a "Relay" USB MIDI device enumerate on the front USB-C?**
   libDaisy's USB MIDI on the Seed2 DFM's external pins has never been
   tried on this board; the SDK's CDC transport uses the same
   `UsbHandle::Init(FS_EXTERNAL)` path and works, so it should.
2. Patch CV OUT scale and offset: 4.99 V full scale is read off the
   schematic, not measured. Gate out level likewise.
3. Patch USB MIDI on the Seed's micro-USB alongside the bootloader's DFU on
   the same port (the app owns the port after the 2 s boot window).
4. RAM headroom: Patch DTCM at 80 % (two libDaisy MIDI queues at 36 KB
   each plus the picker's buffers), Lab DTCM at 86 % (the SDK's own use
   plus one MIDI queue), with the picker and a QSPI save both running from
   the same stack. Builds; not exercised.
5. Alchemy jack polarity and level: `SetVolts(5.0f)` on every backend, and
   the codec-out gates on J9/J10 (24-bit, block-rate, nominal scaling).
6. Latency: MIDI → jack goes through the 1 ms poll; the MCP flush is
   ~430 µs of I²C inside that poll when a value changed.
7. Pitch clamping ±5 V on the Lab and 0..5 V on the Patch: notes outside
   the range clip silently (the Patch shows "!").

## Ideas not built

USB host mode on the Lab's front port (plug a keyboard straight in) needs
VBUS out and CC pull-ups the carrier may not have — unverified. TRS MIDI on
the Lab's expansion header USART1. A CV-in on J1/J2 (trigger jacks) for an
external clock. MPE. Velocity curves. Per-voice poly aftertouch outs.
