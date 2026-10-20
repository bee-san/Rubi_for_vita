/* core/hook_select.c (docs/native-hooks.md): which text a job shows, OCR's or a hook stream's. The
 * similarity on its own (the compared form, containment, coverage, short and empty texts, texts longer
 * than the comparison keeps, a brute-force reference on random strings), then the selection's state
 * machine job by job: the automatic pick's agreement, lock and unlock, a pinned stream, OCR only, ties
 * and many candidates. The texts are synthetic lines shaped like visual-novel dialogue; no game's text. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "acutest.h"
#include "hook_select.h"
#include "../../include/vjo_game_ocr.h"

static VjoHookCand cand(uint32_t id, uint32_t seq, uint32_t site, const char *text)
{
    VjoHookCand c = {id, seq, site, text, text ? strlen(text) : 0};
    return c;
}

static unsigned sim(const char *o, const char *h)
{
    return vjo_hook_similarity(o, strlen(o), h, strlen(h));
}

/* One job's two calls as the game worker makes them: before recognition, then (when no stream answered)
 * after it with OCR's text. Returns the stream whose text the job shows, 0 for OCR's. */
static uint32_t job(VjoHookSelect *st, uint32_t pick, const char *ocr, const VjoHookCand *c, unsigned n,
                    unsigned *score, uint32_t *flags)
{
    uint32_t id = vjo_hook_select_before(st, pick, c, n, flags);
    *score = 0;
    if (id) return id;
    return vjo_hook_select_after(st, pick, ocr, strlen(ocr), c, n, score, flags);
}

static const char LINE1[] = "「明日の朝、駅前の公園でもう一度会おうよ」";
static const char LINE2[] = "「そんなこと急に言われても、困るんだけどな」";
static const char LINE3[] = "「だったら今夜のうちに準備しておけばいいじゃない」";
static const char LINE4[] = "「わかった、じゃあ七時に迎えに行くからね」";

/* ---- the similarity ---- */

static void exact_text(void)
{
    TEST_CHECK(sim(LINE1, LINE1) == 1000);
    /* the compared form: full-width ASCII, punctuation, spaces and line breaks */
    TEST_CHECK(sim("えっ！？　本当に…？", "えっ!? 本当に...?") == 1000);
    TEST_CHECK(sim("ＣＤを三枚買った", "CDを三枚買った") == 1000);
    TEST_CHECK(sim("ＣＤを三枚買った", "cdを三枚買った") == 1000);
    TEST_CHECK(sim("今日は\nいい天気だね", "今日はいい天気だね") == 1000);
    TEST_CHECK(sim("『嘘でしょ』――と彼女は言った。", "嘘でしょと彼女は言った") == 1000);
    TEST_CHECK(sim("（ここは……どこだろう？）", "ここはどこだろう") == 1000);
    TEST_CHECK(sim("【健太】「行くぞ！」〜♪", "健太行くぞ") == 1000);
    /* half-width katakana (CP932's single bytes) with their voiced marks, as full-width */
    TEST_CHECK(sim("ガッコウヘイクヨ", "ｶﾞｯｺｳﾍ ｲｸﾖ") == 1000);
    TEST_CHECK(sim("パンとヴァイオリン", "ﾊﾟﾝとｳﾞｧｲｵﾘﾝ") == 1000);
    TEST_CHECK(sim("ガッコウヘイクヨ", "ｶｯｺｳﾍｲｸﾖ") == 875); /* without the mark: one character differs */
    /* the long vowel mark read as the kanji one (OCR's common confusion) */
    TEST_CHECK(sim("コ一ヒ一を飲もうか", "コーヒーを飲もうか") == 1000);
    /* ・ in a name, the spacing voiced marks */
    TEST_CHECK(sim("ジョン・スミスです", "ジョンスミスです") == 1000);
}

