/* Adrenaline / AdrBubbleBooter SceShell bridge, without newlib's resident state.
 * Copyright (C) 2016-2018 TheFloW; bubble support by Leecherman.
 * Copyright (C) 2026 Rubi contributors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Derived from shoui520/AdrBubbleBooter-oss b603ca04740734abfab0fae192a2a7d2b3cdda2c,
 * integration/adrenaline-v7/overlay/vsh/{main,utils}.c. See README.md.
 */
#ifdef ADR_VSH_HOST
#include "test_boundary.h"
#else
#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <taihen.h>
char *sceLsdbGetName(void *entry);
#endif

static tai_hook_ref_t refs[4];
static SceUID hooks[4] = {-1, -1, -1, -1};
static char app_titleid[12];

static int exists(const char *path)
{
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return 0;
    sceIoClose(fd);
    return 1;
}

/* Only the magic is needed to recognize a bubble. The old module read and
 * retained a complete 320-byte boot record for every catalogue entry. */
static int bubble(const char *path)
{
    unsigned char magic[4];
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return 0;
    int n = sceIoRead(fd, magic, sizeof(magic));
    sceIoClose(fd);
    return n == 4 && magic[0] == 'A' && magic[1] == 'B' && magic[2] == 'B' && !magic[3];
}

static int get_type(void *entry)
{
    int type = TAI_CONTINUE(int, refs[3], entry);
    if (type == 1) {
        const char *name = sceLsdbGetName(entry);
        char boot_inf[64], boot_bin[64];
        /* A catalogue entry can have no name. Never copy it before checking;
         * a title longer than this buffer cannot be an Adrenaline app ID. */
        if (!name || !name[0] || sceClibStrnlen(name, sizeof(app_titleid)) >= sizeof(app_titleid)) return type;
        sceClibStrncpy(app_titleid, name, sizeof(app_titleid));
        sceClibSnprintf(boot_inf, sizeof(boot_inf), "ux0:app/%s/data/boot.inf", name);
        sceClibSnprintf(boot_bin, sizeof(boot_bin), "ux0:app/%s/data/boot.bin", name);
        if (!sceClibStrncmp(name, "PSPEMU", 6) || bubble(boot_bin) || exists(boot_inf)) return 2;
    }
    return type;
}

static int hook_database(void)
{
    if (hooks[3] < 0)
        hooks[3] = taiHookFunctionImport(&refs[3], "SceShell", 0x6BC25E17, 0xDEC358E4, get_type);
    return hooks[3];
}

static int paf_load(int a1, const char *path, int a3, int a4, int a5)
{
    int rc = TAI_CONTINUE(int, refs[2], a1, path, a3, a4, a5);
    if (rc >= 0 && path && !sceClibStrncmp(path, "vs0:vsh/shell/livespace_db.suprx", 32)) hook_database();
    return rc;
}

static int load_module(unsigned id, SceSize args, void *argp, void *unk)
{
    int rc = TAI_CONTINUE(int, refs[0], id, args, argp, unk);
    if (rc >= 0 && id == 0x80000008 && hooks[2] < 0)
        hooks[2] = taiHookFunctionImport(&refs[2], "SceShell", 0x3D643CE8, 0xB3B5DF38, paf_load);
    return rc;
}

static int unload_module(unsigned id, SceSize args, void *argp, void *unk)
{
    int rc = TAI_CONTINUE(int, refs[1], id, args, argp, unk);
    if (rc >= 0 && id == 0x80000008) {
        /* PAF's next load can bind Lsdb again. Drop both import hooks so
         * their old IDs cannot suppress installation in the new modules. */
        for (int i = 3; i >= 2; i--)
            if (hooks[i] >= 0) {
                taiHookRelease(hooks[i], refs[i]);
                hooks[i] = -1;
            }
    }
    return rc;
}

int module_start(SceSize args, void *argp)
{
    (void)args; (void)argp;
    tai_module_info_t info;
    info.size = sizeof(info);
    if (taiGetModuleInfo("SceLsdb", &info) < 0) {
        hooks[0] = taiHookFunctionImport(&refs[0], "SceShell", 0x03FCF19D, 0xC3C26339, load_module);
        hooks[1] = taiHookFunctionImport(&refs[1], "SceShell", 0x03FCF19D, 0xA2F40C4C, unload_module);
    } else {
        hook_database();
    }
    if (app_titleid[0]) sceAppMgrDestroyAppByName(app_titleid);
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (int i = 3; i >= 0; i--)
        if (hooks[i] >= 0) {
            taiHookRelease(hooks[i], refs[i]);
            hooks[i] = -1;
        }
    return SCE_KERNEL_STOP_SUCCESS;
}

#ifndef ADR_VSH_HOST
int _start(SceSize args, void *argp) __attribute__((weak, alias("module_start")));
#endif
