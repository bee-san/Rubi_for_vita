#!/usr/bin/env python3
"""app_capture_patch.py CHECKOUT: turns a vita-jp-overlay checkout's Vita3K smoke app into the promo's capture app.

Only for the capture (never committed to that checkout, never in a release). The app's own sources are unchanged
except for two Vita3K workarounds under VJO_SMOKE_PAD (the smoke build's own define):
  - settings/sysapp.c: Vita3K refuses CPU3 to this app (0x80022004), so a thread it refuses there runs on any core;
  - vita/buttons_vita.c: Vita3K's registry answers 0 for button_assign (○ enters, the Japanese layout); the
    capture says 1 (× enters, a western console), as the film shows.
tools/vita3k_settings_smoke.c gets the promo's pad scripts in place of the smoke's (the harness is otherwise the
same): one for a card with no overlay installed (the app opens on its Install page: the plan, Enter, the run,
its result) and one once it is installed (the app opens on Games: Down, Down, Enter on the second game; Select, the
OCR & lookup page, scrolled so that Read text is the first card; Right: Only when I press L + R). A WAIT step
holds no button until the Install page's run has ended (vjo_install_page_busy), however long Vita3K takes.
vita/CMakeLists.txt: the smoke app gets the release's version string (its header's version label).
Each edit's anchor must be found exactly once, or this stops."""
import sys
from pathlib import Path

root = Path(sys.argv[1])


def edit(rel, old, new):
    p = root / rel
    s = p.read_text(encoding="utf-8")
    if s.count(old) != 1:
        sys.exit(f"app_capture_patch.py: {rel}: the anchor is found {s.count(old)} times:\n{old}")
    p.write_text(s.replace(old, new), encoding="utf-8")


edit("settings/sysapp.c",
     "    return __real_sceKernelCreateThread(name, entry, priority, stack, attr, SCE_KERNEL_CPU_MASK_SYSTEM, option);\n}",
     "#if defined(VJO_SMOKE_PAD)\n"
     "    { /* promo capture under Vita3K only: it refuses CPU3 (0x80022004) to this app; any core then */\n"
     "        SceUID uid = __real_sceKernelCreateThread(name, entry, priority, stack, attr, SCE_KERNEL_CPU_MASK_SYSTEM, option);\n"
     "        return uid == (SceUID)0x80022004 ? __real_sceKernelCreateThread(name, entry, priority, stack, attr, 0, option) : uid;\n"
     "    }\n"
     "#endif\n"
     "    return __real_sceKernelCreateThread(name, entry, priority, stack, attr, SCE_KERNEL_CPU_MASK_SYSTEM, option);\n}")

edit("vita/buttons_vita.c",
     '    int assign = -1;\n    if (sceRegMgrGetKeyInt("/CONFIG/SYSTEM", "button_assign", &assign) < 0)\n        assign = -1;\n',
     "    int assign = -1;\n"
     "#if defined(VJO_SMOKE_PAD)\n"
     "    /* promo capture under Vita3K only: its registry answers 0 (○ enters, the Japanese layout); a western\n"
     "     * console's has 1 (× enters) */\n"
     "    assign = 1;\n"
     "#else\n"
     '    if (sceRegMgrGetKeyInt("/CONFIG/SYSTEM", "button_assign", &assign) < 0)\n'
     "        assign = -1;\n"
     "#endif\n")

edit("vita/CMakeLists.txt",
     "target_compile_definitions(vjo_settings_smoke PRIVATE VJO_STANDALONE=1 VJO_SMOKE_PAD=${VJO_SETTINGS_SMOKE_PAD})",
     "target_compile_definitions(vjo_settings_smoke PRIVATE VJO_STANDALONE=1 VJO_SMOKE_PAD=${VJO_SETTINGS_SMOKE_PAD} "
     "VJO_VERSION_STRING=\"${VJO_VERSION}\")")

h = "tools/vita3k_settings_smoke.c"
s = (root / h).read_text(encoding="utf-8")
a, b = s.find("static const Step script[] = {"), s.find("};\n", s.find("static const Step script[] = {"))
if a < 0 or b < 0 or s.count("static const Step script[] = {") != 1:
    sys.exit(f"app_capture_patch.py: {h}: no single script[]")
