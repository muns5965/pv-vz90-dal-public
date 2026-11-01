/* pv-vz90-dal g4o: Sony VZ90 support - load-time replacement of SonyDAL ('libr'/'Sdal'), RmcExt ('aext'/'SeRm'),
 * Pa1Lib ('libr'/'yP1L'), GE2D ('aexo'/'SeGE') and I2CDriver ('libr'/'i2cD'), and the Sony part of
 * HALSetInitStage(2/3/4). Requirements: local functional specs (analyst role) SonyRepl and HALMisc §1.3-1.5;
 * docs/INTERFACE.md §29. No Sony code of a replaced module ever runs; no Sony hardware is touched.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "sony.h"
#include "hal_events.h"
#include "hal_display.h"

void dbg_puts(const char *s);
void dbg_hex(uint32_t v);
uint32_t os_call(uint32_t fn, uint32_t r9, const uint32_t *args, uint32_t n);
uint32_t *probe_boot_table(void);
uint32_t probe_dal_r9(void);
extern const uint8_t sony_halt_sdal[], sony_halt_rm[], sony_halt_p1[], sony_halt_ge[], sony_halt_i2[];
#define HSTUB(base, i)	((uint32_t)(base) + 12u * (i))

#define FCC(a, b, c, d)	(((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#define T_LIBR	FCC('l', 'i', 'b', 'r')
#define C_SDAL	FCC('S', 'd', 'a', 'l')
#define C_SHAL	FCC('S', 'h', 'A', 'l')

/* Boot dispatch-table indices of the OS services used here (local spec SonyRepl §3) */
#define B_DMDATABASEINFO	0x04b
#define B_DMGETNEXTDB		0x066
#define B_DMOPENBYTYPECR	0x07b
#define B_FTRSET		0x10f
#define B_SYSBATTERYINFO	0x1f8
#define B_SYSFINDMODULE		0x20b
#define B_SYSGETENTRYADDR	0x20e
#define B_SYSLOADMODULE		0x220

static uint32_t os(uint32_t idx, const uint32_t *a, uint32_t n)
{
	return os_call(probe_boot_table()[idx], probe_dal_r9(), a, n);
}

static uint32_t ftr_set(uint32_t crid, uint32_t num, uint32_t value)
{
	const uint32_t a[3] = { crid, num, value };

	return os(B_FTRSET, a, 3);
}

void sony_halt(uint32_t code, uint32_t lr, uint32_t r0, uint32_t r1)
{
	static const char *const mod[] = { "", "SonyDAL", "RmcExt", "Pa1Lib", "GE2D", "Sony main entry" };

	dbg_puts("g4o: Sony replacement entry called that the reference never expects: ");
	dbg_puts(mod[(code >> 8) & 7]);
	dbg_puts(" index ");
	dbg_hex(code & 0xff);
	dbg_puts(" from ");
	dbg_hex(lr);
	dbg_puts(" r0 ");
	dbg_hex(r0);
	dbg_puts(" r1 ");
	dbg_hex(r1);
	dbg_puts("\ng4o: stopped at unexpected Sony entry\n");
	vTaskSuspendAll();
	for (;;)
		;
}

/* ---- Sony detection: 'libr'/'Sdal' database named "SonyDAL" (HALMisc §1.3a) ---- */
static int gSony = -1;

