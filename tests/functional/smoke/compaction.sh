#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
#
# Copyright (C) 2021-2022 Micron Technology, Inc. All rights reserved.

#doc: test cn tree spill, k-compaction and kv-compaction

. common.subr

trap cleanup EXIT
kvdb_create

counter=0

setup () {
    kvs=$(kvs_create "smoke-$counter")
    counter=$((counter+1))

    # first 6 kvsets have keys 0..999
    cmd putgetdel "$home" "$kvs" -p -s   0 -c 100 kvs-oparms cn_maint_disable=true
    cmd putgetdel "$home" "$kvs" -p -s 100 -c 100 kvs-oparms cn_maint_disable=true
    cmd putgetdel "$home" "$kvs" -p -s 200 -c 200 kvs-oparms cn_maint_disable=true
    cmd putgetdel "$home" "$kvs" -p -s 400 -c 200 kvs-oparms cn_maint_disable=true
    cmd putgetdel "$home" "$kvs" -p -s 600 -c 200 kvs-oparms cn_maint_disable=true
    cmd putgetdel "$home" "$kvs" -p -s 800 -c 200 kvs-oparms cn_maint_disable=true
    # next kvset: update 0..199
    cmd putgetdel "$home" "$kvs" -u -s   0 -c 200 kvs-oparms cn_maint_disable=true
    # next kvset: delete 100.299
    cmd putgetdel "$home" "$kvs" -d -s 100 -c 200  kvs-oparms cn_maint_disable=true
    # Final state:
    #   Updated:   0..99
    #   Deleted: 100..299
    #   Put:     300..999
}

verify_keys () {
    # verify the three ranges before compaction
    cmd putgetdel "$home" "$kvs" -U -s   0 -c 100  kvs-oparms cn_maint_disable=true
    cmd putgetdel "$home" "$kvs" -D -s 100 -c 200  kvs-oparms cn_maint_disable=true
    cmd putgetdel "$home" "$kvs" -P -s 300 -c 700  kvs-oparms cn_maint_disable=true
    # prove putgetdel is actually verifying by asking it to verify 0..1000 and expecting failure
    cmd -e putgetdel "$home" "$kvs" -P -s 0 -c 1000  kvs-oparms cn_maint_disable=true
}

verify_shape_before () {
    # verify there are 8 kvsets in root node
    cmd cn_metrics "$home" "$kvs" | cmd grep -P '^n\s+0\s+8 '
}

verify_shape_after_spill () {
    # verify there are no kvsets in root node
    cmd cn_metrics "$home" "$kvs" | cmd -e grep -P '^n\s+0\s+0 '
}

verify_shape_after_kv_compact () {
    # verify there is 1 kvset in root node
    cmd cn_metrics "$home" "$kvs" | cmd grep -P '^n\s+0\s+1 '
}

verify_shape_after_k_compact () {
    # verify there is 1 kvset in root node
    cmd cn_metrics "$home" "$kvs" | cmd grep -P '^n\s+0\s+1 '
}

# spill
setup
verify_shape_before
cmd putbin "$home" "$kvs" -n 100 kvs-oparms cn_close_wait=true cn_compaction_debug=-1
verify_shape_after_spill

# we can't test k_compact or kv_compact easily because the root node is hard-coded to spill
exit 0

# k_compact
setup
verify_shape_before
cmd putbin "$home" "$kvs" -n 100 kvs-oparms cn_close_wait=true cn_compaction_debug=-1 \
    cn_compact_limit=100 cn_compact_spill=100 cn_compact_only=2
verify_shape_after_k_compact

# kv_compact
setup
verify_shape_before
cmd putbin "$home" "$kvs" -n 100 kvs-oparms cn_close_wait=true cn_compaction_debug=-1 \
    cn_compact_limit=100 cn_compact_spill=100 cn_compact_only=2 \
    cn_compact_waste=0
verify_shape_after_kv_compact
