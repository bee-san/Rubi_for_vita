/* The Rubi app runs as a system application, beside a suspended game
 * (vita/CMakeLists.txt: SELF attribute 0x0E, a 50 MiB budget). Its process
 * settings live here, apart from main.c, which the host tests compile.
 *
 * - The main thread runs on CPU3, the system core, at a system priority: a
 *   system application cannot use the game's CPU0-2 or the 0x40-0xBF
 *   application priorities (the same values as VitaShell-sysapp and
 *   jmdict-vita). Every other thread the app creates does too: the link
 *   wraps sceKernelCreateThread (vita/CMakeLists.txt --wrap), as
 *   VitaShell-sysapp's sysapp.c does.
 * - newlib's heap is fixed: its default, 128 MiB, does not fit the budget,
 *   and a failed heap block leaves every malloc failing. The app's own code
 *   does not malloc; the heap serves libvita2d's structures and GXM host
 *   memory, libpng and ScePvf's library, an estimated few hundred KiB. The
 *   glyph atlas is a 256 KiB texture (USER_RW), not heap.
 *
 * tools/check_settings_self.py checks the built eboot.bin and ELF for all of
 * it (the SELF's attribute and budget, these two thread values in its process
 * parameters, the heap, every thread created through the wrapper).
 *
 * libvita2d needs no setting: it finds the system-application budget itself
 * (sceAppMgrGetBudgetInfo), draws to the shared framebuffer and keeps its
 * textures in USER_RW, since a system application has no CDRAM. */
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

/* vita-elf-create finds these two by name. Nothing links to them, so
 * vita/CMakeLists.txt keeps them from --gc-sections with --undefined. */
const unsigned int sceUserMainThreadCpuAffinityMask = SCE_KERNEL_CPU_MASK_SYSTEM;
const int sceUserMainThreadPriority = 0x10000100;
int _newlib_heap_size_user = 8 * 1024 * 1024;

/* A thread on CPU3, at the main thread's priority when the call asks for an
 * application one (below 0x10000000: 0, the caller's, or 64-191); a system
 * priority, the default with an offset, is kept. Whatever CPU it asks for. */
SceUID __real_sceKernelCreateThread(const char *name, SceKernelThreadEntry entry, int priority, SceSize stack,
                                    SceUInt attr, int affinity, const SceKernelThreadOptParam *option);
SceUID __wrap_sceKernelCreateThread(const char *name, SceKernelThreadEntry entry, int priority, SceSize stack,
                                    SceUInt attr, int affinity, const SceKernelThreadOptParam *option)
{
    (void)affinity;
    if (priority < 0x10000000)
        priority = sceUserMainThreadPriority;
    return __real_sceKernelCreateThread(name, entry, priority, stack, attr, SCE_KERNEL_CPU_MASK_SYSTEM, option);
}
