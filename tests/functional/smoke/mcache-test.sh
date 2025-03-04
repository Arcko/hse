#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
#
# Copyright (C) 2021 Micron Technology, Inc. All rights reserved.

#tdoc: mcache data integrity test

. common.subr

trap cleanup EXIT
kvdb_create

seconds=60

storage="$home/capacity/smoke-mdc-test"

cmd rm -fr "$storage"
cmd mkdir "$storage"

cmd mpiotest -T "$seconds" "$storage"

cmd rm -fr "$storage"
