/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2015-2022 Micron Technology, Inc.  All rights reserved.
 */

#define MTF_MOCK_IMPL_perfc

#include <bsd/string.h>
#include <cjson/cJSON.h>
#include <cjson/cJSON_Utils.h>

#include <hse/util/platform.h>
#include <hse/util/alloc.h>
#include <hse/util/slab.h>
#include <hse/util/data_tree.h>
#include <hse/util/minmax.h>
#include <hse/util/parse_num.h>
#include <hse/util/log2.h>
#include <hse/util/event_counter.h>
#include <hse/util/xrand.h>
#include <hse/logging/logging.h>
#include <hse/util/perfc.h>

static const char *const perfc_ctr_type2name[] = {
    "Invalid", "Basic", "Rate", "Latency", "Distribution", "SimpleLatency",
};

struct perfc_ivl *perfc_di_ivl HSE_READ_MOSTLY;

static bool
perfc_ra_emit(struct perfc_rate *const rate, cJSON *const ctr)
{
    bool bad = false;
    struct perfc_val *val;
    u64  dt, dx, ops;
    u64  vadd, vsub;
    u64  curr, prev;
    u64  curr_ns;
    int  i;

    curr_ns = get_time_ns();
    dt = curr_ns - rate->pcr_old_time_ns;

    if (rate->pcr_old_time_ns == 0 || curr_ns < rate->pcr_old_time_ns)
        dt = 0;

    prev = rate->pcr_old_val;

    val = rate->pcr_hdr.pch_val;
    vadd = vsub = curr = 0;

    for (i = 0; i < PERFC_VALPERCNT; ++i) {
        vadd += atomic_read(&val->pcv_vadd);
        vsub += atomic_read(&val->pcv_vsub);
        val += PERFC_VALPERCPU;
    }

    curr = (vadd > vsub) ? (vadd - vsub) : 0;

    rate->pcr_old_time_ns = curr_ns;
    dx = curr - prev;
    rate->pcr_old_val = curr;

    ops = dt > 0 ? (dx * NSEC_PER_SEC) / dt : 0;

    bad |= !cJSON_AddNumberToObject(ctr, "delta_ns", dt);
    bad |= !cJSON_AddNumberToObject(ctr, "current", curr);
    bad |= !cJSON_AddNumberToObject(ctr, "previous", prev);
    bad |= !cJSON_AddNumberToObject(ctr, "rate", ops);

    if (vsub > 0) {
        bad |= !cJSON_AddNumberToObject(ctr, "vadd", vadd);
        bad |= !cJSON_AddNumberToObject(ctr, "vsub", vsub);
    } else {
        bad |= !cJSON_AddNullToObject(ctr, "vadd");
        bad |= !cJSON_AddNullToObject(ctr, "vsub");
    }

    return !bad;
}

static bool
perfc_di_emit(struct perfc_dis *const dis, cJSON *const ctr)
{
    bool bad = false;
    cJSON *histogram;
    unsigned long samples, avg, sum, bound;
    const struct perfc_ivl *ivl = dis->pdi_ivl;

    samples = sum = bound = 0;

    histogram = cJSON_AddArrayToObject(ctr, "histogram");
    if (ev(!histogram))
        return merr(ENOMEM);

    for (size_t i = 0; i < ivl->ivl_cnt + 1; ++i) {
        cJSON *bucket;
        ulong hits, val;
        struct perfc_bkt *bkt;

        bucket = cJSON_CreateObject();
        if (ev(!bucket)) {
            bad = true;
            goto out;
        }

        bkt = dis->pdi_hdr.pch_bktv + i;
        hits = val = 0;

        for (size_t j = 0; j < PERFC_GRP_MAX; ++j) {
            val += atomic_read(&bkt->pcb_vadd);
            hits += atomic_read(&bkt->pcb_hits);
            bkt += PERFC_IVL_MAX + 1;
        }

        avg = (hits > 0) ? val / hits : 0;

        bad |= !cJSON_AddNumberToObject(bucket, "hits", hits);
        bad |= !cJSON_AddNumberToObject(bucket, "average", avg);
        bad |= !cJSON_AddNumberToObject(bucket, "boundary", bound);

        if (bad || !cJSON_AddItemToArray(histogram, bucket)) {
            cJSON_Delete(bucket);
            goto out;
        }

        if (i < ivl->ivl_cnt)
            bound = ivl->ivl_bound[i];
        samples += hits;
        sum += val;
    }

    avg = (samples > 0) ? sum / samples : 0;

    bad |= !cJSON_AddNumberToObject(ctr, "minimum", dis->pdi_min);
    bad |= !cJSON_AddNumberToObject(ctr, "maximum", dis->pdi_max);
    bad |= !cJSON_AddNumberToObject(ctr, "average", avg);

    /* 'sum' and 'hitcnt' field names must match here and for simple lat
     * counters
     */
    bad |= !cJSON_AddNumberToObject(ctr, "sum", sum);
    bad |= !cJSON_AddNumberToObject(ctr, "hits", samples ? samples : 1);
    bad |= !cJSON_AddNumberToObject(ctr, "percentage",
        dis->pdi_pct * 100 / (1.0 * PERFC_PCT_SCALE));

out:
    if (bad)
        cJSON_Delete(ctr);

    return !bad;
}

