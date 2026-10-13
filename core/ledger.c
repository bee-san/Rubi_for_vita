/* Memory ledger core: see ledger.h. No libc; GCC atomics (LDREX/STREX on the
 * Vita), so any thread or hook may add and subtract. */
#include "ledger.h"

static void raise_peak(uint32_t *peak, uint32_t value)
{
    uint32_t seen = *peak;
    while (value > seen && !__sync_bool_compare_and_swap(peak, seen, value))
        seen = *peak;
}

void vjo_ledger_init(VjoLedger *l, const VjoLedgerItemDef *defs, int n)
{
    unsigned char *p = (unsigned char *)l;
    for (unsigned i = 0; i < sizeof(*l); i++)
        p[i] = 0;
    l->defs = defs;
    l->n = n < 0 ? 0 : n > VJO_LEDGER_MAX_ITEMS ? VJO_LEDGER_MAX_ITEMS : n;
}

void vjo_ledger_add(VjoLedger *l, int item, uint32_t bytes)
{
    int pool;
    if (!bytes || !vjo_ledger_known(l, item))
        return;
    pool = l->defs[item].pool;
    raise_peak(&l->peak[item], __sync_add_and_fetch(&l->current[item], bytes));
    raise_peak(&l->pool_peak[pool], __sync_add_and_fetch(&l->pool_current[pool], bytes));
}

/* Removes up to bytes from *current, never below zero; returns what was
 * removed, so the pool loses exactly what the item lost. */
static uint32_t take(uint32_t *current, uint32_t bytes)
{
    uint32_t seen = *current;
    while (!__sync_bool_compare_and_swap(current, seen, seen > bytes ? seen - bytes : 0))
        seen = *current;
    return seen > bytes ? bytes : seen;
}

void vjo_ledger_sub(VjoLedger *l, int item, uint32_t bytes)
{
    if (!bytes || !vjo_ledger_known(l, item))
        return;
    take(&l->pool_current[l->defs[item].pool], take(&l->current[item], bytes));
}
