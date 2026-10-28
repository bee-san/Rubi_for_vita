#include "hook_profile_file.h"
#include <string.h>

static uint32_t u32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
int vjo_hook_profile_read(VjoProfileRead read, void *ud, uint32_t size, const char *title, void *dst, unsigned cap)
{
    uint8_t row[VJO_PROFILE_DB_ROW];
    uint32_t lo = 0, hi, end;
    if (!title || strlen(title) != 9 || !dst || cap < VJO_PROFILE_BYTES || size < VJO_PROFILE_DB_HEADER ||
        read(ud, row, VJO_PROFILE_DB_HEADER, 0) != VJO_PROFILE_DB_HEADER || memcmp(row, "vjohkdb1", 8) || u32(row + 12))
        return -1;
    hi = u32(row + 8);
    if (!hi || hi > VJO_PROFILE_DB_TITLES_MAX) return -1;
    end = VJO_PROFILE_DB_HEADER + VJO_PROFILE_DB_ROW * hi;
    if (end > size) return -1;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2, offset, bytes;
        int cmp;
        if (read(ud, row, sizeof(row), VJO_PROFILE_DB_HEADER + VJO_PROFILE_DB_ROW * mid) != sizeof(row)) return -1;
        for (unsigned i = 0; i < 9; i++)
            if (!((row[i] >= 'A' && row[i] <= 'Z') || (row[i] >= '0' && row[i] <= '9'))) return -1;
        offset = u32(row + 12); bytes = u32(row + 16);
        if (row[9] || row[10] || row[11] || offset < end || offset > size || !bytes ||
            bytes > VJO_PROFILE_BYTES || bytes > size - offset) return -1;
        cmp = memcmp(title, row, 9);
        if (cmp < 0) hi = mid;
        else if (cmp > 0) lo = mid + 1;
        else return read(ud, dst, bytes, offset) == (int)bytes ? (int)bytes : -1;
    }
    return 0;
}