static void
perfc_read_hdr(struct perfc_ctr_hdr *hdr, u64 *vadd, u64 *vsub)
{
    struct perfc_val *val = hdr->pch_val;

    *vadd = *vsub = 0;

    /* Must skip by values-per-cpu due to how multiple per-cpu values
     * from different counters are packed into cache lines.  E.g.,
     * summing over val[i].pcv_vadd would go horribly awry...
     */
    for (int i = 0; i < PERFC_VALPERCNT; ++i) {
        *vadd += atomic_read(&val->pcv_vadd);
        *vsub += atomic_read(&val->pcv_vsub);
        val += PERFC_VALPERCPU;
    }
}

void
perfc_read(struct perfc_set *pcs, const u32 cidx, u64 *vadd, u64 *vsub)
{
    struct perfc_seti *pcsi;

    pcsi = perfc_ison(pcs, cidx);
    if (pcsi)
        perfc_read_hdr(&pcsi->pcs_ctrv[cidx].hdr, vadd, vsub);
}

static size_t
perfc_emit_handler_ctrset(struct dt_element *const dte, cJSON *const root)
{
    cJSON *ctrs;
    cJSON *ctrset;
    merr_t err = 0;
    bool bad = false;
    struct perfc_ctr_hdr *ctr_hdr;
    struct perfc_seti *seti = dte->dte_data;

    INVARIANT(dte);
    INVARIANT(cJSON_IsArray(root));

    ctrset = cJSON_CreateObject();
    if (ev(!ctrset))
        return merr(ENOMEM);

    bad |= !cJSON_AddStringToObject(ctrset, "path", dte->dte_path);
    bad |= !cJSON_AddStringToObject(ctrset, "name", seti->pcs_ctrseti_name);
    bad |= !cJSON_AddNumberToObject(ctrset, "enabled", seti->pcs_handle->ps_bitmap);

    if (ev(bad)) {
        err = merr(ENOMEM);
        goto out;
    }

    ctrs = cJSON_AddArrayToObject(ctrset, "counters");
    if (ev(!ctrs)) {
        err = merr(ENOMEM);
        goto out;
    }

    /* Emit all the counters of the counter set instance. */
    for (uint32_t cidx = 0; cidx < seti->pcs_ctrc; cidx++) {
        cJSON *ctr;
        u64 vadd, vsub;

        ctr = cJSON_CreateObject();
        if (ev(!ctr)) {
            err = merr(ENOMEM);
            goto out;
        }

        ctr_hdr = &seti->pcs_ctrv[cidx].hdr;

        bad |= !cJSON_AddStringToObject(ctr, "name", seti->pcs_ctrnamev[cidx].pcn_name);
        bad |= !cJSON_AddStringToObject(ctr, "header", seti->pcs_ctrnamev[cidx].pcn_hdr);
        bad |= !cJSON_AddStringToObject(ctr, "description", seti->pcs_ctrnamev[cidx].pcn_desc);
        bad |= !cJSON_AddStringToObject(ctr, "type", perfc_ctr_type2name[ctr_hdr->pch_type]);
        bad |= !cJSON_AddNumberToObject(ctr, "level", ctr_hdr->pch_level);
        bad |= !cJSON_AddNumberToObject(ctr, "enabled",
            (seti->pcs_handle->ps_bitmap & (1ul << cidx)) >> cidx);

        switch (ctr_hdr->pch_type) {
        case PERFC_TYPE_BA:
            perfc_read_hdr(ctr_hdr, &vadd, &vsub);
            vadd = vadd > vsub ? vadd - vsub : 0;

            bad |= !cJSON_AddNumberToObject(ctr, "value", vadd);

            break;

        case PERFC_TYPE_RA:
            bad |= !perfc_ra_emit(&seti->pcs_ctrv[cidx].rate, ctr);

            break;

        case PERFC_TYPE_SL:
            perfc_read_hdr(ctr_hdr, &vadd, &vsub);

            bad |= !cJSON_AddNumberToObject(ctr, "sum", vadd);
            bad |= !cJSON_AddNumberToObject(ctr, "hits", vsub);

            break;

        case PERFC_TYPE_DI:
        case PERFC_TYPE_LT:
            bad |= !perfc_di_emit(&seti->pcs_ctrv[cidx].dis, ctr);

            break;

        default:
            break;
        }

        if (bad || !cJSON_AddItemToArray(ctrs, ctr)) {
            err = merr(ENOMEM);
            goto out;
        }
    }

out:
    if (ev(err)) {
        cJSON_Delete(ctrset);
    } else {
        cJSON_AddItemToArray(root, ctrset);
    }

    return err;
}

