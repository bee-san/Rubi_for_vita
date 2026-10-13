/* SceShell plugin entry. Same bootstrap pattern as PSVshellPlus: wait until
 * SceShell has paf running (its sceAVConfigRegisterCallback import is called
 * late in startup), then load our RCO as a paf plugin and start the worker.
 *
 * The boot-safety guard comes first: in a safe boot (the previous one did
 * not complete) the plugin writes its note and unloads itself, so SceShell
 * keeps every page we would have taken. */
#include <psp2/io/fcntl.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/vshbridge.h>
#include <taihen.h>

#include "../core/boot_guard.h"
#include "shell.h"

void vjo_plugin_load(void);

static tai_hook_ref_t s_hook_ref;
static SceUID s_hook_id = -1;

static SceInt32 sceAVConfigRegisterCallback_patched(SceUID cbid, SceInt32 a2)
{
    SceInt32 ret;
    int rc;
    vjo_log("bootstrap: Paf callback entered");
    vjo_plugin_load();
    vjo_log("bootstrap: Paf plugin loaded");
    rc = vjo_worker_start();
    if (rc < 0) {
        /* Degraded, not dead: the plugin stays loaded without its threads,
         * and this boot never reaches "ok", so the next one is safe. */
        vjo_log("worker start failed %d", rc);
    } else {
        vjo_log("bootstrap: worker started");
        vjo_guard_mark(VJO_BOOT_STAGE_WORKER);
    }
    ret = TAI_CONTINUE(SceInt32, s_hook_ref, cbid, a2);
    taiHookRelease(s_hook_id, s_hook_ref); /* one-shot */
    s_hook_id = -1;
    return ret;
}

/* The plugin declines to run (a configuration problem, not a crash): the
 * guard must not count this boot as a failure. */
static int decline(void)
{
    vjo_guard_mark(0);
    vjo_status_close();
    return SCE_KERNEL_START_NO_RESIDENT;
}

void _start() __attribute__((weak, alias("module_start")));
int module_start(SceSize args, const void *argp)
{
    SceInt32 opt[2];
    SceUID fd;
    SceUID kmod;
    (void)args;
    (void)argp;

    vjo_status_reset();
    vjo_log("bootstrap: shell API %d entered", VJO_API_VERSION);
    if (vjo_guard_safe_mode()) {
        vjo_status_close();
        return SCE_KERNEL_START_NO_RESIDENT;
    }
    vjo_guard_mark(VJO_BOOT_STAGE_SHELL);
    fd = sceIoOpen(VJO_RCO_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) {
        vjo_log("RCO missing at " VJO_RCO_PATH " (0x%08X): not starting", fd);
        return decline();
    }
    sceIoClose(fd);

    kmod = _vshKernelSearchModuleByName("VitaJPOverlay_Kernel", opt);
    if (kmod <= 0) {
        vjo_log("kernel plugin not loaded (0x%08X): check *KERNEL in config.txt", kmod);
        return decline();
    }
    vjo_log("bootstrap: kernel resident, installing Paf callback");

    /* SceShell import of SceAVConfig: sceAVConfigRegisterCallback */
    s_hook_id = taiHookFunctionImport(&s_hook_ref, "SceShell", 0x79E0F03F, 0xFB5E3E74,
                                      sceAVConfigRegisterCallback_patched);
    if (s_hook_id < 0) {
        vjo_log("hooking sceAVConfigRegisterCallback failed 0x%08X", s_hook_id);
        return decline();
    }
    vjo_log("bootstrap: waiting for Paf");
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, const void *argp)
{
    (void)args;
    (void)argp;
    if (s_hook_id >= 0)
        taiHookRelease(s_hook_id, s_hook_ref);
    s_hook_id = -1;
    return vjo_worker_stop() < 0 ? SCE_KERNEL_STOP_FAIL : SCE_KERNEL_STOP_SUCCESS;
}
