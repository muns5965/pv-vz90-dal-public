# Boot ↔ DAL interface: observed facts (stages g2c–g4i-fix)

All facts below were **observed at the call boundary** of the local reference build (GDB breakpoints, register and
memory reads, an entry-trace hook in a local emulator build). No reference code was copied or translated, and no
Sony ROM data is stored in this repository. "Observed" = seen in the reference; "Probe" = reproduced by this project's probe;
"Inference" = not yet confirmed. Function **names** marked *(pvdal symbol)* come from the symbol table of the
local reference DAL build (address match of the slot target) plus its one-line declaration; no function bodies were read.

## 1. Module header (first words of a module image)

| Offset | Value seen | Meaning |
|---|---|---|
| +0x00 | `b <entry>` | ARM branch to the module entry function |
| +0x04 | 0x24 | byte offset of the export table (array of `b` instructions) |
| +0x08 | 4 | byte offset, inside the `[r9]` globals-pointer table, of this module's globals pointer |
| +0x0c | 2 | not yet interpreted |

Boot (`boot` 10003): 854 export slots; entry function = header branch target.

## 2. r9 block

r9 points at word 0 of a block; negative offsets hold call tables.

| Slot | Observed (DAL r9, reference) | Meaning |
|---|---|---|
| [r9-12] | 0 | unused at this stage |
| [r9-8] | table pointer | call table: a call loads the table address from r9-8 and jumps to the word at byte offset 4·slot |
| [r9-4] | table pointer | call table, same access pattern via r9-4 |
| [r9] | pointer to the globals-pointer table | `[[r9]+hdr[2]]` = module globals |

**New in g2c (observed):** the module globals area itself starts with the same layout — Boot replaces r9 with
its globals pointer (the word at `[r9]` + 4, i.e. its entry in the globals-pointer table) and from then on uses that as r9. In the reference the 16 bytes at
globals-12 .. globals+0 already equal the DAL's own `[r9-12] .. [r9]` (0, tab-8, tab-4, globals table) **before** the
first Boot call, i.e. they are set up by the DAL/loader, not by Boot.

## 3. 0xC8 data abort (g2a/g2b)

Cause (probe, confirmed by register decode): a 32-bit store to r1 + 0x24 with r1 = 0xa4 because `[[r9]+4]` (Boot's globals
pointer) was 0. Fix: link a globals area at `[r9 table] + hdr[2]`.

## 4. Entry 1: command 0x7ffe

| | Observed | Probe |
|---|---|---|
| registers | r0 = 0x7ffe, r1 = r2 = 0, r3 = entry address | same |
| CPU | System mode, CPSR 0x1f (IRQ/FIQ enabled) | same (g2c) |
| interrupt controller | mask = timer only, alarm 0 | same (g2c) |
| result | returns 0 | returns 0 |
| side effects | 8 globals words written: +0x8c/+0x94/+0x9c/+0xc8/+0xd0/+0xd8 = globals+0x400, +0xf8 = globals+0x68, +0x114 = globals+0xa4 | identical (relative) |
| stack | — | 4 bytes of System stack used, guard intact |

## 5. Entry 2: Boot export slot 0

Observed (trace + GDB): after 0x7ffe returns, the DAL calls **export slot 0** (module + hdr[1] + 0) with
r0..r3 = 0, r9 = the DAL r9, a fresh System stack (reference sp 0x2000e730), CPSR 0x1f. Boot saves its callee-saved registers,
switches r9 to its globals (§2) and then calls through `[r9-4]` table slots (first call: through a short call stub that jumps via
the `[r9-4]` table at byte offset 0x1b4 = slot 0x6d; in the reference that slot
holds an address inside the DAL). Later trace entries back into Boot
return addresses inside the DAL, and three of them match the HALDisplayGetAttributes calls for attributes 4, 5, 0xb.

Probe (g2c): calling export 0 the same way reaches the veneer and faults when the stub reads the slot-0x6d table entry through a table pointer of 0,
because the probe's globals area did not yet carry the §2 header (`[globals-4]` = 0). Fixed in g2d (§6).

## 6. First DAL call from Boot (stage g2d)

Probe change: the module globals area is `struct ProbeModuleContext { prefix[4]; globals[] }`; the globals pointer
`[[r9]+hdr[2]]` = `&globals[0]`; globals-12 = 0, -8 = `[r9-8]` table, -4 = `[r9-4]` table, +0 = `[r9]` table.
This layout is set only as observed for Boot; it is not assumed for other modules.

| | Reference (GDB at the veneer) | Probe (trap) |
|---|---|---|
| table / slot | `[r9-4]`, byte offset 0x1b4 = slot 0x6d (target inside the DAL) | table 1 (`[r9-4]`), slot 0x6d |
| return address | boot+0x2ddac | boot+0x2ddac |
| r0, r1, r2 | 0, 0, 0 | 0, 0, 0 |
| r3 | **0** | **module+0x24** (probe leaves r3 = called address, see below) |
| r9 | Boot globals (its own) | Boot globals (`&globals[0]`) |
| stack depth from export-0 entry | 0xa8 bytes | 0xa8 bytes |
| CPSR | 0x4000001f | 0x6000001f (System mode; only N/Z/C/V flags differ) |
| result | returns r0 = 0 (r1 = r2 = 0) | stopped at the trap (no implementation) |

0x7ffe with the prefix in place: same return value (0) and the same 8 globals writes as before (the preset
globals+0 word is excluded from the report).

Difference to resolve: `probe_call_module` always passes r3 = the called address (true for the 0x7ffe entry call);
for export 0 the reference passes r3 = 0. r3 is not used before slot 0x6d is reached, but the probe should pass 0
for export 0 to match.

## 7. Entry state corrections (stage g2e)

`probe_call_module` now takes r3 and the entry CPSR per call: 0x7ffe → r3 = entry, CPSR 0x6000001f; export 0 →
r3 = 0, CPSR 0x4000001f (both as in the reference trace). The g2d flag difference came from the probe's own compare
instructions before the call (`msr cpsr_c` only set the mode/mask bits); the flags are now set with `msr cpsr_fc`.
With this, the slot-0x6d call matches the reference in r0..r3 (all 0), r9, stack depth and CPSR (0x4000001f).

## 8. `[r9-4]` slot 0x6d — HALSetInitStage *(pvdal symbol)*, declared `void (uint8_t stage)`

Reference observation window (local trace build: from the function's first instruction to Boot's return address):
r0 = 0 (stage 0); 28 843 instructions, all in debug-print helpers (formatting/putchar); **no memory write outside
the caller's stack, no PV MMIO read or write**; returns with r0..r2 = 0. Within that window the call therefore has
no observed side effect. "No side effect observed in the window" ≠ "no side effect": later stages (r0 ≠ 0) are not
covered.

Probe: diagnostic replay rule (the only one) — slot 0x6d with r0 = 0 returns immediately with r0..r2 = 0. It is
logged as a replay and is not counted as an implementation.

## 9. `[r9-4]` slot 0x9f — KALTaskGetCurrentID *(pvdal symbol)*, declared `Err (uint32_t *tidP)`

Next call after the 0x6d replay, from boot+0x2ddf4. Probe: r0 = pointer into Boot's stack frame (sp + 0x24),
r1 = globals + 0x614, r2 = 0, same stack depth as the reference. Reference window: 106 instructions; enters the
reference kernel through SWI; writes **`*tidP = 4`**; other writes only to the kernel's own stack area; returns
r0 = 0 (errNone). Consequence for the design: Boot export 0 already runs inside a kernel task context in the
reference (current task id 4), i.e. the DAL starts export 0 as a task. **Probe stops here (no replay rule).**

## 10. Boot export 0 inside a FreeRTOS task (stage g4-pre)

Image `g4pre` (local-only when built with the Boot module): 0x7ffe runs before the scheduler exactly as in g2e;
export 0 then runs in a FreeRTOS task ("boot", priority 2, 8 KiB stack) via `probe_call_in_task` (System mode,
r0..r3 = 0, flags Z as in the reference). Treated as a design hypothesis: the reference shows a task context
(KAL id 4), not necessarily that export 0 must be a separate task.

