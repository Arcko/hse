#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
#
# Copyright (C) 2021-2022 Micron Technology, Inc. All rights reserved.

#doc: quick longtest focused on cn

. common.subr

trap cleanup EXIT
kvdb_create

kvs=$(kvs_create smoke-0)

threads=4
keys=100000
seed=$RANDOM

# This test requires '--once' to ensure each thread of each invocation of
# longtest runs through exactly one iteration.
for (( phase = 0; phase <= 9; phase++ )); do
    cmd longtest "$home" "$kvs" -t "$threads" -c "$keys" --mphase $((1<<phase)) --verify 100 --seed "$seed" --once -v
    cmd cn_metrics "$home" "$kvs"
done
