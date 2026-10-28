/* One game's native hook definitions, loaded before any hook is installed.
 * The disk archive's index is searched by the kernel; only this title's blob is
 * copied to the game. No file access or allocation occurs in a text hook. */
#ifndef VJO_HOOK_PROFILE_H
#define VJO_HOOK_PROFILE_H
#include "hook_luna.h"
#include "hook_place.h"
#include "hook_ops.h"
#include "hook_profile_file.h"

#define VJO_PROFILE_LUNA_MAX 16u
#define VJO_PROFILE_VERIFIED_MAX 8u
#define VJO_PROFILE_HEADER 72u
#define VJO_PROFILE_POST 256u

typedef struct {
    uint8_t raw[VJO_PROFILE_BYTES];
    VjoLunaSite luna[VJO_PROFILE_LUNA_MAX];
    VjoPlaceVerified verified[VJO_PROFILE_VERIFIED_MAX];
    unsigned nl, nv, nf, no, filters, ops, strings;
} VjoHookProfile;

/* p->raw already holds n bytes. Refused files publish no profile. The caller
 * owns p until all hooks have stopped; never replace a profile under live hooks. */
int vjo_hook_profile_parse(VjoHookProfile *p, const char *title, unsigned n);
const VjoHookProfile *vjo_hook_profile_current(void);
void vjo_hook_profile_clear(void);
unsigned vjo_hook_profile_filter(unsigned id); /* wire offset, 0: absent */
int vjo_hook_profile_op(unsigned offset, LunaOp *out);
uint32_t vjo_hook_profile_checksum(const uint8_t *p, unsigned n);
int vjo_hook_profile_revision(uint8_t out[32]);

/* Build-time catalogue access, excluded from the shipped worker. IDs below
 * 256 name filters; VJO_PROFILE_POST + func names a capture postprocessor. */
#ifndef VJO_EXTERNAL_HOOK_PROFILES
const LunaOp *vjo_hook_profile_export_ops(unsigned id);
#endif
#endif
