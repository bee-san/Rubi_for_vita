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
    VjoHookCand c = {id, seq, site, text, text ? strlen(text) : 0, id * 0x100u, 0, 0};
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
    /* a stream that agrees may lack up to 30 % of the box (docs/native-hooks.md, audit-hooks-text N3): a
       box that grows by clicks, whose stream holds its new segment, and the edge itself */
    s = sim("「えっ」「それって、昨日の放課後に話していたあの件のこと？」", "「それって、昨日の放課後に話していたあの件のこと？」");
    TEST_CHECK_(s == 916, "%u", s);
    TEST_CHECK(sim("一二三四五六七八九十", "一二三四五六七") == VJO_HOOK_AGREE);
    TEST_CHECK(sim("一二三四五六七八九十", "一二三四五六") == 600);
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
    /* NULL state or candidates */
    TEST_CHECK(vjo_hook_select_before(NULL, 9, c, 1, &flags) == 0 && flags == 0);
    TEST_CHECK(vjo_hook_select_after(NULL, VJO_HOOK_PICK_AUTO, LINE1, strlen(LINE1), c, 1, &score, &flags) == 0);
    TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_AUTO, LINE1, strlen(LINE1), NULL, 3, &score, &flags) == 0);
    /* NULL out-parameters are allowed */
    c[0] = cand(9, 1, 0, LINE1);
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

/* ---- adaptive selection, with actual distinct content rather than background sequence numbers ---- */

static void reading(VjoHookCand *c, unsigned line)
{
    static char text[128];
    snprintf(text, sizeof(text), "会話の新しいページ番号%uです", line);
    *c = cand(9, line, 0, text);
}

static VjoHookSelect learn(void)
{
    VjoHookSelect st = {0};
    VjoHookCand c;
    unsigned score;
    uint32_t flags;
    reading(&c, 1);
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, c.text, &c, 1, &score, &flags) == 9);
    reading(&c, 2);
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, c.text, &c, 1, &score, &flags) == 9);
    TEST_CHECK(st.interval == 8 && st.locked == 9 && st.streak == 2);
    return st;
}

static void distinct_initial_lines(void)
{
    VjoHookSelect st = {0};
    VjoHookCand c = cand(9, 1, 0, "「ガッコウヘイクヨ」");
    unsigned score;
    uint32_t flags;
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, c.text, &c, 1, &score, &flags) == 9);
    TEST_CHECK(!st.interval && st.streak == 1);
    /* changed raw writes, width, punctuation and whitespace still form the same line */
    c = cand(9, 1000, 0, " ｶﾞｯｺｳﾍ ｲｸﾖ！ ");
    for (unsigned i = 0; i < 5; i++) {
        c.seq++;
        TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, c.text, &c, 1, &score, &flags) == 9);
    }
    TEST_CHECK(!st.interval && st.streak == 1);
    c = cand(9, 2000, 0, LINE2);
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, LINE2, &c, 1, &score, &flags) == 9);
    TEST_CHECK(st.interval == 8 && st.streak == 2);
    TEST_CHECK(flags == (VJO_HOOK_R_AGREED | VJO_HOOK_R_LOCKED));
}

static void exact_adaptive_cadence(void)
{
    VjoHookSelect st = learn();
    VjoHookCand c;
    unsigned score, checks = 0;
    uint32_t flags;
    /* 8,16,32,32 readings, including each checked reading. Sequence jumps do not count. */
    for (unsigned line = 1; line <= 88; line++) {
        int scheduled = line == 8 || line == 24 || line == 56 || line == 88;
        reading(&c, line + 2);
        c.seq = line * 1000u;
        uint32_t id = vjo_hook_select_before(&st, VJO_HOOK_PICK_AUTO, &c, 1, &flags);
        TEST_CHECK_((id == 0) == scheduled, "fresh reading %u, interval %u, count %u", line, st.interval, st.unchecked);
        if (!id) {
            checks++;
            TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_AUTO, c.text, c.n, &c, 1, &score, &flags) == 9);
        }
        TEST_CHECK(st.interval == (line < 8 ? 8u : line < 24 ? 16u : 32u));
        /* Reopening unchanged dialogue follows the existing OCR path but changes no cadence. */
        VjoHookSelect was = st;
        for (unsigned reopen = 0; reopen < 2; reopen++) {
            c.seq++;
            TEST_CHECK(vjo_hook_select_before(&st, VJO_HOOK_PICK_AUTO, &c, 1, &flags) == 0);
            TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_AUTO, c.text, c.n, &c, 1, &score, &flags) == 9);
            TEST_CHECK(st.interval == was.interval && st.unchecked == was.unchecked && st.misses == was.misses);
        }
    }
    TEST_CHECK(checks == 4 && st.unchecked == 0);
}