static void wrong_characters(void)
{
    /* 20 characters; OCR misreads some of them */
    const char *h = "今夜は満月だから外に出て星を眺めよう";
    unsigned n = 0, s;
    for (const char *p = h; *p; p++) n += ((unsigned char)*p & 0xC0) != 0x80;
    TEST_CHECK(n == 18);
    s = sim("今夜は満月だから外に出て星を眺めよう", h);
    TEST_CHECK(s == 1000);
    s = sim("今夜は満月だから外に山て星を眺めよラ", h); /* two substitutions: 16 of 18 */
    TEST_CHECK_(s == 888, "%u", s);
    TEST_CHECK(s >= VJO_HOOK_AGREE);
    s = sim("今夜は満月だから外出て星を眺めよう。", h); /* one character lost */
    TEST_CHECK_(s == 944, "%u", s);
    /* one character read twice: o is 19, and the extra one is an edit too: 17 of 19 */
    s = sim("今夜はは満月だから外に出て星を眺めよう", h);
    TEST_CHECK_(s == 894, "%u", s);
    /* at the threshold: 5 wrong of 18 is 722, 6 wrong is 666 */
    s = sim("今夜は満月たから外に山て星を跳めよラ", h);
    TEST_CHECK_(s == 777, "%u", s);
    s = sim("今夜は満目たから外に山て星を跳めよラ", h);
    TEST_CHECK_(s == 722, "%u", s);
    TEST_CHECK(s >= VJO_HOOK_AGREE);
    s = sim("令夜は満目たから外に山て星を跳めよラ", h);
    TEST_CHECK_(s == 666, "%u", s);
    TEST_CHECK(s < VJO_HOOK_AGREE);
}

static void name_and_line(void)
{
    /* OCR's region holds the speaker's name too: h is found whole inside it */
    const char *line = "そろそろ帰らないと母さんに怒られるよ";
    unsigned s = sim("健太\nそろそろ帰らないと母さんに怒られるよ", line);
    TEST_CHECK_(s == 900, "%u", s); /* 18 of 20 */
    TEST_CHECK(sim("【健太】「そろそろ帰らないと母さんに怒られるよ」", line) == s);
    /* the name's characters lower it as much as they are long: a long name and a short line do not agree */
    s = sim("佐々木健太郎\nもう帰ろう", "もう帰ろう");
    TEST_CHECK_(s == 454, "%u", s);
    TEST_CHECK(s < VJO_HOOK_AGREE);
    /* and the reverse: the stream holds the name, OCR the name and the line */
    s = sim("健太\nそろそろ帰らないと母さんに怒られるよ", "健太");
    TEST_CHECK(s == 0); /* two characters: never */
    s = sim("佐々木健太郎\nそろそろ帰らないと母さんに怒られるよ", "佐々木健太郎");
    TEST_CHECK_(s == 250, "%u", s);
}

static void backlog_and_prefetch(void)
{
    char page[256];
    /* the previous line (backlog) or the next one (prepared ahead of the screen) against the shown one */
    TEST_CHECK(sim(LINE2, LINE1) < 300);
    TEST_CHECK(sim(LINE2, LINE3) < 300);
    TEST_CHECK(sim(LINE3, LINE4) < 300);
    /* a page with the previous line still on screen: neither the backlog line nor the newest line alone
     * agrees (showing either would lose the other's text) */
    snprintf(page, sizeof(page), "%s\n%s", LINE1, LINE2);
    TEST_CHECK(sim(page, LINE1) < VJO_HOOK_AGREE);
    TEST_CHECK(sim(page, LINE2) < VJO_HOOK_AGREE);
    /* a stream holding the whole page while the screen shows its last line: neither */
    TEST_CHECK(sim(LINE2, page) < VJO_HOOK_AGREE);
    /* the automatic pick keeps OCR's text and starts no streak */
    {
        VjoHookSelect st = {0};
        VjoHookCand c[2] = {cand(9, 1, 0, LINE1), cand(10, 2, 0, LINE3)};
        unsigned score;
        uint32_t flags;
        TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, LINE2, c, 2, &score, &flags) == 0);
        TEST_CHECK(flags == 0 && score < 300 && st.streak == 0 && st.streak_id == 0 && st.locked == 0);
    }
}

