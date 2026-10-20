/* API 31: the text source (config.ini text_source, docs/native-hooks.md). The Shell sets it whenever it
 * reads its config; a game worker reads it once, as its game starts, to decide which native text hooks it
 * installs there. The kernel only holds the value: one word of .bss, no hook, no allocation, no thread.
 * Until the Shell sets one it is VJO_TEXT_SOURCE_OCR (no hooks anywhere), and a safe boot reads OCR
 * whatever was set (the boot-safety guard: nothing new runs then). */
#include "vjo_kernel.h"
#include "../core/boot_guard.h"

static uint32_t text_source; /* VJO_TEXT_SOURCE_*: 0, VJO_TEXT_SOURCE_OCR, until the Shell sets one */

/* The registered Shell only (as vjoSetTriggers): a mode of VJO_TEXT_SOURCE_* or VJO_ERR_ARG. */
int vjoSetTextSource(uint32_t mode)
{
    if (g.shell_pid <= 0 || ksceKernelGetProcessId() != g.shell_pid)
        return VJO_ERR_PERM;
    if (mode >= VJO_TEXT_SOURCE_COUNT)
        return VJO_ERR_ARG;
    if (__atomic_exchange_n(&text_source, mode, __ATOMIC_ACQ_REL) != mode)
        klog("text source %u", (unsigned)mode); /* when it changes, not at every config read */
    return 0;
}

/* Any caller (the game worker in its module_start): VJO_TEXT_SOURCE_*. */
int vjoGetTextSource(void)
{
    if (boot_mode() != VJO_BOOT_NORMAL)
        return (int)VJO_TEXT_SOURCE_OCR;
    return (int)__atomic_load_n(&text_source, __ATOMIC_ACQUIRE);
}