static void restart_confirmation(void)
{
    VjoHookSelect st;
    VjoHookLearned saved = {1, 0x1234, 2, 32}, out;
    VjoHookCand c = cand(3, 1, 1, LINE1);
    unsigned score;
    uint32_t flags;
    c.key = saved.key;
    c.encoding = saved.encoding;
    TEST_CHECK(vjo_hook_select_restore(&st, &saved));
    TEST_CHECK(st.interval == 32 && st.confirm && !st.locked);
    TEST_CHECK(vjo_hook_select_learned(&st, &out) && !memcmp(&saved, &out, sizeof(out)));
    TEST_CHECK(vjo_hook_select_before(&st, VJO_HOOK_PICK_AUTO, &c, 1, &flags) == 0);
    TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_AUTO, LINE1, strlen(LINE1), &c, 1, &score, &flags) == 3);
    TEST_CHECK(st.interval == 32 && !st.confirm && st.unchecked == 0 && st.locked == 3);
    c.text = LINE2; c.n = strlen(LINE2); c.seq = 2;
    TEST_CHECK(vjo_hook_select_before(&st, VJO_HOOK_PICK_AUTO, &c, 1, &flags) == 3 && st.unchecked == 1);
    TEST_CHECK(vjo_hook_select_learned(&st, &out) && !memcmp(&saved, &out, sizeof(out)));
    /* Identity follows the source rather than its slot. */
    c.id = 12; c.text = LINE3; c.n = strlen(LINE3); c.seq++;
    TEST_CHECK(vjo_hook_select_before(&st, VJO_HOOK_PICK_AUTO, &c, 1, &flags) == 12 && st.locked == 12);
    TEST_CHECK(vjo_hook_select_learned(&st, &out) && !memcmp(&saved, &out, sizeof(out)));
    saved.interval = 31;
    TEST_CHECK(!vjo_hook_select_restore(&st, &saved));
    TEST_CHECK(!vjo_hook_select_learned(&st, &out) && !out.interval);
    TEST_CHECK(!vjo_hook_select_restore(&st, NULL));
    saved.interval = 32; saved.site = 2;
    TEST_CHECK(!vjo_hook_select_restore(&st, &saved));
}

static void startup_disagreement_recovery(void)
{
    VjoHookSelect st;
    VjoHookLearned saved = {0, 0x900, 0, 32}, out;
    VjoHookCand c = cand(9, 1, 0, LINE1);
    unsigned score;
    uint32_t flags;
    TEST_CHECK(vjo_hook_select_restore(&st, &saved));
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, LINE2, &c, 1, &score, &flags) == 0);
    TEST_CHECK(st.interval == 32 && st.misses == 1 && st.confirm);
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, LINE2, &c, 1, &score, &flags) == 0 && st.misses == 1);
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, "……", &c, 1, &score, &flags) == 0 && st.misses == 1);
    c.text = LINE3; c.n = strlen(LINE3); c.seq++;
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, LINE3, &c, 1, &score, &flags) == 9);
    TEST_CHECK(st.interval == 8 && !st.misses && !st.confirm && st.unchecked == 0);
    TEST_CHECK(vjo_hook_select_learned(&st, &out) && out.interval == 8);
    /* Two fresh comparable startup disagreements unlock; an inconclusive line keeps the first. */
    TEST_CHECK(vjo_hook_select_restore(&st, &saved));
    c.text = LINE1; c.n = strlen(LINE1); c.seq++;
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, LINE2, &c, 1, &score, &flags) == 0 && st.misses == 1);
    c.text = LINE3; c.n = strlen(LINE3); c.seq++;
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, "…", &c, 1, &score, &flags) == 0 && st.misses == 1);
    c.text = LINE4; c.n = strlen(LINE4); c.seq++;
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, LINE1, &c, 1, &score, &flags) == 0);
    TEST_CHECK(flags == VJO_HOOK_R_UNLOCK && !st.interval && !st.locked);
    TEST_CHECK(!vjo_hook_select_learned(&st, &out));
    /* A stale hook can stay unchanged while the actual screen advances. Empty OCR must not erase
     * the last comparable screen: a new screen is the second disagreement, an unchanged one is not. */
    TEST_CHECK(vjo_hook_select_restore(&st, &saved));
    c = cand(9, 1, 0, LINE1);
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, LINE2, &c, 1, &score, &flags) == 0 && st.misses == 1);
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, "…", &c, 1, &score, &flags) == 0 && st.misses == 1);
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, LINE2, &c, 1, &score, &flags) == 0 && st.misses == 1);
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, LINE3, &c, 1, &score, &flags) == 0);
    TEST_CHECK(flags == VJO_HOOK_R_UNLOCK && !st.interval);
}

static void scheduled_disagreement(void)
{
    VjoHookSelect st = learn();
    VjoHookCand c;
    unsigned score;
    uint32_t flags;
    for (unsigned i = 3; i <= 9; i++) {
        reading(&c, i);
        TEST_CHECK(vjo_hook_select_before(&st, VJO_HOOK_PICK_AUTO, &c, 1, &flags) == 9);
    }
    reading(&c, 10);
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, LINE1, &c, 1, &score, &flags) == 0);
    TEST_CHECK(st.misses == 1 && st.interval == 8 && st.unchecked == 7);
    for (unsigned i = 0; i < 3; i++) {
        c.seq++;
        TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, LINE1, &c, 1, &score, &flags) == 0 && st.misses == 1);
    }
    reading(&c, 11);
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, c.text, &c, 1, &score, &flags) == 9);
    TEST_CHECK(!st.misses && st.interval == 8 && st.unchecked == 0);
    /* More trust still resets to8 after a single disagreement and successful next line. */
    VjoHookLearned high = {0, 0x900, 0, 32};
    TEST_CHECK(vjo_hook_select_restore(&st, &high));
    reading(&c, 12);
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, c.text, &c, 1, &score, &flags) == 9);
    st.unchecked = 31;
    reading(&c, 13);
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, LINE1, &c, 1, &score, &flags) == 0 && st.misses == 1);
    reading(&c, 14);
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, c.text, &c, 1, &score, &flags) == 9 && st.interval == 8);
}