static void short_strings(void)
{
    TEST_CHECK(VJO_HOOK_MIN_CHARS == 4);
    TEST_CHECK(sim("はいはい", "はいは") == 0);       /* h: 3 characters */
    TEST_CHECK(sim("はいは", "はいは") == 0);
    TEST_CHECK(sim("はい、はい。", "はいはい") == 1000); /* 4: enough */
    TEST_CHECK(sim("はいは", "はいはい") == 0);        /* o: 3 characters */
    TEST_CHECK(sim("ありがとう、また明日ね", "ありがとう") == 500); /* 5 of 10 */
    TEST_CHECK(sim("……！？", "……！？") == 0);       /* nothing left to compare */
    TEST_CHECK(sim("Ａ・Ｂ・Ｃ", "ABC") == 0);
}

static void empty_texts(void)
{
    VjoHookSelect st = {0}, zero = {0};
    VjoHookCand c[1] = {cand(9, 1, 0, LINE1)};
    unsigned score = 77;
    uint32_t flags = 77;
    TEST_CHECK(sim("", "") == 0);
    TEST_CHECK(sim(LINE1, "") == 0);
    TEST_CHECK(sim("", LINE1) == 0);
    TEST_CHECK(vjo_hook_similarity(NULL, 0, LINE1, strlen(LINE1)) == 0);
    TEST_CHECK(vjo_hook_similarity(LINE1, strlen(LINE1), NULL, 0) == 0);
    TEST_CHECK(vjo_hook_similarity(LINE1, 0, LINE1, strlen(LINE1)) == 0);
    /* a NUL ends a text before its length */
    TEST_CHECK(vjo_hook_similarity("今日は晴れ\0だね", 20, "今日は晴れ", strlen("今日は晴れ")) == 1000);
    /* no candidates */
    TEST_CHECK(vjo_hook_select_before(&st, VJO_HOOK_PICK_AUTO, NULL, 0, &flags) == 0 && flags == 0);
    TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_AUTO, LINE1, strlen(LINE1), c, 0, &score, &flags) == 0);
    TEST_CHECK(score == 0 && flags == 0 && !memcmp(&st, &zero, sizeof(st)));
    /* a candidate without text is none */
    c[0] = cand(9, 1, 0, "");
    TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_AUTO, LINE1, strlen(LINE1), c, 1, &score, &flags) == 0);
    c[0] = cand(9, 1, 0, NULL);
    TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_AUTO, LINE1, strlen(LINE1), c, 1, &score, &flags) == 0);
    TEST_CHECK(!memcmp(&st, &zero, sizeof(st)));
    /* OCR read nothing: no evidence, so a locked stream misses nothing and nothing is chosen */
    st.locked = 9; st.locked_seq = 4; st.streak_id = 9; st.streak = 3; st.misses = 1;
    zero = st;
    c[0] = cand(9, 4, 0, LINE1);
    TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_AUTO, "", 0, c, 1, &score, &flags) == 0);
    TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_AUTO, "……", strlen("……"), c, 1, &score, &flags) == 0);
    TEST_CHECK(score == 0 && flags == 0 && !memcmp(&st, &zero, sizeof(st)));
    /* NULL state or candidates */
    TEST_CHECK(vjo_hook_select_before(NULL, 9, c, 1, &flags) == 0 && flags == 0);
    TEST_CHECK(vjo_hook_select_after(NULL, VJO_HOOK_PICK_AUTO, LINE1, strlen(LINE1), c, 1, &score, &flags) == 0);
    TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_AUTO, LINE1, strlen(LINE1), NULL, 3, &score, &flags) == 0);
    /* NULL out-parameters are allowed */
    TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_AUTO, LINE1, strlen(LINE1), c, 1, NULL, NULL) == 9);
}

