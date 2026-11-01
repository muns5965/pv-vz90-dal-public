/* pv-vz90-dal g4j: miscellaneous HAL services. Requirements: local functional spec (analyst role, HALMisc); see
 * docs/INTERFACE.md §24. Identity values come from the ROM card header at run time; constants are those the OS
 * expects on this platform; LED/vibrator/sound hardware is reported as absent.
 * Not handled here (the probe stops): HALReset, HALPowerSleepReady (no reset/sleep path on PV), HALErrDisplay
 * (an OS fatal-error report: stopping is the useful behaviour while booting).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "hal_misc.h"
#include "interrupt.h"
#include "../src/pv_hw.h"

void dbg_puts(const char *s);
void dbg_hex(uint32_t v);
uint32_t probe_call_with_r9(uint32_t fn, uint32_t r9, uint32_t a0, uint32_t a1);

#define CARD	((const uint8_t *)PV_ROM_BASE)		/* card header at the ROM base (signature at +8) */
#define IRQ_KEY	0xfacef00du				/* interrupt numbers arrive XOR-encoded with this value */

static uint16_t gIrqAll = 1, gAutoOffSec = 120;
static uint32_t gAutoOffEvt, gDbgLockout;
static struct { uint32_t h, data, r9; } gIrqH[32];

static uint32_t rd32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void irq_trampoline_common(uint32_t n)
{
	if (gIrqH[n].h)
		probe_call_with_r9(gIrqH[n].h, gIrqH[n].r9, gIrqH[n].data, 0);
}
#define TRAMP(n) static void tramp##n(void) { irq_trampoline_common(n); }
TRAMP(0) TRAMP(1) TRAMP(2) TRAMP(3) TRAMP(4) TRAMP(5) TRAMP(6) TRAMP(7)
TRAMP(8) TRAMP(9) TRAMP(10) TRAMP(11) TRAMP(12) TRAMP(13) TRAMP(14) TRAMP(15)
static void (*const gTramp[16])(void) = { tramp0, tramp1, tramp2, tramp3, tramp4, tramp5, tramp6, tramp7,
	tramp8, tramp9, tramp10, tramp11, tramp12, tramp13, tramp14, tramp15 };

static int decode_irq(uint32_t v, uint32_t *n)
{
	*n = v ^ IRQ_KEY;
	return *n < 16;				/* this DAL wires sources 0..15 */
}

static uint32_t rom_token(uint32_t name, uint32_t *dataP, uint16_t *sizeP)
{
	const uint8_t *p = (const uint8_t *)rd32le(CARD + 0x64);
	uint32_t guard = 0;

	if ((uint32_t)p < PV_ROM_BASE || (uint32_t)p >= PV_ROM_BASE + 0x1000)
		return 0xffffu;
	while (guard++ < 64) {
		uint32_t n = rd32le(p), len = rd32le(p + 4);

		if (n == 0xffffffffu || len > 0x1000)
			break;
		if (n == name) {
			if (dataP)
				*dataP = (uint32_t)(p + 8);
			if (sizeP)
				*sizeP = (uint16_t)len;
			return 0;
		}
		p += 8 + ((len + 3) & ~3u);	/* entries are padded to 4 bytes */
	}
	return 0xffffu;
}

static void put8(uint32_t a, uint8_t v) { if (a) *(uint8_t *)a = v; }
static void put16(uint32_t a, uint16_t v) { if (a) *(uint16_t *)a = v; }
static void put32(uint32_t a, uint32_t v) { if (a) *(uint32_t *)a = v; }

