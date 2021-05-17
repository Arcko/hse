/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2015-2020 Micron Technology, Inc.  All rights reserved.
 */

#include "c0_ingest_work.h"

#include <hse_util/platform.h>
#include <hse_util/bonsai_tree.h>

merr_t
c0_ingest_work_init(struct c0_ingest_work *c0iw)
{
    struct bin_heap2 *minheap;
    merr_t            err;

    assert(c0iw);

    memset(c0iw, 0, sizeof(*c0iw));
    c0iw->c0iw_magic = (uintptr_t)c0iw;

    err = bin_heap2_create(HSE_C0_KVSET_ITER_MAX, bn_kv_cmp, &minheap);
    if (ev(err))
        return err;

    c0iw->c0iw_minheap = minheap;

    return 0;
}

void
c0_ingest_work_reset(struct c0_ingest_work *c0iw)
{
    assert(c0iw->c0iw_magic == (uintptr_t)c0iw);

    bin_heap2_reset(c0iw->c0iw_minheap);
    c0iw->c0iw_iterc = 0;

    memset(c0iw->c0iw_mbv, 0, sizeof(c0iw->c0iw_mbv));
}

void
c0_ingest_work_fini(struct c0_ingest_work *w)
{
    if (!w)
        return;

    assert(w->c0iw_magic == (uintptr_t)w);
    w->c0iw_magic = 0xdeadc0de;

    /* GCOV_EXCL_START */

    if (w->t0 > 0) {
        struct c0_usage *u = &w->c0iw_usage;

        w->t3 = (w->t3 > w->t0) ? w->t3 : w->t0;
        w->t4 = (w->t4 > w->t3) ? w->t4 : w->t3;
        w->t5 = (w->t5 > w->t4) ? w->t5 : w->t4;
        w->t6 = (w->t6 > w->t5) ? w->t6 : w->t5;

        hse_log(
            HSE_WARNING "c0_ingest: gen %lu/%lu width %u/%u "
                        "keys %lu tombs %lu keykb %lu valkb %lu "
                        "rcu %lu queue %lu bhprep %lu "
                        "build %lu c0ingest %lu "
                        "finwait %lu cningest %lu destroy %lu total %lu",
            (ulong)w->gen,
            (ulong)w->gencur,
            w->c0iw_usage.u_count,
            w->c0iw_iterc,
            (ulong)(u->u_keys + u->u_tombs),
            (ulong)u->u_tombs,
            (ulong)u->u_keyb / 1024,
            (ulong)u->u_valb / 1024,
            (ulong)(w->c0iw_tenqueued - w->c0iw_tingesting) / 1000,
            (ulong)(w->t0 - w->c0iw_tenqueued) / 1000,
            (ulong)(w->t3 - w->t0) / 1000,
            (ulong)(w->t4 - w->t3) / 1000,
            (ulong)(w->t5 - w->t4) / 1000,
            (ulong)(w->t7 - w->t6) / 1000,
            (ulong)(w->t8 - w->t7) / 1000,
            (ulong)(w->t9 - w->t8) / 1000,
            (ulong)(w->t9 - w->t0) / 1000);
    }

    /* GCOV_EXCL_STOP */

    bin_heap2_destroy(w->c0iw_minheap);
}
