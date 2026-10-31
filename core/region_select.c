#include "region_select.h"

#include <string.h>

/* Stick deflection, beyond the dead zone, that counts as full: both ends of
 * the 0..255 range reach full speed (the centre is 128). */
#define STICK_RANGE (127 - VJO_RSEL_DEAD)
#define PAD_HOLD_MAX_US 60000000u
static int32_t clamp_q8(int32_t v, int size)
{
    if (v < 0)
        return 0;
    if (v > (size << 8) - 1)
        return (size << 8) - 1;
    return v;
}

static int clamp_px(int v, int size)
{
    return v < 0 ? 0 : v >= size ? size - 1 : v;
}

void vjo_rsel_begin(VjoRegionSelect *s, int64_t now_us)
{
    memset(s, 0, sizeof(*s));
    s->x = (VJO_RSEL_W / 2) << 8 | 128;
    s->y = (VJO_RSEL_H / 2) << 8 | 128;
    s->last_us = now_us;
}

/* One stick axis's move this step, 1/256 px. dt16: the step in 16 us units
 * (at most VJO_RSEL_MAX_STEP_US / 16 = 3,125), so every product fits 32 bits. */
static int32_t stick_move(int v, uint32_t held_us, uint32_t dt16)
{
    int d = v - 128, m = d < 0 ? -d : d;
    uint32_t t, speed, ramp;
    if (m <= VJO_RSEL_DEAD)
        return 0;
    m -= VJO_RSEL_DEAD;
    if (m > STICK_RANGE)
        m = STICK_RANGE;
    t = (uint32_t)m * 256u / STICK_RANGE; /* deflection, 1/256 */
    /* px/s in 1/256 px: at most 600 * 256 = 153,600 */
    speed = VJO_RSEL_SLOW * 256u + (VJO_RSEL_FAST - VJO_RSEL_SLOW) * t * t / 256u;
    ramp = held_us >= VJO_RSEL_RAMP_MS * 1000u ? 256u : held_us * 256u / (VJO_RSEL_RAMP_MS * 1000u);
    speed = speed * (256u + (VJO_RSEL_ACCEL - 1) * ramp * t / 256u) / 256u;
    /* speed * dt16 <= 307,200 * 3,125 < 2^32; 62,500 x 16 us = 1 s */
    return (int32_t)(speed * dt16 / 62500u) * (d < 0 ? -1 : 1);
}

/* The pixels a d-pad direction has moved after being held for us: one at the
 * press, then the repeat. */
static uint32_t pad_steps(uint32_t us)
{
    uint32_t ms = us / 1000u, n = 1;
    if (ms > VJO_RSEL_REPEAT_MS) {
        ms -= VJO_RSEL_REPEAT_MS;
        n += (ms < 1000u ? ms : 1000u) * VJO_RSEL_REPEAT / 1000u;
        if (ms > 1000u)
            n += (ms - 1000u) * VJO_RSEL_REPEAT_FAST / 1000u;
    }
    return n;
}