/* ---- the comparison against a brute-force reference ---- */

static unsigned ref_distance(const unsigned *o, unsigned m, const unsigned *h, unsigned n)
{
    static unsigned d[64][64]; /* d[i][j]: h's first i characters against o's part ending at j */
    unsigned best = ~0u;
    for (unsigned j = 0; j <= m; j++) d[0][j] = 0;
    for (unsigned i = 1; i <= n; i++) {
        d[i][0] = i;
        for (unsigned j = 1; j <= m; j++) {
            unsigned v = d[i - 1][j - 1] + (h[i - 1] != o[j - 1]);
            if (d[i - 1][j] + 1 < v) v = d[i - 1][j] + 1;
            if (d[i][j - 1] + 1 < v) v = d[i][j - 1] + 1;
            d[i][j] = v;
        }
    }
    for (unsigned j = 0; j <= m; j++)
        if (d[n][j] < best) best = d[n][j];
    return best;
}

static void random_against_reference(void)
{
    static const char *const kana[] = {"あ", "い", "う", "か", "き", "の", "、", "ー"};
    unsigned agreed = 0, below = 0;
    srand(12345);
    for (unsigned t = 0; t < 3000; t++) {
        unsigned ok[60], o[60], h[60], m = 0, n = 0, lo = (unsigned)rand() % 40, lh = (unsigned)rand() % 30;
        unsigned from = lo ? (unsigned)rand() % lo : 0;
        char os[400] = "", hs[400] = "";
        for (unsigned i = 0; i < lo; i++) {
            ok[i] = (unsigned)rand() % 8;
            strcat(os, kana[ok[i]]);
            if (ok[i] != 6) o[m++] = ok[i]; /* 、 is dropped */
        }
        if (t % 3 && lh > lo - from) lh = lo - from;
        for (unsigned i = 0; i < lh; i++) {
            /* two pairs in three: h copies o from `from` on, a character in five changed */
            unsigned k = t % 3 && from + i < lo && rand() % 5 ? ok[from + i] : (unsigned)rand() % 8;
            strcat(hs, kana[k]);
            if (k != 6) h[n++] = k;
        }
        unsigned want = 0, top = n > m ? n : m;
        if (n >= VJO_HOOK_MIN_CHARS && m >= VJO_HOOK_MIN_CHARS) {
            unsigned d = ref_distance(o, m, h, n);
            want = d >= n ? 0 : (n - d) * 1000u / top;
        }
        unsigned got = sim(os, hs);
        TEST_CHECK_(got == want, "pair %u: %u, the reference %u (o %u, h %u characters)", t, got, want, m, n);
        if (got >= VJO_HOOK_AGREE) agreed++;
        else if (got) below++;
    }
    /* both sides of the threshold are covered */
    TEST_CHECK_(agreed > 100 && below > 1000, "%u agreed, %u below", agreed, below);
}

static void longer_than_compared(void)
{
    /* 300 characters each: the comparison keeps 256 of each and counts the rest of h as unmatched, so
     * a cut text never scores higher than it would whole */
    static char a[2048], b[2048], tail[2048];
    a[0] = b[0] = tail[0] = '\0';
    for (unsigned i = 0; i < 300; i++) strcat(a, i % 2 ? "か" : "き");
    TEST_CHECK_(sim(a, a) == 853, "%u", sim(a, a)); /* 256 of 300 */
    TEST_CHECK(sim(a, a) >= VJO_HOOK_AGREE);
    /* past character 256 they differ: no higher */
    strcpy(b, a);
    memcpy(b + 3 * 280, "あ", 3);
    TEST_CHECK(sim(b, a) <= sim(a, a));
    /* h past what is compared of o (o's characters 300-339): not found, conservatively */
    for (unsigned i = 0; i < 40; i++) strcat(tail, "の");
    strcat(b, tail);
    TEST_CHECK(sim(b, tail) == 0);
}

