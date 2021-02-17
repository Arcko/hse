/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2015-2021 Micron Technology, Inc.  All rights reserved.
 */

#define _GNU_SOURCE

#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ftw.h>

#include <3rdparty/rbtree.h>

#include <hse_util/mutex.h>
#include <hse_util/string.h>
#include <hse_util/slab.h>
#include <hse_util/logging.h>
#include <hse_util/event_counter.h>
#include <hse_util/minmax.h>

#include "mblock_file.h"
#include "io.h"
#include "omf.h"
#include "mclass.h"

#define MBLOCK_FILE_META_HDRLEN (4096)
#define MBLOCK_FILE_META_OIDLEN (8)

#define MBLOCK_FILE_SIZE_MAX ((1ULL << MBID_BLOCK_BITS) << MBLOCK_SIZE_SHIFT)

#define MBLOCK_FILE_UNIQ_DELTA (1024)

/**
 * struct mblock_rgn -
 * @rgn_node:  rb-tree linkage
 * @rgn_start: first available key
 * @rgn_end:   last available key (not inclusive)
 */
struct mblock_rgn {
    struct rb_node rgn_node;
    uint32_t       rgn_start;
    uint32_t       rgn_end;
};

/**
 * struct mblock_rgnmap -
 */
struct mblock_rgnmap {
    struct mutex    rm_lock;
    struct rb_root  rm_root;

    struct kmem_cache *rm_cache __aligned(SMP_CACHE_BYTES);
};

/**
 * struct mblock_file - mblock file handle (one per file)
 *
 * @mbfsp: reference to the fileset handle
 * @smap:  space map
 * @mmap:  mblock map
 * @io:    io handle for sync/async rw ops
 *
 * maxsz: maximum file size (2TiB with 16-bit block offset)
 *
 * fd:   file handle
 * name: file name
 *
 */
struct mblock_file {
    struct mblock_rgnmap rgnmap;

    struct mblock_fset *mbfsp;
    struct mblock_map  *mmap;
    struct io_ops       io;

    size_t         maxsz;
    enum mclass_id mcid;
    int            fileid;
    int            data_fd;

    __aligned(SMP_CACHE_BYTES)
    struct mutex uniq_lock;
    uint32_t uniq;

    __aligned(SMP_CACHE_BYTES)
    struct mutex meta_lock;
    char *meta_addr;
};

/**
 * Region map interfaces.
 */

static merr_t
mblock_rgnmap_init(struct mblock_file *mbfp, const char *name)
{
    struct kmem_cache    *rmcache = NULL;
    struct mblock_rgnmap *rgnmap;
    struct mblock_rgn    *rgn;

    uint32_t rmax;

    rmcache = kmem_cache_create(name, sizeof(*rgn), __alignof(*rgn), 0, NULL);
    if (ev(!rmcache))
        return merr(ENOMEM);

    rgnmap = &mbfp->rgnmap;
    mutex_init(&rgnmap->rm_lock);
    rgnmap->rm_root = RB_ROOT;

    rgn = kmem_cache_alloc(rmcache);
    if (!rgn) {
        kmem_cache_destroy(rmcache);
        return merr(ENOMEM);
    }

    rgn->rgn_start = 1;
    rmax = mbfp->maxsz >> MBLOCK_SIZE_SHIFT;
    rgn->rgn_end = rmax + 1;

    mutex_lock(&rgnmap->rm_lock);
    rb_link_node(&rgn->rgn_node, NULL, &rgnmap->rm_root.rb_node);
    rb_insert_color(&rgn->rgn_node, &rgnmap->rm_root);
    mutex_unlock(&rgnmap->rm_lock);

    rgnmap->rm_cache = rmcache;

    return 0;
}

static uint32_t
mblock_rgn_alloc(struct mblock_rgnmap *rgnmap)
{
    struct mblock_rgn *rgn;
    struct rb_root    *root;
    struct rb_node    *node;
    uint32_t           key;

    rgn = NULL;
    key = 0;

    mutex_lock(&rgnmap->rm_lock);
    root = &rgnmap->rm_root;
    node = rb_first(root);

    if (node) {
        rgn = rb_entry(node, struct mblock_rgn, rgn_node);

        key = rgn->rgn_start++;

        if (rgn->rgn_start < rgn->rgn_end)
            rgn = NULL;
        else
            rb_erase(&rgn->rgn_node, root);
    }
    mutex_unlock(&rgnmap->rm_lock);

    if (rgn)
        kmem_cache_free(rgnmap->rm_cache, rgn);

    return key;
}