/**
 * perfc_emit_handler() - the output fits into a YAML document. spacing is driven by
 * YAML context.
 * @dte:
 * @yc:
 *
 * A performance (with its preceding data and perfc elements
 * looks like this:
 * data:
 *   - perfc:
 *     - path: /data/perfc/mpool/mpool_01223/open_count
 *       count: 17
 *
 * Fields are indented 6 spaces.
 */
static merr_t
perfc_emit_handler(struct dt_element *const dte, cJSON *const ctrset)
{
    return perfc_emit_handler_ctrset(dte, ctrset);
}

/**
 * perfc_remove_handler_ctrset()
 * @dte:
 *
 * Handle called by the tree to free a counter set instance.
 */
static void
perfc_remove_handler_ctrset(struct dt_element *dte)
{
    free(dte->dte_data);
    free(dte);
}

static void
perfc_remove_handler(struct dt_element *dte)
{
    perfc_remove_handler_ctrset(dte);
}

struct dt_element_ops perfc_ops = {
    .dto_emit = perfc_emit_handler,
    .dto_remove = perfc_remove_handler,
};

static struct dt_element_ops perfc_root_ops = { 0 };

merr_t
perfc_init(void)
{
    static struct dt_element hse_dte_perfc = {
        .dte_ops = &perfc_root_ops,
        .dte_file = REL_FILE(__FILE__),
        .dte_line = __LINE__,
        .dte_func = __func__,
        .dte_path = PERFC_DT_PATH,
    };
    u64    boundv[PERFC_IVL_MAX];
    u64    bound;
    merr_t err;

    /* Create the bounds vector for the default latency distribution
     * histogram.  The first ten bounds run from 100ns to 1us with a
     * 100ns step.  The remaining bounds run from 1us on up initially
     * with a power-of-two step, and then with a power-of-four step,
     * rounding each bound down to a number that is readable (i.e.,
     * having only one or two significant digits).
     */
    assert(NELEM(boundv) > 9);
    bound = 100;
    for (int i = 0; i < PERFC_IVL_MAX; ++i) {
        ulong b;
        ulong mult;

        /* The first ten bounds run from 100ns to 1us with a 100ns
         * step...
         */
        if (i < 9) {
            boundv[i] = bound * (i + 1);
            continue;
        }

        /* ... and the remaining bounds run from 1us on up initially
         * with a power-of-two step, and then with a power-of-four step,
         * rounding each bound down to a number that is readable (i.e.,
         * having only one or two significant digits).
         */
        if (bound == 100)
            bound = 1000;

        mult = 1;
        b = bound;
        while (b > 30) {
            b /= 10;
            mult *= 10;
        }

        boundv[i] = b * mult;
        bound *= i < 23 ? 2 : 4;
    }

    err = perfc_ivl_create(PERFC_IVL_MAX, boundv, &perfc_di_ivl);
    if (ev(err))
        return err;

    err = dt_add(&hse_dte_perfc);
    if (ev(err)) {
        perfc_ivl_destroy(perfc_di_ivl);
        perfc_di_ivl = NULL;
        return err;
    }

    return 0;
}

