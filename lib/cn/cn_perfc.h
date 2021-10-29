/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2015-2020 Micron Technology, Inc.  All rights reserved.
 */

/*
 * CN performance counter family.
 *
 * There is one counter set per logical operation of CN.
 */
#ifndef CN_CN_PERFC_H
#define CN_CN_PERFC_H

#include <hse/kvdb_perfc.h>

#include <hse_util/perfc.h>
#include <hse_ikvdb/cn_perfc.h>

struct cn;

uint
cn_perfc_mclass_get_idx(uint agegroup, uint dtype, uint mclass);

void
cn_perfc_alloc(struct cn *cn, uint prio);

void
cn_perfc_free(struct cn *cn);


#endif