static void inconclusive_and_missing(void)
{
    VjoHookSelect st;
    VjoHookLearned high = {0, 0x900, 0, 32};
    VjoHookCand c;
    unsigned score;
    uint32_t flags;
    TEST_CHECK(vjo_hook_select_restore(&st, &high));
    reading(&c, 1);
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, c.text, &c, 1, &score, &flags) == 9);
    reading(&c, 2);
    TEST_CHECK(vjo_hook_select_before(&st, VJO_HOOK_PICK_AUTO, &c, 1, &flags) == 9 && st.unchecked == 1);
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, "メニュー画面", NULL, 0, &score, &flags) == 0);
    TEST_CHECK(st.interval == 32 && st.confirm && !st.misses && st.unchecked == 1);
    reading(&c, 3);
    TEST_CHECK(vjo_hook_select_before(&st, VJO_HOOK_PICK_AUTO, &c, 1, &flags) == 0);
    TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_AUTO, c.text, c.n, &c, 1, &score, &flags) == 9);
    TEST_CHECK(st.interval == 32 && !st.confirm && st.unchecked == 0);
    /* Empty/too-short hook text is also temporary, not a comparable disagreement. */
    c = cand(9, 4, 0, "はい");
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, LINE1, &c, 1, &score, &flags) == 0 && !st.misses && st.interval == 32);
    c = cand(9, 5, 0, LINE1);
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, "……", &c, 1, &score, &flags) == 0 && !st.misses && st.interval == 32);
    /* An inconclusive read supplied no dialogue/evidence. Its first valid comparison may confirm. */
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, LINE1, &c, 1, &score, &flags) == 9 && !st.confirm);
    c = cand(9, 6, 0, LINE2);
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, LINE2, &c, 1, &score, &flags) == 9 && !st.confirm && st.interval == 32);
}

static void inconclusive_check_cadence(void)
{
    VjoHookSelect st = learn();
    VjoHookCand c;
    unsigned score;
    uint32_t flags;
    for (unsigned i = 3; i <= 9; i++) {
        reading(&c, i);
        TEST_CHECK(vjo_hook_select_before(&st, VJO_HOOK_PICK_AUTO, &c, 1, &flags) == 9);
    }
    reading(&c, 10);
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, "…", &c, 1, &score, &flags) == 0);
    TEST_CHECK(st.unchecked == 7 && st.interval == 8 && !st.confirm);
    reading(&c, 11);
    TEST_CHECK(vjo_hook_select_before(&st, VJO_HOOK_PICK_AUTO, &c, 1, &flags) == 0);
    TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_AUTO, c.text, c.n, &c, 1, &score, &flags) == 9);
    TEST_CHECK(st.interval == 16 && st.unchecked == 0 && !st.confirm);
    /* A reopen before the check is due can also have empty OCR. Its next fresh line needs a
     * confirmation, while the successful confirmation preserves the interval instead of promoting. */
    reading(&c, 12);
    TEST_CHECK(vjo_hook_select_before(&st, VJO_HOOK_PICK_AUTO, &c, 1, &flags) == 9);
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, "…", &c, 1, &score, &flags) == 0);
    TEST_CHECK(st.confirm && st.interval == 16 && st.unchecked == 1);
    reading(&c, 13);
    TEST_CHECK(vjo_hook_select_before(&st, VJO_HOOK_PICK_AUTO, &c, 1, &flags) == 0);
    TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_AUTO, c.text, c.n, &c, 1, &score, &flags) == 9);
    TEST_CHECK(!st.confirm && st.interval == 16 && st.unchecked == 0);
}

static void source_invalidation(void)
{
    VjoHookSelect st = learn();
    VjoHookCand c = cand(9, 3, 0, LINE3);
    unsigned score;
    uint32_t flags;
    c.key++;
    TEST_CHECK(vjo_hook_select_before(&st, VJO_HOOK_PICK_AUTO, &c, 1, &flags) == 0);
    TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_AUTO, "", 0, &c, 1, &score, &flags) == 0);
    TEST_CHECK(flags == VJO_HOOK_R_UNLOCK && !st.interval && !st.streak);
    st = learn(); c = cand(9, 3, 0, LINE3); c.encoding++;
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, LINE3, &c, 1, &score, &flags) == 9);
    TEST_CHECK(flags == (VJO_HOOK_R_UNLOCK | VJO_HOOK_R_AGREED) && !st.interval && st.streak == 1);
    st = learn(); c = cand(9, 3, 1, LINE3);
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, LINE3, &c, 1, &score, &flags) == 9);
    TEST_CHECK(flags == (VJO_HOOK_R_UNLOCK | VJO_HOOK_R_AGREED | VJO_HOOK_R_SITE) && !st.interval);
    /* A slot reused between the first and second agreements cannot borrow its predecessor's streak. */
    st = (VjoHookSelect){0}; c = cand(9, 1, 0, LINE1);
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, LINE1, &c, 1, &score, &flags) == 9);
    c = cand(9, 2, 0, LINE2); c.key++;
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, LINE2, &c, 1, &score, &flags) == 9 && st.streak == 1 && !st.interval);
}

