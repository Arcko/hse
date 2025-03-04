#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
#
# Copyright (C) 2021-2022 Micron Technology, Inc. All rights reserved.

#doc: simple_client test

. common.subr

trap cleanup EXIT
kvdb_create

keys=1000
kvs=$(kvs_create smoke-0)
cmd simple_client "$home" "$kvs" -c "$keys"
