#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
#
# Copyright (C) 2021-2022 Micron Technology, Inc. All rights reserved.

. common.subr

trap cleanup EXIT

kvdb_create
kvs_create "kvs1"
kvs_create "kvs2" prefix.length=32