| | Reference | g4pre |
|---|---|---|
| 0x6d call | r0..r3 = 0, depth 0xa8 | r0..r3 = 0, depth 0xa8, replayed |
| 0x9f KALTaskGetCurrentID | `*tidP = 4`, returns 0 | **implemented** (`g4pre/kal_task.c`): `*tidP = 1`, returns 0 |
| next call | [r9-4] slot 0xa0 from boot+0x2de10 | [r9-4] slot 0xa0 from boot+0x2de10, r0 = the ID just returned (1), r1 = Boot stack buffer, r2 = 1 |
| CPSR at the calls | 0x4000001f / 0x6000001f | 0x4000005f / 0x6000005f — **FIQ masked** (the port's initial task SPSR); PV has no FIQ source used here |

KAL task IDs (temporary rule, documented in `kal_task.h`): assigned 1, 2, 3, … in registration order; the
reference's value 4 is not reproduced or hard-coded. The error value for a non-KAL caller is a placeholder.

Slot 0xa0 = **KALTaskGetInfo** *(pvdal symbol, via the reference DAL table: table contents dumped from the reference
run and matched against the reference ELF symbols)*, declared `Err (uint32_t tid, struct KalTaskInfo *info)`.
Boot passes the ID it received from 0x9f. **g4pre stops here (not implemented).**

## 11. `[r9-4]` slot 0xa0 — KALTaskGetInfo (stage g4a)

Sources: functional requirements written by a separate analyst agent from the reference implementation
(local-only, prose, no code; reviewed before use) + reference observation (trace window and read watch on the
record). Implementer did not read the reference body.

Record (caller-owned, 36 bytes): +0x00 user data, +0x04 resume address, +0x08 stack pointer, +0x0C stack value
(below), +0x10 priority (encoded Palm scale), +0x14 tag, +0x18..+0x23 not written. Errors: tid 0 / unknown →
0xF00C, nothing written.

| | Reference (tid 4) | g4a (tid 1) |
|---|---|---|
| r0/r1 at the call | tid, record on Boot's stack | same |
| +0x00 | 0x2000c728 | untouched (Boot task has no KAL creation data) |
| +0x04 | 0x1000874a (inside the reference kernel call) | 0x1004de10 (return address into Boot) |
| +0x08 | 0x2000e668 | 0x200020f4 (Boot sp at the call) |
| +0x0C | 0xffffe0d0 | 0xffffe0ec |
| +0x08 + +0x0C | 0x2000c738 = lowest stack address | 0x200001e0 = FreeRTOS stack base of the task |
| +0x10, +0x14 | 0x9c, 0x66727374 | untouched |
| bytes written | 24 | 12 (none outside +0x00..+0x17) |
| return | 0 | 0 |

Boot reads only +0x08 and +0x0C after the call (read watch: two Boot instructions), then passes +0x0C on in r1 to
the next call — consistent with Boot computing the stack's lowest address as +0x08 + +0x0C. The reference's
"negative" +0x0C is therefore reproduced on purpose (bit-compatible meaning), not a guess.

Next call: `[r9-4]` slot 0x51 from boot+0x2de54 — **HALMemoryGetMemoryMap** *(pvdal symbol)*, declared with no
arguments, returning a pointer to a constant memory-map record (reference returned a ROM address). r1 at the call
carries +0x0C (reference 0xffffe0d0, g4a 0xffffe0ec). **g4a stops here.**

Open: CPSR FIQ mask at task level (port's initial task SPSR 0x5f vs reference 0x1f) — unchanged, no effect seen;
FreeRTOS -Os issue — fixed in `freertos-arm926-port-pv` (g4-pre), still in force.

## 12. `[r9-4]` slot 0x51 — HALMemoryGetMemoryMap (stage g4b)

Sources: analyst spec (local-only) + reference observation (read watch on the returned record).
Contract: no arguments; returns a never-NULL pointer to the same read-only record; no side effects.

Record, 8 bytes: +0x00 u8 entry count, +0x01..+0x03 padding, +0x04 pointer to an entry array (no terminator).
Entry, 12 bytes: +0x00 u8 kind (0 ROM, 1 dynamic RAM, 2 storage RAM), padding, +0x04 base, +0x08 size.

Observed Boot use (reference): reads count and array pointer, then **searches by kind** (reads the kind byte of all
3 entries on each lookup, 38 lookups during boot), reads ROM base many times, ROM size twice, dynamic and storage
base/size once or twice each.

PV memory layout of this DAL (`g4pre/hal_memory.h`; a platform design decision of this project — it uses the same
split as the reference PV build so that runs can be compared; recorded as such):

| Region | Base | Size | Reported |
|---|---|---|---|
| ROM window (PV mapping) | 0x10000000 | 32 MiB | kind 0 |
| DAL private (data, bss, stacks, FreeRTOS heap) | 0x20000000 | 1 MiB (g4b image ends at 0x2001fed8) | no |
| framebuffer / palette reserve | 0x20100000 | 1 MiB | no |
| dynamic RAM | 0x20200000 | 6 MiB | kind 1 |
| storage RAM | 0x20800000 | 24 MiB | kind 2 |

Boot-time check (new): PV sysctl RAM size (0x30000900, CloudpilotEmu `pv_sysctl.cpp`) must be ≥ 32 MiB, else halt.

g4b result: Boot takes the map and next calls **table 2 (`[r9-8]`) slot 0x170** from boot+0x2dee8 with
r0 = storage base 0x20800000, r1 = storage size 0x01800000, r2 = pointer into Boot's stack, r3 = 6. Probe stops here.

## 13. `[r9-8]` is the Boot module's own dispatch table (observed, g4b)

Reference: `[r9-8]` entry *i* = the branch target of Boot export *i* (checked for exports 0, 1 and 0x170: 0x1059f2e4,
0x10596410, 0x10598588 — each equals the resolved `b` at boot + 0x24 + 4*i). So the DAL (module loader role) must
fill `[r9-8]` with the resolved targets of the Boot export table; the probe currently traps every `[r9-8]` slot.
Slot 0x170 is therefore a Boot-internal function, not a DAL service.

## 14. DAL slot statistics (`[r9-4]` table, reference run; corrected in g4b)

The reference `[r9-4]` table has **241 entries (slots 0x00–0xF0)**; all 241 resolve exactly to symbols of the
reference DAL. The 12 non-zero words found further on (offsets 0x100–0x104, 0x3EE–0x3FF) are other RAM data
(pointers into RAM, small integers), not table entries; the earlier count "253 valid / 244 matched / 9 unresolved"
(g4a review) is withdrawn — it treated those words as slots and matched some against data symbols. The full map
stays local-only.

| Slot | Name *(pvdal symbol)* | This DAL |
|---|---|---|
| `[r9-4]` 0x6d | HALSetInitStage | diagnostic replay, stage 0 only |
| `[r9-4]` 0x9f | KALTaskGetCurrentID | implemented (temporary ID rule, temporary error code) |
| `[r9-4]` 0xa0 | KALTaskGetInfo | implemented; +0x00 user data / +0x10 priority / +0x14 tag written for tasks registered with KAL attributes (g4i-fix, §23); other-task query untested |
| `[r9-4]` 0x51 | HALMemoryGetMemoryMap | implemented |
| `[r9-8]` 0x170 | Boot-internal (export 0x170) | reached directly through the module dispatch table (g4c, §15) |
| `[r9-4]` 0x0b | HALDisplayGetAttributes | implemented (g4d, §16) |
| `[r9-4]` 0x4d | HALKeyGetState | implemented, pre-driver path (g4e, §17) |
| `[r9-4]` 0x62 | HALRandomInitializeSeed | implemented (g4f, §18) |
| `[r9-4]` 0x92 / 0x95 | KALMutexCreate / KALMutexReserve | implemented (g4g, §19) |
| `[r9-4]` 0xa4 | KALTaskSwitching | implemented (g4g, §19) |
| `[r9-4]` 0x52 / 0x53 / 0x55 | HALMemory Get/SetStorageAreaProtectionState, GetAreaSize | implemented, logical state only (g4i, §22) |
| `[r9-4]` 0x84 | HALDisplayDrawBootScreen | **temporary bypass, not implemented** (returns 0, draws nothing; §21) |

## 15. Module dispatch table built by the DAL (stage g4c)

At load time the DAL builds the module's `[r9-8]` table in RAM: entry *i* = destination of export *i*
(module + hdr[1] + 4·*i* holds an unconditional ARM branch; the destination is computed from its offset field).
Entries are filled while the export word is such a branch and its destination lies inside the module image;
all other entries keep the trap stub. Boot: 854 (0x356) exports resolved, export 0 → module + 0x2dd84 (same
offset as the reference). The globals prefix (globals-8) points to the same table.

Result: Boot runs past its internal call (export 0x170), calls HALMemoryGetMemoryMap three more times (return
addresses boot+0x26d04 and boot+0x2e2cc match reference trace entries 7 and 9), then calls **`[r9-4]` slot 0x0b
HALDisplayGetAttributes** from boot+0x2dc3c with r0 = 4 (attribute 4; reference trace entry 10 returns to the same
address). Stops there (not implemented).

## 16. Display, early-boot subset (stage g4d)

Sources: analyst spec (local-only) + PV display device (CloudpilotEmu `pv_display.cpp`: BASE, STRIDE, DEPTH code
0..4 = 1/2/4/8/16 bpp, CLUT write port, read-only SPEC with width/4-1, height/4-1, density/36-1).

DAL start-up (`g4pre/hal_display.c`): panel size and density from SPEC (480×320 with `--display 480x320`, 320×480
by default, 144 dpi), 16 bpp, stride = width·2, framebuffer at the reserved 0x20100000 (base programmed last),
cleared to white. Implemented: HALDisplayGetAttributes and HALDisplaySetAttributes for the attribute IDs of the
early-boot set (size, stride, density, depths, brightness/contrast, VZ90 layout / input-area offsets / framebuffer
address); HALDisplaySetPalette (Palm OS RGBColorType records → CLUT port); Doze/Wake/DrawNotify/SendUpdateArea as
no hardware action. Attribute IDs not implemented here (e.g. 0x8009, which needs the Sony shared page that this
image does not map) make the trap stop instead of returning a guessed value.

Observed in g4d (landscape): Boot requests attributes 4 (width → 480), 5 (height → 320) and 0x0b (maximum depth →
16) twice, from the same return addresses as the reference, then calls **`[r9-4]` slot 0x4d HALKeyGetState**
(not a display function) — stop. Portrait (default display) returns 320 / 480 and reaches the same point.
No other display function was called yet; screen output itself is not verified in this stage.

## 17. Key input, early-boot subset (stage g4e)

Sources: analyst spec (local-only) + PV key/touch devices (CloudpilotEmu `pv_keys.cpp`, `pv_touch.cpp`, `SoC.cpp`):
keys at 0x30000500 (read = key word, any write clears IRQ 3), touch at 0x30000300 (read = x | y<<16 and clears
IRQ 1; pen up = x = y = -1).

HALKeyGetState(void) returns the key-bit word itself (not an error code; 0 = no key). Bits are the PV bits except
nav select: PV 0x08000000 → keyBitNavSelect 0x04000000. Before the input driver is brought up the value is read
directly from the key register — read only, never written, so a pending key IRQ is not cleared. The driver path
(IRQ 3/1 handling, one read per touch sample, event queue, tracked state, auto-repeat, pen calibration) is not
implemented yet; HALKeySetMask / GetRates / SetRates store and return values only.

Observed: Boot calls HALKeyGetState once from boot+0x2df9c (no key: 0). With a key held from power-on (test build
`G4_DEFS=-DG4_INPUT_TEST`, emulator `key` command): hard1 → 0x8, up → 0x2, select → 0x04000000 (remapped).
In all cases Boot then calls **`[r9-4]` slot 0x62 HALRandomInitializeSeed** (not input) — stop. Portrait reaches the
same point.

## 18. Random numbers and time (stage g4f)

Sources: analyst spec (local-only) + PV devices (CloudpilotEmu `pv_rtc.cpp`: +0 = host time in Palm OS epoch seconds,
+4 write clears the 1 Hz IRQ 2; `pv_timer.cpp`: free-running 1 kHz 64-bit counter, a write to LO only clears the alarm
signal).

- HALRandomInitializeSeed(UInt32 *randomSeedP): **returns** a 32-bit seed through the pointer (it does not set a
  seed); each call advances the shared generator, so repeated calls differ. HALRandomGetBits(numBits, bitsP) writes
  ceil(numBits/8) bytes. Generator: xorshift32, seeded at DAL start from the RTC seconds and the uptime counter —
  not a fixed seed (two runs gave 0x4bb469a1 and 0x15226859); not cryptographically strong.
- HALTimeGetSeconds / SetSeconds: RTC + an in-RAM offset (set changes only the offset). HALTimeGetSystemTime: ms
  since power-on (timer LO). HALDelay(µs): busy-wait on the 1 kHz counter, ms rounded up + 1, capped at 2 s, gives up
  if the counter stops.
- Not implemented: HALTimeGetAlarm / SetAlarm (need clock-event delivery to the OS), HALTimeGetSystemTimerInterval.

Observed: Boot calls HALRandomInitializeSeed once (boot+0x2e07c), then **`[r9-4]` slot 0x92 KALMutexCreate** with
r0 = output pointer and r1 = 'MemM' (kernel group) — stop. Portrait reaches the same point.

## 19. KAL mutexes, semaphores, task switching (stage g4g)

Sources: analyst spec (local-only). Slot numbers 0x92–0x95 for the mutex calls agree with the public Ecliptic
project's table (MIT; Mutex Create/Delete/Reserve/Release = 146–149).

`g4pre/kal_sync.c`: KAL handles are opaque non-zero values from one counter shared by all object kinds, mapped to
FreeRTOS objects. Mutexes are recursive (FreeRTOS recursive mutex; release by a non-owner → 0xF009); counting
semaphores without an upper limit. Timeouts: signed ms, negative = forever, 0 = no wait, rounded up to the 10 ms
FreeRTOS tick. Return codes: 0, 0x0504 timeout, 0x0102 no slot, 0xF002 NULL output, 0xF00C unknown handle, 0xFFFF
for the null mutex handle. Known differences: FreeRTOS orders waiters by priority (reference: arrival order); delete
does not yet release waiters with 0xF009. KALTaskSwitching(false/true) maps to vTaskSuspendAll / xTaskResumeAll
with an 8-bit nesting count; an extra enable returns 0xF009.

Observed (test image): KALMutexCreate(tag 'MemM') → handle 1, KALMutexReserve(1, forever) → 0,
KALTaskSwitching(off), then **`[r9-4]` 0x53 HALMemorySetStorageAreaProtectionState** (memory group) — stop.

## 20. Full Palm OS ROM image with this DAL (track B, local-only)

The ROM is built locally with the user's existing ROM tooling (not part of this repository); generated images and the
Sony databases never leave the local machine. This repository provides:
- `make build/dal-fullrom.bin`: the DAL linked to run in place at 0x10000230 (first ROM heap chunk; its first word is
  the ARM branch the card header's reset word jumps to);
- `tools/mkdalres.py`: the two small resources the builder requires in the DAL database. Neither is read by Palm OS;
  in the reference design only the DAL's own start-up reads them, and this DAL reads neither:
  'amdd' 19000 = data-initialisation template (big-endian header word, three segments of {signed big-endian offset,
  run-coded stream ending in 0x00}, then two little-endian relocation counts) — written as three empty segments;
  'amdi' 19000 = module data sizes below / above r9 (big-endian words) — written as 0x28, 0x10.
- In a full image the probe finds Boot 'boot' 10003 through the storage header (+0xF8) instead of the test marker.

Result: the emulator recognises the image (PV device), the DAL runs from 0x10000230, Boot 10003 is found at the same
address as in the reference image, and the Boot call sequence of the test image is reproduced with the reference's
exact return addresses (0x6d, 0x9f, 0xa0, 0x51 ×3, 0x0b ×3). The path then differs from the Boot-only test image:
Boot calls **`[r9-4]` 0x84 HALDisplayDrawBootScreen** (the full ROM contains the boot-screen bitmaps). Stop there.
Portrait (320×480) reaches the same point.

## 21. HALDisplayDrawBootScreen — temporary bypass (stage g4h-skip)

Called by Boot in the full ROM with (x, y, bmpP) = (0, 0, boot-screen bitmap family); the first family member is a
320×320, 8 bpp, compressed bitmap. Drawing it needs decompression and palette conversion, which is **not
implemented**: the call returns 0 (errNone, the documented return value) and draws nothing, so the screen stays
the white of display initialisation. It is logged as a bypass and is not counted as an implementation.
Unverified assumption: the function only draws and changes no other state that later code depends on.

Result (full ROM, 480×320 and 320×480): after the bypass the full-ROM path rejoins the sequence seen in the Boot-only
image — HALKeyGetState, HALRandomInitializeSeed, KALMutexCreate('MemM'), KALMutexReserve, KALTaskSwitching(off) —
and stops at **`[r9-4]` 0x53 HALMemorySetStorageAreaProtectionState**.

## 22. Storage-area protection (stage g4i)

Sources: analyst spec (local-only) + PV CPU/MPU facts from CloudpilotEmu (`cp15mpu.cpp`, `MPU.cpp`: 8 regions, access
codes none / privileged RW / privileged RW + user RO / full; no code makes memory read-only for privileged modes;
reads are not checked).

HALMemorySetStorageAreaProtectionState(Boolean): TRUE = storage RAM (0x20800000, 24 MiB) writable, FALSE = read-only;
no return value, no nesting (last call wins). HALMemoryGetStorageAreaProtectionState returns the recorded value
(FALSE until first set). **The protection is a logical state only**: Palm OS tasks run in privileged System mode
and the MPU is off, so storage writes are never blocked in either state — as in the reference PV build. Each set call
in the g4i image performs a write/read-back check at the end of storage RAM: it succeeds in both states.
HALMemoryGetAreaSize(addr, access) returns 1024 inside the DAL's kernel heap (FreeRTOS heap array), else 0.

Observed (full ROM, both orientations): Boot alternates Set(TRUE) … Set(FALSE) around storage work, together with
HALMemoryGetMemoryMap, KALMutexRelease/Reserve and KALTaskSwitching(on/off) — the pattern of a memory-manager
critical section. After a second KALTaskGetInfo (from boot+0x438f0) Boot **writes to 0x105980e8, an address inside
Boot's own code in ROM** → data abort (`W of 4 bytes to 0x105980e8 failed`). The Boot-only image ends the same way.
Leading hypothesis (not verified): Boot uses a KALTaskGetInfo field this DAL leaves untouched for non-KAL tasks — the
+0x00 user-data word, which in the reference is 0x2000c728 (just past Boot's 0x668-byte globals) — and writes
through it. Stop here.

## 23. Boot task user-data word — cause of the ROM write fault (stage g4i-fix)

Observation (this DAL, full ROM): the record returned by the second KALTaskGetInfo had +0x00 = 0x105980e4 — stack
contents left in a field this DAL did not write (the first call showed 0xa5a5a5a5, the FreeRTOS stack-fill pattern);
the faulting write went to 0x105980e8 = that value + 4. So Boot reads +0x00 as a pointer and writes at +4.

Requirement (analyst spec, local-only): the task that runs Boot export 0 carries a user-data word pointing to a
dedicated, zero-filled, writable block (the reference uses 8 bytes, allocated before the task is created, never
freed); the observed reference address is only a consequence of its allocation order and is not reproduced. Tasks
created through KALTaskCreate return their creator's value unchanged (not yet implemented here).

Fix: `kal_task_register_attrs()`; the boot task gets a 32-byte zero-filled static block, Palm priority 100 and the
diagnostic tag 'pvbt'; KALTaskGetInfo now writes +0x00, +0x10 ((0x200 − priority) & 0xFF = 0x9c, the encoding seen
in the reference record) and +0x14 for such tasks.

Result (full ROM, 480×320 and 320×480): no fault; 327 DAL calls (HALMemoryGetMemoryMap 292×, storage protection and
KAL mutex/switching sections, three KALTaskGetInfo calls with +0x00 = the block address), then **`[r9-4]` 0x6d
HALSetInitStage with r0 = 1** — stage 1 is outside the diagnostic replay rule (stage 0 only), so the probe stops.
The Boot-only image runs the same way.

## 24. Init stages, events and misc HAL (stage g4j)

Requirement (analyst spec, local-only): HALSetInitStage(0..4). Stage 1 (memory and Data Manager up) brings up the
drivers: hardware-event task, RTC interrupt masked, touch and key interrupts unmasked; HALKeyGetState then reports
the tracked state. Stage 2 = Sony start-up (SonyDAL load with its exports replaced, silkscreen database), stage 3 =
Sony 'ShAl' and generic features, stage 4 = nothing. HALEventRegisterCallBack/HALEventPost keep one callback per
event type and run it with the registering module's r9. Misc group: processor ID, OEM IDs and ROM tokens from the
card header, battery/dock/power constants, interrupt-handler table, debug output, I-cache invalidate, sound off.
HALErrDisplay logs the OS text and stops the probe.

Implemented: stages 0/1/4; **stages 2 and 3 are logged as not implemented (temporary)**.
Result: full ROM passes stages 0, 1, 2, then stops at `[r9-4]` 0xAF RALGetModuleInfo. Boot-only image (no
databases) now ends in the OS error "DataMgr.c, Line:3718, Null dbH passed" after stage 1 — expected for an image
without storage databases.

## 25. RAL module loader (stage g4k)

Requirement (analyst spec, local-only): the ten RAL slots 0xAD..0xB6. Globals-pointer table of 0x400 entries indexed
by module ID (ID = (H/4)*32 + L/4 from header +8/+0xA), exposed as four quarter-pointers at [r9+0..12] of every r9
world; below r9: [r9-4] DAL table, [r9-8] Boot table, [r9-12] UI table (0 until stage 1), [r9-16] 0. Memory needed =
below (≥ 0x28) + above (≥ 16, from the info resource) + (1 + 2N) words of dispatch block for modules other than
DAL/Boot/UI. Load: zero the area, unpack the data template (3 segments of run/literal codes, then data→data and
data→code relocation lists), write the system words, resolve exports (ARM B chains, ARM long thunks, Thumb B.W and
B.N+NOP), build a stub array (`ldr pc, [pc, #-(4N+8)]` per export) or the fixed UI table, publish, send 0x7FFE.
Load policy refuses 'vdrv' and some Sony/Palm driver libraries with 0xF20F. Unload, UnloadNext (downward scan for
use count 0), GetEntryAddresses, LinkClient (stub array), PatchEntry, GetStaticBase, SetA5 (dormant app world),
CallWithNewStack. One recursive lock serialises RAL calls.

Also from this stage: Boot's own data template ('boot' 10001, located through the storage header) is unpacked into
Boot's globals before 0x7FFE (VZ90: one 1056-byte segment at r9+0, no relocations — Boot previously ran with
all-zero globals); Boot's table carries its entry count in the word before it and 0xFFFFFFFF padding up to 909.

Result: UI ('rsrc'/'puil', ID 2), PACE and further libraries load; the 'vdrv' serial drivers are refused; then the
OS stops with "PalmOSMain.c, Line:506, Can't install Serial Manager." (next section).

## 26. Serial Manager install with no serial drivers (stage g4k)

Requirement (analyst spec, local-only): every 'vdrv' module is refused on PV, so the OS's SerialMgrInstall finds no
port and returns serErrNoDevicesAvail (0x030B), which PalmOSMain treats as fatal. Before the boot task starts, the
DAL replaces Boot entry 0x19B (SerialMgrInstall) through RALPatchEntry by a pass-through that always runs the
original (it sets up the Serial Manager globals) and maps exactly 0x030B to 0; any other result is passed on.
Result: "SerialMgrInstall (original) returned 0x030b (no serial ports) -> 0"; boot continues to HALScreenInit.

## 27. HALScreen group and remaining KAL (stages g4l, g4m)

HALScreen (analyst spec, local-only): HALScreenInit(outP, palette) writes the address of a 32-byte screen bitmap
descriptor (BitmapTypeV3 header, version 0x83, flags 0x007A at 16 bpp / 0x005A indexed, then the colour-table and
bits pointers) and copies the given colour table into a 1028-byte current-palette object; UpdateBitmap, GetColortable,
Palette (translation tables, CLUT), DefaultPalette, Lock/Unlock (frozen copy after the framebuffer).
Not implemented: the re-check of the palette against the OS standard 8 bpp table (DefaultPalette stays false after
the first HALScreenPalette); the standard table is not loaded into the CLUT at display init (irrelevant at 16 bpp).

KAL (analyst specs, local-only): event groups with all 32 bits usable, FIFO broadcast waiters, 12-byte wait request
(bits, 1 = all / 2 = any, timeout ms); mailboxes of 32-bit words, Send never blocks (0xF00A when full), depth 0
hands over only to a waiting task; tasks dormant until KALTaskStart, entry r0 = Start parameter, r9 = creator's
r9, return from the entry ends the task; KALTaskWait/Wake counting signal (cap 1023), WaitClr; one-shot timers run
in a dedicated task above all OS tasks with the r9 of the last KALTimerSet. Palm priority (lower = more urgent,
0..0x1FF) → FreeRTOS 29..1, monotonic. GetInfo slots and KALTaskExit: logged, 0xF009.
Known differences: FreeRTOS tick is 10 ms (timeouts rounded up), KALTaskWake does not refuse targets blocked on other
objects, Suspend of a task blocked in KALTaskWait is not distinguished, task IDs follow the temporary
registration-order rule (§9).

## 28. HALDraw — temporary display bypass (stage g4n-skip)

Requirement (analyst spec, interface only): HALDrawFindIndexes (0x16/0xBF) writes the nearest index byte of each
RGBColorType record (implemented; exhaustive search where the reference uses an approximate one for a confirmed
standard palette); HALDraw_GetSupportedDensity (0xCD) iterates 0→72→108→144→216→288→0 (implemented).
The drawing entry points 0xBE Bitmap, 0xC0 GetPixel, 0xC1 Chars, 0xC2 Line, 0xC3 Rectangle, 0xC4 Pixels are a
**TEMPORARY bypass**: nothing is drawn and no refresh is requested; Bitmap returns 0, GetPixel the placeholder 0,
the others return nothing. Each call is logged as `g4n-skip` with its destination (screen/offscreen). The OS state can
progress, but the screen keeps showing the initial white framebuffer. Sound slots return the PV "no hardware" codes.

Result (full ROM): KAL tasks and timers are created ('SeSy' task), Sony extensions load, then the OS loads
'libr'/'Sdal' (SonyDAL); with no DAL-side replacement its own code runs and reads Sony hardware at 0x80160004 →
data abort. Cause known: the DAL-side SonyDAL/SeRm/yP1L replacements and the stage 2/3 Sony actions are not yet
implemented (next stage).

## 29. Sony library replacements and stage 2–4 Sony hook (stage g4o)

Requirement (analyst specs, local-only): from stage 1 on, a device with a 'libr'/'Sdal' database named "SonyDAL" is
Sony. Every fresh RALLoadModule of SonyDAL ('libr'/'Sdal'), RmcExt ('aext'/'SeRm'), Pa1Lib ('libr'/'yP1L'), GE2D
('aexo'/'SeGE', 23 exports, loadable only on Sony) and I2CDriver ('libr'/'i2cD', 4 exports, only on Sony) gets its
export table and main entry replaced by DAL routines after publishing and before 0x7FFE, so no original instruction of
these modules runs. SonyDAL: 45 routines (fixed values, small state, or a stop for entries the reference never
expects); RmcExt/Pa1Lib: every export stops; GE2D: entry 22 (context lock) returns 0, the rest stop; I2CDriver
answers "no device acknowledges". Stage 2: SysLoadModule('libr','Sdal'), SonyDAL entry 1 with 2 (opens the 'aslk'
or 'silk' / 'ShAl' silkscreen database and leaves it open), feature 'btse' 1. Stage 3: 'ShAl' 20020/20022/20024/
20027/20031, entry 1 with 3 ('ShAl' 20005/20021/20000/20050), then 'fway', 'PALM', 'dnsT', 'scnP'. Stage 4: entry 1
with 4. OS services are reached through Boot's dispatch table (DmGetNextDatabaseByTypeCreator 0x66, DmDatabaseInfo
0x4B, DmOpenDatabaseByTypeCreator 0x7B, FtrSet 0x10F, SysBatteryInfoV40 0x1F8, SysFindModule 0x20B,
SysGetEntryAddresses 0x20E, SysLoadModule 0x220) with the DAL's own r9 world.
Also: HALTimeGetAlarm/SetAlarm (1 Hz RTC interrupt only while armed, due alarms raise the clock event); Sony shared
page initialised at display init in full ROM images; attribute 0x8009.

Fixes found on the way: OS code calls DAL entries with sp only 4-byte aligned (the trap entry now realigns; DAL code
is built for ARMv5T without LDRD/STRD); DAL C code is built with -ffixed-r9 so the OS r9 survives every DAL path
(an OS routine run through RALCallWithNewStack had received a DAL-internal r9).

Result (full ROM, 320×480 and 480×320): no fault. HALSetInitStage 0–4 all pass; UI, PACE, Sony libraries and
extensions load; three ARM applications receive their launch codes. Then the OS enters a **relaunch loop**: a 68k
application is started through PACE, erases its window, the Sony extension asks for layout 1 (refused, as the
reference does), the application exits and is started again (~3 per second). The reference reaches the same
refusal without looping; the first visible difference is earlier, in stage 3: the reference's Sony input-area code
sets the user-area size (attribute 0x8000) to 0 and then to 2, this DAL only sees the 0. Cause not yet found.

## 30. Comparison with the reference boot; DAL as a patchable module (stage g4p)

Method (local-only tooling, nothing committed): a DAL call log added to the local trace build of the emulator records
every entry to a DAL slot (slot, caller, r0–r3, r9, sp), its return value (r0/r1) and, for selected slots, the other
registers, the r9 world and the memory behind pointer arguments. The same log is taken from the reference DAL's run
and from this DAL's run on the same Sony ROM content; the two call sequences are aligned (noise from polling tasks
and DAL-internal calls removed) and the first divergence is examined. Only slot numbers, argument/return values and
call order are compared.

Findings and fixes:
1. The Sony input-area extension ('aext'/'SeSi') looks up the DAL itself with RALGetModuleInfo (code = the DAL image)
   and patches DAL entries 0x57–0x5A (pen) with RALPatchEntry. This DAL had no module header and a ROM-resident
   table, so the lookup failed and SeSi's 0x7FFE returned 0x050A (initialisation abandoned; the later layout/user-area
   sequence never happened). Now the DAL image starts with a module header (ID 0, revision 1, 0xD0 exports, 'rsrc' /
   'pdal' / 'boot' 19000, flags patchable|system, export table of branches to the slot entries), the DAL table
   ([r9-4]) is a RAM copy with its count word, and RALPatchEntry works on module 0. SeSi now initialises, sets the user
   area as in the reference, and I2CDriver loads.
