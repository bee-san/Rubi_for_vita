/* The debug build's read-only PSP probe (docs/native-hooks.md "PSP probe (debug build)"): its pure-C
 * half, host-tested (host/tests/test_psp_probe.c). game_ocr/psp_probe.c gives it the worker's reads in the
 * PSP emulator's process (VjoPspOps) and calls vjo_psp_probe_job after each OCR job whose request asked for
 * the probe (config.ini psp_probe, include/vjo_game_ocr.h VJO_GAME_OCR_S_PSP_PROBE). Everything the probe
 * decides is here: what to read, whether a read lies inside a block the lookups found, what the words at
 * Luna's sites are (the MIPS classifier), when to probe again, and every log line. Nothing here reads
 * memory but through the ops, allocates, or does I/O.
 *
 * The addresses, from source (reports/hooks-psp.md sections 1-3 and 5), none seen on a console yet:
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

#include "../include/vjo_game_ocr.h" /* VjoGameOcrResult: a job's line */

#define VJO_PSP_VIEW_RAM 0x70000000u        /* PSP 0x08000000, RAM's start */
#define VJO_PSP_VIEW_USER 0x70800000u       /* PSP 0x08800000, the user partition's start */
#define VJO_PSP_VIEW_ADRENALINE 0x73CDE000u /* PSP 0xABCDE000, SceAdrenaline */
#define VJO_PSP_TITLEID_OFFSET 0x98u        /* SceAdrenaline.titleid */
#define VJO_PSP_TITLEID_BYTES 12u
#define VJO_PSP_LUNA_BASE 0x08804000u       /* PPSSPP's main module base, assumed on a console */
#define VJO_PSP_BLOCKS 3u                   /* the blocks at the three VJO_PSP_VIEW_* addresses */
#define VJO_PSP_SITES_MAX 16u               /* sites read per title, at most */
#define VJO_PSP_SITE_WORDS 4u               /* words read per site: target - 8 to target + 4 */
#define VJO_PSP_LINE_MAX 400u               /* a log line, its NUL included, at most */
#define VJO_PSP_ACCESS_R 0x04u              /* SCE_KERNEL_MEMORY_ACCESS_R (vita-headers psp2/kernel/sysmem.h:41) */
#define VJO_PSP_MEMTYPE_CACHED 0xD0u        /* SCE_KERNEL_MEMORY_TYPE_NORMAL (psp2common/kernel/sysmem.h:20-21) */
#define VJO_PSP_MEMTYPE_UNCACHED 0x80u      /* SCE_KERNEL_MEMORY_TYPE_NORMAL_NC */
#define VJO_PSP_LOG_FORMAT 2u               /* the "psp probe N:" prefix: bumped when a line changes */
/* Report section 5's fourth step: one timed read of 1 MiB of the view, at the user partition's start (where
 * a RAM scan, the report's design c, would read), once per process, after its pages' lookups agree. */
#define VJO_PSP_READ_ADDR VJO_PSP_VIEW_USER
#define VJO_PSP_READ_BYTES 0x00100000u
#define VJO_PSP_PAGE 0x1000u
/* A pass (steps 1-4, the "start" line to "done" or "stop") runs after the process's first job and again when
 * what the lookups and the title ID show changes (Adrenaline's XMB, then a game; another game), at most this
 * often per process; a job line pair is written for each job, at most VJO_PSP_JOB_LINES_MAX pairs. */
#define VJO_PSP_PASSES_MAX 8u
#define VJO_PSP_JOB_LINES_MAX 1000u

/* What the worker found at one Vita address: sceKernelFindMemBlockByAddr's answer and
 * sceKernelGetMemBlockInfoByAddr's, raw. */
typedef struct {
    uint32_t addr;    /* the Vita address looked up */
    int32_t uid;      /* sceKernelFindMemBlockByAddr: the block's UID, < 0 an error */
    int32_t info_rc;  /* sceKernelGetMemBlockInfoByAddr: 0, < 0 an error (the fields below are then 0) */
    uint32_t base;    /* mappedBase */
    uint32_t size;    /* mappedSize */
    uint32_t memtype; /* memoryType: VJO_PSP_MEMTYPE_CACHED or _UNCACHED */
    uint32_t access;  /* SCE_KERNEL_MEMORY_ACCESS_* */
    uint32_t type;    /* the block's SceKernelMemBlockType */
} VjoPspBlock;

