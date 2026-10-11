#include "acutest.h"
#include "anki.h"
#include <string.h>
static unsigned char mem[4096];
static VjoArena A;
static void setup(void) { vjo_arena_init(&A, mem, sizeof(mem)); }
static void test_anki_fields(void)
{
    VjoBuf b;
    char *s;
#define BUF(expr) (setup(), vjo_buf_init(&b, &A), (expr), s = vjo_buf_cstr(&b))
    /* the word in bold, escaped, line breaks as <br>, CR dropped */
    BUF(vjo_anki_sentence(&b, "猫が\r\n好き<3&\"", 0, 3));
    TEST_CHECK(!strcmp(s, "<b>猫</b>が<br>好き&lt;3&amp;&quot;"));
    BUF(vjo_anki_sentence(&b, "猫が好き", 6, 12));
    TEST_CHECK(!strcmp(s, "猫が<b>好き</b>"));
    BUF(vjo_anki_sentence(&b, "猫が\n好き", 3, 7)); /* across a line break */
    TEST_CHECK(!strcmp(s, "猫<b>が<br></b>好き"));
    BUF(vjo_anki_sentence(&b, "𠮷野家", 0, 4)); /* 4-byte code point */
    TEST_CHECK(!strcmp(s, "<b>𠮷</b>野家"));
    BUF(vjo_anki_sentence(&b, "猫", -1, -1));
    TEST_CHECK(!strcmp(s, "猫"));
    BUF(vjo_anki_sentence(&b, "猫", 0, 99)); /* out of range: no bold */
    TEST_CHECK(!strcmp(s, "猫"));

    BUF(vjo_anki_furigana(&b, "言葉", "ことば"));
    TEST_CHECK(!strcmp(s, "言葉[ことば]"));
    BUF(vjo_anki_furigana(&b, "食べる", "たべる"));
    TEST_CHECK(!strcmp(s, "食べる[たべる]"));
    BUF(vjo_anki_furigana(&b, "ある", "ある"));
    TEST_CHECK(!strcmp(s, "ある"));
    BUF(vjo_anki_furigana(&b, "テレビ", "てれび")); /* no kanji */
    TEST_CHECK(!strcmp(s, "テレビ"));
    BUF(vjo_anki_furigana(&b, "時々", "ときどき"));
    TEST_CHECK(!strcmp(s, "時々[ときどき]"));

    {
        const char *m[] = {"to eat", "to live on <sth>"};
        BUF(vjo_anki_definition(&b, m, 2));
        TEST_CHECK(!strcmp(s, "<ol><li>to eat</li><li>to live on &lt;sth&gt;</li></ol>"));
        BUF(vjo_anki_definition(&b, m, 0));
        TEST_CHECK(!strcmp(s, ""));
    }
#undef BUF

}
TEST_LIST = {{"note_fields", test_anki_fields}, {NULL, NULL}};