/* ---- the selection, job by job ---- */

static void lock_and_unlock(void)
{
    VjoHookSelect st = {0};
    VjoHookCand c[2];
    unsigned score;
    uint32_t flags;
    c[1] = cand(10, 1, 0, "セーブしますか");
    /* job 1: the stream agrees with OCR: its text is shown, not yet locked */
    c[0] = cand(9, 2, 0, LINE1);
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, LINE1, c, 2, &score, &flags) == 9);
    TEST_CHECK(flags == VJO_HOOK_R_AGREED && score == 1000);
    TEST_CHECK(st.locked == 0 && st.streak_id == 9 && st.streak == 1);
    /* job 2: new text, still not locked, so OCR reads again; the second agreement in a row locks it */
    c[0] = cand(9, 3, 0, LINE2);
    TEST_CHECK(vjo_hook_select_before(&st, VJO_HOOK_PICK_AUTO, c, 2, &flags) == 0 && flags == 0);
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, "「そんなこと急に言われても、困るんだけとな」", c, 2, &score, &flags) == 9);
    TEST_CHECK(flags == (VJO_HOOK_R_AGREED | VJO_HOOK_R_LOCKED) && score >= VJO_HOOK_AGREE && score < 1000);
    TEST_CHECK(st.locked == 9 && st.locked_seq == 3 && st.streak == 2 && st.misses == 0);
    /* job 3: new text in the locked stream: it answers without OCR */
    c[0] = cand(9, 4, 0, LINE3);
    TEST_CHECK(vjo_hook_select_before(&st, VJO_HOOK_PICK_AUTO, c, 2, &flags) == 9);
    TEST_CHECK(flags == (VJO_HOOK_R_LOCKED | VJO_HOOK_R_NO_OCR) && st.locked_seq == 4);
    /* job 4: the screen changed but the stream did not (a choice menu): OCR reads, the stream disagrees */
    TEST_CHECK(vjo_hook_select_before(&st, VJO_HOOK_PICK_AUTO, c, 2, &flags) == 0 && flags == 0);
    TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_AUTO, "どこへ行く？\n学校\n公園", strlen("どこへ行く？\n学校\n公園"),
                                     c, 2, &score, &flags) == 0);
    TEST_CHECK(flags == 0 && st.locked == 9 && st.misses == 1 && st.streak == 0);
    /* job 5: the next agreement keeps the lock and forgets the miss */
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, LINE3, c, 2, &score, &flags) == 9);
    TEST_CHECK(flags == (VJO_HOOK_R_AGREED | VJO_HOOK_R_LOCKED) && st.misses == 0 && st.locked_seq == 4);
    /* jobs 6, 7: two disagreements in a row unlock it */
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, LINE4, c, 2, &score, &flags) == 0);
    TEST_CHECK(flags == 0 && st.locked == 9 && st.misses == 1);
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, LINE4, c, 2, &score, &flags) == 0);
    TEST_CHECK(flags == VJO_HOOK_R_UNLOCK && st.locked == 0 && st.locked_seq == 0 && st.misses == 0);
    /* unlocked: new text no longer answers without OCR */
    c[0] = cand(9, 5, 0, LINE4);
    TEST_CHECK(vjo_hook_select_before(&st, VJO_HOOK_PICK_AUTO, c, 2, &flags) == 0 && flags == 0);
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, LINE4, c, 2, &score, &flags) == 9 && flags == VJO_HOOK_R_AGREED);
    /* a locked stream absent from a job (its slot was busy) misses nothing */
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, LINE4, c, 2, &score, &flags) == 9 && st.locked == 9);
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, LINE1, c + 1, 1, &score, &flags) == 0);
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, LINE1, c + 1, 1, &score, &flags) == 0);
    TEST_CHECK(st.locked == 9 && st.misses == 0);
}

