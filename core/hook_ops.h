/* Shared filter interpreter operations; program data live in a per-game profile. */
#ifndef VJO_HOOK_OPS_H
#define VJO_HOOK_OPS_H
#include <stdint.h>
enum {
    OP_END,
    OP_DEL,     /* a removed wherever it is (StringFilter, CharFilter, strReplace(s, a)) */
    OP_REP,     /* a replaced by b wherever it is (strReplace, StringCharReplacer) */
    OP_SUB,     /* re::sub(s, a, b) on a byte string: bytes on a UTF-8 site, characters on a CP932 one */
    OP_SUBW,    /* re::sub on a wide string: characters */
    OP_CALL,    /* another filter (arg) */
    OP_IFEQ,    /* a text equal to a: dropped */
    OP_PREFIX,  /* a text that does not start with a: dropped */
    OP_UPTO,    /* the text up to the end of its first a (none: dropped) */
    OP_AFTER,   /* the text after its first a (none: unchanged) */
    OP_CUT,     /* substr(from, size - from - drop) in CP932 bytes: arg = from | drop << 4 */
    OP_ENDDROP, /* a text that ends with a loses its last byte */
    OP_P00449,  /* PCSG00449's ending */
    OP_WRAP,    /* PCSG00272: a name before #n# into 【】 */
    OP_NAMEPOS, /* PCSG00935: the name after #n#Pos[..] (pattern a) to the front, in 【】 */
    OP_TRIM,    /* Trim(): iswspace at both ends */
    OP_KANA,    /* remapkatakana */
    OP_ASCII,   /* all_ascii: a text of ASCII only is dropped */
    OP_BETWEEN  /* StringFilterBetween(a, b) */
};

typedef struct {
    uint8_t op, enc, arg;
    const char *a, *b;
} LunaOp;


#endif
