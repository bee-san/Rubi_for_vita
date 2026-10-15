/* Adrenaline's PSP launches: the files of an AdrBubbleBooter bubble (made by
 * Adrenaline Bubble Manager), PSP disc IDs, and the small description of a
 * bubble's game that the Overlay app writes for the Shell. Pure functions over
 * bounded buffers; the Shell, the Overlay app and the host tests share them.
 *
 * A bubble is the app ux0:app/<ID>. AdrBubbleBooter's kernel and SceShell
 * plugins treat an app as a PSP-emulator app when its ID starts with PSPEMU, its
 * data/boot.bin starts with the magic "ABB\0", or a data/boot.inf exists
 * (AdrBubbleBooter-oss b603ca0 integration/adrenaline-v7/overlay/kernel/main.c:
 * 123-145, overlay/vsh/main.c:46-62); this module reads those files the same way. */
#ifndef VJO_PSPBUBBLE_H
#define VJO_PSPBUBBLE_H
#include <stddef.h>

#define VJO_ABB_MAGIC 0x00424241u /* "ABB\0", little-endian, at boot.bin 0x00 */
/* AdrBubbleBooter's boot.bin: 0x140 bytes with the target's path at 0x40
 * (src/common/adrenaline_compat.h:11-31); any other size is its older layout,
 * the path at 0x20, which adrbubblebooter.suprx converts at launch
 * (src/vita/adrbubblebooter/main.c:76-105). The path field is 256 bytes. */
#define VJO_ABB_BOOT_SIZE 0x140u
#define VJO_ABB_PATH_MAX 256u
/* bootconv.suprx reads at most this much of a boot.inf (src/vita/bootconv/main.c:199). */
#define VJO_ABB_INF_MAX 1024u

/* The target (ISO, CSO, EBOOT.PBP, ...) of a boot.bin image of len bytes, len
 * being the file's size: 0 with the NUL-terminated path in out, -1 when the
 * magic is missing (not a bubble), -2 when the path is empty, unterminated or
 * not device-qualified ("ux0:..."), or does not fit in cap. */
int vjo_abb_boot_bin(const void *data, size_t len, char *out, size_t cap);

/* The target of a boot.inf (text, len bytes): its last "PATH=" line, as
 * bootconv.suprx converts it ("ms0:/ISO/X.ISO" becomes "ux0:pspemu/ISO/X.ISO"
 * for Adrenaline's default memory stick, ux0; src/vita/bootconv/main.c:88-110,
 * 192-211). 0, or -1 with no usable PATH line. */
int vjo_abb_boot_inf(const char *text, size_t len, char *out, size_t cap);

/* ux0:data/VitaJPOverlay/psp/<bubble ID>.txt, written by the Overlay app for
 * the Shell: line 1 the game's disc ID ("" when unknown), line 2 its title,
 * line 3 the target's path (for people; the Shell does not read it). */
#define VJO_PSP_INFO_DIR "ux0:data/VitaJPOverlay/psp"
#define VJO_PSP_INFO_MAX 512u
/* Formats the description: its length, or -1 when it does not fit or a field
 * holds a line break or a disc ID that vjo_game_id_valid refuses. */
int vjo_psp_info_format(char *out, size_t cap, const char *disc, const char *title, const char *path);
/* Splits a description read into a NUL-terminated buffer, in place, at its
 * line feeds (the app writes no CR): its disc ID line (check it with
 * vjo_game_id_valid), and in *title and *path the next two ("" when missing). */
char *vjo_psp_info_lines(char *text, char **title, char **path);
#endif