/* ScePspemu, as taiHEN and the module manager describe it (nothing in ScePspemu is called): which build it
 * is (its NID: the four builds Adrenaline patches, report section 1) and where its code is, for a later
 * round's ScePspemuConvertAddress (segment 0 + 0x6364, report section 2). */
typedef struct {
    int32_t rc;         /* taiGetModuleInfo("ScePspemu") */
    uint32_t nid;       /* its module_nid (0 when rc < 0) */
    int32_t modid;      /* its modid (0 when rc < 0) */
    int32_t info_rc;    /* sceKernelGetModuleInfo(modid) (0 not asked when rc < 0) */
    uint32_t seg0;      /* segment 0's vaddr and memsz (0 when info_rc < 0) */
    uint32_t seg0_size;
} VjoPspModule;

/* The worker's reads (game_ocr/psp_probe.c; a fake RAM image in the host tests). word, bytes and sum read
 * PSP RAM through the view, and the runner calls them only for ranges vjo_psp_inside accepts. */
typedef struct {
    void *ud;
    void (*module)(void *ud, VjoPspModule *out);
    void (*block)(void *ud, uint32_t addr, VjoPspBlock *out); /* both lookups at addr */
    int32_t (*find)(void *ud, uint32_t addr);                 /* sceKernelFindMemBlockByAddr: the UID, < 0 none */
    uint32_t (*word)(void *ud, uint32_t addr);                /* one aligned 32-bit load */
    void (*bytes)(void *ud, uint32_t addr, uint8_t *out, uint32_t n); /* n single-byte loads */
    uint32_t (*sum)(void *ud, uint32_t addr, uint32_t n);    /* the 32-bit sum of the n / 4 words at addr */
    uint64_t (*now_us)(void *ud);
    void (*put)(void *ud, const char *line, size_t n);        /* one line, written before the next read */
} VjoPspOps;

/* What a pass saw, to tell when the next job must pass again. */
typedef struct {
    uint32_t ok;                          /* bit i: blocks[i] passed vjo_psp_block_ok */
    int32_t uid[VJO_PSP_BLOCKS];
    uint32_t base[VJO_PSP_BLOCKS], size[VJO_PSP_BLOCKS];
    uint32_t title_read;                  /* 1: title holds the 12 bytes read */
    uint8_t title[VJO_PSP_TITLEID_BYTES];
} VjoPspSeen;

/* The probe's state for one process: zero to begin with (static). */
typedef struct {
    unsigned passes;    /* passes begun */
    unsigned jobs;      /* jobs seen */
    unsigned job_lines; /* jobs whose lines were written */
    int timed;          /* step 4 was made (or refused): once per process */
    VjoPspSeen seen;    /* the last pass's (when passes > 0) */
} VjoPspProbe;

/* PSP address -> its Vita address in Adrenaline's view (CONVERT_ADDRESS). */
static inline uint32_t vjo_psp_vita_addr(uint32_t psp_addr)
{
    return 0x68000000u + (psp_addr & 0x0FFFFFFFu);
}

/* 1 when the lookup found b's block, the block holds b->addr and it is readable; else 0. The probe goes
 * past its first step only when all three blocks are. */
int vjo_psp_block_ok(const VjoPspBlock *b);

/* The index of the first of the n_blocks blocks that vjo_psp_block_ok accepts and that holds all the n bytes
 * at Vita address addr (n >= 1), or -1 (and for a range that wraps past 0xFFFFFFFF). */
int vjo_psp_holder(const VjoPspBlock *blocks, unsigned n_blocks, uint32_t addr, uint32_t n);

/* 1 when vjo_psp_holder finds a block, else 0: every read the probe makes is behind this check. */
int vjo_psp_inside(const VjoPspBlock *blocks, unsigned n_blocks, uint32_t addr, uint32_t n);

/* SceAdrenaline.titleid's 12 raw bytes -> the game's ID: 1 when they start with 4 capital letters and 5
 * digits followed by a NUL ("ULJM05889"), id then those 9 characters and a NUL; else 0 and id "". */
int vjo_psp_title_id(const uint8_t raw[VJO_PSP_TITLEID_BYTES], char id[10]);