int hal_misc_dispatch(uint32_t slot, uint32_t *args, uint32_t r9)
{
	uint32_t n, r = 0;

	switch (slot) {
	case 0x60: r = 0x00160000u; break;			/* HALProcessorID */
	case 0x44: r = 0x0820u; break;				/* HALGetHwrMiscFlags */
	case 0x45: r = 0; break;				/* HALGetHwrMiscFlagsExt */
	case 0x6f: r = 10000; break;				/* HALSilkscreenGetID (Sony device) */
	case 0x7f: r = rd32le(CARD + 0x80); break;		/* HALOEMGetCompanyID */
	case 0x80: case 0x81: r = rd32le(CARD + 0x84); break;	/* HALOEMGetHALID / GetDeviceID */
	case 0x85: r = rom_token(args[1], (uint32_t *)args[2], (uint16_t *)args[3]); break;	/* HALGetROMToken */
	case 0x07: {						/* HALBatteryGetInfo: 8 optional outputs */
		const uint32_t *sp = &args[6];		/* outputs 5..8 are on the caller's stack */

		put16(args[1], 10); put16(args[2], 5); put16(args[3], 2); put32(args[4], 60000);
		put8(sp[0], 5); put8(sp[1], 0); put8(sp[2], 0); put8(sp[3], 99);
		r = 0;
		break;
	}
	case 0x13: put16(args[1], 0); r = 0; break;		/* HALDockStatus */
	case 0x46: r = 0; break;				/* HALGetHwrWakeUp */
	case 0x5c: r = gAutoOffSec; break;
	case 0x5e: gAutoOffSec = (uint16_t)args[1]; r = 0; break;
	case 0x5b: r = gAutoOffEvt; break;
	case 0x5d: gAutoOffEvt = args[1]; r = 0; break;
	case 0x47:						/* HALInterruptAllSetStatus */
		r = gIrqAll;
		gIrqAll = (uint16_t)args[1];
		break;
	case 0x48: r = gIrqAll; break;
	case 0x49:						/* HALInterruptGetHandler */
		if (!decode_irq(args[1], &n)) { r = 0xff00u; break; }
		put32(args[2], gIrqH[n].h);
		put32(args[3], gIrqH[n].data);
		r = 0;
		break;
	case 0x4a:						/* HALInterruptSetHandler */
		if (!decode_irq(args[1], &n)) { r = 0xff00u; break; }
		gIrqH[n].h = args[2]; gIrqH[n].data = args[3]; gIrqH[n].r9 = r9;
		pic_registerIrq((uint8_t)n, gTramp[n], 0);
		r = 0;
		break;
	case 0x4b:						/* HALInterruptSetState */
		if (!decode_irq(args[1], &n)) { r = 1; break; }
		r = (PV_IC_MASK >> n) & 1;
		if (args[2] & 0xff)
			pic_enableInterrupt((uint8_t)n);
		else
			pic_disableInterrupt((uint8_t)n);
		break;
	case 0x02: case 0xcc:					/* HALDbgMessage / dprintf: log the string as given */
		dbg_puts("g4j:   OS debug: ");
		if (args[1])
			dbg_puts((const char *)args[1]);
		dbg_puts("\n");
		r = 0;
		break;
	case 0x78: case 0x79: r = 0; break;			/* HALTraceInit / Close */
	case 0x7a: case 0x7b: case 0x7c: case 0x7d:		/* HALTraceOutput*: format string only, no formatting */
		if (args[2]) {
			dbg_puts("g4j:   OS trace: ");
			dbg_puts((const char *)args[2]);
			dbg_puts("\n");
		}
		r = 0;
		break;
	case 0x7e: r = 0; break;				/* HALTraceOutputB */
	case 0xb7: r = gDbgLockout; gDbgLockout = args[1] & 0xff; break;	/* DALSetDbgLockout */
	case 0xba:						/* HALInvalidateICache */
		__asm volatile ("mcr p15, 0, %0, c7, c5, 0" :: "r"(0) : "memory");
		r = 0;
		break;
	case 0xbb: r = 0; break;				/* HALHandleReset */
	case 0xca: case 0x70: r = 0; break;			/* HALSoundInitialize / HALSoundOff: no audio on PV */
	/* g4n, local spec HALDrawIface §4: no tone or stream hardware on PV */
	/* HALSoundPlay(freq, amp, duration): g4w, the reference was measured returning 0x080A for every call with a
	 * non-zero duration (theme runs, same arguments on both DALs), not the 0x020A of the spec; duration 0 is
	 * not measured and keeps the spec value */
	case 0x71: r = args[3] ? 0x080a : 0x0201; break;
	case 0xc5:						/* HALSoundOpen(name, flags, errP) -> stream ref */
		if (args[3])
			*(uint16_t *)args[3] = 0x020a;
		r = 0xffffffffu;
		break;
	case 0xc6: r = 0x020b; break;				/* HALSoundClose */
	case 0xc7: case 0xc8:					/* HALSoundRead / HALSoundWrite(ref, buf, size, errP) */
		if (args[4])
			*(uint16_t *)args[4] = 0x020b;
		r = 0;
		break;
	case 0xc9:						/* HALSoundIoctl(ref, cmd, param) */
		if ((args[1] & 0xffff) == 0xffff && args[2] == 9 && args[3])
			*(uint32_t *)args[3] = 0;
		r = 0x020b;
		break;
	case 0xce: r = 0xffff; break;				/* HALPlaySmf */
	case 0x03: case 0x04: case 0x05: case 0x06: r = 1; break;	/* HALAttn*: no LED / vibrator */
	default:
		return 0;
	}
	args[1] = r;
	return 1;
}

