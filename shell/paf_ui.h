/* Shared by the paf pages (overlay.cpp, strip.cpp): screen geometry and
 * paf::ui::Text helpers. paf layout: origin at the screen centre, y up. */
#ifndef VJO_PAF_UI_H
#define VJO_PAF_UI_H

#include <limits> /* before paf.h: its vector uses numeric_limits */
#include <paf.h>
#include <psp2/kernel/clib.h>

extern "C" {
#include "../core/styled.h"
#include "../core/colour_scheme.h"
#include "shell.h"
}

#define SCREEN_W 960
#define SCREEN_H 544
#define BOX_W 920.0f /* text width: the screen less the side margins */

/* paf::math::v2/v4 keep their floats private. */
static __attribute__((noinline)) paf::math::v2 v2f(float x, float y)
{
    paf::math::v2 v;
    float f[2] = {x, y};
    sceClibMemcpy(&v, f, sizeof(f));
    return v;
}

static inline float v2_get(const paf::math::v2 &v, int k)
{
    float f[2];
    sceClibMemcpy(f, &v, sizeof(f));
    return f[k];
}

/* One implementation in overlay.cpp, shared by text, highlights, surfaces and the subtitle strip. */
paf::math::v4 rgba(uint32_t rgb, float alpha = 1.0f);
void vjo_colour_page(paf::ui::Scene *page);
paf::ui::Widget *vjo_paf_child(paf::ui::Scene *page, const char *name);
void vjo_paf_pos(paf::ui::Widget *w, float x, float y);
void vjo_paf_size(paf::ui::Widget *w, float width, float height);

static inline void text_set(paf::ui::Text *w, const VjoStyled *st)
{
    if (!w)
        return;
    w->SetString(paf::wstring((const wchar_t *)st->text, st->len));
    /* Text past the semantic spans (a clipped kanji block) still needs the theme's base colour. */
    w->SetStyleAttribute(paf::graph::TextStyleAttribute_Color, 0, st->len, rgba(VJO_RGB_TEXT));
    for (int i = 0; i < st->n_spans; i++) {
        const VjoSpan *sp = &st->spans[i];
        /* Point takes a v2 (glyph width, height), as in GrapheneCt's NetStream */
        w->SetStyleAttribute(paf::graph::TextStyleAttribute_Point, sp->start, sp->len,
                             v2f((float)sp->px, (float)sp->px));
        w->SetStyleAttribute(paf::graph::TextStyleAttribute_Color, sp->start, sp->len, rgba(sp->rgb));
    }
}

/* Laid-out size of a Text's content; 0 if not laid out yet. */
static __attribute__((noinline)) float text_bound(paf::ui::Text *w, int k)
{
    paf::math::v2 b;
    if (!w || w->GetBounds(b) < 0)
        return 0.0f;
    return v2_get(b, k);
}

static inline float text_height(paf::ui::Text *w)
{
    return text_bound(w, 1);
}

static __attribute__((noinline)) void font_px(int *ja, int *en)
{
    vjo_view_lock();
    *ja = vjo_font_px(g_view.font_size_ja);
    *en = vjo_font_px(g_view.font_size_en);
    vjo_view_unlock();
}

/* overlay.cpp: this frame's step of a paf text's re-wrap (wrap_fix.h): the
 * unit to put a '\n' before, else 0. */
struct VjoWrap;
uint32_t vjo_wrap_paf(VjoWrap *w, paf::ui::Text *t, const uint16_t *u, uint32_t n, uint32_t room);

/* strip.cpp: the subtitles strip. One frame: the page is open while want
 * (closed otherwise) and rendered on a new strip_version. */
void vjo_strip_frame(paf::Plugin *plugin, int want, unsigned version);
int vjo_strip_is_open(void);

#endif