2. HALScreenDefaultPalette returned false after the first HALScreenPalette because the standard-palette re-check was
   missing; the OS then took a different path. The check is now done on lookups against 256-entry tables, using the
   table the OS passed to HALScreenInit as the standard (the OS standard 8 bpp table is not built in; recorded).
3. Waits/delays while KALTaskSwitching(off) is in force no longer block (FreeRTOS forbids it; the reference keeps the
   caller running): they poll.

Result: no fault in either orientation; applications now draw (bitmaps, text, lines — bypassed). The relaunch loop
remains: after its first drawing the launched 68k application exits and is started again; in the reference the same
point leads into the event loop (HALPowerGetAutoOffEvtTime, HALPenRawToScreen, event-group waits). Next divergence
under investigation.

## 31. HostControl SWI, pen conversion — the relaunch loop resolved (stage g4q)

Method: the DAL call log of §30 was extended (local-only tool) with every 68k system trap (trap word, 68k pc, stack
arguments) and every return from 68k code, in the same sequence, and with a jump trace (every non-sequential pc
change) started at a chosen call. Addresses are normalised per module (code base and size) before comparison.

Findings:
- 68k traps of both runs match up to the first UI application. The reference then starts "Welcome" (appl/setp),
  this DAL started "Preferences" (appl/pref), which switches to the Digitizer panel; the shell restarted Preferences
  forever. DAL call sequences up to that point were identical, so the choice came from somewhere else.