/* Luna's PSP sites (core/hook_luna_psp.inc) of title_id (its 9 characters are read; a NUL or anything
 * but 4 capital letters and 5 digits matches no title): writes at most cap of their PPSSPP addresses, and
 * never more than VJO_PSP_SITES_MAX, into addr, in the table's order, and returns how many it wrote.
 * *total, when not NULL, gets the number of sites the title has (0 for one Luna does not list). */
unsigned vjo_psp_luna_sites(const char *title_id, uint32_t *addr, unsigned cap, unsigned *total);

/* ---- the MIPS (Allegrex) site classifier: the words as loaded, little-endian; report section 3 ---- */

/* A branch or jump, an instruction with a delay slot: j, jal, jr, jalr, beq, bne, blez, bgtz and their likely
 * forms, the REGIMM branches (bltz, bgez, bltzal, bgezal and their likely forms), and the coprocessor
 * branches (bc0*, bc1* of the FPU, bc2* of Allegrex's VFPU: bvf, bvt and their likely forms). */
int vjo_mips_is_cti(uint32_t w);
/* A branch or jump that links (returns to its own address + 8): jal, jalr, bltzal, bgezal, bltzall, bgezall. */
int vjo_mips_is_call(uint32_t w);
/* What a two-word patch (j trampoline; nop) must not overwrite: a branch or jump, syscall, break, a COP0
 * instruction (eret and the rest: none is a game's), ll or sc. */
int vjo_mips_unsafe(uint32_t w);
/* addiu sp, sp, -N (N > 0): a function's first instruction, the frame it takes. */
int vjo_mips_is_frame(uint32_t w);

enum {
    VJO_PSP_CLASS_UNREAD, /* a word was outside the blocks (not read): no class */
    VJO_PSP_CLASS_ZERO,   /* all four words are 0: nothing is loaded there (or the base is not 0x08804000) */
    VJO_PSP_CLASS_DELAY,  /* the word before the target is a branch or jump: the target is its delay slot */
    VJO_PSP_CLASS_BRANCH, /* the target or the word after it (the two a patch overwrites) is unsafe */
    VJO_PSP_CLASS_ENTRY,  /* the target takes a frame (addiu sp, sp, -N): a function's entry */
    VJO_PSP_CLASS_RETURN, /* two words before the target is a call: the target is where it returns */
    VJO_PSP_CLASS_OTHER,  /* passed the checks, neither entry nor return: a mid-function block start */
    VJO_PSP_CLASSES
};

/* The class of a site from its four words (target - 8, - 4, + 0, + 4); bit k of mask set: word k was read.
 * The checks run in the enum's order: unread, zero, delay, branch, then entry before return. */
unsigned vjo_psp_classify(const uint32_t words[VJO_PSP_SITE_WORDS], unsigned mask);
/* "unread", "zero", "delay", "branch", "entry", "return", "other"; "?" for any other value. */
const char *vjo_psp_class_name(unsigned cls);

/* ---- the runner ---- */

/* After one job in the PSP emulator's process whose request asked for the probe (r: its result, NULL for none;
 * flags and speed: its request's VJO_GAME_OCR_F_* and _S_*; job_us: its time in the worker). While fewer than
 * VJO_PSP_PASSES_MAX passes have run, it looks the three blocks up again (step 1's lookups, no read) and reads
 * the title ID behind the check, then passes (steps 1-4, each line put before the next read) after the
 * process's first job and whenever the blocks or the title ID differ from the last pass's (Adrenaline's XMB,
 * then a game). Step 4, the timed 1 MiB read, is made once per process, in the first pass that gets that far,
 * and only after each of its 256 pages' lookups names the block that holds it. Then the job's two lines (at
 * most VJO_PSP_JOB_LINES_MAX jobs; the first one past them gets a line that says so). Every read of PSP RAM
 * goes through o->word, o->bytes or o->sum, only for ranges vjo_psp_inside accepts in that job's lookups. */
void vjo_psp_probe_job(VjoPspProbe *p, const VjoPspOps *o, const VjoGameOcrResult *r, uint32_t flags,
                       uint32_t speed, uint32_t job_us);

/* The log's lines. Each writes one line, "psp probe 2: ...\n", into out (cap bytes), NUL-terminated (cut
 * to cap - 1 bytes when longer; nothing when cap is 0), and returns its length before the NUL. Every
 * line these can write is shorter than VJO_PSP_LINE_MAX. Numbers are raw, in hex unless counted. */
