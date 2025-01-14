/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2021-2022 Micron Technology, Inc.  All rights reserved.
 */

#ifndef HSE_KVDB_OMF_VERSION_H
#define HSE_KVDB_OMF_VERSION_H

enum {
    GLOBAL_OMF_VERSION1 = 1,
    GLOBAL_OMF_VERSION2 = 2,
    GLOBAL_OMF_VERSION3 = 3,
    GLOBAL_OMF_VERSION4 = 4,
};

enum {
    CNDB_VERSION1 = 1,
};

enum {
    HBLOCK_HDR_VERSION1 = 1
};

enum {
    VGROUP_MAP_VERSION1 = 1
};

enum {
    KBLOCK_HDR_VERSION6 = 6,
};

enum {
    VBLOCK_FOOTER_VERSION1 = 1,
};

enum {
    BLOOM_OMF_VERSION5 = 5,
};

enum {
    WBT_TREE_VERSION6 = 6,
};

enum {
    CN_TSTATE_VERSION1 = 1,
    CN_TSTATE_VERSION2 = 2,
};

enum {
    MBLOCK_METAHDR_VERSION1 = 1,
    MBLOCK_METAHDR_VERSION2 = 2,
};

enum {
    MDC_LOGHDR_VERSION1 = 1,
    MDC_LOGHDR_VERSION2 = 2,
};

enum {
    WAL_VERSION1 = 1,
    WAL_VERSION2 = 2,
};

enum {
    KVDB_META_VERSION1 = 1,
    KVDB_META_VERSION2 = 2,
};

#define GLOBAL_OMF_VERSION     GLOBAL_OMF_VERSION4

/* In the event one of the following versions in incremented, increment the
 * global OMF version.
 */

#define CNDB_VERSION           CNDB_VERSION1
#define HBLOCK_HDR_VERSION     HBLOCK_HDR_VERSION1
#define VGROUP_MAP_VERSION     VGROUP_MAP_VERSION1
#define KBLOCK_HDR_VERSION     KBLOCK_HDR_VERSION6
#define VBLOCK_FOOTER_VERSION  VBLOCK_FOOTER_VERSION1
#define BLOOM_OMF_VERSION      BLOOM_OMF_VERSION5
#define WBT_TREE_VERSION       WBT_TREE_VERSION6
#define CN_TSTATE_VERSION      CN_TSTATE_VERSION2
#define MBLOCK_METAHDR_VERSION MBLOCK_METAHDR_VERSION2
#define MDC_LOGHDR_VERSION     MDC_LOGHDR_VERSION2
#define WAL_VERSION            WAL_VERSION2
#define KVDB_META_VERSION      KVDB_META_VERSION2

#endif
