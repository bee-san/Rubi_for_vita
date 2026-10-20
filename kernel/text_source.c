/* API 31: the text source (config.ini text_source, docs/native-hooks.md). The Shell sets it whenever it
 * reads its config; a game worker reads it once, as its game starts, to decide which native text hooks it
 * installs there. The kernel only holds the value and the loader's table below: 24 B of .bss, no hook, no
 * allocation, no thread. Until the Shell sets one it is VJO_TEXT_SOURCE_OCR (no hooks anywhere), and a
 * safe boot reads OCR whatever was set (the boot-safety guard: nothing new runs then).
 *
 * Only a process the loader started the worker in as ON (kernel/loader.c) is given the Shell's value
 * (audit-hooks F1): any other caller reads OCR, so a worker that a leftover *TITLEID line of taiHEN's
 * config loads into a game that is OFF installs no hook, and turning a game OFF stops its hooks. The
 * loader decides per process start: it records the process before the worker's module_start runs, in a
 * fixed table of TEXT_SOURCE_PIDS (the newest replacing the oldest; one game runs at a time), and clears
 * a process's entry at each start that is not ON, so a process ID used again is not taken for the old
 * process. */
#include "vjo_kernel.h"
#include "../core/boot_guard.h"

#define TEXT_SOURCE_PIDS 4
static uint32_t text_source; /* the word, mode | flags: 0, VJO_TEXT_SOURCE_OCR, until the Shell sets one */
static SceUID on_pid[TEXT_SOURCE_PIDS]; /* 0: free */
static uint32_t on_next;                /* the slot the next ON start takes, modulo TEXT_SOURCE_PIDS */

/* The registered Shell only (as vjoSetTriggers): the text source's word, a VJO_TEXT_SOURCE_* mode in its low
 * byte (VJO_TEXT_SOURCE_MODE_MASK) and no other bit than VJO_TEXT_SOURCE_NO_SITES (config.ini hook_sites =
 * off, audit-hooks F3), kept whole; any other word is VJO_ERR_ARG. */
int vjoSetTextSource(uint32_t word)
{
    if (g.shell_pid <= 0 || ksceKernelGetProcessId() != g.shell_pid)
        return VJO_ERR_PERM;
    if ((word & VJO_TEXT_SOURCE_MODE_MASK) >= VJO_TEXT_SOURCE_COUNT ||
        (word & ~(VJO_TEXT_SOURCE_MODE_MASK | VJO_TEXT_SOURCE_NO_SITES)))
        return VJO_ERR_ARG;
    if (__atomic_exchange_n(&text_source, word, __ATOMIC_ACQ_REL) != word)
        klog("text source %u", (unsigned)word); /* the word (256 + the mode with no sites), when it changes */
    return 0;
}

/* The loader, at each process start (kernel/loader.c worker_for): the process ID's old entry goes (should an
 * ID come again, it is not taken for the old process), and with on the process is recorded, before its
 * worker starts. pid <= 0 is never recorded. Plain word loads and stores (a CAS loop, unrolled, was 164 B
 * of the kernel's text): two starts at once could at worst lose an entry, so that game's worker reads OCR
 * (no hooks), never give one to a process that was not ON. */
void text_source_started(SceUID pid, int on)
{
    if (pid <= 0) return;
    for (unsigned i = 0; i < TEXT_SOURCE_PIDS; i++)
        if (__atomic_load_n(&on_pid[i], __ATOMIC_RELAXED) == pid)
            __atomic_store_n(&on_pid[i], 0, __ATOMIC_RELAXED);
    if (on)
        __atomic_store_n(&on_pid[__atomic_fetch_add(&on_next, 1u, __ATOMIC_RELAXED) % TEXT_SOURCE_PIDS], pid,
                         __ATOMIC_RELEASE);
}

/* Any caller (the game worker in its module_start): the Shell's word (its mode and VJO_TEXT_SOURCE_NO_SITES) in
 * a process the loader started the worker in as ON, else VJO_TEXT_SOURCE_OCR. */
int vjoGetTextSource(void)
{
    SceUID pid = ksceKernelGetProcessId();
    if (boot_mode() != VJO_BOOT_NORMAL || pid <= 0)
        return (int)VJO_TEXT_SOURCE_OCR;
    for (unsigned i = 0; i < TEXT_SOURCE_PIDS; i++)
        if (__atomic_load_n(&on_pid[i], __ATOMIC_ACQUIRE) == pid)
            return (int)__atomic_load_n(&text_source, __ATOMIC_ACQUIRE);
    return (int)VJO_TEXT_SOURCE_OCR;
}