void
perfc_fini(void)
{
    dt_remove_recursive(PERFC_DT_PATH);

    perfc_ivl_destroy(perfc_di_ivl);
    perfc_di_ivl = NULL;
}

merr_t
perfc_ivl_create(int boundc, const u64 *boundv, struct perfc_ivl **ivlp)
{
    struct perfc_ivl *ivl;
    size_t            sz;
    int               i, j;

    *ivlp = NULL;

    if (ev(boundc < 1 || boundc > PERFC_IVL_MAX))
        return merr(EINVAL);

    sz = sizeof(*ivl);
    sz += sizeof(ivl->ivl_bound[0]) * boundc;
    sz = ALIGN(sz, HSE_ACP_LINESIZE);

    ivl = aligned_alloc(HSE_ACP_LINESIZE, sz);
    if (ev(!ivl))
        return merr(ENOMEM);

    memset(ivl, 0, sz);
    ivl->ivl_cnt = boundc;
    memcpy(ivl->ivl_bound, boundv, sizeof(*boundv) * boundc);

    i = j = 0;
    while (i < NELEM(ivl->ivl_map) && j < ivl->ivl_cnt) {
        ivl->ivl_map[i] = j;

        if ((1ul << i) < ivl->ivl_bound[j])
            ++i;
        else
            ++j;
    }

    if (j >= ivl->ivl_cnt)
        --j;

    while (i < NELEM(ivl->ivl_map))
        ivl->ivl_map[i++] = j;

    *ivlp = ivl;

    return 0;
}

void
perfc_ivl_destroy(struct perfc_ivl *ivl)
{
    free(ivl);
}

static enum perfc_type
perfc_ctr_name2type(const char *ctrname, char *type, char *family, char *mean)
{
    static const char list[] = "BA,RA,LT,DI,SL"; /* must be in perfc_type order */
    const char *pc;
    int n;

    n = sscanf(ctrname, "PERFC_%[A-Z]_%[A-Z0-9]_%[_A-Z0-9]", type, family, mean);

    if (n == 3 && type[1]) {
        pc = strstr(list, type);
        if (pc)
            return ((pc - list) / 3) + 1;
    }

    return PERFC_TYPE_INVAL;
}

static merr_t
perfc_access_dte(void *data, void *ctx)
{
    struct perfc_set *setp = ctx;
    struct perfc_seti *seti = data;

    seti->pcs_handle = setp;
    setp->ps_seti = seti;

    return 0;
}

merr_t
perfc_alloc_impl(
    uint                     prio,
    const char              *group,
    const struct perfc_name *ctrv,
    size_t                   ctrc,
    const char              *ctrseti_name,
    const char              *file,
    int                      line,
    struct perfc_set *       setp)
{
    enum perfc_type typev[PERFC_CTRS_MAX];
    char family[DT_PATH_ELEMENT_MAX];
    struct perfc_seti *seti = NULL;
    struct dt_element *dte = NULL;
    char path[DT_PATH_MAX];
    void *valdata, *valcur;
    size_t valdatasz, sz;
    size_t familylen;
    merr_t err = 0;
    u32 n, i;
    int rc;

    if (!group || !ctrv || ctrc < 1 || ctrc > PERFC_CTRS_MAX || !setp)
        return merr(EINVAL);

    setp->ps_seti = NULL;
    setp->ps_bitmap = 0;

    if (!ctrseti_name)
        ctrseti_name = "set";

    family[0] = '\000';
    familylen = 0;