static merr_t
mblock_rgn_insert(struct mblock_rgnmap *rgnmap, uint32_t key)
{
    struct mblock_rgn *this, *to_free = NULL;
    struct rb_root *root;
    struct rb_node *node, **new, *parent;

    uint32_t start, end;
    merr_t   err = 0;

    mutex_lock(&rgnmap->rm_lock);
    root = &rgnmap->rm_root;
    node = root->rb_node;

    while (node) {
        this = rb_entry(node, struct mblock_rgn, rgn_node);

        if (key < this->rgn_start)
            node = node->rb_left;
        else if (key >= this->rgn_end)
            node = node->rb_right;
        else
            break; /* Found overlapping node */
    };

    if (!node) {
        err = merr(ENOENT);
        goto exit;
    }

    if (key == this->rgn_start) {
        this->rgn_start++;
        if (this->rgn_start == this->rgn_end) {
            rb_erase(&this->rgn_node, root);
            to_free = this;
        }
        node = NULL;
    } else if (key == this->rgn_end - 1) {
        this->rgn_end--;
        node = NULL;
    }

    if (!node)
        goto exit;

    /* Split the current node and find a position for the new node */
    start = this->rgn_start;
    end = key;
    this->rgn_start = key + 1;

    new = &node->rb_left;
    parent = node;

    while (*new) {
        this = rb_entry(*new, struct mblock_rgn, rgn_node);
        parent = *new;

        if (this->rgn_end == start) {
            this->rgn_end = end;
            new = NULL;
            break;
        }

        new = &(*new)->rb_right;
    }

    if (new) {
        struct mblock_rgn *rgn;

        /* Allocate new node and link it at parent */
        rgn = kmem_cache_alloc(rgnmap->rm_cache);
        if (ev(!rgn)) {
            err = merr(ENOMEM);
            goto exit;
        }

        rgn->rgn_start = start;
        rgn->rgn_end = end;

        rb_link_node(&rgn->rgn_node, parent, new);
        rb_insert_color(&rgn->rgn_node, root);
    }

exit:
    mutex_unlock(&rgnmap->rm_lock);

    if (to_free)
        kmem_cache_free(rgnmap->rm_cache, to_free);

    return err;
}

static merr_t
mblock_rgn_free(struct mblock_rgnmap *rgnmap, uint32_t key)
{
    struct mblock_rgn *this, *that;
    struct rb_node **new, *parent;
    struct rb_node *nxtprv;
    struct rb_root *root;

    merr_t err = 0;

    assert(rgnmap && key > 0);

    this = that = NULL;
    parent = NULL;
    nxtprv = NULL;

    mutex_lock(&rgnmap->rm_lock);
    root = &rgnmap->rm_root;
    new = &root->rb_node;

    while (*new) {
        this = rb_entry(*new, struct mblock_rgn, rgn_node);
        parent = *new;

        if (key < this->rgn_start) {
            if (key == this->rgn_start - 1) {
                --this->rgn_start;
                nxtprv = rb_prev(*new);
                new = NULL;
                break;
            }
            new = &(*new)->rb_left;
        } else if (key >= this->rgn_end) {
            if (key == this->rgn_end) {
                ++this->rgn_end;
                nxtprv = rb_next(*new);
                new = NULL;
                break;
            }
            new = &(*new)->rb_right;
        } else {
            new = NULL;
            err = merr(ENOENT);
            break;
        }
    }

    if (nxtprv) {
        that = rb_entry(nxtprv, struct mblock_rgn, rgn_node);

        if (this->rgn_start == that->rgn_end) {
            this->rgn_start = that->rgn_start;
            rb_erase(&that->rgn_node, root);
        } else if (this->rgn_end == that->rgn_start) {
            this->rgn_end = that->rgn_end;
            rb_erase(&that->rgn_node, root);
        } else {
            that = NULL;
        }
    } else if (new) {
        struct mblock_rgn *rgn;

        rgn = kmem_cache_alloc(rgnmap->rm_cache);
        if (rgn) {
            rgn->rgn_start = key;
            rgn->rgn_end = key + 1;

            rb_link_node(&rgn->rgn_node, parent, new);
            rb_insert_color(&rgn->rgn_node, root);
        }
    }
    mutex_unlock(&rgnmap->rm_lock);

    if (that)
        kmem_cache_free(rgnmap->rm_cache, that);

    return err;
}

