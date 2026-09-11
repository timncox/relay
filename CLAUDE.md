---
status: active
last_touched: 2026-09-11
---

# relay

MIDI to CV for the **Daisy Patch** (`patch/`) and the **Hermetic Alchemy
Lab V2** (`alchemy/`) from one hardware-free engine (`core/relay.c`) with a
native test harness (`test/`). Read `README.md` first: the engine's rules,
the jack maps, and the "What is unproven" list are the contract. The name
is a placeholder (`/rename-project`).

## Build

    make -C test        native tests, ASan + UBSan; run after touching core/
    make -C patch       Daisy Patch -> patch/build/relay_patch.bin (BOOT_SRAM)
    make -C alchemy     Alchemy Lab -> alchemy/build/relay_alchemy.bin (BOOT_SRAM)

No submodules: the Patch build uses `~/tim-os/daisy-sdk/libDaisy`
(3bd68f2, shared with the other Patch ports); the Alchemy build uses
`~/tim-os/daisy-sdk/alchemy-sdk` (7b1cf0c) and its own
`vendor/libDaisy` (08f2965, built 2026-09-11). Two different libDaisy
commits; never point both at one tree.

**Never run make with a module attached in DFU** (memory
`feedback_no_builds_with_board_in_dfu.md`). Default goals here only build.

## Staging

- Patch: `~/tim-os/daisy-sdk/stage-patch-card.sh` copies
  `patch/build/relay_patch.bin` to `patch-card/modules/relay.bin`.
- Alchemy: copy `alchemy/build/relay_alchemy.bin` into
  `~/tim-os/daisy-sdk/alchemy-card/alchemy/`, then onto the module's card
  through a HostLink-capable family firmware:
  `node tools/hostlink-fs.mjs put <bin> /alchemy/relay_alchemy.bin`.
  Relay itself has no HostLink in MIDI mode (README, "USB is one class per
  boot").

## Shared by copy

`patch/module_picker.*` with the belt/smack/mark/meld Patch ports;
`alchemy/src/picker.*`, the `.lds`, `tools/hostlink-fs.mjs`, the Makefile
and main-file shape with smack/mark/belt-alchemy. A fix in one belongs in
all.

## Facts worth not rediscovering

- libDaisy's USB device stack is one class per boot (`usbd_mode` global in
  `src/hid/usb.cpp`): USB MIDI and HostLink CDC cannot coexist, on either
  port. The Alchemy Settings "USB port" selector chooses at boot.
- libDaisy's parser turns CC 120..127 into `ChannelMode` events and
  NoteOn velocity 0 into NoteOff; the shims map both back to bytes.
- `hw.midi.Listen()` every loop on the Patch: the UART disables itself on
  overrun and that is the only recovery.
- `PersistentStorage` needs `operator!=` on the settings struct and
  compares against QSPI on every Save; the Patch stores at offset 7 MB.
- Selector pots need hysteresis before they reach the engine: a channel or
  voice-count change silences every voice on purpose.
- Patch DAC: 12-bit, 0..3.3 V × 1.512 = 0..4.99 V (Rev8 sheet 2); trimmed
  by CalScl/CalOfs in the menu.

## Status log

- 2026-09-11 — created. Core + 126 native checks green; both firmwares
  build (Patch 109 KB, DTCM 80 %). NOT flashed, NOT heard, NOT measured.
  First contact, Alchemy: does "Relay" show up as a MIDI device on the
  front USB-C. First contact, Patch: pick "relay" in any module's picker,
  play a note on TRS, watch the note row and the gate box.
