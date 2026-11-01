/* pv-vz90-dal g4k: RAL, the module loader (DAL slots 0xAD..0xB6), written from the functional description in
 * docs/INTERFACE.md §25. SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "ral.h"
#include "sony.h"
#include "../src/pv_hw.h"

void dbg_puts(const char *s);
void dbg_hex(uint32_t v);
void *memset(void *d, int c, unsigned n);
uint32_t probe_call_with_r9(uint32_t fn, uint32_t r9, uint32_t a0, uint32_t a1);
uint32_t *probe_ral_table(void);
uint32_t *probe_boot_table(void);
uint32_t probe_dal_r9(void);
uint32_t probe_boot_module(void);
uint32_t probe_boot_entry(void);
extern const uint32_t probe_tab3[];
uint32_t *probe_dal_table(void);
extern const uint8_t _start[];

#define RAL_IDS		0x400u
#define RAL_MAX_MODS	128u
#define RAL_MAX_LINKS	256u
#define ID_DAL		0u
#define ID_BOOT		1u
#define ID_UI		2u
#define ID_APP		0x5fu
#define BOOT_MIN_ENTRIES 909u
#define UI_CAPACITY	0x180u
#define UI_MIN_ENTRIES	352u
#define BELOW_MIN	0x28u			/* below-r9 area: system words + room for the stock Palm OS layout */
#define FCC(a, b, c, d)	(((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

struct ral_mod {
	uint32_t desc[5];		/* descriptor as passed to RALLoadModule */
	uint32_t code, id, use, r9, area, entry, nexp, flags;
	uint32_t *tab;			/* first resolved address; tab[-1] = entry count */
	uint32_t stubs;			/* callable stub array (non-system modules), 0 otherwise */
	uint8_t used;
};

static struct ral_mod gMods[RAL_MAX_MODS];
static uint8_t gIdx[RAL_IDS];		/* module record index + 1, 0 = not loaded */
static struct { uint16_t client, ref; } gLinks[RAL_MAX_LINKS];
static uint32_t gLinkCount;
static uint32_t gUiStore[1 + UI_CAPACITY];
static uint32_t gDormant[4 + 4];	/* dormant application world: [r9-16..-4] system words, [r9+0..12] quarters */
static int gMemStage;
static SemaphoreHandle_t gLock;

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t rd32be(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static uint32_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }

static void lock(void)
{
	if (gLock && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
		xSemaphoreTakeRecursive(gLock, portMAX_DELAY);
}

static void unlock(void)
{
	if (gLock && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
		xSemaphoreGiveRecursive(gLock);
}

static struct ral_mod *mod_by_id(uint32_t id)
{
	return id < RAL_IDS && gIdx[id] ? &gMods[gIdx[id] - 1] : 0;
}

static struct ral_mod *mod_by_code(uint32_t code)
{
	uint32_t i;

	for (i = 0; i < RAL_MAX_MODS; i++)
		if (gMods[i].used && gMods[i].code == code)
			return &gMods[i];
	return 0;
}

static uint32_t header_id(const uint8_t *code)
{
	return (rd16(code + 0x0a) / 4) * 32 + rd16(code + 0x08) / 4;
}

/* size of a resource from the heap chunk header in front of it (same rule as the Boot locator) */
static uint32_t chunk_size(uint32_t p)
{
	uint32_t w0 = ((const uint32_t *)p)[-2];

	return (w0 & 0x00ffffffu) - 8 - ((w0 >> 24) & 0xf);
}

static uint32_t sext(uint32_t v, unsigned bits)
{
	return (uint32_t)((int32_t)(v << (32 - bits)) >> (32 - bits));
}

/* export / entry resolution (§4): returns the final target, 0 when the slot holds nothing recognisable */
static uint32_t resolve(uint32_t a)
{
	unsigned hop;

	for (hop = 0; hop < 16; hop++) {
		const uint32_t *w = (const uint32_t *)a;

		if ((w[0] >> 24) == 0xea) {			/* ARM B */
			a = a + 8 + (sext(w[0] & 0xffffff, 24) << 2);
			continue;
		}
		if (w[0] == 0xe59fc004u && (w[1] == 0xe08cc00fu || w[1] == 0xe08fc00cu) && w[2] == 0xe12fff1cu) {
			a = a + 12 + w[3];			/* ARM long thunk */
			if (a & 1)
				return a;
			continue;
		}
		if (hop == 0) {
			uint32_t h1 = w[0] & 0xffff, h2 = w[0] >> 16;

			if ((h1 & 0xf800) == 0xf000 && (h2 & 0xd000) == 0x9000) {	/* Thumb-2 B.W */
				uint32_t s = (h1 >> 10) & 1, j1 = (h2 >> 13) & 1, j2 = (h2 >> 11) & 1;
				uint32_t i1 = !(j1 ^ s), i2 = !(j2 ^ s);
				uint32_t off = (s << 24) | (i1 << 23) | (i2 << 22) | ((h1 & 0x3ff) << 12) | ((h2 & 0x7ff) << 1);

				return (a + 4 + sext(off, 25)) | 1;
			}
			if ((h1 & 0xf800) == 0xe000 && (h2 == 0xbf00 || h2 == 0x46c0))	/* Thumb B.N + NOP */
				return (a + 4 + sext((h1 & 0x7ff) << 1, 12)) | 1;
			return 0;
		}
		return a;
	}
	return 0;
}

/* data template (§15): returns 0 on success; every write is checked against [lo, hi) */
static int unpack_template(const uint8_t *d, uint32_t len, uint32_t base, uint32_t code, uint32_t lo, uint32_t hi,
			   uint32_t *relocs)
{
	uint32_t pos = 4, seg, list;

	*relocs = 0;
	if (len < 4)
		return len ? -1 : 0;
	for (seg = 0; seg < 3; seg++) {
		uint32_t out;

		if (pos + 4 > len)
			return -1;
		out = base + rd32be(d + pos);
		pos += 4;
		for (;;) {
			uint32_t b, n, i, k;
			const uint8_t *fixed = 0;
			uint32_t nfixed = 0, lit1 = 0, lit2 = 0;

			if (pos >= len)
				return -1;
			b = d[pos++];
			if (!b)
				break;
			if (b >= 0x10) {				/* runs and literals */
				uint32_t fill = 0;

				if (b >= 0x80) {
					n = (b & 0x7f) + 1;
					if (pos + n > len || out < lo || out + n > hi)
						return -1;
					for (i = 0; i < n; i++)
						((uint8_t *)out)[i] = d[pos + i];
					pos += n;
					out += n;
					continue;
				}
				if (b >= 0x40)
					n = (b & 0x3f) + 1;
				else if (b >= 0x20) {
					if (pos >= len)
						return -1;
					n = (b & 0x1f) + 2;
					fill = d[pos++];
				} else {
					n = (b & 0x0f) + 1;
					fill = 0xff;
				}
				if (out < lo || out + n > hi)
					return -1;
				memset((void *)out, (int)fill, n);
				out += n;
				continue;
			}
			switch (b) {					/* fixed patterns with embedded literal bytes */
			case 1: fixed = (const uint8_t *)"\0\0\0\0\xff\xff"; nfixed = 6; lit1 = 2; break;
			case 2: fixed = (const uint8_t *)"\0\0\0\0\xff"; nfixed = 5; lit1 = 3; break;
			case 3: fixed = (const uint8_t *)"\xa9\xf0\0\0"; nfixed = 4; lit1 = 2; lit2 = 1; break;
			case 4: fixed = (const uint8_t *)"\xa9\xf0\0"; nfixed = 3; lit1 = 3; lit2 = 1; break;
			default: return -1;
			}
			k = nfixed + lit1 + (lit2 ? 1 + lit2 : 0);
			if (pos + lit1 + lit2 > len || out < lo || out + k > hi)
				return -1;
			for (i = 0; i < nfixed; i++)
				*(uint8_t *)out++ = fixed[i];
			for (i = 0; i < lit1; i++)
				*(uint8_t *)out++ = d[pos++];
			if (lit2) {
				*(uint8_t *)out++ = 0;
				for (i = 0; i < lit2; i++)
					*(uint8_t *)out++ = d[pos++];
			}
		}
	}
	for (list = 0; list < 2; list++) {
		uint32_t count, k, add = list ? code : base;
		int32_t off = 0;

		if (pos + 4 > len)
			break;
		count = rd32(d + pos);
		pos += 4;
		for (k = 0; k < count; k++) {
			uint32_t b, delta, a, v;

			if (pos >= len)
				return -1;
			b = d[pos];
			if (b >= 0x80) {
				delta = (b & 0x7f) * 2;
				pos += 1;
			} else if (b >= 0x40) {
				if (pos + 2 > len)
					return -1;
				delta = sext(rd16(d + pos) & 0x3fff, 14) * 2;
				pos += 2;
			} else {
				if (pos + 4 > len)
					return -1;
				delta = sext(rd32(d + pos) & 0x3fffffff, 30) * 2;
				pos += 4;
			}
			off += (int32_t)delta;
			a = base + (uint32_t)off;
			if (a < lo || a + 4 > hi)
				return -1;
			v = rd32((const uint8_t *)a) + add;
			((uint8_t *)a)[0] = (uint8_t)v;
			((uint8_t *)a)[1] = (uint8_t)(v >> 8);
			((uint8_t *)a)[2] = (uint8_t)(v >> 16);
			((uint8_t *)a)[3] = (uint8_t)(v >> 24);
			(*relocs)++;
		}
	}
	return 0;
}

static void sync_code(uint32_t start, uint32_t len)
{
	uint32_t a;

	for (a = start & ~31u; a < start + len; a += 32)
		__asm volatile ("mcr p15, 0, %0, c7, c10, 1" :: "r"(a) : "memory");	/* clean D line */
	__asm volatile ("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");		/* drain write buffer */
	__asm volatile ("mcr p15, 0, %0, c7, c5, 0" :: "r"(0) : "memory");		/* invalidate I-cache */
}

static void write_system_words(uint32_t r9)
{
	uint32_t *w = (uint32_t *)r9, *t = probe_ral_table();

	w[-4] = 0;
	w[-3] = gMemStage ? (uint32_t)&gUiStore[1] : 0;
	w[-2] = (uint32_t)probe_boot_table();
	w[-1] = (uint32_t)probe_dal_table();
	w[0] = (uint32_t)&t[0];
	w[1] = (uint32_t)&t[32];
	w[2] = (uint32_t)&t[64];
	w[3] = (uint32_t)&t[96];
}

static struct ral_mod *new_record(uint32_t id)
{
	uint32_t i;

	for (i = 0; i < RAL_MAX_MODS; i++)
		if (!gMods[i].used) {
			memset(&gMods[i], 0, sizeof(gMods[i]));
			gMods[i].used = 1;
			gMods[i].id = id;
			gIdx[id] = (uint8_t)(i + 1);
			return &gMods[i];
		}
	return 0;
}

static void sizes(const uint32_t *desc, const uint8_t *code, uint32_t *below, uint32_t *above, uint32_t *need)
{
	const uint8_t *info = (const uint8_t *)desc[2];
	uint32_t id = header_id(code), b = BELOW_MIN, a = 16, n = rd32(code + 0x10);

	if (info) {
		if (rd32be(info) > b)
			b = rd32be(info);
		if (rd32be(info + 4) > a)
			a = rd32be(info + 4);
	}
	b = (b + 3) & ~3u;
	a = (a + 3) & ~3u;
	*below = b;
	*above = a;
	*need = b + a + (id <= ID_UI ? 0 : (1 + 2 * n) * 4);
}

/* g4k, SerialMgr description: with every 'vdrv' refused the Serial Manager finds no port and its install returns
 * serErrNoDevicesAvail (0x030B), which PalmOSMain treats as fatal; Boot entry 0x19B runs the original and maps exactly
 * that code to 0 */
#define BOOT_SERIALMGRINSTALL	0x19bu
#define SER_ERR_NO_DEVICES	0x030bu
uint32_t gRalSerialOrig;
void ral_serial_install_wrap(void);
static uint32_t patch_entry(uint32_t ref, uint32_t n, uint32_t fn, uint32_t *oldOut);

uint32_t ral_serial_result(uint32_t r)
{
	dbg_puts("g4k: SerialMgrInstall (original) returned ");
	dbg_hex(r & 0xffff);
	dbg_puts((r & 0xffff) == SER_ERR_NO_DEVICES ? " (no serial ports) -> 0\n" : " -> unchanged\n");
	return (r & 0xffff) == SER_ERR_NO_DEVICES ? 0 : r;
}

/* g4s: speed replacements of four Boot entries (spec BootPatches 1.1): same results as the Palm OS functions
 * (public semantics), written for word-aligned bulk work; they never call the originals */
static uint32_t fast_mem_move(uint8_t *dst, const uint8_t *src, int32_t n)		/* MemMove -> Err 0 */
{
	if (n <= 0 || dst == src)
		return 0;
	if (dst < src || dst >= src + n) {		/* forward */
		if ((((uint32_t)dst | (uint32_t)src) & 3) == 0) {
			for (; n >= 16; n -= 16, dst += 16, src += 16) {
				const uint32_t *a = (const uint32_t *)src;
				uint32_t *q = (uint32_t *)dst, v0 = a[0], v1 = a[1], v2 = a[2], v3 = a[3];

				q[0] = v0; q[1] = v1; q[2] = v2; q[3] = v3;
			}
			for (; n >= 4; n -= 4, dst += 4, src += 4)
				*(uint32_t *)dst = *(const uint32_t *)src;
		}
		while (n--)
			*dst++ = *src++;
	} else {					/* overlapping, destination above: backward */
		dst += n;
		src += n;
		if ((((uint32_t)dst | (uint32_t)src) & 3) == 0) {
			for (; n >= 4; n -= 4) {
				dst -= 4;
				src -= 4;
				*(uint32_t *)dst = *(const uint32_t *)src;
			}
		}
		while (n--)
			*--dst = *--src;
	}
	return 0;
}

static uint32_t fast_mem_set(uint8_t *dst, int32_t n, uint32_t value)		/* MemSet(dst, n, value) -> Err 0 */
{
	uint32_t w = (value & 0xff) * 0x01010101u;

	if (n <= 0)
		return 0;
	while (n && ((uint32_t)dst & 3)) {
		*dst++ = (uint8_t)w;
		n--;
	}
	for (; n >= 32; n -= 32, dst += 32) {	/* 8 words per step (lets the compiler use STM) */
		uint32_t *q = (uint32_t *)dst;

		q[0] = w; q[1] = w; q[2] = w; q[3] = w; q[4] = w; q[5] = w; q[6] = w; q[7] = w;
	}
	for (; n >= 4; n -= 4, dst += 4)
		*(uint32_t *)dst = w;
	while (n--)
		*dst++ = (uint8_t)w;
	return 0;
}

static int32_t fast_mem_cmp(const uint8_t *a, const uint8_t *b, int32_t n)	/* MemCmp -> <0, 0, >0 */
{
	for (; n > 0; n--, a++, b++)
		if (*a != *b)
			return *a < *b ? -1 : 1;
	return 0;
}

static int32_t fast_str_compare_ascii(const uint8_t *a, const uint8_t *b)	/* StrCompareAscii -> <0, 0, >0 */
{
	for (; *a && *a == *b; a++, b++)
		;
	return *a == *b ? 0 : (*a < *b ? -1 : 1);
}

static int boot_patches(void)
{
	static const struct { uint32_t entry; uint32_t fn; const char *name; } speed[] = {
		{ 0x156, (uint32_t)fast_mem_move, "MemMove" }, { 0x16c, (uint32_t)fast_mem_set, "MemSet" },
		{ 0x130, (uint32_t)fast_mem_cmp, "MemCmp" }, { 0x1e1, (uint32_t)fast_str_compare_ascii, "StrCompareAscii" },
	};
	uint32_t r = patch_entry(ID_BOOT, BOOT_SERIALMGRINSTALL, (uint32_t)ral_serial_install_wrap, &gRalSerialOrig), i;

	dbg_puts("g4k: Boot entry 0x19b (SerialMgrInstall) patched: ");
	dbg_hex(r);
	dbg_puts(" original ");
	dbg_hex(gRalSerialOrig);
	dbg_puts("\n");
	if (r)
		return -1;
	for (i = 0; i < sizeof(speed) / sizeof(speed[0]); i++) {
		uint32_t e = patch_entry(ID_BOOT, speed[i].entry, speed[i].fn, 0);

		dbg_puts("g4s: Boot entry ");
		dbg_hex(speed[i].entry);
		dbg_puts(" (");
		dbg_puts(speed[i].name);
		dbg_puts(e ? ") speed replacement NOT installed\n" : ") speed replacement installed\n");
	}
	return 0;
}

int ral_init(void)
{
	uint32_t i;
	struct ral_mod *m;

	gLock = xSemaphoreCreateRecursiveMutex();
	gUiStore[0] = UI_MIN_ENTRIES;
	for (i = 0; i < UI_CAPACITY; i++)
		gUiStore[1 + i] = probe_tab3[i];	/* UI not loaded yet: calls stop in the trap stubs */
	/* DAL (0) and Boot (1) are set up by the startup code; record them so lookups and use counts work */
	m = new_record(ID_DAL);			/* g4p: the DAL image carries a module header (start_rtos.S) */
	m->code = m->desc[3] = (uint32_t)_start;
	m->use = 1;
	m->r9 = probe_dal_r9();
	m->tab = probe_dal_table();
	m->nexp = m->tab[-1];
	m->flags = rd16(_start + 0x22);
	m->entry = (uint32_t)_start;
	if (probe_boot_module()) {
		const uint8_t *code = (const uint8_t *)probe_boot_module();

		m = new_record(ID_BOOT);
		m->code = m->desc[3] = probe_boot_module();
		m->use = 1;
		m->r9 = probe_ral_table()[ID_BOOT];
		m->tab = probe_boot_table();
		m->nexp = m->tab[-1];
		m->entry = probe_boot_entry();
		m->flags = rd16(code + 0x22);
		if (m->nexp > BOOT_SERIALMGRINSTALL)
			return boot_patches();
	}
	return 0;
}

/* Boot's data template ('boot' 10001) and info ('boot' 10002) from the storage header of a full ROM image (§16) */
void probe_boot_template(uint32_t r9, uint32_t areaEnd)
{
	const uint32_t *stor = (const uint32_t *)(PV_ROM_BASE + 0x100u);
	uint32_t tp, ip, above = 16, relocs;

	if (stor[0] != 0xfeedfaceu || !(tp = stor[0x34 / 4]))
		return;
	ip = stor[0x38 / 4];
	if (ip && rd32be((const uint8_t *)ip + 4) > above)
		above = (rd32be((const uint8_t *)ip + 4) + 3) & ~3u;
	dbg_puts("g4k: Boot data template at ");
	dbg_hex(tp);
	dbg_puts(" size ");
	dbg_hex(chunk_size(tp));
	dbg_puts(", above-r9 globals ");
	dbg_hex(above);
	if (r9 + above > areaEnd ||
	    unpack_template((const uint8_t *)tp, chunk_size(tp), r9 + above, probe_boot_module(), r9, areaEnd, &relocs)) {
		dbg_puts(" -> FAILED (not applied)\n");
		return;
	}
	dbg_puts(" -> unpacked, relocations ");
	dbg_hex(relocs);
	dbg_puts("\n");
}

/* stage 1 (memory and Dm ready): the globals table is already full size; the UI table now exists, so every r9 world
 * gets [r9-12] (§2) */
void ral_memory_stage(void)
{
	uint32_t *t = probe_ral_table(), id, n = 0;

	gMemStage = 1;
	for (id = 0; id < RAL_IDS; id++)
		if (t[id]) {
			((uint32_t *)t[id])[-3] = (uint32_t)&gUiStore[1];
			n++;
		}
	dbg_puts("g4k: RAL memory stage: UI table ");
	dbg_hex((uint32_t)&gUiStore[1]);
	dbg_puts(" set in ");
	dbg_hex(n);
	dbg_puts(" r9 worlds\n");
}

static uint32_t get_module_info(const uint32_t *desc, uint8_t *info)
{
	const uint8_t *code = (const uint8_t *)desc[3];
	struct ral_mod *m = mod_by_code(desc[3]);
	uint32_t below, above, need, id = header_id(code);

	sizes(desc, code, &below, &above, &need);
	*(uint32_t *)(info + 0) = m ? m->id : 0xffffffffu;
	*(uint32_t *)(info + 4) = m ? 0 : need;
	*(uint32_t *)(info + 8) = rd32(code + 0x0c);
	*(uint32_t *)(info + 12) = rd32(code + 0x10);
	info[16] = id == ID_APP;
	dbg_puts("g4k:   RALGetModuleInfo code ");
	dbg_hex(desc[3]);
	dbg_puts(" id ");
	dbg_hex(id);
	dbg_puts(" type/creator ");
	dbg_hex(rd32(code + 0x14));
	dbg_puts(" ");
	dbg_hex(rd32(code + 0x18));
	dbg_puts(" -> ref ");
	dbg_hex(*(uint32_t *)info);
	dbg_puts(" need ");
	dbg_hex(*(uint32_t *)(info + 4));
	dbg_puts(" rev ");
	dbg_hex(*(uint32_t *)(info + 8));
	dbg_puts(" exports ");
	dbg_hex(*(uint32_t *)(info + 12));
	dbg_puts("\n");
	return 0;
}

static int refused(uint32_t type, uint32_t crid, uint32_t flags)
{
	if (((flags >> 8) & 0xff) == 0x69)
		return 0;
	if (type == FCC('l', 'i', 'b', 's'))
		return crid != FCC('t', 'w', 'S', '0');
	if (type == FCC('v', 'd', 'r', 'v'))
		return 1;
	if (type == FCC('l', 'i', 'b', 'r'))
		return crid == FCC('N', 'a', 'n', 'd') || crid == FCC('T', 'F', 'F', 'S') || crid == FCC('s', 'd', 's', 'd') ||
		       !sony_load_allowed(type, crid);	/* I2CDriver only on a Sony device (replaced, g4o) */
	if (type == FCC('l', 'i', 'b', 'f'))
		return crid == FCC('N', 'F', 'F', 'S');
	if (type == FCC('a', 'e', 'x', 't'))
		return crid == FCC('b', 't', 's', 'e') || crid == FCC('m', 't', '_', 'e') || crid == FCC('S', 'e', 'S', 'd');
	if (type == FCC('a', 'e', 'x', 'o'))
		return !sony_load_allowed(type, crid);		/* GE2D only on a Sony device (replaced, g4o) */
	return 0;
}

/* returns the error code; *stop = 1 for conditions the description calls fatal */
static uint32_t load_module(const uint32_t *desc, uint32_t area, uint32_t *entryOut, uint32_t *refOut, int *stop)
{
	const uint8_t *code = (const uint8_t *)desc[3];
	uint32_t id = header_id(code), below, above, need, r9, i, n, entry, relocs = 0, *t = probe_ral_table();
	uint32_t type = rd32(code + 0x14), crid = rd32(code + 0x18), flags = rd16(code + 0x22);
	struct ral_mod *m;

	dbg_puts("g4k:   RALLoadModule code ");
	dbg_hex(desc[3]);
	dbg_puts(" id ");
	dbg_hex(id);
	dbg_puts(" type/creator ");
	dbg_hex(type);
	dbg_puts(" ");
	dbg_hex(crid);
	dbg_puts(" area ");
	dbg_hex(area);
	if (id >= RAL_IDS) {
		dbg_puts(" -> invalid ID\n");
		return 0xf202;
	}
	if (!gMemStage && id > ID_BOOT) {
		dbg_puts(" -> before the memory stage (fatal)\n");
		*stop = 1;
		return 0xf20f;
	}
	if ((m = mod_by_code(desc[3])) != 0) {
		m->use++;
		if (entryOut)
			*entryOut = m->entry;
		if (refOut)
			*refOut = m->id;
		dbg_puts(" -> already loaded, use count ");
		dbg_hex(m->use);
		dbg_puts("\n");
		return 0;
	}
	entry = resolve(desc[3]);
	if (id == ID_APP && t[ID_APP]) {
		if (t[ID_APP] == (uint32_t)&gDormant[4]) {
			if (entryOut)
				*entryOut = entry;
			if (refOut)
				*refOut = 0xfffffffeu;
			dbg_puts(" -> application slot dormant: no globals\n");
			return 0xf201;
		}
		dbg_puts(" -> application slot busy\n");
		return 0xf20f;
	}
	if (mod_by_id(id)) {
		dbg_puts(" -> ID already in use by other code\n");
		return 0xf20f;
	}
	if (refused(type, crid, flags)) {
		dbg_puts(" -> refused by load policy\n");
		return 0xf20f;
	}
	if (!entry || !area) {
		dbg_puts(entry ? " -> no data area\n" : " -> main entry not resolvable (fatal)\n");
		*stop = !entry;
		return 0xf20f;
	}
	sizes(desc, code, &below, &above, &need);
	n = rd32(code + 0x10);
	memset((void *)area, 0, need);
	r9 = area + below;
	if (desc[1] && unpack_template((const uint8_t *)desc[1], desc[0], r9 + above, desc[3], area, r9 + above, &relocs)) {
		dbg_puts(" -> data template invalid (fatal)\n");
		*stop = 1;
		return 0xf20f;
	}
	write_system_words(r9);
	if (!(m = new_record(id))) {
		dbg_puts(" -> module table full\n");
		return 0xf20f;
	}
	for (i = 0; i < 5; i++)
		m->desc[i] = desc[i];
	m->code = desc[3];
	m->use = 1;
	m->r9 = r9;
	m->area = area;
	m->entry = entry;
	m->nexp = n;
	m->flags = flags;
	if (id == ID_UI) {
		if (n > UI_CAPACITY) {
			gIdx[id] = 0;
			m->used = 0;
			dbg_puts(" -> too many exports for the UI table\n");
			return 0xf203;
		}
		m->tab = &gUiStore[1];
		for (i = 0; i < UI_CAPACITY; i++)
			m->tab[i] = 0xffffffffu;
		gUiStore[0] = n > UI_MIN_ENTRIES ? n : UI_MIN_ENTRIES;
		m->nexp = gUiStore[0];
	} else {
		uint32_t *blk = (uint32_t *)(r9 + above);

		blk[0] = n;
		m->tab = blk + 1;
		m->stubs = (uint32_t)(blk + 1 + n);
		for (i = 0; i < n; i++)
			blk[1 + n + i] = 0xe51ff000u | (4 * n + 8);	/* ldr pc, [pc, #-(4n+8)]: loads address word i */
	}
	for (i = 0; i < n; i++) {
		uint32_t a = resolve(desc[3] + rd32(code + 4) + 4 * i);

		if (!a) {
			dbg_puts(" -> export ");
			dbg_hex(i);
			dbg_puts(" not resolvable (fatal)\n");
			*stop = 1;
			return 0xf20f;
		}
		m->tab[i] = a;
	}
	if (m->stubs)
		sync_code(m->stubs, 4 * n);
	t[id] = r9;
	{							/* Sony replacements before 0x7FFE (g4o) */
		int k = sony_on_load(type, crid, n, m->tab, &m->entry);

		if (k < 0) {
			*stop = 1;
			return 0xf20f;
		}
		entry = m->entry;
	}
	dbg_puts(" -> r9 ");
	dbg_hex(r9);
	dbg_puts(" need ");
	dbg_hex(need);
	dbg_puts(" relocs ");
	dbg_hex(relocs);
	dbg_puts(" exports ");
	dbg_hex(n);
	dbg_puts(" entry ");
	dbg_hex(entry);
	dbg_puts(", sending 0x7ffe\n");
	i = probe_call_with_r9(entry, r9, 0x7ffe, 0);
	dbg_puts("g4k:   module ");
	dbg_hex(id);
	dbg_puts(" load command returned ");
	dbg_hex(i);
	dbg_puts("\n");
	if (entryOut)
		*entryOut = entry;
	if (refOut)
		*refOut = id;
	return 0;
}

static uint32_t unload(uint32_t ref, uint32_t *descOut, uint32_t *freeOut)
{
	struct ral_mod *m;
	uint32_t i, j, r;

	if (ref == 0xfffffffeu) {
		if (freeOut)
			*freeOut = 0;
		return 0;
	}
	if (ref == ID_APP && probe_ral_table()[ID_APP] == (uint32_t)&gDormant[4])
		return 0xf20f;
	if (!(m = mod_by_id(ref)))
		return 0xf204;
	if (m->use > 1) {
		m->use--;
		if (freeOut)
			*freeOut = 0;
		return 0;
	}
	if (ref <= ID_UI)
		return 0xf204;
	if (descOut)
		for (i = 0; i < 5; i++)
			descOut[i] = m->desc[i];
	r = probe_call_with_r9(m->entry, m->r9, 0x7fff, 0);
	dbg_puts("g4k:   module ");
	dbg_hex(ref);
	dbg_puts(" unload command returned ");
	dbg_hex(r);
	dbg_puts("\n");
	probe_ral_table()[ref] = 0;
	if (freeOut)
		*freeOut = m->area;
	gIdx[ref] = 0;
	m->used = 0;
	for (i = j = 0; i < gLinkCount; i++) {
		if (gLinks[i].client == ref) {
			struct ral_mod *l = mod_by_id(gLinks[i].ref);

			if (l && l->use)
				l->use--;
			continue;
		}
		gLinks[j++] = gLinks[i];
	}
	gLinkCount = j;
	return 0;
}

static uint32_t unload_next(uint32_t *refP, uint32_t *descOut, uint32_t *freeOut)
{
	uint32_t id = *refP >= RAL_IDS ? RAL_IDS : *refP;
	struct ral_mod *m;

	while (id > 1) {
		id--;
		if (id == ID_APP)
			continue;
		if ((m = mod_by_id(id)) != 0 && m->use == 0) {
			*refP = RAL_IDS;
			m->use = 1;
			return unload(id, descOut, freeOut);
		}
	}
	return 0xf207;
}

static uint32_t get_entry_addresses(uint32_t ref, uint32_t first, uint32_t last, uint32_t *out)
{
	struct ral_mod *m = mod_by_id(ref);
	uint32_t i;

	if (!m)
		return 0xf204;
	if (first == 0xffffffffu && last == 0xffffffffu) {
		out[0] = m->entry;
		return 0;
	}
	if (first > last || last >= m->nexp)
		return 0xf205;
	for (i = first; i <= last; i++)
		out[i - first] = m->tab[i];
	return 0;
}

static uint32_t link_client(uint32_t ref, uint32_t client, uint32_t rec)
{
	struct ral_mod *m = mod_by_id(ref);
	uint32_t *r = (uint32_t *)(rec & ~3u);

	if (ref <= ID_UI)
		return 0xf206;
	if (!m)
		return 0xf204;
	if (gLinkCount < RAL_MAX_LINKS) {
		gLinks[gLinkCount].client = (uint16_t)client;
		gLinks[gLinkCount].ref = (uint16_t)ref;
		gLinkCount++;
	} else
		dbg_puts("g4k:   link table full (link not recorded)\n");
	r[0] = (rec & 1) ? (uint32_t)m->tab : m->stubs;
	r[1] = ref;
	return 0;
}

static uint32_t patch_entry(uint32_t ref, uint32_t n, uint32_t fn, uint32_t *oldOut)
{
	struct ral_mod *m = mod_by_id(ref);
	uint32_t a;

	if (!m)
		return 0xf204;
	if (n >= m->nexp || !((m->flags & 2) || (ref >= 5 && (m->flags & 1))))
		return 0xf205;
	if (oldOut)
		*oldOut = m->tab[n];
	a = (fn & 1) ? fn : resolve(fn);
	m->tab[n] = a ? a : fn;
	return 0;
}

uint64_t ral_call_new_stack(uint32_t fn, uint32_t param, uint32_t top, uint32_t r9);

int ral_dispatch(uint32_t slot, uint32_t *args, uint32_t r9)
{
	uint32_t r = 0;
	int stop = 0;

	if (slot < 0xad || slot > 0xb6)
		return 0;
	if (slot == 0xad) {			/* RALCallWithNewStack(fn, param, top) */
		uint64_t v;

		dbg_puts("g4k:   RALCallWithNewStack fn ");
		dbg_hex(args[1]);
		dbg_puts(" stack top ");
		dbg_hex(args[3]);
		dbg_puts(" param ");
		dbg_hex(args[2]);
		if (args[2] >= 0x20000000u && args[2] < 0x22000000u && !(args[2] & 3)) {
			uint32_t i;

			dbg_puts(" [");
			for (i = 0; i < 12; i++) {
				dbg_puts(" ");
				dbg_hex(((const uint32_t *)args[2])[i]);
			}
			dbg_puts(" ]");
			for (i = 3; i < 12; i++) {		/* diagnostics: printable text behind pointer words */
				const uint8_t *q = (const uint8_t *)((const uint32_t *)args[2])[i];
				char t[40];
				uint32_t j;

				if ((uint32_t)q < 0x20000000u || (uint32_t)q >= 0x21fff000u)
					continue;
				for (j = 0; j < 39; j++)
					t[j] = (q[j] >= 0x20 && q[j] < 0x7f) ? (char)q[j] : '.';
				t[39] = 0;
				dbg_puts(" {");
				dbg_hex(i);
				dbg_puts(":");
				dbg_puts(t);
				dbg_puts("}");
			}
		}
		dbg_puts("\n");
		v = ral_call_new_stack(args[1], args[2], args[3], r9);
		args[1] = (uint32_t)v;
		args[2] = (uint32_t)(v >> 32);
		return 1;
	}
	lock();
	switch (slot) {
	case 0xae:
		r = get_entry_addresses(args[1], args[2], args[3], (uint32_t *)args[4]);
		dbg_puts("g4k:   RALGetEntryAddresses ");
		dbg_hex(args[1]);
		dbg_puts(" ");
		dbg_hex(args[2]);
		dbg_puts("..");
		dbg_hex(args[3]);
		dbg_puts(" -> ");
		dbg_hex(r);
		dbg_puts("\n");
		break;
	case 0xaf:
		r = get_module_info((const uint32_t *)args[1], (uint8_t *)args[2]);
		break;
	case 0xb0:
		if (args[1] >= RAL_IDS)
			r = 0xf20f;
		else
			*(uint32_t *)args[2] = probe_ral_table()[args[1]];
		break;
	case 0xb1:
		r = link_client(args[1], args[2], args[3]);
		dbg_puts("g4k:   RALLinkClient ");
		dbg_hex(args[1]);
		dbg_puts(" client ");
		dbg_hex(args[2]);
		dbg_puts(" -> ");
		dbg_hex(r);
		dbg_puts("\n");
		break;
	case 0xb2:
		r = load_module((const uint32_t *)args[1], args[2], (uint32_t *)args[3], (uint32_t *)args[4], &stop);
		break;
	case 0xb3:
		r = patch_entry(args[1], args[2], args[3], (uint32_t *)args[4]);
		dbg_puts("g4k:   RALPatchEntry ");
		dbg_hex(args[1]);
		dbg_puts(" entry ");
		dbg_hex(args[2]);
		dbg_puts(" -> ");
		dbg_hex(r);
		dbg_puts("\n");
		break;
	case 0xb4:				/* RALSetA5(value) */
		if (args[1] & 1) {
			write_system_words((uint32_t)&gDormant[4]);
			probe_ral_table()[ID_APP] = (uint32_t)&gDormant[4];
		} else
			probe_ral_table()[ID_APP] = args[1];
		break;
	case 0xb5:
		r = unload(args[1], (uint32_t *)args[2], (uint32_t *)args[3]);
		dbg_puts("g4k:   RALUnloadModule ");
		dbg_hex(args[1]);
		dbg_puts(" -> ");
		dbg_hex(r);
		dbg_puts("\n");
		break;
	default:
		r = unload_next((uint32_t *)args[1], (uint32_t *)args[2], (uint32_t *)args[3]);
		dbg_puts("g4k:   RALUnloadNext -> ");
		dbg_hex(r);
		dbg_puts("\n");
		break;
	}
	unlock();
	args[1] = r;
	return !stop;
}