static void pinned_and_ocr(void)
{
    VjoHookSelect st = learn(), was = st;
    VjoHookCand c[2] = {cand(3, 4, 1, "はい"), cand(9, 5, 0, LINE2)};
    unsigned score;
    uint32_t flags;
    TEST_CHECK(vjo_hook_select_before(&st, 3, c, 2, &flags) == 3);
    TEST_CHECK(flags == (VJO_HOOK_R_PINNED | VJO_HOOK_R_NO_OCR | VJO_HOOK_R_SITE));
    TEST_CHECK(vjo_hook_select_after(&st, 3, LINE2, strlen(LINE2), c, 2, &score, &flags) == 3);
    TEST_CHECK(flags == (VJO_HOOK_R_PINNED | VJO_HOOK_R_SITE));
    TEST_CHECK(vjo_hook_select_before(&st, VJO_HOOK_PICK_OCR, c, 2, &flags) == 0);
    TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_OCR, LINE2, strlen(LINE2), c, 2, &score, &flags) == 0);
    TEST_CHECK(!memcmp(&st, &was, sizeof(st)));
}

/* Luna entries already identify a game's extraction/filter script. They need neither the generic
 * source's initial comparisons nor its learned cadence, including on short or repeated dialogue. */
static void luna_bypasses_learning(void)
{
    VjoHookSelect st = learn(), was = st;
    VjoHookCand c[2] = {cand(2, 1, VJO_HOOK_CAND_SITE | VJO_HOOK_CAND_LUNA, "はい"),
                        cand(9, 100, 0, LINE3)};
    uint32_t flags;
    st.unchecked = st.interval - 1u; /* a generic check is due, but Luna takes precedence */
    was = st;
    for (unsigned i = 0; i < 72; i++) {
        c[0].seq = i / 2u + 1u; /* repeated dialogue and enough fresh readings to pass the maximum interval */
        c[0].text = i / 2u % 2u ? "うん" : "はい";
        c[0].n = strlen(c[0].text);
        TEST_CHECK(vjo_hook_select_before(&st, VJO_HOOK_PICK_AUTO, c, 2, &flags) == 2);
        TEST_CHECK(flags == (VJO_HOOK_R_SITE | VJO_HOOK_R_NO_OCR));
        TEST_CHECK(!memcmp(&st, &was, sizeof(st)));
    }
    /* A missing Luna stream resumes the generic cadence at the same check, without borrowing trust. */
    c[0].text = ""; c[0].n = 0;
    TEST_CHECK(vjo_hook_select_before(&st, VJO_HOOK_PICK_AUTO, c, 2, &flags) == 0);
    TEST_CHECK(!flags && !memcmp(&st, &was, sizeof(st)));
    st = (VjoHookSelect){0};
    c[0] = cand(2, 1, VJO_HOOK_CAND_LUNA, "はい"); /* the Luna bit alone is not a table site */
    TEST_CHECK(vjo_hook_select_before(&st, VJO_HOOK_PICK_AUTO, c, 1, &flags) == 0 && !flags);
}

static void luna_newest_and_explicit_picks(void)
{
    const uint32_t luna = VJO_HOOK_CAND_SITE | VJO_HOOK_CAND_LUNA;
    VjoHookSelect st = learn(), was = st;
    VjoHookCand c[4] = {cand(5, 10, luna, LINE1), cand(2, 11, luna, "はい"),
                        cand(9, 100, 0, LINE3), cand(1, 11, luna, LINE2)};
    uint32_t flags;
    unsigned score;
    TEST_CHECK(vjo_hook_select_before(&st, VJO_HOOK_PICK_AUTO, c, 4, &flags) == 1); /* stable lower-ID tie */
    c[0].seq = UINT32_MAX - 1u;
    c[1].seq = 1; c[3].seq = UINT32_MAX;
    TEST_CHECK(vjo_hook_select_before(&st, VJO_HOOK_PICK_AUTO, c, 4, &flags) == 2); /* sequence wrap */
    TEST_CHECK(!memcmp(&st, &was, sizeof(st)));
    TEST_CHECK(vjo_hook_select_before(&st, 9, c, 4, &flags) == 9);
    TEST_CHECK(flags == (VJO_HOOK_R_PINNED | VJO_HOOK_R_NO_OCR));
    TEST_CHECK(vjo_hook_select_before(&st, 5, c, 4, &flags) == 5);
    TEST_CHECK(flags == (VJO_HOOK_R_PINNED | VJO_HOOK_R_NO_OCR | VJO_HOOK_R_SITE));
    TEST_CHECK(vjo_hook_select_before(&st, VJO_HOOK_PICK_OCR, c, 4, &flags) == 0 && !flags);
    TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_OCR, LINE1, strlen(LINE1), c, 4,
                                    &score, &flags) == 0);
    TEST_CHECK(!memcmp(&st, &was, sizeof(st)));
}

