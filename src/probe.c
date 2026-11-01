/* pv-vz90-dal stage g2a probe: enter a module image placed in ROM and report the first interface it needs.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include "pv_hw.h"

void dbg_puts(const char *s);
void dbg_hex(uint32_t v);
uint32_t probe_call_module(uint32_t entry, uint32_t r9, uint32_t a0, uint32_t a1, uint32_t a3, uint32_t cpsr);
/* CPSR at the observed reference entries (trace, stage g2c): only the condition flags differ */
#define CPSR_ENTRY_7FFE		0x6000001fu
#define CPSR_ENTRY_EXPORT0	0x4000001fu
/* observed at the reference DAL's Boot entry call (stage g2a, GDB): r0 = 0x7ffe, r1 = 0, r2 = 0, r3 = entry, System mode,
 * [r9-12] = 0, [r9-8] / [r9-4] = tables, [r9] = globals */
#define PROBE_ENTRY_CMD	0x7ffe
extern const uint32_t probe_tab1[], probe_tab2[], probe_tab3[];

/* Boot module location: test images (tools/mkimg.py) carry it at ROM + 0x20000 with a 'MODL' marker; in a full ROM
 * image the storage header (ROM + 0x100, signature 0xFEEDFACE) holds the address of Boot 'boot' 10003 at +0xF8 and the
 * size is taken from the heap chunk header in front of that data */
static uint32_t gModAddr, gModSize;
#define PROBE_MODULE_ADDR	gModAddr

static int locate_module(void)
{
	const uint32_t *marker = (const uint32_t *)(PV_ROM_BASE + 0x18000u + 0x100u);
	const uint32_t *stor = (const uint32_t *)(PV_ROM_BASE + 0x100u);

	if (marker[0] == 0x4c444f4du && marker[1] == 0x20000u && marker[2]) {
		gModAddr = PV_ROM_BASE + 0x20000u;
		gModSize = marker[2];
		return 0;
	}
	if (stor[0] == 0xfeedfaceu && stor[0xf8 / 4] > PV_ROM_BASE + 0x200u && !(stor[0xf8 / 4] & 3)) {
		uint32_t w0 = ((const uint32_t *)stor[0xf8 / 4])[-2];

		gModAddr = stor[0xf8 / 4];
		gModSize = (w0 & 0x00ffffffu) - 8 - ((w0 >> 24) & 0xf);
		dbg_puts("g4g: full ROM image, Boot 10003 at ");
		dbg_hex(gModAddr);
		dbg_puts(" size ");
		dbg_hex(gModSize);
		dbg_puts("\n");
		return 0;
	}
	return -1;
}

static uint32_t gProbeGlobals[1024];					/* block that [r9] points to: per-module globals pointer table */
/* stage g2d: module globals area with the 16-byte prefix observed in the reference run (g2c, docs/INTERFACE.md §2).
 * The module's globals pointer is &gProbeModCtx.globals[0]; prefix[1..3] = globals-12/-8/-4. Observed for Boot only. */
struct ProbeModuleContext {
	uint32_t prefix[4];
	uint32_t globals[0x1000 / 4];
};
static struct ProbeModuleContext gProbeModCtx;
#define gProbeModuleGlobals	(gProbeModCtx.globals)
static uint32_t gProbeGlobalsInit0;					/* value preset in globals+0 (excluded from the write report) */
static uint32_t gProbeR9Block[4 + 16];
/* g4p: the DAL dispatch table ([r9-4]) lives in RAM so that RALPatchEntry can patch DAL entries (Sony SilkMgr patches
 * the pen entries); word 0 = entry count */
#define DAL_ENTRIES	0xd0u
static uint32_t gDalTabStore[1 + 256];
#define gDalTab	(gDalTabStore + 1)					/* r9 = &gProbeR9Block[4]: [-4]/[-8]/[-12] = tables, [0] = globals */

/* stage g2e diagnostic replay rule (the only one): [r9-4] slot 0x6d with r0 == 0. In the reference run this call
 * (stage 0) executed only debug-print code: no memory writes outside the stack, no PV MMIO access, r0..r2 = 0 on
 * return (observation window, docs/INTERFACE.md §7). Replayed calls are not an implementation of the function. */