static merr_t
mblock_rgn_find(struct mblock_rgnmap *rgnmap, uint32_t key)
{
    struct mblock_rgn *this;
    struct rb_node *cur;

    assert(rgnmap && key > 0);

    mutex_lock(&rgnmap->rm_lock);
    cur = rgnmap->rm_root.rb_node;

    while (cur) {
        this = rb_entry(cur, struct mblock_rgn, rgn_node);

        if (key < this->rgn_start)
            cur = cur->rb_left;
        else if (key >= this->rgn_end)
            cur = cur->rb_right;
        else
            break;
    }

    mutex_unlock(&rgnmap->rm_lock);

    return cur ? merr(ENOENT) : 0;
}

/**
 * Mblock file meta interfaces.
 */

static uint32_t
block_id(uint64_t mbid)
{
    return mbid & MBID_BLOCK_MASK;
}

static uint64_t
block_off(uint64_t mbid)
{
    return ((uint64_t)block_id(mbid)) << MBLOCK_SIZE_SHIFT;
}

size_t
mblock_file_meta_len(void)
{
    size_t mblkc;

    mblkc = MBLOCK_FILE_SIZE_MAX >> MBLOCK_SIZE_SHIFT;

    return MBLOCK_FILE_META_HDRLEN + mblkc * MBLOCK_FILE_META_OIDLEN;
}

static merr_t
mblock_file_meta_format(struct mblock_file *mbfp, struct mblock_filehdr *fh)
{
    char *addr;
    int   rc;

    addr = mbfp->meta_addr;

    omf_mblock_filehdr_pack_htole(fh, addr);

    rc = msync((void *)((unsigned long)addr & PAGE_MASK), PAGE_SIZE, MS_SYNC);
    if (ev(rc < 0))
        return merr(errno);

    return 0;
}

static merr_t
mblock_file_meta_load(struct mblock_file *mbfp)
{
    struct mblock_filehdr fh = {};
    char *                addr, *bound;
    size_t                mblkc = 0;

    addr = mbfp->meta_addr;

    /* Validate per-file header */
    omf_mblock_filehdr_unpack_letoh(&fh, addr);
    if (fh.fileid != mbfp->fileid)
        return merr(EBADMSG);

    mbfp->uniq = fh.uniq + MBLOCK_FILE_UNIQ_DELTA;

    bound = addr + mblock_file_meta_len();
    addr += MBLOCK_FILE_META_HDRLEN;

    while (addr < bound) {
        struct mblock_oid_omf *mbomf;
        uint64_t               mbid;
        merr_t                 err;

        mbomf = (struct mblock_oid_omf *)addr;

        mbid = omf_mblk_id(mbomf);
        if (mbid != 0) {
            mblkc++; /* Debug */

            err = mblock_file_insert(mbfp, mbid);
            if (ev(err))
                return merr(EBADMSG);
        }

        addr += MBLOCK_FILE_META_OIDLEN;
    }

    hse_log(
        HSE_NOTICE "%s: mclass %d, file-id %d found %lu valid mblocks, uniq %u.",
        __func__,
        mbfp->mcid,
        mbfp->fileid,
        mblkc,
        mbfp->uniq);

    return 0;
}

static merr_t
mblock_file_meta_log(struct mblock_file *mbfp, uint64_t *mbidv, int mbidc, bool delete)
{
    struct mblock_oid_omf *mbomf;

    uint32_t blockid;
    char    *addr;
    int      rc;
    merr_t   err = 0;

    if (ev(!mbfp || !mbidv))
        return merr(EINVAL);

    if (mbidc > 1)
        return merr(ENOTSUP);

    addr = mbfp->meta_addr;
    blockid = block_id(*mbidv);

    addr += MBLOCK_FILE_META_HDRLEN;
    addr += (blockid * MBLOCK_FILE_META_OIDLEN);

    mbomf = (struct mblock_oid_omf *)addr;

    mutex_lock(&mbfp->meta_lock);

    omf_set_mblk_id(mbomf, delete ? 0 : *mbidv);

    rc = msync((void *)((unsigned long)addr & PAGE_MASK), PAGE_SIZE, MS_SYNC);
    if (ev(rc < 0))
        err = merr(errno);

    mutex_unlock(&mbfp->meta_lock);

    return err;
}

/**
 * Mblock file interfaces.
 */

merr_t
mblock_file_open(
    struct mblock_fset  *mbfsp,
    struct media_class  *mc,
    int                  fileid,
    int                  flags,
    char                *meta_addr,
    struct mblock_file **handle)
{
    struct mblock_file *mbfp;
    enum mclass_id      mcid;

    int    fd, rc, dirfd;
    merr_t err = 0;
    char   name[32], rname[32];
    bool   create = false;

    if (ev(!mbfsp || !mc || !meta_addr || !handle))
        return merr(EINVAL);

    if (flags == 0 || !(flags & (O_RDWR | O_RDONLY | O_WRONLY)))
        flags |= O_RDWR;