- The jump trace from the last common call shows the first execution difference: the HostControl library (0xB7C
  bytes of code) executes `SWI 0x123457` with r0 = 0x101 (hostSelectorGetHostID). The reference DAL's SWI handler
  answers r0 = 0 (a real Palm OS device); this DAL sent every SWI to the FreeRTOS yield, so r0 came back as 0x101 —
  the OS took it for a non-device host and chose a different start-up path.
- Requirement (analyst spec, local-only): SWIs are told apart by the comment field (ARM: low 24 bits, Thumb: low 8
  bits). HostControl (0x123457 / 0xAC), selector in r0: 0x101, 0x102, 0x400, 0xC03, 0xC04 → 0; 0xC00 → 1; 0x103 →
  1 for the implemented selectors; anything else → 0xFFFF; only r0 changes. Angel semihosting (0xAB) 0x03/0x04
  write to the debug log. Other numbers are fatal. Implemented in `g3pre/start_rtos.S` (dispatch; SWI 0 stays the
  FreeRTOS yield) and `g4pre/hal_misc.c`.
- Next stop: HALPenRawToScreen (0x57, reached through the pen entries Sony SeSi patched). Pen conversion and
  calibration are implemented per the calibration model (identity by default, 16.16 slope + offset per axis);
  **not yet persisted** in the private preference the reference uses (recorded difference).