int sony_condition(void)
{
	uint32_t st[8], lid = 0, r;
	char name[40];

	if (hal_get_init_stage() < 1 || hal_get_init_stage() > 4)
		return 0;
	if (gSony >= 0)
		return gSony;
	{
		const uint32_t a[6] = { 1, (uint32_t)st, T_LIBR, C_SDAL, 0, (uint32_t)&lid };

		r = os(B_DMGETNEXTDB, a, 6) & 0xffff;
	}
	if (r == 0x0207) {
		gSony = 0;
	} else if (r) {
		dbg_puts("g4o: Sony detection: DmGetNextDatabaseByTypeCreator error ");
		dbg_hex(r);
		dbg_puts("\ng4o: stopped at Sony detection\n");
		vTaskSuspendAll();
		for (;;)
			;
	} else {
		const uint32_t a[14] = { lid, (uint32_t)name, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
		static const char want[] = "SonyDAL";
		int i;

		name[0] = 0;
		r = os(B_DMDATABASEINFO, a, 14) & 0xffff;
		gSony = !r;
		for (i = 0; gSony && i < 8; i++)
			if (name[i] != want[i])
				gSony = 0;
	}
	dbg_puts(gSony ? "g4o: Sony device detected (SonyDAL database present)\n" : "g4o: not a Sony device\n");
	return gSony;
}

/* ---- SonyDAL replacement routines (SonyRepl §1.1-1.3) ---- */
static uint32_t gPortState = 0x0000000cu;	/* feature 'ShAl' 20050 points here */
static uint8_t gRotState;
static uint32_t gSleepCb[2][2], gWakeCb[2][2];
static struct { uint8_t inPercent; uint16_t thr, dispThr; } gThr[16];
static uint16_t gCpuIdx;
static const char gCpuName[] = "PV ARM926EJ";
static const char gNameSlow[] = "slow", gNameMed[] = "med", gNameFast[] = "fast";
static const struct { uint16_t one, mhz; const char *name; } gCpuProf[3] = {
	{ 1, 100, gNameSlow }, { 1, 200, gNameMed }, { 1, 300, gNameFast } };

static uint32_t gSonyCalls[45];
static void slog(uint32_t idx, uint32_t a0, uint32_t a1)
{
	if (idx < 45 && gSonyCalls[idx]++ < 8) {
		dbg_puts("g4o: SonyDAL export ");
		dbg_hex(idx);
		dbg_puts(" r0 ");
		dbg_hex(a0);
		dbg_puts(" r1 ");
		dbg_hex(a1);
		dbg_puts("\n");
	}
}

static uint32_t s_zero(void) { return 0; }
static uint32_t s_main(uint32_t cmd) { (void)cmd; return 0; }
static uint32_t s_version(void) { slog(2, 0, 0); return 1; }

static uint32_t s_sleep_set(uint32_t slot, uint32_t cb, uint32_t data)
{
	slog(3, (uint32_t)slot, 0);
	if (slot >= 2 || gSleepCb[slot][0])
		sony_halt((1u << 8) | 3, 0, slot, cb);
	gSleepCb[slot][0] = cb;
	gSleepCb[slot][1] = data;
	return 0;
}

static uint32_t s_wake_set(uint32_t slot, uint32_t cb, uint32_t data)
{
	slog(4, (uint32_t)slot, 0);
	if (slot >= 2 || gWakeCb[slot][0])		/* checks the wake slot (the reference checks the sleep slot) */
		sony_halt((1u << 8) | 4, 0, slot, cb);
	gWakeCb[slot][0] = cb;
	gWakeCb[slot][1] = data;
	return 0;
}

static uint32_t s_cpu_config(uint32_t op, void *data)
{
	uint16_t *h = data;
	uint32_t i;

	slog(8, op, 0);
	switch (op) {
	case 1: *h = 3; return 0;
	case 2: *(const void **)data = gCpuProf; return 0;
	case 3: *h = gCpuIdx; return 0;
	case 4: gCpuIdx = *h < 3 ? *h : 0; return 0;
	case 5: *h = gCpuProf[gCpuIdx].one; return 0;
	case 6: return 1;
	case 8: *h = sizeof(gCpuName); return 0;
	case 9:
		for (i = 0; i < sizeof(gCpuName); i++)
			((char *)data)[i] = gCpuName[i];
		return 0;
	case 13: return 0xffff0000u;
	default: return 0;
	}
}

static uint32_t s_port_get(uint32_t *out) { slog(12, 0, 0); *out = gPortState; return 0; }
static uint32_t s_hold_mask(uint32_t sel, uint32_t *out) { slog(15, sel, 0); *out = sel == 1 ? 2 : 0; return 0; }
static uint32_t s_rot_set(uint32_t st) { slog(16, st, 0); if (st > 2) return 1; gRotState = (uint8_t)st; return 0; }
static uint32_t s_rot_get(uint32_t *out) { slog(17, gRotState, 0); *out = gRotState; return 0; }

static int led_ok(uint32_t id) { return id == 1 || id == 2 || id == 4 || id == 8 || id == 16; }

static uint32_t s_led_set(uint32_t id, uint32_t on, uint32_t z)
{
	slog(19, (uint32_t)id, 0);
	(void)on;
	return z ? 2 : led_ok(id) ? 0 : 1;
}

static uint32_t s_led_get(uint32_t id, uint8_t *on, uint32_t *extra)
{
	slog(20, (uint32_t)id, 0);
	if (!on && !extra)
		return 2;
	if (!led_ok(id))
		return 1;
	if (on)
		*on = 0;
	if (extra)
		*extra = 0;
	return 0;
}

static uint32_t s_adc_read(uint32_t ch, uint16_t *out) { slog(21, ch, 0); *out = 0; return 0; }
static uint32_t s_ext_port(uint32_t b, const uint32_t *p) { slog(24, b, 0); (void)p; return 0; }
static uint32_t s_minus1(void) { slog(32, 0, 0); return 0xffffffffu; }
static uint32_t s_batt_pct(void) { slog(34, 0, 0); return 99; }

static uint32_t s_batt_thr(uint32_t set, uint32_t idx, uint8_t *inPct, uint16_t *thr, uint16_t *disp)
{
	slog(35, (uint32_t)set, 0);
	if (!inPct || !thr || !disp || idx >= 16)
		return 1;
	if (set) {
		gThr[idx].inPercent = *inPct;
		gThr[idx].thr = *thr;
		gThr[idx].dispThr = *disp;
	} else {
		*inPct = gThr[idx].inPercent;
		*thr = gThr[idx].thr;
		*disp = gThr[idx].dispThr;
	}
	return 0;
}

static uint32_t s_v2p(uint32_t cv, uint16_t *pct)
{
	slog(37, (uint32_t)cv, 0);
	cv &= 0xffff;
	*pct = (uint16_t)(cv < 370 ? 0 : cv > 420 ? 100 : (cv - 370) * 100 / 50);
	return 0;
}

static uint32_t s_p2v(uint32_t pct, uint16_t *cv)
{
	slog(38, (uint32_t)pct, 0);
	pct &= 0xffff;
	*cv = (uint16_t)(370 + (pct ? 5000 / pct : 0));	/* the reference does not guard 0 */
	return 0;
}

static uint32_t s_batt_info(uint8_t *idx, uint16_t *flags, uint8_t *pct, uint16_t *charge, uint16_t *usable,
			    uint16_t *third)
{
	slog(39, (uint32_t)idx, 0);
	const uint32_t dock = 0;			/* HALDockStatus on PV: nothing docked */
	uint8_t plugged = 0, osPct = 0;

	if (!idx)
		return 2;
	if (*idx == 0xff) {
		*idx = 1;
		return 0;
	}
	if (*idx > 1)
		return 2;
	{
		const uint32_t a[7] = { 0, 0, 0, 0, 0, (uint32_t)&plugged, (uint32_t)&osPct };

		os(B_SYSBATTERYINFO, a, 7);
	}
	if (*idx == 0) {
		if (charge) *charge = 0xffff;
		if (usable) *usable = 0xffff;
		if (third) *third = 0xffff;
		if (plugged) {
			if (pct) *pct = (dock & 8) ? 0xff : 100;
			if (flags) {
				*flags = (uint16_t)((*flags & ~0x0au) | 0xc5);
				*flags = (dock & 8) ? (uint16_t)((*flags & ~0x20u) | 0x10) : (uint16_t)((*flags & ~0x10u) | 0x20);
			}
		} else {
			if (pct) *pct = osPct;
			if (flags) {
				*flags = (uint16_t)((*flags & ~0xbau) | 0x05);
				*flags = (dock & 4) ? (uint16_t)(*flags | 0x40) : (uint16_t)(*flags & ~0x40u);
			}
		}
		return 0;
	}
	if (pct) *pct = 0xff;
	if (charge) *charge = 0xffff;
	if (usable) *usable = 0xffff;
	if (third) *third = 0xffff;
	if (flags) *flags = (uint16_t)((*flags & ~0xf6u) | 0x09);
	return (dock & 4) ? 1 : 0;
}

static uint32_t s_backup(void) { slog(42, 0, 0); return 0x700d; }

/* SonyDalMain (export 1) */
static uint32_t gSilkDb;
static const char gCpuProfileName[] = "med";
static uint32_t s_dal_main(uint32_t stage)
{
	slog(1, stage, 0);
	if (stage == 2) {
		const uint32_t a1[3] = { FCC('a', 's', 'l', 'k'), C_SHAL, 0x0005 };
		const uint32_t a2[3] = { FCC('s', 'i', 'l', 'k'), C_SHAL, 0x0005 };

		if (!(gSilkDb = os(B_DMOPENBYTYPECR, a1, 3)))
			gSilkDb = os(B_DMOPENBYTYPECR, a2, 3);
		dbg_puts("g4o: SonyDalMain(2): silkscreen layout database ");
		dbg_hex(gSilkDb);
		dbg_puts(gSilkDb ? " (left open)\n" : "\n");
		if (!gSilkDb) {
			dbg_puts("g4o: stopped at missing Sony silkscreen database\n");
			vTaskSuspendAll();
			for (;;)
				;
		}
	} else if (stage == 3) {
		ftr_set(C_SHAL, 20005, 1);
		ftr_set(C_SHAL, 20021, (uint32_t)gCpuProfileName);
		ftr_set(C_SHAL, 20000, 0x009926dfu);
		ftr_set(C_SHAL, 20050, (uint32_t)&gPortState);
		dbg_puts("g4o: SonyDalMain(3): 'ShAl' features 20005 20021 20000 20050 set\n");
	}
	return 0;
}

#define H(i)	HSTUB(sony_halt_sdal, i)
static uint32_t sdal_entry(uint32_t i)
{
	switch (i) {
	case 1: return (uint32_t)s_dal_main;
	case 2: return (uint32_t)s_version;
	case 3: return (uint32_t)s_sleep_set;
	case 4: return (uint32_t)s_wake_set;
	case 8: return (uint32_t)s_cpu_config;
	case 12: return (uint32_t)s_port_get;
	case 15: return (uint32_t)s_hold_mask;
	case 16: return (uint32_t)s_rot_set;
	case 17: return (uint32_t)s_rot_get;
	case 19: return (uint32_t)s_led_set;
	case 20: return (uint32_t)s_led_get;
	case 21: return (uint32_t)s_adc_read;
	case 24: return (uint32_t)s_ext_port;
	case 32: case 33: return (uint32_t)s_minus1;
	case 34: return (uint32_t)s_batt_pct;
	case 35: return (uint32_t)s_batt_thr;
	case 37: return (uint32_t)s_v2p;
	case 38: return (uint32_t)s_p2v;
	case 39: return (uint32_t)s_batt_info;
	case 42: case 43: case 44: return (uint32_t)s_backup;
	default: return H(i);		/* 0, 5-7, 9-11, 13, 14, 18, 22, 23, 25-31, 36, 40, 41 */
	}
}

/* ---- GE2D / I2CDriver ---- */
static uint32_t ge_main(uint32_t cmd)
{
	if ((cmd & 0xffff) == 0x7ffe || (cmd & 0xffff) == 0x7fff)
		return 0;
	sony_halt((5u << 8), 0, cmd, 0);
	return 0x0505;
}

static uint32_t ge_lock(uint32_t op) { (void)op; return 0; }
static uint32_t i2_open(void) { return 0; }
static uint32_t i2_read(uint32_t ad, void *b, uint32_t n) { return (!(ad & 1) || !b || !n) ? 2 : 1; }
static uint32_t i2_write(uint32_t ad, const void *b, uint32_t n) { return ((ad & 1) || !b || !n) ? 2 : 1; }
static uint32_t i2_wr(uint32_t ad, const void *w, void *r, uint32_t wn, uint32_t rn)
{
	(void)ad;
	return (!w || !r || !wn || !rn) ? 2 : 1;
}

/* ---- load-time hook (RAL: after publishing, before 0x7FFE) ---- */
int sony_load_allowed(uint32_t type, uint32_t crid)
{
	if ((type == FCC('a', 'e', 'x', 'o') && crid == FCC('S', 'e', 'G', 'E')) || (type == T_LIBR && crid == FCC('i', '2', 'c', 'D')))
		return sony_condition();
	return 1;
}

int sony_on_load(uint32_t type, uint32_t crid, uint32_t n, uint32_t *tab, uint32_t *entry)
{
	uint32_t i, kind;

	if (type == T_LIBR && crid == C_SDAL)
		kind = 1;
	else if (type == FCC('a', 'e', 'x', 't') && crid == FCC('S', 'e', 'R', 'm'))
		kind = 2;
	else if (type == T_LIBR && crid == FCC('y', 'P', '1', 'L'))
		kind = 3;
	else if (type == FCC('a', 'e', 'x', 'o') && crid == FCC('S', 'e', 'G', 'E'))
		kind = 4;
	else if (type == T_LIBR && crid == FCC('i', '2', 'c', 'D'))
		kind = 5;
	else
		return 0;
	if (!sony_condition())
		return kind >= 4 ? -1 : 0;
	if ((kind == 1 && n > 45) || ((kind == 2 || kind == 3) && n > 64) || (kind == 4 && n != 23) || (kind == 5 && n != 4)) {
		dbg_puts("g4o: Sony module with an unexpected export count ");
		dbg_hex(n);
		dbg_puts("\n");
		return -1;
	}
	for (i = 0; i < n; i++)
		switch (kind) {
		case 1: tab[i] = sdal_entry(i); break;
		case 2: tab[i] = HSTUB(sony_halt_rm, i); break;
		case 3: tab[i] = HSTUB(sony_halt_p1, i); break;
		case 4: tab[i] = i < 22 ? HSTUB(sony_halt_ge, i) : (uint32_t)ge_lock; break;
		default:
			tab[i] = i == 0 ? (uint32_t)i2_open : i == 1 ? (uint32_t)i2_read : i == 2 ? (uint32_t)i2_write : (uint32_t)i2_wr;
			break;
		}
	*entry = kind >= 4 ? (uint32_t)ge_main : (uint32_t)s_main;
	dbg_puts("g4o: Sony module replaced (");
	dbg_puts(kind == 1 ? "SonyDAL" : kind == 2 ? "RmcExt" : kind == 3 ? "Pa1Lib" : kind == 4 ? "GE2D" : "I2CDriver");
	dbg_puts("): main entry and ");
	dbg_hex(n);
	dbg_puts(" exports\n");
	(void)s_zero;
	return 1;
}

/* ---- Sony part of HALSetInitStage (HALMisc §1.3-1.5, SonyRepl §4) ---- */
static uint32_t entry1(int load)
{
	uint32_t ref = 0, e = 0, r;
	const uint32_t a[5] = { T_LIBR, C_SDAL, 0, 0, (uint32_t)&ref };

	r = os(load ? B_SYSLOADMODULE : B_SYSFINDMODULE, a, 5) & 0xffff;
	if (!r && ref) {
		const uint32_t b[4] = { ref, 1, 1, (uint32_t)&e };

		r = os(B_SYSGETENTRYADDR, b, 4) & 0xffff;
	}
	dbg_puts(load ? "g4o: SysLoadModule('libr','Sdal') ref " : "g4o: SysFindModule('libr','Sdal') ref ");
	dbg_hex(ref);
	dbg_puts(" err ");
	dbg_hex(r);
	dbg_puts(" entry 1 ");
	dbg_hex(e);
	dbg_puts("\n");
	if (r || !ref || !e) {
		dbg_puts("g4o: stopped at SonyDAL lookup\n");
		vTaskSuspendAll();
		for (;;)
			;
	}
	return e;
}

static uint32_t f_halt22(void) { sony_halt((1u << 8) | 36, 0, 0, 0); return 0; }
static uint32_t f_halt24(void) { sony_halt((1u << 8) | 0xf0, 0, 0, 0); return 0; }
static uint32_t f_ioctl31(uint32_t sel)
{
	if (sel <= 0x13)
		sony_halt((1u << 8) | 0xf1, 0, sel, 0);
	return 0x0502;
}

static uint8_t gBtse[8];

void sony_stage(uint32_t stage)
{
	uint32_t e, r9 = probe_dal_r9();

	if (stage < 2 || stage > 4 || !sony_condition())
		return;
	e = entry1(stage == 2);
	if (stage == 3) {
		ftr_set(C_SHAL, 20020, 1);
		ftr_set(C_SHAL, 20027, 3);
		ftr_set(C_SHAL, 20022, (uint32_t)f_halt22);
		ftr_set(C_SHAL, 20024, (uint32_t)f_halt24);
		ftr_set(C_SHAL, 20031, (uint32_t)f_ioctl31);
	}
	{
		const uint32_t a[1] = { stage };

		os_call(e, r9, a, 1);
	}
	if (stage == 2) {			/* generic: feature 'btse' 1 = 8-byte block (BE address of bytes 4..7, 68k code) */
		uint32_t p = (uint32_t)&gBtse[4];

		gBtse[0] = (uint8_t)(p >> 24); gBtse[1] = (uint8_t)(p >> 16); gBtse[2] = (uint8_t)(p >> 8); gBtse[3] = (uint8_t)p;
		gBtse[4] = 0x70; gBtse[5] = 0x10; gBtse[6] = 0x4e; gBtse[7] = 0x75;
		ftr_set(FCC('b', 't', 's', 'e'), 1, (uint32_t)gBtse);
	} else if (stage == 3) {		/* generic features */
		ftr_set(FCC('f', 'w', 'a', 'y'), 0, 0x00010000u);
		ftr_set(FCC('P', 'A', 'L', 'M'), 0, 16);
		{
			uint32_t err, v = 0;
			uint16_t dens = 0;

			ftr_set(FCC('d', 'n', 's', 'T'), 0, hal_display_get_attr(0x1f, &dens, &err) && !err ? dens : 72);
			ftr_set(FCC('s', 'c', 'n', 'P'), 1, hal_display_get_attr(0xe000, &v, &err) && !err ? v : 60);
		}
		dbg_puts("g4o: stage 3 features set ('ShAl' 20020/20022/20024/20027/20031, fway, PALM, dnsT, scnP)\n");
	}
}