/* "process <title>": the first line a process writes (the worker's title: NPXS10028, PSPEMUCFW, a bubble's) */
size_t vjo_psp_line_process(char *out, size_t cap, const char *title);
/* "start: pass P after job S" */
size_t vjo_psp_line_start(char *out, size_t cap, unsigned pass, uint32_t job_seq);
/* "ScePspemu: rc 0x... nid 0x... modid 0x... info 0x... seg0 0x... size 0x..." */
size_t vjo_psp_line_module(char *out, size_t cap, const VjoPspModule *m);
/* "block 0x...: uid 0x... info 0x... base 0x... size 0x... memtype 0x... (cached|uncached|?) access 0x...
 * type 0x... ok|bad" */
size_t vjo_psp_line_block(char *out, size_t cap, const VjoPspBlock *b);
/* "titleid 0x...: <24 hex digits> \"<printable>\"", or "titleid 0x...: outside the blocks" (read 0) */
size_t vjo_psp_line_titleid(char *out, size_t cap, uint32_t vita_addr, int read, const uint8_t *raw);
/* "luna <id>: T sites, R read" (T > 0), or "luna <id>: not listed" (T == 0); id may be "" */
size_t vjo_psp_line_sites(char *out, size_t cap, const char *id, unsigned total, unsigned read);
/* "site 0x<psp> at 0x<vita>: W0 W1 W2 W3 <class>": the words at vita - 8, - 4, + 0, + 4; bit k of mask
 * clear: word k was outside the blocks and not read ("--------"); the class is vjo_psp_classify's */
size_t vjo_psp_line_site(char *out, size_t cap, uint32_t psp_addr, uint32_t vita_addr,
                         const uint32_t words[VJO_PSP_SITE_WORDS], unsigned mask);
/* "classes: entry E return R branch B delay D other O zero Z unread U" (count[VJO_PSP_CLASSES]) */
size_t vjo_psp_line_classes(char *out, size_t cap, const unsigned *count);
/* Step 4, before the read (so a fault in it leaves this line last): "read 0x...: 0x... bytes, P pages in block
 * 0x... (A us): reading" when all P pages' lookups named the block (block_uid) that holds the range; else
 * "read 0x...: 0x... bytes: page 0x... in block 0x..., not 0x...: not read" (bad_page, its lookup's bad_uid,
 * block_uid), or "read 0x...: 0x... bytes: outside the blocks" (outside: no block holds the range) */
size_t vjo_psp_line_read_check(char *out, size_t cap, uint32_t addr, uint32_t n, int outside, unsigned pages,
                               uint32_t bad_page, int32_t bad_uid, int32_t block_uid, uint32_t check_us);
/* Step 4, after it: "read 0x...: 0x... bytes in B us, sum 0x..." (the 32-bit sum of its words) */
size_t vjo_psp_line_read_done(char *out, size_t cap, uint32_t addr, uint32_t n, uint32_t read_us, uint32_t sum);
/* "stop: <why>" or "done" (why NULL) */
size_t vjo_psp_line_end(char *out, size_t cap, const char *why);
/* A job's two lines, its capture and its OCR (r: the job's result; flags and speed: its request's; job_us: its
 * time in the worker):
 * "job S: rc R, flags 0x... speed 0x..., capture src C mem M block B, fb WxH pitch P fmt 0x..., region WxH at
 *  X,Y, rows R strips N tears T in U us, sum 0x..." and
 * "job S ocr: lines L rec K frames F, us find A load B rec C job D, free U need W+V reserve R fallback F,
 *  weights none|read|resident resident B, from T, chars N hash 0x..." (the shown text's UTF-8 characters and
 *  its FNV-1a: log.txt has the text itself, in the debug build) */
size_t vjo_psp_line_job_capture(char *out, size_t cap, const VjoGameOcrResult *r, uint32_t flags, uint32_t speed);
size_t vjo_psp_line_job_ocr(char *out, size_t cap, const VjoGameOcrResult *r, uint32_t job_us);
/* "jobs: N, the most per process: no more job lines" */
size_t vjo_psp_line_jobs_cap(char *out, size_t cap, unsigned jobs);

#endif