- The palette-standard re-check now runs only for lookups at indexed depths (HALScreenDefaultPalette now returns
  0, 1, 1, … exactly as in the reference run).
- Test images: image offsets 0x100–0x1FF are kept zero, because Boot reads the storage-header range of the ROM; in
  the stand-alone images that range was DAL code, and a code change made the Boot-only run fault there.

Result (full ROM, 320×480 and 480×320): no fault, no stop, one application start: the OS starts "Welcome" (the
first-boot setup application, as the reference does on a fresh RAM) and stays in its event loop. Per-slot call counts
now match the reference run (HALPowerGet/SetAutoOff*, HALPenRawToScreen, event-group waits, HALGetHwrWakeUp,
HALGetROMToken, HALSoundOpen …). The Launcher itself comes after Welcome has been completed by the user
(calibration taps etc.); drawing is still bypassed, so nothing is visible yet.

## 32. HALDraw for 16 bpp, key events, HALDelay rewrite — Welcome completed (stage g4r)
Target buffer check (before drawing): every HALDraw call carries a canvas (+0 clip rectangle, +8 draw state,
+0xC destination bitmap). Observed destinations: the on-screen window (bits = framebuffer 0x20100000, 320×320
area, 16 bpp RGB565 little-endian, rowBytes 640) and off-screen 16 bpp windows (e.g. the status bar, 320×30). On PV
the panel shows the framebuffer directly; HALScreenUpdate/lock calls have no copy step, so a draw into the
framebuffer is visible at once. Only 16 bpp, uncompressed destinations are drawn; anything else is logged.

Draw state as observed on this ROM (the analyst spec had it at other offsets): bytes +0 transfer mode, +1 pattern,
+2 underline mode, +3 font id, word +4 font pointer, +8..+0xF custom pattern rows, +0x10 fore/back/text index
bytes, +0x14/+0x18/+0x1C fore/back/text RGBColorType, +0x20 coordinate system, +0x22 flags. Reading the mode as a
32-bit word (font id in the top byte) had made all text with a non-zero font id invisible.

Implemented (`g4pre/hal_draw.c`, independent): Rectangle (fill/frame/rounded), Line, Pixels, GetPixel, Chars
(v1/v2 fonts, font maps with lead bytes, density scaling, underline), uncompressed Bitmap (integer scaling, colour
lookup, transparency, transfer modes). Key events (HALInput spec): masked key presses post {chr, 0, modifiers}
to callback 1, 100 ms auto-repeat. HALKeyResetDoubleTap (0x4E, called by the OS on every pen-down) returns 0.
HALDelay rewritten from its requirement (wait at least the requested time on the 1 ms timer, bounded).

Compressed bitmaps (measured): Welcome and the status bar use them (flags 0x0D at 8 bpp, 0x2D at 16 bpp; ~16
calls up to the Welcome screen). They are not drawn (logged): the Welcome illustration box and some status-bar
icons are missing. None of them gates progress through Welcome. Kept as a later, independent implementation from
the public format description.

Corrections found by comparing with reference screenshots: built-in and custom fill patterns are defined in
standard-density pixels (2×2 cells at density 144: the dotted gray field frames); the standard 8 bpp palette has
red changing every 18 indices and blue every 6 (the derived order had them swapped: blue icons came out red).