#define REPLAY_ID_6D	((1u << 12) | 0x6du)
static uint32_t gReplayCount;

/* sp at the module entry of the current call (System stack top, or the task's sp in probe_call_in_task) */
uint32_t gProbeEntrySp;
uint32_t gProbeTrapR9;		/* r9 of the module that made the current DAL call */

/* stage g4-pre: implemented slots (weak default: none). Returns 1 when the call was handled; the handler writes
 * the return value to args[1] (r0) and may change args[2..4] (r1..r3). */
/* g4k: Boot's data template (weak default: none; g4pre/ral.c unpacks it in a full ROM image) */
__attribute__((weak)) void probe_boot_template(uint32_t r9, uint32_t areaEnd)
{
	(void)r9; (void)areaEnd;
}

__attribute__((weak)) int probe_impl(uint32_t id, uint32_t *args, uint32_t r9)
{
	(void)id; (void)args; (void)r9;
	return 0;
}

/* g4o: quiet trace (set by the g4 image): the full CALL lines are printed only for the first call of each
 * (slot, caller) pair, plus a summary line every 50000 calls; the default (g1/g2 probe images) prints every call */
int gProbeQuiet __attribute__((weak));
__attribute__((weak)) void probe_mute(int on) { (void)on; }	/* g4 image: drops routine handler log lines */
#define QUIET_PAIRS	4096u
static uint32_t gPairs[QUIET_PAIRS], gCalls, gDistinct;
static uint8_t gSlotSeen[0x1000 / 8];

static int first_pair(uint32_t id, uint32_t lr)
{
	uint32_t key = (lr ^ (id << 20) ^ id) | 1u, h = (key * 2654435761u) >> 20, n;

	if (!(gSlotSeen[(id & 0xfff) >> 3] & (1u << (id & 7)))) {
		gSlotSeen[(id & 0xfff) >> 3] |= (uint8_t)(1u << (id & 7));
		gDistinct++;
	}
	for (n = 0; n < QUIET_PAIRS; n++, h = (h + 1) & (QUIET_PAIRS - 1)) {
		if (gPairs[h] == key)
			return 0;
		if (!gPairs[h]) {
			gPairs[h] = key;
			return 1;
		}
	}
	return 0;
}

/* Stop diagnostics are excluded only from quiet images (DAL_LOG=0). Included in the g1/g2e probes (DAL_LOG
 * undefined) and in logging DAL images. */
#if !defined(DAL_LOG) || DAL_LOG
/* g4w: diagnostics for a stop (not in quiet images, see above). Prints the text that r0..r3 and the first 48 words of the
 * caller's stack point to, when such a word points into ROM or RAM at a printable NUL-terminated string. Used to
 * identify what the OS was doing when an unimplemented entry was called (for example a fatal-error screen). */
/* end of the readable region holding address a (ROM window or the RAM actually present), 0 if a is in neither */
static uint32_t region_end(uint32_t a)
{
	uint32_t ramEnd = PV_RAM_BASE + PV_SYSCTL_RAMSIZE;

	if (a >= PV_ROM_BASE && a < PV_ROM_BASE + 0x02000000u)
		return PV_ROM_BASE + 0x02000000u;	/* 32 MiB ROM window, mapped as a whole on PV */
	if (a >= PV_RAM_BASE && a < ramEnd)
		return ramEnd;
	return 0;
}

static int printable_at(uint32_t a)
{
	const char *t = (const char *)a;
	uint32_t n, end = region_end(a);

	if (!end)
		return 0;
	for (n = 0; n < 72 && a + n < end && t[n]; n++)	/* never reads past the end of the region */
		if (t[n] < 0x20 || t[n] > 0x7e)
			return 0;
	return n >= 3 && a + n < end && !t[n];		/* NUL-terminated inside the region and inside 72 bytes */
}

