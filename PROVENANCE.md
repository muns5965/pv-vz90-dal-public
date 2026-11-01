# Provenance

Every file in this tree is new code written for this project. Rules:

- No rePalm code (its license forbids uploading any part of it to GitHub/GitLab), not copied, translated or adapted.
- No Palm OS source code (e.g. "limited release" archives) and no Palm OS SDK / Sony SDK files.
- No Sony ROM data. ROM images are built and kept locally by the user and are never committed.

## Sources of facts used

| File | Facts taken from | License of source |
|---|---|---|
| `src/pv_hw.h` | PV register addresses/semantics: CloudpilotEmu `src/uarm/uarm/soc_pv.cpp`, `pv_ic.cpp`, `pv_uart.cpp`, `pv_timer.cpp` (commit 5e3a22e2) | GPL-3.0 |
| `src/start.S` | ARMv5 architecture (vector table, CPSR modes, CP15 c1 V bit); PV map from `pv_hw.h` | n/a |
| `src/main.c` | `pv_hw.h` | n/a |
| `src/link.ld` | PV ROM/RAM bases from `pv_hw.h` | n/a |
| `src/probe.c`, `src/probe_tables.S` | Stage g2a probe. Module header/branch layout read from the first words of a module image; Boot entry convention (r0 = 0x7ffe, r1 = r2 = 0, r3 = entry, System mode, [r9-12] = 0, [r9-8]/[r9-4] = tables, [r9] = globals) **observed with GDB at the Boot entry boundary of the local reference build** (no code inspected). Stage g2b: module header word 2 = byte offset of the module globals pointer in the `[r9]` table, from the observed `[[r9]+4]` = value Boot moves into r9 (GDB memory search); fault report decodes only the base-register field of the single faulting instruction word | n/a |
| `src/probe.c` (g2c) | Stage g2c: export-slot-0 call and its arguments, IRQ/CPSR state at entry, module-globals header layout — **observed** in the reference build with GDB and an entry-trace hook in a local, never-committed emulator build (records only pc/registers when execution enters an address range). See `docs/INTERFACE.md` | n/a |
| `src/probe.c` (g2d) | Module-globals prefix layout and the slot-0x6d call arguments/return value — observed with GDB in the reference build (register reads at the Boot veneer and at the return address) | n/a |
| `docs/INTERFACE.md` | Interface facts observed at the reference call boundary (see above); no code inspected beyond the decoded faulting/veneer instructions | n/a |
| `src/probe.c`, `src/probe_tables.S` (g2e) | Per-call r3/CPSR, replay rule for slot 0x6d, trap frame with lr — observed facts only (trace windows, GDB) | n/a |
| `g3pre/*` | New PV code implementing the driver functions called by the FreeRTOS ARM926 port (names taken from that port's call sites); PV semantics from `pv_hw.h` | n/a |
| `third_party/freertos-kernel/`, `third_party/freertos-arm926-port/` | **Unmodified copies** from jkovacic/FreeRTOS-GCC-ARM926ejs @ aa1507f (FreeRTOS Kernel V10.4.0 + ARM926EJ-S port); checksums in `third_party/SHA256SUMS`; see `third_party/README.md` | MIT |
| `third_party/freertos-arm926-port-pv/` | Copy of the port with one change in `portISR.c` (vTickISR: inline `BL` without lr clobber → C calls), marked in the file; see its README | MIT (modified) |
| `g3pre/regcheck.S` | New: register-preservation test task | n/a |
| `g4pre/*`, `src/probe*.{c,S}` (g4pre) | New: KAL task-ID layer on FreeRTOS (requirement from INTERFACE §9), integration main, `probe_call_in_task`, implemented-slot hook | n/a |
| `g4pre/hal_memory.{c,h}` (g4b) | Record format from the analyst spec + observation; PV RAM size register from CloudpilotEmu `pv_sysctl.cpp` (GPL-3.0); RAM split = same values as the reference PV build, chosen for comparability (documented in INTERFACE §12) | n/a |
| `g4pre/hal_display.{c,h}` (g4d) | Analyst spec + CloudpilotEmu `pv_display.cpp` register semantics (GPL-3.0) + Palm OS RGBColorType layout. **Limitation:** in the earlier rePalm-based work (stage 35b, local only) the implementer edited the reference `halDisplay.c` (VZ90 attributes 0x8001–0x8006); the analyst/implementer separation therefore does not hold for the display code, and this file was written from the spec and device facts with that prior exposure. **g4x:** the file was rewritten by a separate implementer without that exposure (see the g4x paragraph below); the note on the earlier version stays | n/a |
| `g4pre/hal_input.{c,h}` (g4e) | Analyst spec + CloudpilotEmu `pv_keys.cpp`/`pv_touch.cpp`/`SoC.cpp` device facts (GPL-3.0); Palm OS key-bit names | n/a |
| `g4pre/hal_time.{c,h}` (g4f) | Analyst spec + CloudpilotEmu `pv_rtc.cpp`/`pv_timer.cpp` (GPL-3.0); xorshift32 (generic public-domain construction). HALDelay: the implementer wrote the reference DAL's VZ90 HALDelay himself in stage 35b (own code, local tree); this is a fresh rewrite from the spec | n/a |
| `g4pre/kal_sync.{c,h}`, `kal_task.c` (g4g) | Analyst spec (KALSync); FreeRTOS recursive mutex / counting semaphore / scheduler suspend APIs | n/a |
| `g4pre/link_fullrom.ld`, `tools/mkdalres.py` (track B) | ROM-image facts from the analyst documents (ROMImage, DALResources); the ROM itself is built locally with the user's existing tooling, which is not part of this repository | n/a |
| `tools/mkimg.py` | ROM card header fields recognised by CloudpilotEmu `src/common/rom_info5.cpp` | GPL-3.0 |

This project is GPL-3.0-or-later (see `LICENSE`) so that CloudpilotEmu-derived definitions can be used.

## Interface facts taken from the local reference DAL build (separate category)

Function names and one-line declarations of DAL slots that Boot was **observed** to call are looked up in the symbol
table of the local rePalm-based reference build (address of the slot target = symbol address). Only names/prototypes
are used; function bodies are not read; behaviour is described from observation of the reference run. The full
slot table is kept local-only (`slotmap-local.tsv`) and is not part of this repository. Slots recorded so far:
0x6d HALSetInitStage, 0x9f KALTaskGetCurrentID, 0xa0 KALTaskGetInfo (docs/INTERFACE.md §8–10). Since g4-pre the slot →
name map is produced by dumping the reference DAL's `[r9-4]` table from the reference run and matching the entries
against the reference ELF symbols (local-only `slotmap-full-local.tsv`; the order of rePalm's export list in its
source does not equal the slot order). Limitation: while looking up the 0xa0 declaration the implementer also saw
the reference header's `struct KalTaskInfo` field list with its comments; this is recorded here and the field
layout will be re-derived from observation before it is used. This category is flagged for the
source/licence audit before any upload.

## Analyst / implementer separation (from stage g4a)

From g4a, reference function bodies are read only by a separate analyst agent (own context, read access to the
reference tree, writes a prose spec to the local-only `specs/` folder; no code, internal names or algorithms). The
implementer reviews the spec for code content and implements from it, public sources (FreeRTOS) and observation.
Limitation: both roles are the same model in the same session, so this is weaker than a human clean room.
Specs so far: KALTaskGetInfo (`g4pre/kal_task.c`), HALMemoryGetMemoryMap (`g4pre/hal_memory.c`), HALDisplay early-boot set (`g4pre/hal_display.c`), HAL key/pen input (`g4pre/hal_input.c`), HAL random/time (`g4pre/hal_time.c`), KAL sync (`g4pre/kal_sync.c`), ROM image layout and DAL resources (track B), init stages and misc HAL
(`g4pre/hal_events.c`, `g4pre/hal_misc.c`), RAL module loader (`g4pre/ral.c`), Serial Manager install
(`g4pre/ral.c`, Boot entry 0x19B mapping), HALScreen group (`g4pre/hal_display.c`), remaining KAL
(`g4pre/kal_obj.c`, `g4pre/kal_task.c`), HALDraw calling contracts (interface only; drawing is bypassed).
The bitmap/drawing engine is deliberately not implemented in this phase (see docs/INTERFACE.md §28).
g4r: the drawing engine (`g4pre/hal_draw.c`) is written from the analyst's HALDraw spec, public Palm OS format
descriptions and observation (draw-state layout, pattern scale and the standard-palette component order were
corrected from emulator observation and reference screenshots, not from source). `g4pre/hal_time.c` HALDelay was
rewritten from a stated requirement (wait at least the requested time on the 1 ms timer, bounded), not by editing
the earlier version. Slot identification for 0x4E (HALKeyResetDoubleTap): the implementer read the ordered list of
exported function names in the reference's export table (names only, no bodies); behaviour comes from the
HALInput spec. Token audit (10-grams, local tool) after g4r: hal_draw.c 0.4 %, hal_time.c 3.2 %; the longest shared
runs are generic C (loop headers, parameter lists, the public grey levels).
g4s: `g4pre/bmp_codec.c` (bitmap compression/expansion) is written from the public Palm OS bitmap compression
formats (ScanLine, RLE, PackBits), checked against independent Python test vectors (`tools/test_bmp_codec.py`);
the reference decoder was not read by the implementer. Compressed destinations, indexed destinations and the
16 bpp colour derivation follow analyst specs HALDrawCompressed and HALDrawColour16 (prose, reviewed for code
content). Token audit after g4s: bmp_codec.c 0.2 %, hal_draw.c 0.4 % (longest shared runs: `default: return -1`,
parameter lists, public grey levels).
g4t: `g4pre/hal_bootscreen.c` follows analyst spec HALDisplayDrawBootScreen and the public BitmapType V0..V3
layouts; the little-endian bitmap variant (version bit 0x80) was identified from the ROM data. The Boot speed
replacements in `g4pre/ral.c` implement the public Palm OS semantics of MemMove/MemSet/MemCmp/StrCompareAscii.
g4u: the standard-palette colour lookup in `g4pre/hal_display.c` was derived from black-box measurements of the
reference DAL (inputs and outputs only, via the emulator's debugger stub); no reference source was read for it. To
confirm the entry was ARM code the implementer looked at its first eight instructions in a disassembly; nothing
else of that function was examined. The 24-cell exception table is measured data.
g4v: the SONY_NX70V colour mode is derived from black-box measurements of Sony's NX70V DAL running the user's own
NX70V ROM in the emulator (inputs and outputs only; no Sony code was disassembled or read).
g4w: the return codes changed in g4w (HALSoundPlay 0x080A, KAL timeout 0x0501 including KALTaskDelay) were taken
from black-box comparison of the two DALs' return values in emulator runs (OS calls and their results only). The
Color Schemes tests and the pen, power-key and update-area measurements are emulator observations (screenshots,
call logs, the user's own RAM databases exported by the emulator and read as data). No reference or Sony code was
read; the emulator instrumentation used for this stays local. The HALPenCalibrate rule (refusal before init stage 3)
was derived from direct calls of the pen entries through the debugger stub on both DALs (inputs and outputs only).
g4x: `g4pre/hal_display.c` was rewritten because of the prior-exposure note above. The new implementer (an AI agent
of the same model family as the earlier implementer, started in a new session without the earlier conversation)
worked in an isolated copy of the source tree that did not contain `hal_display.c`, its history or any rePalm
material, and was given only the analyst specs HALDisplay, HALScreen and HALDrawIface §1, the emulator's
`pv_display.cpp` and a supplement with this DAL's integration rules (calling conventions, deviations from the
specs, the measured cube exception table, the SONY_NX70V rule, the lock rules). The supplement contains
behavioural statements and measured data only, but its author had read the earlier version. The implementer's file
accesses were logged and checked: only the package and the copied tree were read (one stray build log in the
parent directory, created and deleted by the implementer). Verification: a host black-box comparison of the old
and new versions (both colour modes, four panel configurations, all 2^24 colours against the standard palette,
random, grey, short and modified tables, every attribute ID at all depths, palette and lock sequences) gives
identical results, register writes and memory, except two cases where the old version misbehaved and the new one
follows the spec (a start + count wrap-around in HALDisplaySetPalette, and a panel larger than the framebuffer
area); emulator runs (native boot and Welcome in portrait and landscape, browser setup and one test round) show
no difference beyond run-to-run timing. Token audit (10-grams): against the reference `halDisplay.c` (four local
copies) 0.0–0.2 %, longest shared run 9–15 tokens (the brightness formula and a volatile cast from the spec);
against the earlier version 19.3 % (14.1 % without the data table), longest run 59 tokens (a four-byte compare
helper), expected from the shared spec wording and the same model. Shared identifiers (124): 27 C keywords and types, 50
from the fixed interface (`hal_display.h`, `pv_hw.h`, `hal_memory.h`, parameter names), 7 FreeRTOS API names, 27
that are spec terms or one-letter locals, and 13 others (`depth_code`, `gDriverName`, `gLockCount`, `gPalState`,
`gXlat1/2/4/8`, `put16`, `put32`, `same4`, `dg`/`db`), which name spec concepts (DEPTH code, driver name, lock count,
palette-standard state, translation tables, the "all 4 bytes equal" test). The 59-token run is `same4`, a four-way
byte comparison written in the plainest form (`a[0] == b[0] && ... && a[3] == b[3]`). Limitation: this is weaker evidence than a clean
room with a human implementer and an independently written supplement.
History audit (all 22 commits, 116 file versions): highest share 15.9 % (a small header), longest shared run 34
tokens (CP15 cache-maintenance inline assembly and a pixel-depth switch), all generic. Earlier versions of
`g4pre/hal_time.c` (up to 7f6b70c) contain the HALDelay that was replaced in g4r; before any publication the
history should be squashed or recreated so that only reviewed versions are published (repository is private).

g4w (bundle and ROM format cross-check): `tools/mkbundle.py` and the bundle format (INTERFACE §38) are original work.
To cross-check the ROM container format that the emulator-side loader rebuilds, the user supplied the "Palm OS 4.0
Limited Sources" (Palm, Inc., all rights reserved; a limited-release source set from the public archive
ardiri/dev-palmos, whose README grants no rights to redistribute or modify). Roles were separated: an analyst (a
separate agent) read only that source folder, without access to this project's code, the loader or its notes, and
wrote a format fact sheet (offsets, sizes, bit ranges, field meanings, evidence by file and structure name; no code,
pseudocode or comments). The implementer did not open the source and received only the fact sheet, and compared it
with measurements of the ROM images. Scope: ROM tokens (not answerable from that source) and card / storage / heap /
chunk / database structures. Nothing in this repository was changed on the basis of it, and it is not used for the
DAL slot behaviour. The source and the fact sheet stay local-only. Palm OS 4.0 is a 68k system; its facts are
recorded as such and not taken as OS 5 rules. Each fact is marked definition (declared), behaviour (what the
4.0 code does) or inference. Outcome of the comparison with the measured images: chunk-header bit layout, unused
record-list chaining, the storage-header CRC (checked by 4.0 only for RAM stores; zero in every ROM image here) and the
unchecked card-header checksum agree; chunk alignment (2 vs 4 bytes) and database entry layout (from 0x4E, 10-byte
resource entries vs from 0x50, 12 bytes) differ as expected between the 68k and ARM systems; the original VZ90 ROM ends
its heap with a 4-byte zero after the last free chunk, as 4.0 does, while the rebuilt image does not (a loader-side
question; the loader now ends the heap the same way, after a separate test image passed); the ROM token traversal
rule is not in that source. Record keeping: commit 16f1e2d (first version of this paragraph; author and committer
muns5965, 2026-10-31 00:06:36 +0900, PROVENANCE.md only) carries this session's trailer, but how it was created is
not established from the available session record; the analyst's record shows no repository access.

## Local-only material (never committed)

- Module images extracted from the user's own ROM files for probing (e.g. Boot `boot` 10003) and the probe ROMs built from them.