static void ties_and_fallback(void)
{
    VjoHookSelect st = {0};
    VjoHookCand c[3] = {cand(9, 10, 0, LINE1), cand(12, 11, 0, LINE1), cand(15, 9, 0, LINE1)};
    unsigned score;
    uint32_t flags;
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, LINE1, c, 3, &score, &flags) == 12);
    for (unsigned i = 0; i < 3; i++) { c[i].text = LINE2; c[i].n = strlen(LINE2); }
    c[0].seq = 100;
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, LINE2, c, 3, &score, &flags) == 12 && st.interval == 8);
    st = (VjoHookSelect){0}; c[2] = cand(2, 1, 1, LINE1);
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, LINE1, c, 3, &score, &flags) == 2);
    /* A different source may supply checked, agreeing text without replacing the trusted identity
     * after just one disagreement. */
    st = learn();
    c[0] = cand(9, 3, 0, LINE1); c[1] = cand(12, 3, 0, LINE2);
    st.unchecked = 7;
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, LINE2, c, 2, &score, &flags) == 12);
    TEST_CHECK(st.locked == 9 && st.misses == 1 && !(flags & VJO_HOOK_R_LOCKED));
    c[0] = cand(9, 4, 0, LINE3); c[1] = cand(12, 4, 0, LINE4);
    TEST_CHECK(job(&st, VJO_HOOK_PICK_AUTO, LINE4, c, 2, &score, &flags) == 12);
    TEST_CHECK((flags & VJO_HOOK_R_UNLOCK) && !st.interval && st.streak == 1 && st.streak_id == 12);
}

/* OCR refused for memory: only a Rubi verified table site's text answers, unconfirmed, the newest one; Luna's
 * sites, CALL callers, short texts and pinned or OCR-only picks never do; nothing is learned. */
static void no_memory_verified_site(void)
{
    VjoHookCand c[5] = {cand(9, 7, 0, LINE1), cand(2, 8, 1, LINE3), cand(1, 5, 1, LINE1), cand(3, 9, 1, "あい"),
                        cand(4, 6, 1, LINE3)};
    uint32_t flags = 123;
    c[2].verified = 1; /* a verified site, older than the Luna site 2 */
    c[3].verified = 1; /* verified, newest, but too short */
    c[4].verified = 1; /* verified, newer than 1 */
    TEST_CHECK(vjo_hook_select_no_memory(VJO_HOOK_PICK_AUTO, c, 5, &flags) == 4);
    TEST_CHECK(flags == (VJO_HOOK_R_UNCONFIRMED | VJO_HOOK_R_NO_OCR | VJO_HOOK_R_SITE));
    c[4].verified = 0;
    TEST_CHECK(vjo_hook_select_no_memory(VJO_HOOK_PICK_AUTO, c, 5, &flags) == 1);
    /* a CALL caller is never verified, even if marked so; neither is a Luna site (verified 0) */
    c[2].verified = 0;
    c[0].verified = 1;
    flags = 123;
    TEST_CHECK(vjo_hook_select_no_memory(VJO_HOOK_PICK_AUTO, c, 5, &flags) == 0 && flags == 0);
    c[2].verified = 1;
    TEST_CHECK(vjo_hook_select_no_memory(VJO_HOOK_PICK_OCR, c, 5, &flags) == 0 && flags == 0);
    TEST_CHECK(vjo_hook_select_no_memory(2, c, 5, &flags) == 0); /* pinned: the job's own path answers it */
    TEST_CHECK(vjo_hook_select_no_memory(VJO_HOOK_PICK_AUTO, NULL, 0, NULL) == 0);
    /* the seq wraps: the newer of two verified sites by serial order */
    VjoHookCand w[2] = {cand(1, 0xFFFFFFF0u, 1, LINE1), cand(4, 3, 1, LINE3)};
    w[0].verified = w[1].verified = 1;
    TEST_CHECK(vjo_hook_select_no_memory(VJO_HOOK_PICK_AUTO, w, 2, &flags) == 4);
}

/* ---- API 43: trusted hooks (vjo_hook_select_trusted, VJO_GAME_OCR_S_TRUST) ---- */

/* Luna's sites and Rubi's verified sites answer from their first line, short and unchanged text too, the newest
 * among them (a tie: the lower ID); a CALL caller never does before it is learned; the state does not move. */