void probe_stop_context(const uint32_t *args)
{
	const uint32_t *sp = &args[6];
	uint32_t k;

	for (k = 0; k < 4 + 48; k++) {
		uint32_t v;

		if (k >= 4) {
			uint32_t addr = (uint32_t)&sp[k - 4], end = region_end(addr);

			if (!end || end - addr < sizeof(uint32_t))
				break;			/* the stack word must lie wholly inside ROM or RAM */
		}
		v = k < 4 ? args[1 + k] : sp[k - 4];

		if (!printable_at(v))
			continue;
		dbg_puts(k < 4 ? "g4w:   text at r" : "g4w:   text at [sp+");
		dbg_hex(k < 4 ? k : 4 * (k - 4));
		dbg_puts(k < 4 ? " " : "] ");
		dbg_hex(v);
		dbg_puts(" \"");
		dbg_puts((const char *)v);
		dbg_puts("\"\n");
	}
}

#endif

uint32_t probe_trap(uint32_t id, uint32_t lr, uint32_t *args, uint32_t r9)
{
	int show = 1;

#if defined(DAL_LOG) && !DAL_LOG
	(void)lr;				/* quiet image (g4u): no call trace, no per-call bookkeeping */
	if (probe_impl(id, args, r9))
		return 1;
	show = 0;
	goto stop;
#endif
	if (gProbeQuiet) {
		show = first_pair(id, lr);
		if (!(++gCalls % 50000u)) {
			dbg_puts("g4o: summary: calls ");
			dbg_hex(gCalls);
			dbg_puts(" distinct slots ");
			dbg_hex(gDistinct);
			dbg_puts("\n");
		}
	}
	if (!show)
		goto handle;
	dbg_puts("g2d: CALL table ");
	dbg_hex(id >> 12);
	dbg_puts(" slot ");
	dbg_hex(id & 0xfff);
	dbg_puts(" (offset ");
	dbg_hex((id & 0xfff) * 4);
	dbg_puts(") from ");
	dbg_hex(lr);
	dbg_puts(" r0..r3 ");
	dbg_hex(args[1]); dbg_puts(" "); dbg_hex(args[2]); dbg_puts(" ");
	dbg_hex(args[3]); dbg_puts(" "); dbg_hex(args[4]); dbg_puts("\n");
	dbg_puts("g2d:   r9 ");
	dbg_hex(r9);
	dbg_puts(" sp ");
	dbg_hex((uint32_t)&args[6]);
	dbg_puts(" cpsr ");
	dbg_hex(args[0]);
	dbg_puts(" [r9-12..r9] ");
	dbg_hex(((const uint32_t *)r9)[-3]); dbg_puts(" ");
	dbg_hex(((const uint32_t *)r9)[-2]); dbg_puts(" ");
	dbg_hex(((const uint32_t *)r9)[-1]); dbg_puts(" ");
	dbg_hex(((const uint32_t *)r9)[0]);
	dbg_puts(" depth ");
	dbg_hex(gProbeEntrySp - (uint32_t)&args[6]);
	dbg_puts("\n");
handle:
	probe_mute(!show);
	gProbeTrapR9 = r9;			/* diagnostics only: several tasks may be inside the DAL at once */
	if (probe_impl(id, args, r9)) {	/* caller's r9 (g4j: OS callbacks, g4m: task create, timers) */
		probe_mute(0);
		if (show) {
			dbg_puts("g4pre:   implemented, returning r0 ");
			dbg_hex(args[1]);
			dbg_puts("\n");
		}
		return 1;
	}
	probe_mute(0);
	if (!show) {				/* stopping: show the call that stops */
		dbg_puts("g2d: CALL table ");
		dbg_hex(id >> 12);
		dbg_puts(" slot ");
		dbg_hex(id & 0xfff);
		dbg_puts(" from ");
		dbg_hex(lr);
		dbg_puts("\n");
	}
#if !defined(DAL_LOG) || DAL_LOG
	probe_stop_context(args);
#endif
#if defined(DAL_LOG) && !DAL_LOG
stop:
#endif
	if (id == REPLAY_ID_6D && args[1] == 0 && gReplayCount < 4) {
		gReplayCount++;
		dbg_puts("g2e:   diagnostic replay (observed: stage 0, no side effects in the observed window), returning r0 = 0\n");
		args[1] = args[2] = args[3] = 0;
		return 1;
	}
	dbg_puts("g2e: stopped at first call without a replay rule\n");
	return 0;
}

extern uint32_t __stack_sys_bottom[], __stack_sys_top[];
#define STACK_FILL	0x57a7c4edu
#define STACK_GUARD	0x6a12d0e5u