    /*
     * Verify all the counter names in the set and determine their types.
     *
     * The counter name syntax is:
     *
     * PERFC_<type>_<family>_<meaning>
     *
     * <type>     one of "BA", "RA", "LT", "DI", "SI"
     * <family>   [A-Z0-9]+
     * <meaning>  [_A-Z0-9]+
     *
     * where all counters in a set must have the same <family>, and then
     * <meaning> distinguishes different counters of the same type (so
     * hierarchically speaking <family> should come before <type> ...)
     */
    for (i = 0; i < ctrc; i++) {
        const char *ctrname = ctrv[i].pcn_name;
        char typebuf[64], fambuf[64], meanbuf[64];

        if (strlen(ctrname) >= sizeof(typebuf)) {
            err = merr(ENAMETOOLONG);
            goto errout;
        }

        typev[i] = perfc_ctr_name2type(ctrname, typebuf, fambuf, meanbuf);

        if (typev[i] == PERFC_TYPE_INVAL) {
            err = merr(EINVAL);
            goto errout;
        }

        if (familylen == 0) {
            familylen = strlcpy(family, fambuf, sizeof(family));
            continue;
        }

        /* Check that the family name is the same for all
         * the set counters
         */
        if (strcmp(family, fambuf)) {
            err = merr(EINVAL);
            goto errout;
        }
    }

    assert(familylen > 0);

    sz = snprintf(path, sizeof(path), "%s/%s/%s/%s",
                  PERFC_DT_PATH, group, family, ctrseti_name);
    if (sz >= sizeof(path)) {
        err = merr(EINVAL);
        goto errout;
    }

    err = dt_access(path, perfc_access_dte, setp);
    if (ev(err)) {
        if (merr_errno(err) != ENOENT)
            goto errout;

        /* Reset because we will allocate the new data tree element. */
        err = 0;
    } else {
        return 0;
    }

    dte = aligned_alloc(__alignof__(*dte), sizeof(*dte));
    if (ev(!dte)) {
        err = merr(ENOMEM);
        goto errout;
    }

    memset(dte, 0, sizeof(*dte));
    dte->dte_ops = &perfc_ops;
    strlcpy(dte->dte_path, path, sizeof(dte->dte_path));

    /* Allocate the counter set instance in one big chunk.
     */
    sz = sizeof(*seti) + sizeof(seti->pcs_ctrv[0]) * ctrc;
    sz = roundup(sz, HSE_ACP_LINESIZE);

    for (n = i = 0; i < ctrc; ++i) {
        enum perfc_type type = typev[i];

        if (!(type == PERFC_TYPE_DI || type == PERFC_TYPE_LT))
            ++n;
    }

    n = ctrc - n + (roundup(n, 4) / 4) + 1;

    valdatasz = sizeof(struct perfc_val) * PERFC_VALPERCNT * PERFC_VALPERCPU * n + 1;

    seti = aligned_alloc(HSE_ACP_LINESIZE, ALIGN(sz + valdatasz, HSE_ACP_LINESIZE));
    if (!seti) {
        err = merr(ENOMEM);
        goto errout;
    }

    memset(seti, 0, sz + valdatasz);
    strlcpy(seti->pcs_path, path, sizeof(seti->pcs_path));
    strlcpy(seti->pcs_famname, family, sizeof(seti->pcs_famname));
    strlcpy(seti->pcs_ctrseti_name, ctrseti_name, sizeof(seti->pcs_ctrseti_name));
    seti->pcs_handle = setp;
    seti->pcs_ctrnamev = ctrv;
    seti->pcs_ctrc = ctrc;

    valdata = (char *)seti + sz;
    valcur = NULL;
    n = 0;