static void trusted_sites_answer_without_ocr(void)
{
    const uint32_t luna = VJO_HOOK_CAND_SITE | VJO_HOOK_CAND_LUNA;
    VjoHookSelect st = {0}, was;
    VjoHookCand c[4] = {cand(9, 50, 0, LINE3), cand(3, 10, VJO_HOOK_CAND_SITE, LINE1), cand(1, 9, luna, LINE2),
                        cand(4, 10, VJO_HOOK_CAND_SITE, LINE4)};
    uint32_t flags;
    c[1].verified = c[3].verified = 1;
    was = st;
    /* the verified sites 3 and 4 hold the newest text (10): a tie, the lower ID */
    TEST_CHECK(vjo_hook_select_trusted(&st, VJO_HOOK_PICK_AUTO, c, 4, &flags) == 3);
    TEST_CHECK(flags == (VJO_HOOK_R_TRUSTED | VJO_HOOK_R_SITE | VJO_HOOK_R_NO_OCR));
    TEST_CHECK(!memcmp(&st, &was, sizeof(st)));
    c[2].seq = 11; /* Luna's site copies a newer line */
    TEST_CHECK(vjo_hook_select_trusted(&st, VJO_HOOK_PICK_AUTO, c, 4, &flags) == 1);
    /* the same text again, and a short one ("はい", 2 characters): still the site's, as Luna's always are */
    for (unsigned again = 0; again < 3; again++)
        TEST_CHECK(vjo_hook_select_trusted(&st, VJO_HOOK_PICK_AUTO, c, 4, &flags) == 1 && (flags & VJO_HOOK_R_TRUSTED));
    c[2].text = "はい"; c[2].n = strlen(c[2].text); c[2].seq = 12;
    TEST_CHECK(vjo_hook_select_trusted(&st, VJO_HOOK_PICK_AUTO, c, 4, &flags) == 1);
    TEST_CHECK(!memcmp(&st, &was, sizeof(st)));
    /* a site neither Luna's nor verified (a Rubi entry that failed nothing yet has no verified mark: none here),
     * and a CALL caller: not trusted */
    c[0] = cand(9, 60, 0, LINE3);
    c[1] = cand(3, 10, VJO_HOOK_CAND_SITE, LINE1);
    TEST_CHECK(vjo_hook_select_trusted(&st, VJO_HOOK_PICK_AUTO, c, 2, &flags) == 0 && !flags);
    /* the Luna bit alone is not a table site; the verified mark alone on a CALL caller neither */
    c[0] = cand(2, 1, VJO_HOOK_CAND_LUNA, "はい");
    c[1] = cand(10, 1, 0, LINE1);
    c[1].verified = 1;
    TEST_CHECK(vjo_hook_select_trusted(&st, VJO_HOOK_PICK_AUTO, c, 2, &flags) == 0 && !flags);
    /* a pinned or OCR-only pick: never */
    c[0] = cand(3, 10, VJO_HOOK_CAND_SITE, LINE1);
    c[0].verified = 1;
    TEST_CHECK(vjo_hook_select_trusted(&st, 3, c, 1, &flags) == 0 && !flags);
    TEST_CHECK(vjo_hook_select_trusted(&st, VJO_HOOK_PICK_OCR, c, 1, &flags) == 0 && !flags);
    TEST_CHECK(vjo_hook_select_trusted(&st, VJO_HOOK_PICK_AUTO, c, 0, &flags) == 0);
    TEST_CHECK(vjo_hook_select_trusted(NULL, VJO_HOOK_PICK_AUTO, c, 1, &flags) == 0);
    /* an empty candidate is no answer */
    c[0].n = 0;
    TEST_CHECK(vjo_hook_select_trusted(&st, VJO_HOOK_PICK_AUTO, c, 1, &flags) == 0);
}

/* A CALL caller is learned as before (two agreements, then checks at 8 and 16) and trusted once its interval is
 * VJO_HOOK_TRUST_INTERVAL: four OCR agreements; from then on every reading, a repeated one too, answers without
 * OCR and no check falls due. The OCR comparisons over 120 fresh readings: 4, against 0.9.7's 6. */