    flags &= O_RDWR | O_RDONLY | O_WRONLY | O_CREAT;
    if (flags & O_CREAT) {
        create = true;
        flags |= O_EXCL;
    }

    mcid = mclass_id(mc);
    dirfd = mclass_dirfd(mc);
    snprintf(name, sizeof(name), "%s-%d-%d", MBLOCK_DATA_FILE_PFX, mcid, fileid);

    rc = faccessat(dirfd, name, F_OK, 0);
    if (rc < 0 && errno == ENOENT && !create)
        return merr(ENOENT);
    if (rc == 0 && create)
        return merr(EEXIST);

    mbfp = calloc(1, sizeof(*mbfp));
    if (ev(!mbfp))
        return merr(ENOMEM);

    mbfp->data_fd = -1;
    mbfp->mbfsp = mbfsp;
    mbfp->meta_addr = meta_addr;
    mbfp->fileid = fileid;
    mbfp->mcid = mcid;

    mbfp->maxsz = MBLOCK_FILE_SIZE_MAX;
    snprintf(rname, sizeof(rname), "%s-%d-%d", "rgnmap", mcid, fileid);
    err = mblock_rgnmap_init(mbfp, rname);
    if (ev(err)) {
        free(mbfp);
        return err;
    }

    if (create) {
        struct mblock_filehdr fh = {};

        fh.fileid = fileid;
        err = mblock_file_meta_format(mbfp, &fh);
    } else {
        err = mblock_file_meta_load(mbfp);
    }
    if (ev(err))
        goto err_exit;

    fd = openat(dirfd, name, flags | O_DIRECT | O_SYNC, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        err = merr(errno);
        hse_elog(
            HSE_ERR "%s: open/create data file failed, file name %s: @@e", err, __func__, name);
        goto err_exit;
    }
    mbfp->data_fd = fd;

    /* ftruncate to the maximum size to make it a sparse file */
    rc = ftruncate(fd, mbfp->maxsz);
    if (rc < 0) {
        err = merr(errno);
        hse_elog(HSE_ERR "%s: Truncating data file failed, file name %s: @@e", err, __func__, name);
        goto err_exit;
    }

    mbfp->io = io_sync_ops;

    mutex_init(&mbfp->uniq_lock);
    mutex_init(&mbfp->meta_lock);

    *handle = mbfp;

    return 0;

err_exit:
    mblock_file_close(mbfp);

    if (create)
        unlinkat(dirfd, name, 0);

    return err;
}

void
mblock_file_close(struct mblock_file *mbfp)
{
    struct mblock_rgnmap *rgnmap;
    struct mblock_rgn    *rgn, *next;

    if (!mbfp)
        return;

    rgnmap = &mbfp->rgnmap;

    rbtree_postorder_for_each_entry_safe(rgn, next, &rgnmap->rm_root, rgn_node)
    {
        kmem_cache_free(rgnmap->rm_cache, rgn);
    }

    if (rgnmap->rm_cache) {
        kmem_cache_destroy(rgnmap->rm_cache);
        rgnmap->rm_cache = NULL;
    }

    if (mbfp->data_fd != -1)
        close(mbfp->data_fd);

    free(mbfp);
}

merr_t
mblock_file_insert(struct mblock_file *mbfp, uint64_t mbid)
{
    return mblock_rgn_insert(&mbfp->rgnmap, block_id(mbid) + 1);
}

static merr_t
mblock_uniq_gen(struct mblock_file *mbfp, uint32_t *uniqout)
{
    uint32_t uniq;
    merr_t   err = 0;

    mutex_lock(&mbfp->uniq_lock);

    uniq = ++mbfp->uniq;
    if (uniq % MBLOCK_FILE_UNIQ_DELTA == 0) {
        struct mblock_filehdr fh = {};

        fh.fileid = mbfp->fileid;
        fh.uniq = uniq;

        err = mblock_file_meta_format(mbfp, &fh);
    }

    mutex_unlock(&mbfp->uniq_lock);

    if (!err && uniqout)
        *uniqout = uniq;

    return err;
}