int vjo_rsel_update(VjoRegionSelect *s, const VjoRselInput *in)
{
    int64_t step = in->now_us - s->last_us;
    uint32_t dt = step <= 0 ? 0u : step > VJO_RSEL_MAX_STEP_US ? VJO_RSEL_MAX_STEP_US : (uint32_t)step;
    int ev = VJO_RSEL_NONE;
    s->last_us = in->now_us;

    if (!in->touch) {
        if (s->touching == 1 && s->anchored)
            ev = VJO_RSEL_LIFT;
        s->touching = 0;
    } else if (s->touching != 2) {
        int tx = clamp_px(in->tx, VJO_RSEL_W), ty = clamp_px(in->ty, VJO_RSEL_H);
        if (!s->touching) { /* a new drag: the first corner where it starts */
            s->touching = 1;
            s->anchored = 1;
            s->ax = tx;
            s->ay = ty;
            ev = VJO_RSEL_CORNER;
        }
        s->x = tx << 8 | 128;
        s->y = ty << 8 | 128;
    }

    if (s->touching == 1) {
        /* the finger has the cursor; the d-pad and the stick wait for it */
        s->pad_on = 0;
        s->stick_us = 0;
    } else {
        for (int i = 0; i < 4; i++) {
            uint32_t bit = VJO_RSEL_UP << i, n;
            if (!(in->held & bit)) {
                s->pad_on &= ~bit;
                continue;
            }
            if (in->pressed & bit) {
                s->pad_on |= bit;
                s->pad_us[i] = 0;
                s->pad_steps[i] = 0;
            } else if (s->pad_on & bit) {
                if (s->pad_us[i] < PAD_HOLD_MAX_US) /* no wrap: a minute of repeat is the end */
                    s->pad_us[i] += dt;
            } else {
                continue; /* held since before the selection began */
            }
            n = pad_steps(s->pad_us[i]);
            if (n != s->pad_steps[i]) {
                /* whole pixels: the cursor snaps to the moved axis's grid */
                int d = (int)(n - s->pad_steps[i]) * (i == 0 || i == 3 ? -1 : 1);
                s->pad_steps[i] = n;
                if (i & 1)
                    s->x = (clamp_px((s->x >> 8) + d, VJO_RSEL_W) << 8) | 128;
                else
                    s->y = (clamp_px((s->y >> 8) + d, VJO_RSEL_H) << 8) | 128;
            }
        }
        {
            int dx = (int)in->lx - 128, dy = (int)in->ly - 128;
            if (dx < 0)
                dx = -dx;
            if (dy < 0)
                dy = -dy;
            if (dx <= VJO_RSEL_DEAD && dy <= VJO_RSEL_DEAD) {
                s->stick_us = 0;
            } else {
                s->stick_us += dt;
                if (s->stick_us > VJO_RSEL_RAMP_MS * 1000u)
                    s->stick_us = VJO_RSEL_RAMP_MS * 1000u;
                s->x = clamp_q8(s->x + stick_move(in->lx, s->stick_us, dt >> 4), VJO_RSEL_W);
                s->y = clamp_q8(s->y + stick_move(in->ly, s->stick_us, dt >> 4), VJO_RSEL_H);
            }
        }
    }

    if (in->pressed & in->cancel) {
        if (!s->anchored)
            return VJO_RSEL_CANCEL;
        s->anchored = 0;
        if (s->touching)
            s->touching = 2; /* the finger still down must not start a drag again */
        return VJO_RSEL_BACK;
    }
    if (in->pressed & in->enter) {
        int x, y, w, h;
        if (!s->anchored) {
            s->anchored = 1;
            s->ax = s->x >> 8;
            s->ay = s->y >> 8;
            return VJO_RSEL_CORNER;
        }
        vjo_rsel_box(s, &x, &y, &w, &h);
        return w >= VJO_RSEL_MIN && h >= VJO_RSEL_MIN ? VJO_RSEL_SAVE : VJO_RSEL_SMALL;
    }
    return ev;
}

int vjo_rsel_preset(VjoRegionSelect *s, uint32_t rows)
{
    if (rows - 1u >= VJO_RSEL_H) /* 0, or more than the screen */
        return 0;
    s->anchored = 1;
    s->ax = 0;
    s->ay = VJO_RSEL_H - (int)rows;
    s->x = (VJO_RSEL_W - 1) << 8 | 128;
    s->y = (VJO_RSEL_H - 1) << 8 | 128;
    return 1;
}

void vjo_rsel_publish(uint32_t *suggest, uint32_t rows)
{
    uint32_t was = __atomic_load_n(suggest, __ATOMIC_RELAXED);
    while (was != VJO_RSEL_TAKEN &&
           !__atomic_compare_exchange_n(suggest, &was, rows, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}

uint32_t vjo_rsel_take(uint32_t *suggest)
{
    uint32_t rows = __atomic_load_n(suggest, __ATOMIC_RELAXED);
    if (rows - 1u >= VJO_RSEL_H || /* none, or taken */
        !__atomic_compare_exchange_n(suggest, &rows, VJO_RSEL_TAKEN, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        return 0;
    return rows;
}

void vjo_rsel_cursor(const VjoRegionSelect *s, int *x, int *y)
{
    *x = s->x >> 8;
    *y = s->y >> 8;
}

int vjo_rsel_box(const VjoRegionSelect *s, int *x, int *y, int *w, int *h)
{
    int cx = s->x >> 8, cy = s->y >> 8;
    if (!s->anchored)
        return 0;
    *x = cx < s->ax ? cx : s->ax;
    *y = cy < s->ay ? cy : s->ay;
    *w = (cx < s->ax ? s->ax - cx : cx - s->ax) + 1;
    *h = (cy < s->ay ? s->ay - cy : cy - s->ay) + 1;
    return 1;
}

static uint16_t norm_up(int v, int size)
{
    return (uint16_t)(((uint32_t)v * 65536u + (uint32_t)size - 1u) / (uint32_t)size);
}

static uint16_t norm_down(int v, int size)
{
    uint32_t n = (uint32_t)v * 65536u / (uint32_t)size;
    return (uint16_t)(n > 65535u ? 65535u : n);
}

VjoRect vjo_rsel_rect(int x, int y, int w, int h)
{
    VjoRect r;
    x = clamp_px(x, VJO_RSEL_W);
    y = clamp_px(y, VJO_RSEL_H);
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    if (w > VJO_RSEL_W - x)
        w = VJO_RSEL_W - x;
    if (h > VJO_RSEL_H - y)
        h = VJO_RSEL_H - y;
    r.x = norm_up(x, VJO_RSEL_W);
    r.y = norm_up(y, VJO_RSEL_H);
    r.w = norm_down(w, VJO_RSEL_W);
    r.h = norm_down(h, VJO_RSEL_H);
    return r;
}
