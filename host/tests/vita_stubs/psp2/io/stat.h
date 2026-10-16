#include "fcntl.h"
/* sceIoChstat's fields (psp2common/kernel/iofilemgr.h): the host suites that
 * call it implement it. */
#ifndef SCE_CST_SIZE
#define SCE_CST_SIZE 0x0004
#endif
int sceIoChstat(const char *file, SceIoStat *stat, int bits);