static void locked_nothing_new(void)
{
    VjoHookSelect st = {9, 5, 9, 2, 0};
    VjoHookCand c[1] = {cand(9, 5, 1, LINE1)};
    unsigned score;
    uint32_t flags;
    /* the same seq as the last job shown: run OCR */
    TEST_CHECK(vjo_hook_select_before(&st, VJO_HOOK_PICK_AUTO, c, 1, &flags) == 0 && flags == 0);
    /* OCR still shows that line: the stream's text, agreed and locked */
    TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_AUTO, "明日の朝、駅前の公園でもう一度会おうよ",
                                     strlen("明日の朝、駅前の公園でもう一度会おうよ"), c, 1, &score, &flags) == 9);
    TEST_CHECK(flags == (VJO_HOOK_R_AGREED | VJO_HOOK_R_LOCKED | VJO_HOOK_R_SITE) && score == 1000);
    TEST_CHECK(st.locked == 9 && st.locked_seq == 5 && st.streak == 3 && st.misses == 0);
}

static void locked_new_text(void)
{
    VjoHookSelect st = {9, 5, 9, 2, 1};
    VjoHookCand c[2] = {cand(3, 7, 1, LINE1), cand(9, 6, 0, LINE2)};
    uint32_t flags;
    TEST_CHECK(vjo_hook_select_before(&st, VJO_HOOK_PICK_AUTO, c, 2, &flags) == 9);
    TEST_CHECK(flags == (VJO_HOOK_R_LOCKED | VJO_HOOK_R_NO_OCR));
    /* only the seq moved; the misses wait for a job that runs OCR */
    TEST_CHECK(st.locked == 9 && st.locked_seq == 6 && st.misses == 1 && st.streak == 2);
    /* the next job: nothing new again */
    TEST_CHECK(vjo_hook_select_before(&st, VJO_HOOK_PICK_AUTO, c, 2, &flags) == 0);
    /* a site that is locked answers with VJO_HOOK_R_SITE */
    st.locked = 3;
    TEST_CHECK(vjo_hook_select_before(&st, VJO_HOOK_PICK_AUTO, c, 2, &flags) == 3);
    TEST_CHECK(flags == (VJO_HOOK_R_LOCKED | VJO_HOOK_R_NO_OCR | VJO_HOOK_R_SITE) && st.locked_seq == 7);
}

static void pinned_stream(void)
{
    VjoHookSelect st = {9, 2, 9, 2, 1}, was = st;
    VjoHookCand c[2] = {cand(3, 4, 1, LINE3), cand(9, 5, 0, LINE2)};
    unsigned score;
    uint32_t flags;
    /* pinned with text: that text, without OCR, whatever OCR would read, and no state change */
    TEST_CHECK(vjo_hook_select_before(&st, 3, c, 2, &flags) == 3);
    TEST_CHECK(flags == (VJO_HOOK_R_PINNED | VJO_HOOK_R_NO_OCR | VJO_HOOK_R_SITE));
    TEST_CHECK(vjo_hook_select_before(&st, 9, c, 2, &flags) == 9 && flags == (VJO_HOOK_R_PINNED | VJO_HOOK_R_NO_OCR));
    TEST_CHECK(!memcmp(&st, &was, sizeof(st)));
    /* pinned without text: OCR, and OCR's text kept even where another stream agrees */
    TEST_CHECK(vjo_hook_select_before(&st, 4, c, 2, &flags) == 0 && flags == 0);
    TEST_CHECK(vjo_hook_select_after(&st, 4, LINE2, strlen(LINE2), c, 2, &score, &flags) == 0);
    TEST_CHECK(flags == 0 && score == 1000); /* the best stream's similarity, for the log */
    TEST_CHECK(!memcmp(&st, &was, sizeof(st)));
    /* after() with the pinned stream among the candidates: its text, agreeing or not */
    TEST_CHECK(vjo_hook_select_after(&st, 3, LINE2, strlen(LINE2), c, 2, &score, &flags) == 3);
    TEST_CHECK(flags == (VJO_HOOK_R_PINNED | VJO_HOOK_R_SITE) && score < VJO_HOOK_AGREE);
    TEST_CHECK(vjo_hook_select_after(&st, 9, LINE2, strlen(LINE2), c, 2, &score, &flags) == 9);
    TEST_CHECK(flags == (VJO_HOOK_R_PINNED | VJO_HOOK_R_AGREED) && score == 1000);
    TEST_CHECK(!memcmp(&st, &was, sizeof(st)));
}

