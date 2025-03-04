/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2022 Micron Technology, Inc.  All rights reserved.
 */

#define MTF_MOCK_IMPL_mblk_desc

#include <sys/mman.h>

#include <hse/mpool/mpool_structs.h>
#include <hse/mpool/mpool.h>

#include <hse/error/merr.h>

#include <hse/util/assert.h>
#include <hse/util/compiler.h>
#include <hse/util/event_counter.h>
#include <hse/util/page.h>
#include <hse/util/minmax.h>

#include <hse/ikvdb/limits.h>

#include "kvs_mblk_desc.h"

merr_t
mblk_mmap(struct mpool *mp, uint64_t mbid, struct kvs_mblk_desc *md)
{
    merr_t err;
    struct mblock_props props;
    const void *base;

    err = mpool_mblock_props_get(mp, mbid, &props);
    if (ev(err))
        return err;

    err = mpool_mblock_mmap(mp, mbid, &base);
    if (ev(err))
        return err;

    assert(props.mpr_alloc_cap % PAGE_SIZE == 0);
    assert(props.mpr_write_len % PAGE_SIZE == 0);

    md->map_base = (void *)base;
    md->alen_pages = props.mpr_alloc_cap / PAGE_SIZE;
    md->wlen_pages = props.mpr_write_len / PAGE_SIZE;
    md->ra_pages = props.mpr_ra_pages;
    md->mclass = props.mpr_mclass;
    md->mbid = mbid;

    /* Verify mappings to pages and smaller int types don't lose information.
     */
    assert(md->alen_pages * PAGE_SIZE == props.mpr_alloc_cap);
    assert(md->wlen_pages * PAGE_SIZE == props.mpr_write_len);
    assert(md->mclass == props.mpr_mclass);

    return 0;
}

merr_t
mblk_munmap(struct mpool *mp, struct kvs_mblk_desc *md)
{
    merr_t err = 0;

    INVARIANT(mp);
    INVARIANT(md);

    if (md->map_base) {
        assert(md->mbid);
        err = mpool_mblock_munmap(mp, md->mbid);
        if (!err)
            md->map_base = NULL;
    }

    return err;
}

merr_t
mblk_madvise_pages(const struct kvs_mblk_desc *md, size_t pg, size_t pg_cnt, int advice)
{
    const size_t wlen_pages = md->wlen_pages;
    size_t ra_pages;
    size_t chunk = 0;

    if (pg >= wlen_pages)
        return merr(EINVAL);

    if (pg + pg_cnt > wlen_pages)
        pg_cnt = wlen_pages - pg;

    if (pg_cnt == 0)
        return 0;

    ra_pages = (advice == MADV_WILLNEED) ? md->ra_pages : pg_cnt;
    if (ev(!ra_pages))
        return 0;

    for (size_t pg_end = pg + pg_cnt; pg < pg_end; pg += chunk) {
        int rc;

        chunk = min_t(size_t, pg_end - pg, ra_pages);

        /* Cast away the const of map_base for madvise().
         */
        rc = madvise((void *)md->map_base + (pg * PAGE_SIZE), chunk * PAGE_SIZE, advice);
        if (rc)
            return merr(errno);
    }

    return 0;
}

#if HSE_MOCKING
#include "kvs_mblk_desc_ut_impl.i"
#endif