Result (full ROM, 320×480): with real taps the Welcome flow runs exactly as on the reference: intro, tap target
1 (top left), tap target 2 (bottom right), check target (centre), region/date/time, "setup complete", data entry,
"done". The OS then starts MediaLauncher (the VZ90 home application) and MediaLMovie. After completing Welcome the
MediaLauncher screen differs from the reference screenshot in 244 pixels, all inside one status-bar icon (a
compressed bitmap); everything else is pixel-identical. Settings persistence: after completing Welcome, a soft
reset — in the same run, and again after saving the emulator session and starting from it — does not run Welcome
again: both the reference and this DAL start Preferences on its Date & Time panel after the reset (identical
screens), and the Home silk button then opens MediaLauncher. Landscape (480×320): boots to Welcome, no fault.
Pen: down and up samples reach the OS callback; HALKeyResetDoubleTap is called on every pen-down. Keys: hardware
key events are generated as specified; not yet exercised against the reference in a scripted run.

Open items found in g4r runs (not hidden): (1) a scripted hard-key run from MediaLauncher (hard1, down, hard4)
leaves MediaLauncher unchanged after hard1/down on both DALs; after hard4 this DAL starts CLIE_Org_DateBook, which
draws into 8 bpp off-screen windows (not drawn yet), then CLIE_Org_Address, where the emulator reports a 68k write to
address 0 followed by an undefined-instruction exception. The reference DAL also stops in that run (data abort,
fault screen). Cause not yet analysed; outside the Welcome → Launcher goal, recorded as the next investigation.
(2) Indexed (8 bpp) destinations and compressed bitmap sources are not drawn.

## 33. Compressed bitmaps, indexed destinations, screen comparison (stage g4s)
Measured first (debug dump of every compressed source): ROM bitmaps use PackBits (type 2, 8 bpp and 16 bpp, one
row of output per row of input; 16 bpp units are whole pixels) and ScanLine (type 0, e.g. 16 bpp 54×54 icons and
1 bpp 42×105 images); the compression type is byte +0x0D of the descriptor, the bits pointer points at the data
just after the size prefix, +0x20 holds the compressed size. RLE (type 1) was not seen. The OS also creates
compressed 16 bpp *destinations*: the save-behind copy of the screen under a modal dialog (date picker) is drawn
INTO a compressed bitmap and drawn back when the dialog closes; without support the screen came back white.

Implementation:
- `g4pre/bmp_codec.c`: expansion (ScanLine, RLE, PackBits 8/16) and per-row encoding, written from the public
  format description; `tools/test_bmp_codec.py` checks both directions against independent Python encoders /
  decoders on 300 random bitmaps per type (1800 cases, 0 failures; an unknown type is rejected).
- Compressed source: expanded into a temporary plain copy (FreeRTOS heap), drawn, freed. Version < 2 means ScanLine.
- Compressed destination (spec HALDrawCompressed, analyst): a paint-mode copy from (0,0) at equal depth and density
  is encoded into the destination's own buffer and the size prefix (header size − 0x24 bytes, little-endian,
  counting itself) is written; otherwise the descriptor's bits pointer is moved back onto the prefix and the
  compressed flag cleared, and drawing proceeds as for a plain bitmap. Only Bitmap accepts compressed
  destinations; the other primitives refuse them (logged) instead of writing over encoded data.
- Indexed destinations (1/2/4/8 bpp): pixel access by depth and pixelFormat (3 = first pixel in the low bits);
  fore/back/text colours are the index bytes +0x10/+0x11/+0x12; bitmap sources are copied index-for-index at equal
  depth, otherwise translated through the nearest colour of the destination table (CPO when it has none); 16 bpp
  sources map per pixel to the nearest index (no ordered dithering yet). GetPixel handles indexed bitmaps.
- Same source and destination pixels (scrolling) are read from a snapshot.
- Fill patterns: only the custom pattern is enlarged with density (2×2 cells at 144); built-in gray patterns use
  one bit per pixel (spec; the frames seen on screen use the custom pattern).

Screen comparison (exact, every pixel, reference vs this DAL, same ROM and inputs; the clock area excluded):
Welcome intro, the three tap targets, region/date/time, date picker, dialog closed again, "setup complete", data
entry, MediaLauncher. The Bluetooth status icon and the Welcome illustration (compressed bitmaps) now match. The
only remaining differences are UI greys: the reference shows BB BB BB where this DAL shows CC CC CC (text) and
22 22 22 where this DAL shows 33 33 33 (date picker background). Cause, verified: the draw state the OS builds
holds text/fore index 0xDE (grey BB) in the reference run and 0x19 (cube CC) in this run — the OS chose different
palette indexes because HALDrawFindIndexes answered differently: the reference uses an approximate search over
candidate lists for the standard palette that sends cube greys to grey-ramp entries; this DAL does the exact
search the spec allows as a stand-in. A debug-only test mapping CC→index 222 and 33→216 made 10 of the 11 screens
pixel-identical (the date picker keeps a 33/22 difference). Not adopted: copying that behaviour would need the
reference's candidate lists; kept as a recorded difference pending a decision.
(Correction to §32: the "244 pixels, Bluetooth icon only" figure used a tolerance of 24 per channel; with exact
comparison the grey difference above is also present.)

Other results: hard4 Organizer stop classified as common — both DALs show the emulator's "memory fault in PACE
writing at 0x00000000" followed by a data abort at the same code (PCs normalised by module); this DAL's report
"vector 4" is the data abort (not an undefined instruction as written in §32). HALKeyResetDoubleTap: the reference
returns 0 on every call (trace), as this DAL does. Settings survive a soft reset (Welcome not repeated; both DALs
open Preferences Date & Time, then Home opens MediaLauncher). Full ROM 320×480 and 480×320: no fault.

## 34. Verification pass: call path, CPU state, remaining bypasses (stage g4t)
A. Call path to the Launcher (trace of both DALs, same ROM, same taps, Welcome completed, DAL-internal calls
separated by caller address): the OS calls the same 88 slots in both runs; the slots seen only in the reference
(0x0E, 0x5A, 0x65, 0x8F and one extra 0x8D/0x9C/0xA3/0x90) are calls the reference DAL makes to itself (its own
event task and mailbox, screen refresh notifications). Ordered OS call sequence without time-driven slots: 1446
vs 1443 entries, 99.6 % identical. Differences: (1) at init stage 2 the reference loads one extra module for its
optional FtrGet logging patch (BootPatches 1.2, not adopted); (2) the DAL module itself has a different size;
(3) two neighbouring module loads swap order once (timing). Draw-call counts differ only with elapsed time
(animations).
B. CPU state at 15 checkpoints (HALSetInitStage, HALScreenInit, HALEventRegisterCallBack, KALTaskCreate/Start):
same r9 world classes (DAL/Boot/UI tables, globals) at every point; differences: CPSR F bit (this DAL keeps FIQ
masked; PV has no FIQ source), callee-saved registers inherited from DAL frames (the OS saves them), unused
argument registers, task ID numbering (the reference allocates IDs for its own tasks first).
C. Remaining non-functional entries:
- implemented in g4t: HALDisplayDrawBootScreen (was a bypass) — the boot image is drawn per spec
  HALDisplayDrawBootScreen; the first boot frame is pixel-identical to the reference; the four Boot speed
  replacements (MemMove 0x156, MemSet 0x16C, MemCmp 0x130, StrCompareAscii 0x1E1, BootPatches 1.1).
- normal no-ops (no PV hardware): sound, attention/LED/vibrator, dock status, trace output, HALHandleReset.
- recorded differences: HALDrawFindIndexes exact search (§33); FtrGet logging patch and the ErrDisplay fatal-alert
  patch (Boot 0x09F) not installed; KAL *GetInfo and KALTaskExit answer 0xF009 (never called up to the Launcher);
  KALTaskGetCurrentID numbering rule; pen calibration not kept in the private record 'psys' 0x9900; 16→8 bpp
  dithering; RmcExt/Pa1Lib and halting SonyDAL entries stop when called (not called so far).
- not implemented, stop when called: 29 table entries (none is called by the OS in the reference run up to the
  Launcher).
Boot time: the debug log on the PV UART costs about 9 s of emulated boot; `make DAL_LOG=0` builds a quiet image.
Quiet image: Palm boot image to the cleared screen in 4.0 s (reference 2.0 s); log image about 12.5 s.

## 35. Bounds checks, colour lookup measured on the reference, indexed self-test, boot time (stage g4u)
Compressed data: `bmp_unpack` now takes the encoded length and returns BMP_ERR_SHORT when the input ends before
the requested rows are complete and BMP_ERR_BAD for an RLE count of 0; it never reads past the length and never
writes past the requested rows. The length comes from the size prefix in front of the bits pointer (it counts
itself; measured: ROM bitmaps store it big-endian, the save-behind bitmaps this DAL compresses store it
little-endian, and +0x20 is reliable only with 4-byte prefixes, so it is not used). Only the rows a draw needs
are expanded. `tools/test_bmp_codec.py` adds truncated and random-byte inputs under ASan/UBSan (3600 cases, 0
failures). Compressed destinations: the OS buffer was measured through its memory-manager chunk header: header
+ rowBytes × height bytes, i.e. rowBytes × height − prefix bytes after the bits pointer, which is the capacity
used; the only compressed destination seen (save-behind) is a full 320×320 copy, so partial copies did not occur.
Boot image parser: every header, colour table, link and data range is checked to lie in ROM or RAM, depths and
rowBytes against the width, colour-table counts ≤ 256 (missing entries use the standard palette), the encoded
length against the size prefix. The size test that chooses the scale keeps the reference rule (either dimension).