static void trusted_learned_source_after_four_agreements(void)
{
    VjoHookSelect st = {0};
    VjoHookCand c;
    unsigned score, ocr = 0, ocr_097 = 0;
    uint32_t flags;
    TEST_CHECK(VJO_HOOK_TRUST_INTERVAL == 32u);
    for (int trust = 0; trust <= 1; trust++) {
        st = (VjoHookSelect){0};
        for (unsigned line = 1; line <= 120; line++) {
            uint32_t id = 0;
            reading(&c, line);
            if (trust) id = vjo_hook_select_trusted(&st, VJO_HOOK_PICK_AUTO, &c, 1, &flags);
            TEST_CHECK_(!trust || !id || st.interval >= 32u, "line %u trusted at interval %u", line, st.interval);
            if (!id) id = vjo_hook_select_before(&st, VJO_HOOK_PICK_AUTO, &c, 1, &flags);
            if (!id) {
                if (trust) ocr++;
                else ocr_097++;
                TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_AUTO, c.text, c.n, &c, 1, &score, &flags) == 9);
            }
            /* lines 1, 2 (the lock), 10 (the first check: 16) and 26 (the second: 32), then trusted */
            if (trust) TEST_CHECK_(st.interval == (line < 2 ? 0u : line < 10 ? 8u : line < 26 ? 16u : 32u),
                                   "line %u interval %u", line, st.interval);
            if (trust && line > 26) {
                TEST_CHECK(id == 9 && flags == (VJO_HOOK_R_TRUSTED | VJO_HOOK_R_LOCKED | VJO_HOOK_R_NO_OCR));
                /* the same reading again (the overlay reopened on it): no OCR either, and no cadence moves */
                VjoHookSelect was = st;
                c.seq++;
                TEST_CHECK(vjo_hook_select_trusted(&st, VJO_HOOK_PICK_AUTO, &c, 1, &flags) == 9);
                TEST_CHECK(!memcmp(&st, &was, sizeof(st)));
            }
        }
    }
    TEST_CHECK_(ocr == 4 && ocr_097 == 6, "%u with trust, %u without", ocr, ocr_097);
    TEST_CHECK(st.strong == 1);
    /* API 49: export gives the trusted interval for a trusted (strong) source only, and a source restored at it (an
     * earlier launch's trust, the exact executable) is trusted at once, before any OCR, in the runtime slot this
     * launch gives it; vjo_hook_select_before still asks its confirmation (hook_trust = off) */
    VjoHookLearned saved;
    TEST_CHECK(vjo_hook_select_learned(&st, &saved) && saved.interval == 32 && saved.key == 9 * 0x100u);
    TEST_CHECK(vjo_hook_select_restore(&st, &saved) && st.confirm && !st.locked && st.strong);
    reading(&c, 500);
    c.id = 14; /* another runtime slot this launch */
    TEST_CHECK(vjo_hook_select_trusted(&st, VJO_HOOK_PICK_AUTO, &c, 1, &flags) == 14 && st.locked == 14);
    TEST_CHECK(flags == (VJO_HOOK_R_TRUSTED | VJO_HOOK_R_LOCKED | VJO_HOOK_R_NO_OCR));
    {
        VjoHookSelect off = st; /* the same restore with hook_trust = off: its confirmation OCR first */
        TEST_CHECK(vjo_hook_select_before(&off, VJO_HOOK_PICK_AUTO, &c, 1, &flags) == 0);
    }
    /* one that reached 32 with weaker agreements (not strong) is exported at 16: restored untrusted, it needs its
     * close agreement again */
    st.strong = 0;
    TEST_CHECK(vjo_hook_select_learned(&st, &saved) && saved.interval == 16);
    TEST_CHECK(vjo_hook_select_restore(&st, &saved) && !st.strong && st.interval == 16);
    TEST_CHECK(vjo_hook_select_trusted(&st, VJO_HOOK_PICK_AUTO, &c, 1, &flags) == 0 && !flags);
    TEST_CHECK(vjo_hook_select_before(&st, VJO_HOOK_PICK_AUTO, &c, 1, &flags) == 0);
    TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_AUTO, c.text, c.n, &c, 1, &score, &flags) == 14);
    TEST_CHECK(!st.strong && vjo_hook_select_trusted(&st, VJO_HOOK_PICK_AUTO, &c, 1, &flags) == 0 && !flags);
    /* a pending disagreement is no trust either: exported at 16 */
    st.interval = 32; st.strong = 0; st.misses = 1;
    TEST_CHECK(vjo_hook_select_learned(&st, &saved) && saved.interval == 16);
    st.interval = 8; /* below the trusted interval: exported as it is */
    TEST_CHECK(vjo_hook_select_learned(&st, &saved) && saved.interval == 8);
}

/* The agreement that grants trust must be close (VJO_HOOK_TRUST_SCORE, 900): a source whose checks at 32 agree at
 * 700-899 (the screen's text holds the line and more: the line before, part of the box) stays checked at 32 as in
 * 0.9.7, OCR every 32nd line; a close agreement at a later check trusts it; a disagreement takes trust back before
 * it is used. */
static void trusted_needs_a_close_agreement(void)
{
    VjoHookSelect st = learn();
    VjoHookCand c;
    char ocr[160];
    unsigned score, checks = 0;
    uint32_t flags;
    st.interval = 32;
    st.unchecked = 0;
    for (unsigned line = 3; line <= 3 + 64; line++) {
        reading(&c, line);
        /* OCR reads the line and a short name beside it: 15 of 18 characters are the hook's, about 830 */
        snprintf(ocr, sizeof(ocr), "%s佐藤", c.text);
        TEST_CHECK(vjo_hook_select_trusted(&st, VJO_HOOK_PICK_AUTO, &c, 1, &flags) == 0);
        if (vjo_hook_select_before(&st, VJO_HOOK_PICK_AUTO, &c, 1, &flags)) continue;
        checks++;
        TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_AUTO, ocr, strlen(ocr), &c, 1, &score, &flags) == 9);
        TEST_CHECK_(score >= VJO_HOOK_AGREE && score < VJO_HOOK_TRUST_SCORE, "score %u", score);
        TEST_CHECK(!st.strong && st.interval == 32);
    }
    TEST_CHECK_(checks == 2, "%u checks", checks); /* every 32nd line, as 0.9.7's */
    /* the next check agrees exactly: trusted from the line after it */
    for (unsigned line = 100;; line++) {
        reading(&c, line);
        if (vjo_hook_select_trusted(&st, VJO_HOOK_PICK_AUTO, &c, 1, &flags)) break;
        if (vjo_hook_select_before(&st, VJO_HOOK_PICK_AUTO, &c, 1, &flags)) continue;
        TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_AUTO, c.text, c.n, &c, 1, &score, &flags) == 9);
        TEST_CHECK(score == 1000 && st.strong);
        TEST_ASSERT(line < 140);
    }
    TEST_CHECK(flags == (VJO_HOOK_R_TRUSTED | VJO_HOOK_R_LOCKED | VJO_HOOK_R_NO_OCR));
    /* a disagreement (hook_trust off for a job, its check) takes the strong mark back */
    st.unchecked = st.interval - 1u;
    reading(&c, 200);
    TEST_CHECK(vjo_hook_select_before(&st, VJO_HOOK_PICK_AUTO, &c, 1, &flags) == 0);
    TEST_CHECK(vjo_hook_select_after(&st, VJO_HOOK_PICK_AUTO, "全然関係のない画面の文字がここに表示されている", 69, &c, 1,
                                     &score, &flags) == 0);
    TEST_CHECK(!st.strong && st.misses == 1);
    TEST_CHECK(vjo_hook_select_trusted(&st, VJO_HOOK_PICK_AUTO, &c, 1, &flags) == 0);
}