static void stack_fill(void)
{
	uint32_t *p;

	__stack_sys_bottom[0] = STACK_GUARD;
	for (p = __stack_sys_bottom + 1; p < __stack_sys_top; p++)
		*p = STACK_FILL;
}

static uint32_t stack_used(void)
{
	uint32_t *p = __stack_sys_bottom + 1;

	while (p < __stack_sys_top && *p == STACK_FILL)
		p++;
	return (uint32_t)((uint8_t *)__stack_sys_top - (uint8_t *)p);
}

static int stack_guard_ok(void)
{
	return __stack_sys_bottom[0] == STACK_GUARD;
}

/* the module globals area starts zero-filled; list what the module wrote (0x7ffe effect) */
static void report_globals_changes(void)
{
	uint32_t i, n = 0;

	for (i = 0; i < sizeof(gProbeModuleGlobals) / 4; i++) {
		if (gProbeModuleGlobals[i] == (i ? (i <= 3 ? (uint32_t)&gProbeGlobals[32 * i] : 0) : gProbeGlobalsInit0))
			continue;
		if (n++ < 24) {
			dbg_puts("g2a:   globals +");
			dbg_hex(i * 4);
			dbg_puts(" = ");
			dbg_hex(gProbeModuleGlobals[i]);
			dbg_puts("\n");
		}
	}
	dbg_puts("g2a: module globals words written: ");
	dbg_hex(n);
	dbg_puts("\n");
	for (i = 0; i < 8; i++)
		if (gProbeGlobals[i]) {
			dbg_puts("g2a:   [r9] table +");
			dbg_hex(i * 4);
			dbg_puts(" = ");
			dbg_hex(gProbeGlobals[i]);
			dbg_puts("\n");
		}
}

static uint32_t gEntry, gR9;

/* stage g4c: the module's own dispatch table ([r9-8]). Observed (INTERFACE §13): entry i = destination of the
 * module's export i (each export is an ARM "B" at module + hdr[1] + 4*i). Built at load time in RAM; entries that
 * are not a valid "B" into the module image keep the trap stub, so an unexpected call still stops in probe_trap(). */
#define MOD_TAB_SLOTS	1024
#define BOOT_MIN_ENTRIES	909u	/* RAL description §3: Boot's table is padded to at least this many entries */
static uint32_t gModTabStore[1 + MOD_TAB_SLOTS];	/* [0] = entry count (the word before the table, RAL §2) */
#define gModTab	(gModTabStore + 1)

static uint32_t build_module_table(const uint32_t *hdr, uint32_t modSize)
{
	uint32_t i, n = 0, expOff = hdr[1];

	for (i = 0; i < MOD_TAB_SLOTS; i++)
		gModTab[i] = probe_tab2[i];
	if (expOff & 3 || expOff >= modSize)
		return 0;
	for (i = 0; i < MOD_TAB_SLOTS && expOff + 4 * i + 4 <= modSize; i++) {
		uint32_t w = hdr[expOff / 4 + i], dst;
		int32_t ofs;

		if ((w >> 24) != 0xea)				/* not an unconditional ARM "B": end of the export table */
			break;
		ofs = (int32_t)(w << 8) >> 6;			/* signed 24-bit word offset, in bytes */
		dst = expOff + 4 * i + 8 + (uint32_t)ofs;	/* destination as an offset into the module */
		if (dst >= modSize)
			break;
		gModTab[i] = PROBE_MODULE_ADDR + dst;
		n++;
	}
	for (i = n; i < BOOT_MIN_ENTRIES; i++)		/* padding entries hold 0xFFFFFFFF until patched */
		gModTab[i] = 0xffffffffu;
	gModTabStore[0] = n > BOOT_MIN_ENTRIES ? n : BOOT_MIN_ENTRIES;
	return n;
}

/* stage g4-pre split of probe_run(): probe_prepare() + probe_cmd_7ffe() + export 0. Returns 0 when a module is present
 * and the r9 block / globals are set up. */