HALDrawFindIndexes, palette state "standard": measured black-box on the reference DAL (its entry called through
the emulator's debugger stub with 107,640 colour records; no reference code read; the first instructions of the
entry were looked at only to see that it is ARM code). Result: after the "already equal" and "last entry" checks
each channel is rounded to the nearest colour-cube level ((v + 0x19) / 0x33) and the cube colour's standard index
is returned, except for 24 cube cells that return a grey-ramp entry or one of the five extra colours (table in
`hal_display.c`). This rule reproduces all 107,640 measured answers; the same measurement run against this DAL
gives identical answers for every record. With it, all 11 compared screens of the Welcome → MediaLauncher run
(including the date picker) are pixel-identical to the reference. When entries 230..254 differ from the standard
table the exhaustive search is used (not measured).

Indexed destinations: `g4pre/selftest_draw.c` (images built with `DAL_SELFTEST=1`) draws on the target into
1/4/8 bpp off-screen bitmaps — filled and clipped rectangles, gray pattern, invert, diagonal line, both 4 bpp
pixel orders, 1 bpp, 8→8 copy with and without transparency, 1→8 with fore/back, 16→8 colour lookup, GetPixel —
and checks 330 pixels including the row padding; all pass, and a deliberately broken pixel order is caught.

Boot time, same criterion for both DALs (OS-visible uptime from HALTimeGetSystemTime in a traced run, quiet image):
first HALEventRegisterCallBack / stage 3 / stage 4 at 445 / 516 / 2759 ms (reference) versus, for this DAL,
881 / 988 / 4866 ms before g4u and 405 / 471 / 2647 ms after. Causes found with a PC-sample profile: the word-by-
word MemSet replacement (77 % of the first second; now 8 words per step), per-call trace bookkeeping and the debug
line buffer still active in quiet images (now compiled out with DAL_LOG=0; `src/*.c` now gets the same defines),
and per-pixel calls in 16 bpp fills and copies (now direct loops for solid paint and plain copies).

Open item found in the g4u regression: in one of seven 480×320 runs (Welcome idle, about 80 s after boot) the Sony
extension module 'aext'/'SeFa' called DAL table entry 0 (DALGetGlobalsPtr, r0..r3 = 1, 0x7E, 0x11A, a ROM pointer)
and the run stopped there (this DAL stops on entries it does not implement). The analyst spec DALGetGlobalsPtr says
the reference halts with a fatal error on that entry on this ROM, so a reference run reaching the same call would
also stop; the reference was not seen to reach it (270 s landscape run). Six further runs of this DAL (100–210 s)
did not reach it either. Trigger not identified; recorded, not worked around.

Comparison with Sony's own DAL (PEG-NX70V, Palm OS 5.0; the user's ROM, run in the emulator's NX70V mode; local
measurement only): its HALDrawFindIndexes (table entry 0x16) was called black-box with the same 107,640 records.
It returns a true nearest colour (smallest squared R,G,B distance) for every record; with "lowest index on ties"
107,601 answers match exactly, the other 39 are exact ties resolved differently (e.g. duplicate blacks go to 255).
It agrees with the reference VZ90 DAL in 93,353 records and differs in 14,287: the NX70V DAL does not use the
reference's cube-rounding lookup (CC→BB, 33→22, FFCCCC→C0C0C0 …). Whether Sony's VZ90 (5.2) DAL does is not known. The VZ90's own Sony DAL (5.2) cannot be
run on PV, so which behaviour the real VZ90 shows is not measured. This DAL currently follows the reference VZ90
DAL (g4u decision); the exact search used before g4u matches the NX70V behaviour except for tie handling.

## 36. Colour lookup modes (stage g4v)
`DAL_COLOR_MODE=REFERENCE` (default) and `DAL_COLOR_MODE=SONY_NX70V` select the HALDrawFindIndexes behaviour; nothing
else differs between the two builds. The second mode is named after the device it was measured on, not after the
VZ90, whose own Sony DAL could not be measured.

Further black-box measurements (8,568 records each, with an explicit table argument):
| table | reference VZ90 | Sony NX70V |
|---|---|---|
| 16 greys | intensity rule (same answers on both) | same as reference |
| 16 random colours | exhaustive, 8-bit wrapped differences | exhaustive, true distance |
| standard with entries 230..254 changed | cube lookup; the changed entries are ignored | answered from the standard palette; the changed entries are ignored |
The reference result corrects the earlier assumption (§35) that changed entries 230..254 force the exhaustive
search; this DAL's REFERENCE mode now ignores them as well.

SONY_NX70V mode: all-grey tables use the intensity rule; otherwise the true squared distance, starting from the last
entry and replacing it only by a strictly closer entry from index 0 upward; a table whose entries 0..229 and 255 are
standard is answered from the standard palette.

Results of the same measurements run against this DAL (on target, through the debugger stub):
| inputs | REFERENCE mode = reference | SONY_NX70V mode = NX70V |
|---|---|---|
| 107,640, standard palette | 107,640 / 107,640 | 107,636 / 107,640 |
| 8,568, 16 greys | 8,568 / 8,568 | 8,568 / 8,568 |
| 8,568, 16 random colours | 8,568 / 8,568 | 8,568 / 8,568 |
| 8,568, changed tail | 8,568 / 8,568 | 8,564 / 8,568 |
The four NX70V records not reproduced are exact ties (two candidates at the same distance): inputs 223311 and 332211
(NX70V answers 216, this mode 196), C6C6C6 (225 vs 25) and 078396 (229 vs 104). No simple tie rule (index order,
channel order, absolute or 565-quantised distances) explains all ties; they are left as recorded differences.