scripts = r'''#define WAIT 0xFFFFu  /* frames: hold no button until the Install page's run has ended */
/* promo capture: the card before the install (the app opens on its Install page) */
static const Step script_install[] = {
    {240, 0},                                                   /* 01 the plan (Install the overlay) */
    TAP(ENTER, 2),                                              /*    Install */
    {WAIT, 0},                                                  /*    the run (its frames are in the recording) */
    {240, 0},                                                   /* 02 its result: restart the Vita now */
    TAP(CANCEL, 0),
};
/* promo capture: the overlay installed (the app opens on Games) */
static const Step script_games[] = {
    {200, 0},                                                   /* 01 Games (the icons load) */
    TAP(SCE_CTRL_DOWN, 90),                                     /* 02 the first game */
    TAP(SCE_CTRL_DOWN, 90),                                     /* 03 the second game */
    TAP(ENTER, 150),                                            /* 04 the selected game turned OFF, saved */
    TAP(SCE_CTRL_UP, 90),                                       /* 05 */
    TAP(SCE_CTRL_SELECT, 120),                                  /* 06 OCR & lookup (Model size selected) */
    TAP(SCE_CTRL_DOWN, 60), TAP(SCE_CTRL_DOWN, 60), TAP(SCE_CTRL_DOWN, 60),
    TAP(SCE_CTRL_DOWN, 60), TAP(SCE_CTRL_DOWN, 120),            /* 10-14 the page scrolls to its end */
    TAP(SCE_CTRL_UP, 60), TAP(SCE_CTRL_UP, 60), TAP(SCE_CTRL_UP, 60),
    TAP(SCE_CTRL_UP, 160),                                      /* 15-18 back to Read text, now the first card */
    TAP(SCE_CTRL_RIGHT, 200),                                   /* 19 Read text: Only when I press L + R, saved */
    TAP(SCE_CTRL_LEFT, 200),                                    /* 20 Automatically again, saved */
    TAP(CANCEL, 0),
};
static const Step *script = script_install;
static unsigned script_len = sizeof(script_install) / sizeof(script_install[0]);
'''
s = s[:a] + scripts + s[b + 3:]
old_frames = '''static unsigned script_frames(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < sizeof(script) / sizeof(script[0]); i++) n += script[i].frames;
    return n;
}'''
new_frames = '''static unsigned script_frames(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < script_len; i++) n += script[i].frames == WAIT ? 0 : script[i].frames;
    return n;
}'''
if s.count(old_frames) != 1:
    sys.exit(f"app_capture_patch.py: {h}: no single script_frames()")
s = s.replace(old_frames, new_frames)
old_run = '''static unsigned scripted(unsigned f)
{
    unsigned at = 0;
    for (unsigned i = 0; i < sizeof(script) / sizeof(script[0]); i++) {
        if (f < at + script[i].frames) return script[i].buttons;
        at += script[i].frames;
    }
    return CANCEL; /* past the end: leave */
}'''
new_run = '''/* The script's step and the frames left in it; a WAIT step ends once the run it waits for has ended
 * (it starts on the frame after the Enter that begins it), or after 3 minutes. */
static unsigned step, step_left, step_waited;
static unsigned scripted(unsigned f)
{
    (void)f;
    while (step < script_len) {
        if (script[step].frames == WAIT) {
            if (step_waited++ < 10 || (vjo_install_page_busy() && step_waited < 60 * 180)) return 0;
            step++, step_left = 0, step_waited = 0;
            continue;
        }
        if (!step_left) step_left = script[step].frames;
        if (step_left) {
            unsigned b = script[step].buttons;
            if (!--step_left) step++;
            return b;
        }
        step++;
    }
    return CANCEL; /* past the end: leave */
}'''
if s.count(old_run) != 1:
    sys.exit(f"app_capture_patch.py: {h}: no single scripted()")
s = s.replace(old_run, new_run)
old_first = '''    if (!frame)
        smoke_btn = vjo_buttons_read(); /* as the app read it at its start */'''
new_first = '''    if (!frame) {
        SceIoStat st;
        smoke_btn = vjo_buttons_read(); /* as the app read it at its start */
        if (sceIoGetstat("ux0:data/VitaJPOverlay/installed.txt", &st) >= 0) { /* installed: the Games script */
            script = script_games;
            script_len = sizeof(script_games) / sizeof(script_games[0]);
        }
    }'''
if s.count(old_first) != 1:
    sys.exit(f"app_capture_patch.py: {h}: no single first-frame block")
s = s.replace(old_first, new_first)
inc = '#include "../vita/buttons_vita.h"\n'
if s.count(inc) != 1:
    sys.exit(f"app_capture_patch.py: {h}: no buttons_vita.h include")
s = s.replace(inc, inc + '#include "../settings/install_page.h"\n#include <psp2/io/stat.h>\n')
(root / h).write_text(s, encoding="utf-8")
print("capture patch applied")
