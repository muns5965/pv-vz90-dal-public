# pv-vz90-dal

An independent device abstraction layer (DAL) for running a Sony CLIÉ PEG-VZ90 Palm OS 5 ROM on the
CloudpilotEmu "PV" (paravirtualized) machine. It is written from scratch; see `PROVENANCE.md`.

Status (stage g4w): with the user's own VZ90 ROM image the full ROM boots on the PV machine, the Welcome setup can be
completed with pen input, MediaLauncher starts, settings survive a soft reset, and the compared screens match the
reference DAL pixel for pixel, including all 32 colour themes of Preferences > General (clock digits aside).
Known limits: the power key (a 68k fault occurs on both DALs; the reference shows a fault screen, this DAL stops);
an intermittent fatal error in landscape seen once in 19 runs (cause not found); no sound output on PV; 147 of 241
DAL entries are not verified against the reference (62 stop when called; none of them is called in the web tests);
one-hour web emulator runs (launcher apps opened and closed) pass, nothing longer tested yet; CLIE Organizer 手書きメモ cannot create a new memo (the app clears each new record
with a length read from the record's own uninitialised first word, which here is leftover heap data, so the storage heap
is overwritten and a fatal error follows; same code in the Japanese and English ROMs; why a real device is not affected
is not established; the other Organizer parts — address book, ToDo, Memo Pad — work); no slot driver loads (this DAL's module load policy refuses
the 'libs' Memory Stick, CF and NAND slot libraries with 0xF20F, as specified for PV, and the OS's 68K fallback then
reports sysErrLibNotFound), so the Expansion Manager feature is never registered and 辞書 refuses to start; CFUtility refuses to start because the Serial Manager
features ('smgr' 1/2) are not registered (the OS sets them only when SerialMgrInstall finds a port, and every serial
driver is refused on PV).
Development history and measurements: `docs/INTERFACE.md`.

Earlier stages (still built as regression images):
- **stage g1** – bare-metal bring-up (reset, high vectors, per-mode stacks, debug UART, 1 kHz timer IRQ).
- **stage g2a** – probe that enters a Palm OS module (placed at ROM +0x20000) with trap-filled dispatch tables and reports
  the first interface call or fault.

Build options for the full-ROM DAL (`make build/dal-fullrom.bin`):
- `DAL_LOG=0` – no debug output on the PV debug UART (faster); default 1.
- `DAL_SELFTEST=1` – runs the indexed-drawing self-test at boot (test images only).
- `DAL_COLOR_MODE=REFERENCE|SONY_NX70V` – colour lookup (HALDrawFindIndexes). The default `REFERENCE` reproduces the
  behaviour observed in the rePalm VZ90 reference environment. `SONY_NX70V` reproduces the nearest-colour search
  observed on Sony's PEG-NX70V DAL (Palm OS 5.0), which differs. The behaviour of Sony's original VZ90 (Palm OS 5.2)
  DAL has not been determined.

DAL bundle for an emulator-side ROM loader (`make DAL_LOG=0 build/vz90-dal.bundle`): the full-ROM DAL's `boot`, `amdd`
and `amdi` resources in one file, with the link address checked at build time and SHA-256 hashes and the build ID
recorded (format in docs/INTERFACE.md §38). The loader that uses it lives in the emulator, not in this repository.

Build (arm-none-eabi toolchain): `make` → `build/g1.rom`; local probe image: `make build/probe.rom MODULE=<module file>`

Run: `cp-uarm --ram-size 32 -q build/g1.rom` (output appears on the emulator's stderr).

FreeRTOS test image (stage g3pre, separate from the Boot probe): `make build/g3pre.rom`, run like g1. Expected last line: `g3pre: PASS`.
Note: keep the emulator's stdin open when running unattended (e.g. `sleep 60 | cp-uarm ...`); with stdin at EOF the emulator stops before the first timer IRQ.

Integration test (stage g4pre, local-only with the user's Boot module): `make build/g4pre.rom MODULE=<boot module file>`.
FreeRTOS port variant: `G3_PORT=third_party/freertos-arm926-port G3_PORT_OPT=-O0` selects the unmodified port.