## 37. Build options, Color Schemes, slot inventory, measured return codes (stage g4w)
A. Build. `build/g4/config.stamp` holds the g4 compiler command line and changes only with it; every g4 object
depends on it. REFERENCE -> SONY_NX70V -> REFERENCE without `make clean`: each image byte-identical to the clean
build of that mode; an unchanged rebuild compiles nothing. Hash rules used from here on: clean and incremental builds
of the same source and options must be identical; a logging-only change must leave the quiet image identical; a
functional change is judged by behaviour against the reference within the observed inputs, not by old binaries.
B. Stop diagnostics (logging images and the g1/g2e probes, not in quiet images): on a stop, the text that r0..r3 and
48 stack words point to is printed, reading only inside the ROM window or the RAM present, a string only when it ends
with a NUL within 72 bytes. Checked negatively on g2e (no abort) and positively by a self-test (3 valid texts
printed, 4 invalid pointers rejected). A corrupted frame pointer at the stop is not covered.
C. Color Schemes (Palm OS 5.2 on the VZ90 ROM). There is no separate Colors panel; the theme is chosen in
Preferences > General > テーマカラー (default ソニー); 32 themes. Each theme was selected on both DALs from
corresponding saved sessions and six screens were compared (General panel, theme list, Date & Time panel,
Launcher, after a soft reset, Launcher after the reset): 190 of 192 screens pixel-identical, the other two differ
only in the clock digits. The theme survives a soft reset on both. The OS colour lookup goes through slot 0xBF
(0x16 is the same function and was not called; an earlier "no FindIndexes calls" came from logging 0x16 only and
is withdrawn): 9,006 calls on each DAL; grouped by input RGB and colour-table checksum (the checksum only pairs the
calls), 119 input colours, no case of the same input giving a different index; per-theme counts differ by up to 2.
No OS DisplaySetPalette call during theme changes. The four NX70V tie colours (§36) did not occur in these theme
changes (no claim for other applications). Future theme testing: all 32 only for colour-related changes, otherwise
3-5 representative themes.
D. Slot inventory (local table, 241 slots 0x00-0xF0): reference behaviour (analyst classification), this DAL's
handling (from the dispatch code), observed calls, basis, one verification status per slot and its evidence.
Status: input/output match 2 (0x16/0xBF, one function), return values match 71, call path match 10, call only 8,
difference 1, unverified 149. Not implemented (stop when called): 62 = 32 OEMReserved + 30; the count 29 in §34
could not be reproduced, the same extraction on the g4t source gives 30. Only time/tick results (0x5B, 0x73-0x75)
and void entries (r0 leftovers: 0x5D, 0x6D, 0xC1-0xC4; 0x6A keeps r0 on the reference) are excluded from return
comparison.
E. Return codes changed after measurement (same arguments on both DALs): HALSoundPlay with a duration returns
0x080A (spec said 0x020A); the KAL timeout code is 0x0501 (Palm OS sysErrTimeout; the spec said 0x0504, which is
sysErrNoFreeRAM), measured for KALEventGroupWait timeouts, and a completed KALTaskDelay returns 0x0501 (measured
288 of 288). Other waits share the constant; their timeouts were not observed. After the change the four
representative themes (31, 00, 05, 09) give the same return values for 0x71, 0x8B and 0x9D on both DALs; screens
differ only in clock digits.
F. Remaining difference: HALPenCalibrate during boot after a reset (stages 0-2, after HALPenResetCalibration) gets
the same arguments on both DALs (dig (10,10)/(150,150), scr the same); the reference returns 0xFFFF, this DAL 0. A
difference in the reference's state at that moment is possible but not established. Raw-to-screen conversion was
identical for the 7 raw points observed (before and after the reset). The reference keeps a 34-byte 'psys' 0x9900
record in Unsaved Preferences, this DAL none; its contents read as data look like identity maps (format private).
G. HALScreenSendUpdateArea (0x6A) call counts: the OS calls it from a loop that waits on an event group (0x8B). On
the reference the wait often returns "signalled" thousands of times in a row, giving bursts of 0x6A; before the
KAL fix this DAL showed no bursts, after it bursts also appear (correlation only; source of the signals not
established). HALScreenDrawNotify (0x65) is never called by the OS, so update areas could not be compared.
H. Power. No auto-off within 3.5 minutes on either DAL (General panel, auto-off 2 min): no HALDisplaySleep,
HALPowerSleepReady, Doze or Wake call. The power key makes 68k code (PACE) read address 4 on both DALs; the
reference then shows its own fault screen, this DAL takes the data abort, logs it and stops.
I. Landscape fatal (§36 open item): 'SeFa' is Sony's NewFatalAlert; its slot-0 call drew the RESET button of the
fatal screen, so a fatal error came first. The reference installs its ErrDisplay patch (Boot 0x09F), this DAL does
not, so such an error reaches Sony's screen here; whether the same error occurs on the reference is not
established. Not reproduced in 8 further landscape runs of 150 s (logging build c8ec1f2; 1 in 19 runs overall).
J. Regression after the g4w changes: g1, g2e, g3pre and the Boot-only image unchanged; full ROM portrait and
landscape: no fault, no stop; Welcome walk: screens 1-10 pixel-identical in two runs; screen 0 (the stylus
animation) differs by animation frame (420 and 2780 px), as it also did in earlier stages (g4u: 1888, 1332 px), so
it is timing, not a regression; self-test image passes (330 drawing checks, stop diagnostics). Clean and
incremental builds identical for both colour modes.
K. HALPenCalibrate resolved (supersedes F). Black-box calls through the debugger stub on both DALs (inputs and
outputs only): after boot, every case gives the same result on both (same arguments -> 0; new targets -> the
same new maps; equal raw points -> 0xFFFF; HALPenResetCalibration -> identity). During the boot after a soft
reset, at the OS's call (after HALSetInitStage(2)), density (attribute 0x07 = 144) and both conversion
directions are identity on both DALs, yet the reference returns 0xFFFF for any arguments and leaves the maps
unchanged; right after HALSetInitStage(3) it accepts and produces the same maps as this DAL. This DAL now refuses
calibration before init stage 3 the same way (0xFFFF, maps unchanged); measured again: refusal at the boot call,
acceptance after stage 3 with identical maps, and the theme-31 run returns 0xFFFF like the reference. The analyst
spec did not list this condition. The reference's internal condition is not known: only the refusal after
HALSetInitStage(2) and the acceptance after HALSetInitStage(3) were measured (it may be a readiness flag set at
stage 3 rather than a stage comparison). Boundary check on both DALs with identical results: after stage 2 a
repeated call is refused as well and HALPenResetCalibration returns 0 with identity maps; after stage 3 a repeated
call is accepted. Calls after stage 1 could not be made (the entry did not return under the debugger); stage 4
was not reached in the time limit, the state after boot is covered by the tests above. The observed 'psys' 0x9900
record difference (F) was not required to explain or reproduce the HALPenCalibrate return-value discrepancy.
Slot inventory after K: return values match 72, difference 0 (other counts as in D). "Difference 0" is for the
observed calls only; 149 slots are unverified and "return values match" does not cover all arguments, states or
side effects.
L. Re-analysis of the slots that were called but not compared (no new scenarios; existing boot traces plus one
theme-31 run without the call-log skip list on both DALs). 0x0E, 0x65, 0x8F and 0xA7 are called only by the
reference DAL itself; the OS calls them on neither DAL. 0x51, 0x85, 0x9F and 0xA0 return the same value sets
(output structures not compared). Time-valued entries compared by relation instead of value: after the reset
0x74 is monotonic at about 987/988 ticks per 0x73 second on the two DALs, and 0x5B returns the last 0x5D value
on both (with the same two initial zeros after the reset). HALMemorySetStorageAreaProtectionState (0x53) has no
return value in the spec; r0 was the argument here and 0 on the reference, so it is now 0 (measured again: 0 in
114,751 calls). Unverified slots (153) are the ones the OS never called in these runs, except slot 0 on the fatal
path. Slot status now: input/output 2, return values 80, call only 6, unverified 153.

## 38a. Slots called in the web tests and their comparison (2026-11-01)

Census (emulator-side counter at the DAL trap entry, official bundle 5fdf23fe): fresh boot with Welcome steps and
one round of the web test steps T0-T9 call 94 distinct slots; none of the 62 stop-when-called slots is called.
Seven called slots were unverified; compared with the reference DAL:
- 0x27 HALFlpDivFloat, 0x32 HALFlpFloatToUInt32, 0x35 HALFlpInt32ToFloat: black-box, both DALs' functions executed
  outside the emulator on the same inputs (12,784 / 8,028 / 6,011 cases incl. zeros, denormals, infinities, NaNs,
  out-of-range and boundary values): bit-identical results.
- 0x3B HALFlpMulFloat: 12,784 cases, 20 differ, all with both operands NaN: the reference returns the first NaN, this
  DAL the second (both results are NaN; sign/payload differ). Not changed.
- 0x6C HALScreenUpdateBitmap (Datebook launch, native): one OS call with depth 8 on both, returns 0 on both.
- 0xC4 HALDraw_Pixels (Datebook launch, native): 1,430 calls on both with the same non-pointer arguments (1, 1);
  void (r0 leftovers not compared); the screens differ only in clock digits.
- 0xA7 KALTaskWake: called only by this DAL itself (4,869 calls at boot, 31,990 in T0-T9; no OS caller), as on the
  reference. Remains unverified.
Slot status now: input/output 5, return values 81, call path 1, call only 6, difference 1, unverified 147 (before:
153 after §37 L; six slots changed status here, 0xA7 did not; the README's earlier 149 was the §37 D count and had not
been updated after L). All comparisons are with the rePalm reference DAL, not with Sony's VZ90 DAL; the 0x3B result
compares returned bits only (no exception flags). No DAL behaviour was changed.

## 38. DAL bundle for an emulator-side ROM loader

`make DAL_LOG=0 build/vz90-dal.bundle` packs the full-ROM DAL into one file (`tools/mkbundle.py`) for a loader that
rebuilds a PV image from an original VZ90 ROM image. The DAL's own resources are `boot` (this code), `amdd` and `amdi`
(`tools/mkdalres.py`, §20), all with resource ID 19000 (0x4A38).

Link address. `boot` runs in place as the first ROM heap chunk, at 0x10000230. The script checks this from the ELF at
build time: entry point and `_start` at 0x10000230, `.text` starting there, no loaded section below it in the ROM
window, and the raw image covering exactly 0x10000230 up to the end of the `.data` load image (`.ARM.exidx` may lie
between `.text` and `.data`). Only a binary that passes is hashed into the bundle. A loader does not derive the link
address from the binary; it checks the recorded value, the hashes and the build ID.

Format 1, integers little-endian, header 0x100 bytes:

| Offset | Size | Field |
|---|---|---|
| 0x00 | 8 | magic `VZ90DAL\0` |
| 0x08 | 4 | format version (1) |
| 0x0C | 4 | header size (0x100) |
| 0x10 | 4 | link address of `boot` (0x10000230) |
| 0x14 | 4 | resource count (3) |
| 0x18 | 20 | build ID: git commit, SHA-1 (zero when unknown) |
| 0x2C | 4 | flags: bit 0 uncommitted changes, bit 1 `DAL_LOG` build, bit 2 `DAL_SELFTEST` build |
| 0x30 | 48 | resource table, 3 × 16 bytes in the order boot, amdd, amdi: type (4 ASCII), ID (2), reserved (2, zero), offset (4), length (4) |
| 0x60 | 96 | SHA-256 of each resource, same order |
| 0xC0 | 32 | SHA-256 of the whole bundle, computed with these 32 bytes set to zero |
| 0xE0 | 32 | reserved, zero |

Resources follow the header in table order, each on a 4-byte boundary with at most 3 zero bytes before it, and the
file ends with the last resource. A loader rejects: wrong magic, version, header size, count or link address; non-zero
reserved bytes; a type, ID or reserved field other than above; a resource that starts before the end of the previous
one (or the header), leaves a gap of more than 3 bytes, is empty or runs past the file; trailing bytes; any hash
mismatch. The hashes show the bundle is intact, not who built it.