static void ocr_only(void)
{
    VjoHookSelect st = {9, 2, 9, 2, 1}, was = st;
    VjoHookCand c[1] = {cand(9, 5, 0, LINE2)};
    unsigned score;
    uint32_t flags;
    /* a locked stream with new text, an agreeing one: OCR only, its text kept, nothing changes */
    TEST_CHECK(vjo_hook_select_before(&st, VJO_HOOK_PICK_OCR, c, 1, &flags) == 0 && flags == 0);
    TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_OCR, LINE2, strlen(LINE2), c, 1, &score, &flags) == 0);
    TEST_CHECK(flags == 0 && score == 1000);
    TEST_CHECK(!memcmp(&st, &was, sizeof(st)));
    /* a pick that is neither AUTO, a stream ID nor OCR: as OCR */
    for (uint32_t pick = VJO_HOOK_STREAMS + 1u; pick < 0x200; pick += 37) {
        TEST_CHECK(vjo_hook_select_before(&st, pick, c, 1, &flags) == 0 && flags == 0);
        TEST_CHECK(vjo_hook_select_after(&st, pick, LINE2, strlen(LINE2), c, 1, &score, &flags) == 0 && flags == 0);
    }
    TEST_CHECK(!memcmp(&st, &was, sizeof(st)));
}

static void ties(void)
{
    VjoHookSelect st = {0};
    unsigned score;
    uint32_t flags;
    /* two callers copying the same line: the newer copy */
    VjoHookCand c[3] = {cand(9, 10, 0, LINE1), cand(12, 11, 0, LINE1), cand(15, 9, 0, LINE1)};
    TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_AUTO, LINE1, strlen(LINE1), c, 3, &score, &flags) == 12);
    /* then the streak's stream at equal scores, so the copies do not take turns and the lock comes */
    c[0].seq = 20;
    TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_AUTO, LINE1, strlen(LINE1), c, 3, &score, &flags) == 12);
    TEST_CHECK(flags == (VJO_HOOK_R_AGREED | VJO_HOOK_R_LOCKED) && st.locked == 12);
    /* a site over a caller at equal scores (no streak) */
    memset(&st, 0, sizeof(st));
    c[2] = cand(2, 1, 1, LINE1);
    TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_AUTO, LINE1, strlen(LINE1), c, 3, &score, &flags) == 2);
    TEST_CHECK(flags == (VJO_HOOK_R_AGREED | VJO_HOOK_R_SITE));
    /* seqs compare across a wrap of the clock */
    memset(&st, 0, sizeof(st));
    VjoHookCand w[2] = {cand(9, 0xFFFFFFF0u, 0, LINE1), cand(10, 3, 0, LINE1)};
    TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_AUTO, LINE1, strlen(LINE1), w, 2, &score, &flags) == 10);
    /* a higher score wins over every tie rule */
    memset(&st, 0, sizeof(st));
    st.streak_id = 9;
    st.streak = 1;
    VjoHookCand h[2] = {cand(9, 30, 1, "明日の朝、駅前の公園でもう一度会おう"), cand(10, 3, 0, LINE1)};
    TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_AUTO, LINE1, strlen(LINE1), h, 2, &score, &flags) == 10);
    TEST_CHECK(score == 1000 && st.streak_id == 10 && st.streak == 1);
    /* the locked stream when it agrees, even if another scores higher */
    st.locked = 9; st.locked_seq = 30; st.streak_id = 9; st.streak = 2;
    TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_AUTO, LINE1, strlen(LINE1), h, 2, &score, &flags) == 9);
    TEST_CHECK(flags == (VJO_HOOK_R_AGREED | VJO_HOOK_R_LOCKED | VJO_HOOK_R_SITE) && score == 944);
}