/* g4q: non-scheduler SWIs (local spec SWI): HostControl (SWI 0x123457 / 0xAC, selector in r0) answers as a real
 * device; Angel semihosting (SWI 0xAB) debug output. frame[0..3] = caller's r0..r3; frame[0] receives the result. */
void swi_service(uint32_t num, uint32_t *frame)
{
	uint32_t sel = frame[0];
	static uint32_t logged;

	if (logged++ < 8) {
		dbg_puts("g4q: SWI ");
		dbg_hex(num);
		dbg_puts(" r0 ");
		dbg_hex(sel);
		dbg_puts(" from ");
		dbg_hex(frame[5]);
		dbg_puts("\n");
	}
	if (num == 0x123457u || num == 0xacu) {
		switch (sel) {
		case 0x0c03: case 0x0c04:			/* application start / quit: name in r1 */
			if (frame[1] >= 0x08000000u) {
				dbg_puts(sel == 0x0c03 ? "g4q: HostControl: application start \"" : "g4q: HostControl: application quit \"");
				dbg_puts((const char *)frame[1]);
				dbg_puts("\"\n");
			}
			frame[0] = 0;
			break;
		case 0x0101: case 0x0102: case 0x0400: frame[0] = 0; break;
		case 0x0c00: frame[0] = 1; break;
		case 0x0103:
			frame[0] = frame[1] == 0x0101 || frame[1] == 0x0102 || frame[1] == 0x0103 || frame[1] == 0x0400 ||
				   frame[1] == 0x0c00 || frame[1] == 0x0c03 || frame[1] == 0x0c04;
			break;
		default:
			frame[0] = 0xffff;
			break;
		}
		return;
	}
	if (num == 0xabu) {
		if (sel == 0x03 && frame[1]) {
			char c[2] = { *(const char *)frame[1], 0 };

			dbg_puts(c);
			return;
		}
		if (sel == 0x04 && frame[1]) {
			dbg_puts((const char *)frame[1]);
			return;
		}
		if (sel == 0x150 || sel == 0x151)
			return;
		if (sel == 0x153 || sel == 0x155) {
			frame[0] = 0;
			return;
		}
	}
	dbg_puts("g4q: unsupported SWI ");
	dbg_hex(num);
	dbg_puts(" r0 ");
	dbg_hex(sel);
	dbg_puts("\ng4q: stopped at unsupported SWI\n");
	for (;;)
		;
}
