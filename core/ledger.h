/* Memory ledger: current and peak bytes per named item and per pool, kept by
 * each of our processes (SceShell plugin, kernel plugin, game worker) for
 * the memory it holds. Every allocation wrapper calls vjo_ledger_add on
 * success and vjo_ledger_sub on release; the static budget each item is
 * checked against in CI is include/vjo_ledger_budget.h. Plain C, no libc,
 * atomics only: the Shell's threads and the kernel's hooks share it. Each
 * process prints it with its own logger (status.txt, kernel.txt). */
#ifndef VJO_LEDGER_H
#define VJO_LEDGER_H

#include <stdint.h>

/* The pools of docs/vita-memory.md that our plugins take memory from. */
#define VJO_LEDGER_POOL_SHELL_PAGES 0 /* SceShell's partition: module pages, stacks, TLS, USER_RW blocks */
#define VJO_LEDGER_POOL_SHELL_PAF   1 /* SceShell's PAF global heap (pre-mapped) */
#define VJO_LEDGER_POOL_KERNEL      2 /* ScePhyMemPartKD: kernel module pages, stack, KERNEL_RW blocks */
#define VJO_LEDGER_POOL_GAME        3 /* the game's partition: worker pages, stack, weights, workspace */
#define VJO_LEDGER_POOLS            4

/* The most items one process keeps: the game worker's thirteen since API 55's frame block (VjoGameVocrFrame; twelve
 * since API 54's text spotter, VjoGameVocrSpot, eleven since #368's SC3 charset), the Shell's eleven since line
 * history's block (VJO_SL_PAF_HISTORY), the kernel's five. */
#define VJO_LEDGER_MAX_ITEMS 13

typedef struct {
    const char *name; /* for logs: "VjoNet stack+TLS" */
    uint8_t pool;     /* VJO_LEDGER_POOL_* */
} VjoLedgerItemDef;

typedef struct {
    const VjoLedgerItemDef *defs;
    int n;
    uint32_t current[VJO_LEDGER_MAX_ITEMS];
    uint32_t peak[VJO_LEDGER_MAX_ITEMS];
    uint32_t pool_current[VJO_LEDGER_POOLS];
    uint32_t pool_peak[VJO_LEDGER_POOLS];
} VjoLedger;

/* defs[0..n) describe the items; n <= VJO_LEDGER_MAX_ITEMS (more are ignored). */
void vjo_ledger_init(VjoLedger *l, const VjoLedgerItemDef *defs, int n);
/* bytes were taken for item (0 is a no-op; an unknown item is ignored). */
void vjo_ledger_add(VjoLedger *l, int item, uint32_t bytes);
/* bytes were given back; never below zero. */
void vjo_ledger_sub(VjoLedger *l, int item, uint32_t bytes);

static inline int vjo_ledger_known(const VjoLedger *l, int item)
{
    return l && l->defs && item >= 0 && item < l->n && l->defs[item].pool < VJO_LEDGER_POOLS;
}
static inline uint32_t vjo_ledger_current(const VjoLedger *l, int item)
{
    return vjo_ledger_known(l, item) ? l->current[item] : 0;
}
static inline uint32_t vjo_ledger_peak(const VjoLedger *l, int item)
{
    return vjo_ledger_known(l, item) ? l->peak[item] : 0;
}
static inline uint32_t vjo_ledger_pool_current(const VjoLedger *l, int pool)
{
    return l && pool >= 0 && pool < VJO_LEDGER_POOLS ? l->pool_current[pool] : 0;
}
static inline uint32_t vjo_ledger_pool_peak(const VjoLedger *l, int pool)
{
    return l && pool >= 0 && pool < VJO_LEDGER_POOLS ? l->pool_peak[pool] : 0;
}

#endif
