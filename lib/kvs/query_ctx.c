/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2015-2020 Micron Technology, Inc.  All rights reserved.
 */

#include <hse/util/hash.h>
#include <hse/util/event_counter.h>
#include <hse/util/map.h>

#include <hse/ikvdb/query_ctx.h>

merr_t
qctx_tomb_insert(struct query_ctx *qctx, const void *key, size_t klen)
{
    const uint64_t hash = hse_hash64(key, klen);

    if (!qctx->tomb_map) {
        qctx->tomb_map = map_create(16);
        if (!qctx->tomb_map)
            return merr(ENOMEM);
    }

    return map_insert(qctx->tomb_map, hash, 1);
}

bool
qctx_tomb_seen(struct query_ctx *qctx, const void *key, size_t klen)
{
    const uint64_t hash = hse_hash64(key, klen);

    if (!qctx->tomb_map)
        return false;

    return map_lookup(qctx->tomb_map, hash, NULL);
}