static void many_candidates(void)
{
    static const char *const junk[] = {
        "健太", "セーブしますか？", "data/script/day01.bin", "ロードしますか？", "設定", "オートモード",
        "スキップ", "「こんにちは」", "第一章　はじまりの朝", "体験版はここまでです", "既読スキップ",
        "ボイス音量", "テキスト速度", "BGM音量", "タイトルに戻る", "本当によろしいですか？", "はい",
        "いいえ", "メッセージウィンドウを消す"};
    const char *line = "「駅前の公園でもう一度会おうよ、約束だからね」";
    VjoHookCand c[VJO_HOOK_STREAMS + 4];
    unsigned n = 0, score, best = 0;
    uint32_t flags, best_id = 0;
    VjoHookSelect st = {0};
    for (unsigned i = 0; i < sizeof(junk) / sizeof(junk[0]); i++, n++) c[n] = cand(n + 1, 100 + n, n < 8, junk[i]);
    c[n] = cand(n + 1, 50, 0, LINE1);  /* the line before (backlog) */
    n++;
    c[n] = cand(n + 1, 300, 0, line);  /* the line */
    n++;
    c[n] = cand(n + 1, 301, 0, LINE3); /* the next one, prepared ahead */
    n++;
    TEST_CHECK(n == VJO_HOOK_STREAMS + 2);
    const char *ocr = "健太\n「駅前の公園でもう一度会おうよ、約東だからね」";
    for (unsigned i = 0; i < n; i++) {
        unsigned s = vjo_hook_similarity(ocr, strlen(ocr), c[i].text, c[i].n);
        if (s > best) { best = s; best_id = c[i].id; }
    }
    TEST_CHECK(best_id == 21 && best >= VJO_HOOK_AGREE);
    TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_AUTO, ocr, strlen(ocr), c, n, &score, &flags) == 21);
    TEST_CHECK(score == best && flags == VJO_HOOK_R_AGREED);
    /* the line's stream moved to the front or the back: the same pick */
    VjoHookCand t = c[0]; c[0] = c[n - 2]; c[n - 2] = t;
    memset(&st, 0, sizeof(st));
    TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_AUTO, ocr, strlen(ocr), c, n, &score, &flags) == 21 && score == best);
    /* none agrees: OCR kept, the best similarity reported */
    const char *other = "全然関係のない画面の文字がここに表示されている";
    best = 0;
    for (unsigned i = 0; i < n; i++) {
        unsigned s = vjo_hook_similarity(other, strlen(other), c[i].text, c[i].n);
        if (s > best) best = s;
    }
    TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_AUTO, other, strlen(other), c, n, &score, &flags) == 0);
    TEST_CHECK(score == best && score < VJO_HOOK_AGREE && flags == 0);
}

TEST_LIST = {
    {"exact_text", exact_text},
    {"wrong_characters", wrong_characters},
    {"name_and_line", name_and_line},
    {"backlog_and_prefetch", backlog_and_prefetch},
    {"short_strings", short_strings},
    {"empty_texts", empty_texts},
    {"random_against_reference", random_against_reference},
    {"longer_than_compared", longer_than_compared},
    {"lock_and_unlock", lock_and_unlock},
    {"locked_nothing_new", locked_nothing_new},
    {"locked_new_text", locked_new_text},
    {"pinned_stream", pinned_stream},
    {"ocr_only", ocr_only},
    {"ties", ties},
    {"many_candidates", many_candidates},
    {NULL, NULL}
};
