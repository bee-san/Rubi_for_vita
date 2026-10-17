/* A minimal ZIP writer and reader for the Rubi app's .apkg export
 * (core/apkg.c): stored entries only (method 0, what genanki writes and
 * every Anki importer reads), no ZIP64 (an export stays far below 2 GiB),
 * no encryption, no comments. The writer streams an entry's bytes through
 * its CRC-32, or takes the size and CRC-32 of one written into the file by
 * another writer (the collection's SQLite pages, core/sqlw.c). Either way
 * the CRC-32 is of the bytes as they were written, never of a read-back, so
 * the reader's check (the export's verification) catches a file that does
 * not read back as written. The reader parses what the writer wrote: the
 * end record, the central directory, each local header and each entry's
 * CRC-32.
 * (APPNOTE.TXT 6.3.10, sections 4.3.7, 4.3.12, 4.3.16.) */
#ifndef VJO_ZIPW_H
#define VJO_ZIPW_H

#include <stddef.h>
#include <stdint.h>

#define VJO_ZIP_NAME_MAX 24       /* "collection.anki2", "media", "0".."65532" */
#define VJO_ZIP_MAX_BYTES 0x7FFFFFFFu
#define VJO_ZIP_MAX_ENTRIES 65535

/* The file: n bytes at off. 0, or < 0 (the error). */
typedef struct {
    void *ctx;
    int (*write)(void *ctx, uint64_t off, const void *src, size_t n);
    int (*read)(void *ctx, uint64_t off, void *dst, size_t n);
} VjoZipIo;

typedef struct {
    char name[VJO_ZIP_NAME_MAX];
    uint32_t crc, size, offset; /* offset: its local header */
} VjoZipEntry;

typedef struct {
    VjoZipIo io;
    VjoZipEntry *entries; /* the caller's, cap of them */
    int n, cap;
    int open;         /* entries[n] is being written */
    uint32_t end;     /* the file's length so far */
    uint32_t crc;     /* the open entry's, so far */
    uint16_t time, date; /* every entry's modification time (MS-DOS format) */
    int err;
    uint8_t buf[64 + VJO_ZIP_NAME_MAX];
} VjoZipw;

uint32_t vjo_crc32(uint32_t crc, const void *p, size_t n);
/* The CRC-32 of A then B from crc1 = CRC(A), crc2 = CRC(B) and B's length
 * (the method of zlib's crc32_combine). */
uint32_t vjo_crc32_combine(uint32_t crc1, uint32_t crc2, uint64_t len2);
/* The MS-DOS time and date of a local time. */
void vjo_zip_dos_time(int year, int month, int day, int hour, int minute, int second, uint16_t *time,
                      uint16_t *date);

void vjo_zipw_init(VjoZipw *z, const VjoZipIo *io, VjoZipEntry *entries, int cap, uint16_t time, uint16_t date);
/* Starts entry name: its local header (its CRC and size filled in by
 * vjo_zipw_end); its data starts at *data. */
int vjo_zipw_begin(VjoZipw *z, const char *name, uint32_t *data);
/* Appends to the open entry. */
int vjo_zipw_append(VjoZipw *z, const void *p, size_t n);
/* Ends the open entry. */
int vjo_zipw_end(VjoZipw *z);
/* Ends the open entry whose size bytes another writer wrote at its data
 * offset (vjo_zipw_append was not used), with the CRC-32 of those bytes. */
int vjo_zipw_end_external(VjoZipw *z, uint32_t size, uint32_t crc);
/* The central directory and the end record: the file is complete, *bytes long. */
int vjo_zipw_finish(VjoZipw *z, uint32_t *bytes);

/* Reads the central directory of a file of size bytes written as above into
 * entries (at most cap): their count, or < 0 (not such a file, or an I/O
 * error). Each entry's local header is checked against it. */
int vjo_zip_read(const VjoZipIo *io, uint64_t size, VjoZipEntry *entries, int cap);
/* The entry's data offset (after its local header), or 0 if it is not valid. */
uint32_t vjo_zip_data_offset(const VjoZipIo *io, const VjoZipEntry *e);
/* Reads the entry's data in chunks of buf and checks its CRC-32: 0 or < 0. */
int vjo_zip_check_crc(const VjoZipIo *io, const VjoZipEntry *e, uint8_t *buf, size_t cap);

#endif