int probe_prepare(void)
{
	const uint32_t *hdr;
	uint32_t entry, r9;

	if (locate_module()) {
		dbg_puts("g2a: no module in this image, probe skipped\n");
		return -1;
	}
	hdr = (const uint32_t *)PROBE_MODULE_ADDR;
	if ((hdr[0] >> 24) != 0xea) {
		dbg_puts("g2a: no module image (first word is not a branch)\n");
		return -1;
	}
	entry = PROBE_MODULE_ADDR + 8 + ((int32_t)(hdr[0] << 8) >> 6);
	dbg_puts("g2a: module at ");
	dbg_hex(PROBE_MODULE_ADDR);
	dbg_puts(" header ");
	dbg_hex(hdr[1]); dbg_puts(" "); dbg_hex(hdr[2]); dbg_puts(" "); dbg_hex(hdr[3]);
	dbg_puts(" entry ");
	dbg_hex(entry);
	dbg_puts("\n");

	{
		uint32_t i;

		for (i = 0; i < 256; i++)
			gDalTab[i] = probe_tab1[i];
		gDalTabStore[0] = DAL_ENTRIES;
	}
	gProbeR9Block[3] = (uint32_t)gDalTab;		/* [r9-4]  */
	{
		uint32_t n = build_module_table(hdr, gModSize);

		dbg_puts("g4c: module dispatch table: ");
		dbg_hex(n);
		dbg_puts(" exports resolved, export 0 -> ");
		dbg_hex(gModTab[0]);
		dbg_puts("\n");
	}
	gProbeR9Block[2] = (uint32_t)gModTab;		/* [r9-8]: module's own dispatch table (g4c) */
	gProbeR9Block[1] = 0;						/* [r9-12]: 0 at entry, as observed */
	gProbeR9Block[4] = (uint32_t)gProbeGlobals;	/* [r9]    */
	/* module header word 2 = byte offset of the module's globals pointer in the [r9] table (Boot: 4); observed in the
	 * reference run: [[r9] + 4] = the value Boot moves into r9 before its first globals access (stage g2b) */
	if (hdr[2] < sizeof(gProbeGlobals) && !(hdr[2] & 3))
		gProbeGlobals[hdr[2] / 4] = (uint32_t)gProbeModuleGlobals;
	/* prefix as observed (g2c): globals-12 = 0, -8 = [r9-8] table, -4 = [r9-4] table, +0 = [r9] table */
	gProbeModCtx.prefix[0] = 0;
	gProbeModCtx.prefix[1] = 0;
	gProbeModCtx.prefix[2] = (uint32_t)gModTab;
	gProbeModCtx.prefix[3] = (uint32_t)gDalTab;
	probe_boot_template((uint32_t)gProbeModCtx.globals, (uint32_t)&gProbeModCtx.globals[sizeof(gProbeModCtx.globals) / 4]);
	gProbeModCtx.globals[0] = (uint32_t)gProbeGlobals;
	/* g4j (RAL spec §2): [r9+4..+12] = the other three quarter-pointers of the 0x400-entry globals-pointer table, in the
	 * DAL's block and in Boot's; table[0] = the DAL's own r9 */
	gProbeModCtx.globals[1] = (uint32_t)&gProbeGlobals[32];
	gProbeModCtx.globals[2] = (uint32_t)&gProbeGlobals[64];
	gProbeModCtx.globals[3] = (uint32_t)&gProbeGlobals[96];
	gProbeR9Block[5] = (uint32_t)&gProbeGlobals[32];
	gProbeR9Block[6] = (uint32_t)&gProbeGlobals[64];
	gProbeR9Block[7] = (uint32_t)&gProbeGlobals[96];
	gProbeGlobals[0] = (uint32_t)&gProbeR9Block[4];
	gProbeGlobalsInit0 = gProbeModCtx.globals[0];
	dbg_puts("g2d: globals at ");
	dbg_hex((uint32_t)gProbeModCtx.globals);
	dbg_puts(" [r9 table + hdr[2]] ");
	dbg_hex(gProbeGlobals[hdr[2] / 4]);
	dbg_puts(" prefix ");
	dbg_hex(gProbeModCtx.prefix[1]); dbg_puts(" ");
	dbg_hex(gProbeModCtx.prefix[2]); dbg_puts(" ");
	dbg_hex(gProbeModCtx.prefix[3]); dbg_puts(" +0 ");
	dbg_hex(gProbeModCtx.globals[0]);
	dbg_puts(gProbeGlobals[hdr[2] / 4] == (uint32_t)gProbeModCtx.globals ? " (pointer ok)\n" : " (POINTER MISMATCH)\n");
	r9 = (uint32_t)&gProbeR9Block[4];
	gEntry = entry;
	gR9 = r9;
	return 0;
}