    /* For each counter in the set, initialize the counter according
     * to the counter type.
     */
    for (i = 0; i < ctrc; i++) {
        const struct perfc_name *entry = &ctrv[i];
        struct perfc_ctr_hdr *   pch;
        const struct perfc_ivl * ivl;
        enum perfc_type type;

        ivl = entry->pcn_ivl ?: perfc_di_ivl;
        type = typev[i];

        pch = &seti->pcs_ctrv[i].hdr;
        pch->pch_type = type;
        pch->pch_flags = entry->pcn_flags;
        pch->pch_level = entry->pcn_prio;
        clamp_t(typeof(pch->pch_level), pch->pch_level, PERFC_LEVEL_MIN, PERFC_LEVEL_MAX);

        if (prio >= pch->pch_level)
            setp->ps_bitmap |= (1ULL << i);

        if (type == PERFC_TYPE_DI || type == PERFC_TYPE_LT) {
            struct perfc_dis *dis = &seti->pcs_ctrv[i].dis;

            if (ev(ivl->ivl_cnt > PERFC_IVL_MAX)) {
                err = merr(EINVAL);
                break;
            }

            dis->pdi_pct = entry->pcn_samplepct * PERFC_PCT_SCALE / 100;
            dis->pdi_ivl = ivl;

            pch->pch_bktv = valdata;
            valdata += sizeof(struct perfc_val) * PERFC_VALPERCNT * PERFC_VALPERCPU;
        } else {
            if (!valcur || (n % PERFC_VALPERCPU) == 0) {
                valcur = valdata;
                valdata += sizeof(struct perfc_val) * PERFC_VALPERCNT * PERFC_VALPERCPU;
            }

            pch->pch_val = valcur;
            valcur += sizeof(struct perfc_val);
            ++n;
        }
    }

    if (!err) {
        dte->dte_data = seti;
        setp->ps_seti = seti;

        rc = dt_add(dte);
        if (ev(rc))
            err = merr(rc);
    }

  errout:
    if (err) {
        log_warnx("unable to alloc perf counter %s/%s/%s from %s:%d",
                  err, group, family, ctrseti_name, file, line);
        setp->ps_bitmap = 0;
        setp->ps_seti = NULL;
        free(seti);
        free(dte);
    }

    return err;
}

void
perfc_free(struct perfc_set *set)
{
    struct perfc_seti *seti;

    assert(set);

    seti = set->ps_seti;
    if (!seti)
        return;

    /* The remove handler will free anything hanging from the counter set */
    dt_remove(seti->pcs_path);
    set->ps_seti = NULL;
}

/**
 * perfc_ctrseti_path() - get the counter set path
 * @set:
 */
char *
perfc_ctrseti_path(struct perfc_set *set)
{
    struct perfc_seti *seti = set->ps_seti;

    return seti ? seti->pcs_path : NULL;
}

static_assert(sizeof(struct perfc_val) >= sizeof(struct perfc_bkt), "sizeof perfc_bkt too large");

static HSE_ALWAYS_INLINE void
perfc_latdis_record(struct perfc_dis *dis, u64 sample)
{
    struct perfc_bkt *bkt;
    u32               i;

    if (sample > dis->pdi_max)
        dis->pdi_max = sample;
    else if ((sample < dis->pdi_min) || (dis->pdi_min == 0))
        dis->pdi_min = sample;

    bkt = dis->pdi_hdr.pch_bktv;
    bkt += (hse_getcpu(NULL) % PERFC_GRP_MAX) * (PERFC_IVL_MAX + 1);

    /* Index into ivl_map[] with ilog2(sample) to skip buckets whose bounds
     * are smaller than sample.  Note that we constrain sample to produce an
     * index within the size of the map.
     */
    if (sample > 0) {
        const struct perfc_ivl *ivl = dis->pdi_ivl;

        i = ivl->ivl_map[ilog2(sample & 0x7ffffffffffffffful)];

        while (i < ivl->ivl_cnt && sample >= ivl->ivl_bound[i])
            ++i;

        bkt += i;
    }

    atomic_add(&bkt->pcb_vadd, sample);
    atomic_add(&bkt->pcb_hits, 1);
}

void
perfc_lat_record_impl(struct perfc_dis *dis, u64 sample)
{
    assert(dis->pdi_hdr.pch_type == PERFC_TYPE_LT);

    if (sample % PERFC_PCT_SCALE < dis->pdi_pct)
        perfc_latdis_record(dis, cycles_to_nsecs(get_cycles() - sample));
}

void
perfc_dis_record_impl(struct perfc_dis *dis, u64 sample)
{
    assert(dis->pdi_hdr.pch_type == PERFC_TYPE_DI);

    if (xrand64_tls() % PERFC_PCT_SCALE < dis->pdi_pct)
        perfc_latdis_record(dis, sample);
}

#if HSE_MOCKING
#include "perfc_ut_impl.i"
#endif /* HSE_MOCKING */