merr_t
mblock_file_alloc(struct mblock_file *mbfp, int mbidc, uint64_t *mbidv)
{
    uint64_t mbid;
    uint32_t block, uniq;
    merr_t   err;

    if (ev(!mbfp || !mbidv))
        return merr(EINVAL);

    if (ev(mbidc > 1))
        return merr(ENOTSUP);

    block = mblock_rgn_alloc(&mbfp->rgnmap);
    if (block == 0)
        return merr(ENOSPC);

    err = mblock_uniq_gen(mbfp, &uniq);
    if (ev(err)) {
        mblock_rgn_free(&mbfp->rgnmap, block);
        return err;
    }

    if ((mbfp->fileid & (MBID_FILEID_MASK >> MBID_FILEID_SHIFT)) != mbfp->fileid ||
        (mbfp->mcid & (MBID_MCID_MASK >> MBID_MCID_SHIFT)) != mbfp->mcid ||
        ((block - 1) & MBID_BLOCK_MASK) != block - 1) {
        mblock_rgn_free(&mbfp->rgnmap, block);
        return merr(EBUG);
    }

    mbid = 0;
    mbid |= ((uint64_t)uniq << MBID_UNIQ_SHIFT);
    mbid |= ((uint64_t)mbfp->fileid << MBID_FILEID_SHIFT);
    mbid |= ((uint64_t)mbfp->mcid << MBID_MCID_SHIFT);
    mbid |= (block - 1);

    *mbidv = mbid;

    return 0;
}

merr_t
mblock_file_find(struct mblock_file *mbfp, uint64_t *mbidv, int mbidc)
{
    uint32_t block;
    merr_t   err;

    if (ev(!mbfp || !mbidv))
        return merr(EINVAL);

    if (ev(mbidc > 1))
        return merr(ENOTSUP);

    block = block_id(*mbidv);

    err = mblock_rgn_find(&mbfp->rgnmap, block + 1);
    ev(err);

    return err;
}

merr_t
mblock_file_commit(struct mblock_file *mbfp, uint64_t *mbidv, int mbidc)
{
    merr_t err;
    bool delete = false;

    if (ev(!mbfp || !mbidv))
        return merr(EINVAL);

    if (ev(mbidc > 1))
        return merr(ENOTSUP);

    err = mblock_file_find(mbfp, mbidv, mbidc);
    if (ev(err))
        return err;

    err = mblock_file_meta_log(mbfp, mbidv, mbidc, delete);
    if (ev(err))
        return err;

    return 0;
}

merr_t
mblock_file_abort(struct mblock_file *mbfp, uint64_t *mbidv, int mbidc)
{
    uint32_t block;
    merr_t   err;

    if (ev(!mbfp || !mbidv))
        return merr(EINVAL);

    if (ev(mbidc > 1))
        return merr(ENOTSUP);

    block = block_id(*mbidv);

    err = mblock_rgn_free(&mbfp->rgnmap, block + 1);
    if (ev(err))
        return err;

    return 0;
}

merr_t
mblock_file_delete(struct mblock_file *mbfp, uint64_t *mbidv, int mbidc)
{
    uint64_t block;
    merr_t   err;
    int      rc;
    bool delete = true;

    if (ev(!mbfp || !mbidv))
        return merr(EINVAL);

    if (ev(mbidc > 1))
        return merr(ENOTSUP);

    block = block_id(*mbidv);

    /* First log the delete */
    err = mblock_file_meta_log(mbfp, mbidv, mbidc, delete);
    if (ev(err))
        return err;

    /* Discard mblock */
    rc = fallocate(
        mbfp->data_fd,
        FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
        block_off(*mbidv),
        MBLOCK_SIZE_BYTES);
    ev(rc);

    err = mblock_rgn_free(&mbfp->rgnmap, block + 1);
    if (ev(err))
        return err;

    return 0;
}

merr_t
mblock_file_read(
    struct mblock_file *mbfp,
    uint64_t            mbid,
    const struct iovec *iov,
    int                 iovc,
    off_t               off)
{
    uint64_t roff;
    bool     verify = true;

    if (ev(!mbfp || !iov))
        return merr(EINVAL);

    if (iovc == 0)
        return 0;

    /* TODO: Add offset and len validation */

    if (verify) {
        merr_t err;

        err = mblock_file_find(mbfp, &mbid, 1);
        if (ev(err))
            return err;
    }

    roff = block_off(mbid);
    roff += off;

    return mbfp->io.read(mbfp->data_fd, roff, iov, iovc, 0);
}

merr_t
mblock_file_write(
    struct mblock_file *mbfp,
    uint64_t            mbid,
    const struct iovec *iov,
    int                 iovc,
    off_t               off)
{
    uint64_t woff;
    bool     verify = true;

    if (ev(!mbfp || !iov))
        return merr(EINVAL);

    /* TODO: Add offset and len validation */

    if (iovc == 0)
        return 0;

    if (verify) {
        merr_t err;

        err = mblock_file_find(mbfp, &mbid, 1);
        if (ev(err))
            return err;
    }

    woff = block_off(mbid);
    woff += off;

    return mbfp->io.write(mbfp->data_fd, woff, iov, iovc, 0);
}
