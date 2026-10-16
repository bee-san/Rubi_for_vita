#include <psp2kern/host_stubs.h>
#ifndef VJO_TEST_KERN_STAT_H
#define VJO_TEST_KERN_STAT_H
/* glibc's <sys/stat.h> maps these names to struct timespec members. */
#undef st_atime
#undef st_mtime
#undef st_ctime
typedef struct {
    unsigned short year, month, day, hour, minute, second;
    unsigned int microsecond;
} SceDateTime;
typedef struct {
    unsigned int st_mode, st_attr;
    SceOff st_size;
    SceDateTime st_ctime, st_atime, st_mtime;
    unsigned int st_private[6];
} SceIoStat;
int ksceIoGetstatByFd(SceUID fd, SceIoStat *stat);
int ksceIoGetstat(const char *file, SceIoStat *stat);
int ksceIoMkdir(const char *dir, unsigned mode);
#endif