/* 0x7ffe on the System stack from SVC mode; returns Boot's result */
uint32_t probe_cmd_7ffe(void)
{
	uint32_t entry = gEntry, r9 = gR9, ret, used;

	/* interrupt state as observed at the reference Boot entry (stage g2c): IC mask = timer only, alarm off, nothing
	 * pending. Vectors, IRQ stack and irq_dispatch() are set up by start.S/main.c before this point. */
	PV_TIMER_ALARM = 0;
	PV_TIMER_LO = 0;
	PV_IC_MASK = 1u << PV_IRQ_TIMER;
	stack_fill();
	gProbeEntrySp = (uint32_t)__stack_sys_top;
	ret = probe_call_module(entry, r9, PROBE_ENTRY_CMD, 0, entry, CPSR_ENTRY_7FFE);
	used = stack_used();
	dbg_puts("g2a: module returned ");
	dbg_hex(ret);
	dbg_puts(" [r9-12] ");
	dbg_hex(gProbeR9Block[1]);
	dbg_puts(" [r9-8] ");
	dbg_hex(gProbeR9Block[2]);
	dbg_puts(" [r9-4] ");
	dbg_hex(gProbeR9Block[3]);
	dbg_puts(" [r9] ");
	dbg_hex(gProbeR9Block[4]);
	dbg_puts("\n");
	dbg_puts("g2a: System stack used ");
	dbg_hex(used);
	dbg_puts(" of 0x8000 bytes, bottom guard ");
	dbg_puts(stack_guard_ok() ? "intact\n" : "OVERWRITTEN\n");
	report_globals_changes();
	return ret;
}

uint32_t probe_export0_addr(void)
{
	const uint32_t *hdr = (const uint32_t *)PROBE_MODULE_ADDR;

	return (hdr[1] < 0x1000 && !(hdr[1] & 3)) ? PROBE_MODULE_ADDR + hdr[1] : 0;
}

uint32_t probe_r9(void)
{
	return gR9;
}

void probe_run(void)
{
	const uint32_t *hdr;
	uint32_t r9, ret, used;

	if (probe_prepare())
		return;
	hdr = (const uint32_t *)PROBE_MODULE_ADDR;	/* only valid after probe_prepare() located the module */
	r9 = gR9;
	ret = probe_cmd_7ffe();

	/* g2c: second step observed in the reference run (trace, enter#2): the DAL calls Boot export slot 0
	 * (module + export table offset hdr[1] + 0) with r0..r3 = 0, the same r9 and a fresh System stack. The
	 * first call Boot makes back into the DAL tables lands in probe_trap(), which reports it and stops. */
	if (ret == 0 && hdr[1] < 0x1000 && !(hdr[1] & 3)) {
		uint32_t exp0 = PROBE_MODULE_ADDR + hdr[1];

		dbg_puts("g2c: calling export 0 at ");
		dbg_hex(exp0);
		dbg_puts(" r0..r3 = 0, cpsr 0x4000001f\n");
		PV_TIMER_ALARM = 0;
		stack_fill();
		gProbeEntrySp = (uint32_t)__stack_sys_top;
		ret = probe_call_module(exp0, r9, 0, 0, 0, CPSR_ENTRY_EXPORT0);
		used = stack_used();
		dbg_puts("g2c: export 0 returned ");
		dbg_hex(ret);
		dbg_puts(" stack used ");
		dbg_hex(used);
		dbg_puts("\n");
		report_globals_changes();
	}
}

/* g4j: accessors for the module loader (g4pre/ral.c) */
uint32_t *probe_ral_table(void) { return gProbeGlobals; }
uint32_t *probe_boot_table(void) { return gModTab; }
uint32_t probe_dal_r9(void) { return (uint32_t)&gProbeR9Block[4]; }
uint32_t probe_boot_module(void) { return gModAddr; }
uint32_t probe_boot_entry(void) { return gEntry; }
uint32_t *probe_dal_table(void) { return gDalTab; }