/* OCR resumes by itself when the trusted source has no text, when its slot holds another caller, and while a
 * disagreement is pending (0.9.7's recovery then compares it again); the source's learning is kept. */
static void trusted_source_fails_its_checks(void)
{
    VjoHookSelect st = learn();
    VjoHookCand c, other;
    uint32_t flags;
    st.interval = 32; /* trusted */
    st.strong = 1;
    VjoHookSelect weak = st;
    weak.strong = 0; /* at 32 but no close agreement there yet: not trusted */
    reading(&c, 6);
    TEST_CHECK(vjo_hook_select_trusted(&weak, VJO_HOOK_PICK_AUTO, &c, 1, &flags) == 0 && !flags);
    reading(&c, 7);
    TEST_CHECK(vjo_hook_select_trusted(&st, VJO_HOOK_PICK_AUTO, &c, 1, &flags) == 9);
    /* short text answers (a trusted source's "はい") */
    c.text = "はい"; c.n = strlen(c.text);
    TEST_CHECK(vjo_hook_select_trusted(&st, VJO_HOOK_PICK_AUTO, &c, 1, &flags) == 9);
    /* no text, or no candidate at all: none */
    TEST_CHECK(vjo_hook_select_trusted(&st, VJO_HOOK_PICK_AUTO, &c, 0, &flags) == 0);
    c.n = 0;
    TEST_CHECK(vjo_hook_select_trusted(&st, VJO_HOOK_PICK_AUTO, &c, 1, &flags) == 0);
    /* another caller in its slot (its key) is not the source */
    reading(&other, 8);
    other.key = 0x5555u;
    TEST_CHECK(vjo_hook_select_trusted(&st, VJO_HOOK_PICK_AUTO, &other, 1, &flags) == 0 && st.interval == 32);
    /* a pending disagreement: compared again first */
    reading(&c, 9);
    st.misses = 1;
    TEST_CHECK(vjo_hook_select_trusted(&st, VJO_HOOK_PICK_AUTO, &c, 1, &flags) == 0 && !flags);
    st.misses = 0;
    TEST_CHECK(vjo_hook_select_trusted(&st, VJO_HOOK_PICK_AUTO, &c, 1, &flags) == 9);
    /* a site with text comes first */
    VjoHookCand both[2] = {c, cand(2, 1, VJO_HOOK_CAND_SITE | VJO_HOOK_CAND_LUNA, LINE1)};
    TEST_CHECK(vjo_hook_select_trusted(&st, VJO_HOOK_PICK_AUTO, both, 2, &flags) == 2 && (flags & VJO_HOOK_R_SITE));
}

TEST_LIST = {
    {"trusted_sites_answer_without_ocr", trusted_sites_answer_without_ocr},
    {"trusted_learned_source_after_four_agreements", trusted_learned_source_after_four_agreements},
    {"trusted_source_fails_its_checks", trusted_source_fails_its_checks},
    {"trusted_needs_a_close_agreement", trusted_needs_a_close_agreement},
    {"no_memory_verified_site", no_memory_verified_site},
    {"exact_text", exact_text}, {"wrong_characters", wrong_characters},
    {"name_and_line", name_and_line}, {"backlog_and_prefetch", backlog_and_prefetch},
    {"short_strings", short_strings}, {"empty_texts", empty_texts},
    {"random_against_reference", random_against_reference}, {"longer_than_compared", longer_than_compared},
    {"distinct_initial_lines", distinct_initial_lines}, {"exact_adaptive_cadence", exact_adaptive_cadence},
    {"restart_confirmation", restart_confirmation}, {"startup_disagreement_recovery", startup_disagreement_recovery},
    {"scheduled_disagreement", scheduled_disagreement}, {"inconclusive_and_missing", inconclusive_and_missing},
    {"inconclusive_check_cadence", inconclusive_check_cadence},
    {"source_invalidation", source_invalidation}, {"pinned_and_ocr", pinned_and_ocr},
    {"luna_bypasses_learning", luna_bypasses_learning},
    {"luna_newest_and_explicit_picks", luna_newest_and_explicit_picks},
    {"ties_and_fallback", ties_and_fallback}, {"many_candidates", many_candidates}, {NULL, NULL}
};
