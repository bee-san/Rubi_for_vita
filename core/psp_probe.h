/* The debug build's read-only PSP probe (docs/native-hooks.md "PSP probe (debug build)"): its pure-C
 * half, host-tested (host/tests/test_psp_probe.c). game_ocr/psp_probe.c reads, in the PSP emulator's
 * process, and calls these to check its reads against the blocks it found, to look the running game up
 * in Luna's PSP table and to format each log line. Nothing here reads memory, allocates or does I/O.
 *
 * The addresses, from source (reports/hooks-psp.md sections 1-3), none seen on a console yet:
 * - Adrenaline's view of PSP RAM in the emulator's process: CONVERT_ADDRESS(a) = 0x68000000 +
 *   (a & 0x0FFFFFFF) (TheOfficialFloW/Adrenaline 7d382b7, adrenaline_compat.h:188), so PSP
 *   0x08000000 is Vita 0x70000000 and the user partition's 0x08800000 is Vita 0x70800000;
 * - SceAdrenaline, the struct Adrenaline's PSP side shares, at PSP 0xABCDE000 (Vita 0x73CDE000), its
 *   12-byte titleid at +0x98 (adrenaline_compat.h:164-183; cef/systemctrl/adrenaline.c:133-157 copies
 *   the game's ID into it, NUL-terminated);
 * - Luna's PSP addresses are PPSSPP's, where a game's main module starts at 0x08804000. The probe
 *   assumes a console's is there too: that is one of the things its log answers. */
#ifndef VJO_PSP_PROBE_H
#define VJO_PSP_PROBE_H

#include <stddef.h>
#include <stdint.h>

#define VJO_PSP_VIEW_RAM 0x70000000u        /* PSP 0x08000000, RAM's start */
#define VJO_PSP_VIEW_USER 0x70800000u       /* PSP 0x08800000, the user partition's start */
#define VJO_PSP_VIEW_ADRENALINE 0x73CDE000u /* PSP 0xABCDE000, SceAdrenaline */
#define VJO_PSP_TITLEID_OFFSET 0x98u        /* SceAdrenaline.titleid */
#define VJO_PSP_TITLEID_BYTES 12u
#define VJO_PSP_LUNA_BASE 0x08804000u       /* PPSSPP's main module base, assumed on a console */
#define VJO_PSP_BLOCKS 3u                   /* the blocks at the three VJO_PSP_VIEW_* addresses */
#define VJO_PSP_SITES_MAX 16u               /* sites read per title, at most */
#define VJO_PSP_SITE_WORDS 4u               /* words read per site: target - 8 to target + 4 */
#define VJO_PSP_LINE_MAX 160u               /* a log line, its NUL included, at most */
#define VJO_PSP_ACCESS_R 0x04u              /* SCE_KERNEL_MEMORY_ACCESS_R (vita-headers psp2/kernel/sysmem.h:41) */
#define VJO_PSP_LOG_FORMAT 1u               /* the "psp probe N:" prefix: bumped when a line changes */

/* What the worker found at one Vita address: sceKernelFindMemBlockByAddr's answer and
 * sceKernelGetMemBlockInfoByAddr's, raw. */
typedef struct {
    uint32_t addr;    /* the Vita address looked up */
    int32_t uid;      /* sceKernelFindMemBlockByAddr: the block's UID, < 0 an error */
    int32_t info_rc;  /* sceKernelGetMemBlockInfoByAddr: 0, < 0 an error (the fields below are then 0) */
    uint32_t base;    /* mappedBase */
    uint32_t size;    /* mappedSize */
    uint32_t memtype; /* memoryType: 0xD0 cached, 0x80 uncached (vita-headers psp2common/kernel/sysmem.h:20-21) */
    uint32_t access;  /* SCE_KERNEL_MEMORY_ACCESS_* */
    uint32_t type;    /* the block's SceKernelMemBlockType */
} VjoPspBlock;

/* PSP address -> its Vita address in Adrenaline's view (CONVERT_ADDRESS). */
static inline uint32_t vjo_psp_vita_addr(uint32_t psp_addr)
{
    return 0x68000000u + (psp_addr & 0x0FFFFFFFu);
}

/* 1 when the lookup found b's block, the block holds b->addr and it is readable; else 0. The probe goes
 * past its first step only when all three blocks are. */
int vjo_psp_block_ok(const VjoPspBlock *b);

/* 1 when the n bytes at Vita address addr (n >= 1) lie inside one of the n_blocks blocks that
 * vjo_psp_block_ok accepts; 0 otherwise (and for a range that wraps past 0xFFFFFFFF). */
int vjo_psp_inside(const VjoPspBlock *blocks, unsigned n_blocks, uint32_t addr, uint32_t n);

/* SceAdrenaline.titleid's 12 raw bytes -> the game's ID: 1 when they start with 4 capital letters and 5
 * digits followed by a NUL ("ULJM05889"), id then those 9 characters and a NUL; else 0 and id "". */
int vjo_psp_title_id(const uint8_t raw[VJO_PSP_TITLEID_BYTES], char id[10]);

/* Luna's PSP sites (core/hook_luna_psp.inc) of title_id (its 9 characters are read; a NUL or anything
 * but 4 capital letters and 5 digits matches no title): writes at most cap of their PPSSPP addresses, and
 * never more than VJO_PSP_SITES_MAX, into addr, in the table's order, and returns how many it wrote.
 * *total, when not NULL, gets the number of sites the title has (0 for one Luna does not list). */
unsigned vjo_psp_luna_sites(const char *title_id, uint32_t *addr, unsigned cap, unsigned *total);

/* The log's lines. Each writes one line, "psp probe 1: ...\n", into out (cap bytes), NUL-terminated (cut
 * to cap - 1 bytes when longer; nothing when cap is 0), and returns its length before the NUL. Every
 * line these can write is shorter than VJO_PSP_LINE_MAX. Numbers are raw, in hex unless counted. */
/* "start: text source S" */
size_t vjo_psp_line_start(char *out, size_t cap, uint32_t text_source);
/* "ScePspemu: rc 0x... nid 0x..." (taiGetModuleInfo's answer and the module's NID) */
size_t vjo_psp_line_module(char *out, size_t cap, int32_t rc, uint32_t nid);
/* "block 0x...: uid 0x... info 0x... base 0x... size 0x... memtype 0x... access 0x... type 0x... ok|bad" */
size_t vjo_psp_line_block(char *out, size_t cap, const VjoPspBlock *b);
/* "titleid 0x...: <24 hex digits> \"<printable>\"", or "titleid 0x...: outside the blocks" (read 0) */
size_t vjo_psp_line_titleid(char *out, size_t cap, uint32_t vita_addr, int read, const uint8_t *raw);
/* "luna <id>: T sites, R read" (T > 0), or "luna <id>: not listed" (T == 0); id may be "" */
size_t vjo_psp_line_sites(char *out, size_t cap, const char *id, unsigned total, unsigned read);
/* "site 0x<psp> at 0x<vita>: W0 W1 W2 W3": the words at vita - 8, - 4, + 0, + 4; bit k of mask clear:
 * word k was outside the blocks and not read ("--------") */
size_t vjo_psp_line_site(char *out, size_t cap, uint32_t psp_addr, uint32_t vita_addr,
                         const uint32_t words[VJO_PSP_SITE_WORDS], unsigned mask);
/* "stop: <why>" or "done" (why NULL) */
size_t vjo_psp_line_end(char *out, size_t cap, const char *why);

#endif
