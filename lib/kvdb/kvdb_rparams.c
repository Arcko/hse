/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2015-2022 Micron Technology, Inc.  All rights reserved.
 */

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <bsd/string.h>

#include <hse/hse.h>

#include <hse/mpool/mpool.h>
#include <hse/ikvdb/mclass_policy.h>
#include <hse/ikvdb/param.h>
#include <hse/ikvdb/kvdb_rparams.h>
#include <hse/ikvdb/kvdb_cparams.h>
#include <hse/ikvdb/limits.h>
#include <hse/ikvdb/kvdb_home.h>
#include <hse/ikvdb/wal.h>
#include <hse/util/assert.h>
#include <hse/util/storage.h>
#include <hse/util/compiler.h>
#include <hse/ikvdb/csched.h>

static void
mclass_policies_default_builder(const struct param_spec *ps, void *data)
{
    struct mclass_policy *mclass_policies = data;
    struct mclass_policy *policy;

    assert(mclass_policy_names_cnt() == 6);

    /* Setup capacity_only */
    policy = &mclass_policies[0];
    strlcpy(policy->mc_name, "capacity_only", sizeof("capacity_only"));
    policy->mc_table[HSE_MPOLICY_AGE_ROOT][HSE_MPOLICY_DTYPE_KEY] = HSE_MCLASS_CAPACITY;
    policy->mc_table[HSE_MPOLICY_AGE_ROOT][HSE_MPOLICY_DTYPE_VALUE] = HSE_MCLASS_CAPACITY;
    policy->mc_table[HSE_MPOLICY_AGE_LEAF][HSE_MPOLICY_DTYPE_KEY] = HSE_MCLASS_CAPACITY;
    policy->mc_table[HSE_MPOLICY_AGE_LEAF][HSE_MPOLICY_DTYPE_VALUE] = HSE_MCLASS_CAPACITY;

    /* Setup staging_only */
    policy = &mclass_policies[1];
    strlcpy(policy->mc_name, "staging_only", sizeof("staging_only"));
    policy->mc_table[HSE_MPOLICY_AGE_ROOT][HSE_MPOLICY_DTYPE_KEY] = HSE_MCLASS_STAGING;
    policy->mc_table[HSE_MPOLICY_AGE_ROOT][HSE_MPOLICY_DTYPE_VALUE] = HSE_MCLASS_STAGING;
    policy->mc_table[HSE_MPOLICY_AGE_LEAF][HSE_MPOLICY_DTYPE_KEY] = HSE_MCLASS_STAGING;
    policy->mc_table[HSE_MPOLICY_AGE_LEAF][HSE_MPOLICY_DTYPE_VALUE] = HSE_MCLASS_STAGING;

    /* Setup staging_max_capacity */
    policy = &mclass_policies[2];
    strlcpy(policy->mc_name, "staging_max_capacity", sizeof("staging_max_capacity"));
    policy->mc_table[HSE_MPOLICY_AGE_ROOT][HSE_MPOLICY_DTYPE_KEY] = HSE_MCLASS_STAGING;
    policy->mc_table[HSE_MPOLICY_AGE_ROOT][HSE_MPOLICY_DTYPE_VALUE] = HSE_MCLASS_STAGING;
    policy->mc_table[HSE_MPOLICY_AGE_LEAF][HSE_MPOLICY_DTYPE_KEY] = HSE_MCLASS_STAGING;
    policy->mc_table[HSE_MPOLICY_AGE_LEAF][HSE_MPOLICY_DTYPE_VALUE] = HSE_MCLASS_CAPACITY;

    /* Setup staging_min_capacity */
    policy = &mclass_policies[3];
    strlcpy(policy->mc_name, "staging_min_capacity", sizeof("staging_min_capacity"));
    policy->mc_table[HSE_MPOLICY_AGE_ROOT][HSE_MPOLICY_DTYPE_KEY] = HSE_MCLASS_STAGING;
    policy->mc_table[HSE_MPOLICY_AGE_ROOT][HSE_MPOLICY_DTYPE_VALUE] = HSE_MCLASS_STAGING;
    policy->mc_table[HSE_MPOLICY_AGE_LEAF][HSE_MPOLICY_DTYPE_KEY] = HSE_MCLASS_CAPACITY;
    policy->mc_table[HSE_MPOLICY_AGE_LEAF][HSE_MPOLICY_DTYPE_VALUE] = HSE_MCLASS_CAPACITY;

    /* Setup pmem_only */
    policy = &mclass_policies[4];
    strlcpy(policy->mc_name, "pmem_only", sizeof("pmem_only"));
    policy->mc_table[HSE_MPOLICY_AGE_ROOT][HSE_MPOLICY_DTYPE_KEY] = HSE_MCLASS_PMEM;
    policy->mc_table[HSE_MPOLICY_AGE_ROOT][HSE_MPOLICY_DTYPE_VALUE] = HSE_MCLASS_PMEM;
    policy->mc_table[HSE_MPOLICY_AGE_LEAF][HSE_MPOLICY_DTYPE_KEY] = HSE_MCLASS_PMEM;
    policy->mc_table[HSE_MPOLICY_AGE_LEAF][HSE_MPOLICY_DTYPE_VALUE] = HSE_MCLASS_PMEM;

    /* Setup pmem_max_capacity */
    policy = &mclass_policies[5];
    strlcpy(policy->mc_name, "pmem_max_capacity", sizeof("pmem_max_capacity"));
    policy->mc_table[HSE_MPOLICY_AGE_ROOT][HSE_MPOLICY_DTYPE_KEY] = HSE_MCLASS_PMEM;
    policy->mc_table[HSE_MPOLICY_AGE_ROOT][HSE_MPOLICY_DTYPE_VALUE] = HSE_MCLASS_PMEM;
    policy->mc_table[HSE_MPOLICY_AGE_LEAF][HSE_MPOLICY_DTYPE_KEY] = HSE_MCLASS_PMEM;
    policy->mc_table[HSE_MPOLICY_AGE_LEAF][HSE_MPOLICY_DTYPE_VALUE] = HSE_MCLASS_CAPACITY;

    for (int i = mclass_policy_names_cnt(); i < ps->ps_bounds.as_array.ps_max_len; i++) {
        const size_t HSE_MAYBE_UNUSED sz =
            strlcpy(mclass_policies[i].mc_name, HSE_MPOLICY_DEFAULT_NAME, HSE_MPOLICY_NAME_LEN_MAX);

        assert(sz == strlen(HSE_MPOLICY_DEFAULT_NAME));

        for (int age = 0; age < (int)HSE_MPOLICY_AGE_CNT; age++) {
            for (int dtype = 0; dtype < (int)HSE_MPOLICY_DTYPE_CNT; dtype++) {
                if (age != (int)HSE_MPOLICY_AGE_ROOT && dtype == (int)HSE_MPOLICY_DTYPE_VALUE)
                    mclass_policies[i].mc_table[age][dtype] = HSE_MCLASS_CAPACITY;
                else
                    mclass_policies[i].mc_table[age][dtype] = HSE_MCLASS_STAGING;
            }
        }
    }
}

static bool
mclass_policies_converter(const struct param_spec *ps, const cJSON *node, void *data)
{
    int i;
    const char *ctx = NULL;
    unsigned int dtype_map_sz;
    unsigned int agegroup_map_sz;
    struct mclass_policy *policies;
    const struct mclass_policy_map *dtype_map;
    const struct mclass_policy_map *agegroup_map;
    static const char *policy_allowed_keys[] = { "name", "config" };

    assert(ps);
    assert(node);
    assert(data);
    assert(mclass_policy_get_num_fields() == 2);

    if (!cJSON_IsArray(node))
        return false;

    policies = data;
    agegroup_map = mclass_policy_get_map(0);
    agegroup_map_sz = mclass_policy_get_num_map_entries(0);
    dtype_map = mclass_policy_get_map(1);
    dtype_map_sz = mclass_policy_get_num_map_entries(1);

    i = mclass_policy_names_cnt();
    for (cJSON *policy_json = node->child; policy_json; policy_json = policy_json->next, i++) {
        const char *policy_name;
        cJSON *policy_name_json;
        cJSON *policy_config_json;

        if (i >= HSE_MPOLICY_COUNT)
            return false;
        if (!cJSON_IsObject(policy_json))
            return false;

        /* Make sure there are no unknown keys */
        for (cJSON *n = policy_json->child; n; n = n->next) {
            bool found = false;

            for (size_t i = 0; i < NELEM(policy_allowed_keys); i++) {
                if (!strcmp(policy_allowed_keys[i], n->string)) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                log_err("Unknown key in mclass policy object: %s", n->string);
                return false;
            }
        }

        policy_name_json = cJSON_GetObjectItemCaseSensitive(policy_json, "name");
        if (!policy_name_json || !cJSON_IsString(policy_name_json)) {
            log_err("Key 'name' in media class policy object must be a string");
            return false;
        }

        policy_config_json = cJSON_GetObjectItemCaseSensitive(policy_json, "config");
        if (!policy_config_json || !cJSON_IsObject(policy_config_json)) {
            log_err("Key 'config' in media class policy object must be an object");
            return false;
        }

        policy_name = cJSON_GetStringValue(policy_name_json);
        if (strlen(policy_name) >= HSE_MPOLICY_NAME_LEN_MAX) {
            log_err(
                "Length of media class policy name '%s' is greater than %d",
                policy_name,
                HSE_MPOLICY_NAME_LEN_MAX - 1);
            return false;
        }

        strlcpy(policies[i].mc_name, policy_name, HSE_MPOLICY_NAME_LEN_MAX);

        for (cJSON *agegroup_json = policy_config_json->child; agegroup_json;
             agegroup_json = agegroup_json->next) {
            int agegroup = -1;

            if (!cJSON_IsObject(agegroup_json)) {
                log_err("Media class policy age group must be an object");
                return false;
            }

            for (int j = 0; j < agegroup_map_sz; j++) {
                if (!strcmp(agegroup_json->string, agegroup_map[j].mc_kname)) {
                    agegroup = agegroup_map[j].mc_enum;
                    break;
                }
            }
            if (agegroup == -1) {
                log_err(
                    "Invalid media class policy age group: %s, must be one of sync, root, or leaf",
                    agegroup_json->string);
                return false;
            }

            for (cJSON *dtype_json = agegroup_json->child; dtype_json;
                    dtype_json = dtype_json->next) {
                int dtype = -1;
                enum hse_mclass mclass = HSE_MCLASS_INVALID;

                for (int j = 0; j < dtype_map_sz; j++) {
                    if (!strcmp(dtype_json->string, dtype_map[j].mc_kname)) {
                        dtype = dtype_map[j].mc_enum;
                        break;
                    }
                }
                if (dtype == -1) {
                    log_err(
                        "Invalid media class policy data type: %s, must be one of key or value",
                        dtype_json->string);
                    return false;
                }

                if (!cJSON_IsString(dtype_json)) {
                    log_err("Media class policy must be a string");
                    return false;
                }

                for (int k = HSE_MCLASS_BASE; k < HSE_MCLASS_COUNT; k++) {
                    ctx = cJSON_GetStringValue(dtype_json);
                    if (!strcmp(ctx, hse_mclass_name_get(k))) {
                        mclass = (enum hse_mclass)k;
                        break;
                    }
                }
                if (mclass == HSE_MCLASS_INVALID) {
                    log_err(
                        "Unknown media class in media class policy: %s, "
                        "must be one of capacity or staging or pmem", ctx);
                    return false;
                }

                policies[i].mc_table[agegroup][dtype] = mclass;
            }
        }
    }

    return true;
}

static bool
mclass_policies_validator(const struct param_spec *ps, const void *data)
{
    const struct mclass_policy *policies = data;
    unsigned int                times_matched[HSE_MPOLICY_COUNT] = { 0 };

    assert(ps);
    assert(data);

    /* Make sure all policies have unique names */
    for (size_t i = 0; i < ps->ps_bounds.as_array.ps_max_len; i++) {
        if (!strcmp(policies[i].mc_name, HSE_MPOLICY_DEFAULT_NAME))
            break;

        for (size_t j = 0; j < ps->ps_bounds.as_array.ps_max_len; j++) {
            if (!strcmp(policies[i].mc_name, HSE_MPOLICY_DEFAULT_NAME))
                break;
            if (!strcmp(policies[j].mc_name, policies[i].mc_name))
                times_matched[i]++;

            if (times_matched[i] > 1) {
                log_err("Duplicate media class policy name found: %s", policies[i].mc_name);
                return false;
            }
        }
    }

    return true;
}

static merr_t
mclass_policies_stringify(
    const struct param_spec *const ps,
    const void *const              value,
    char *const                    buf,
    const size_t                   buf_sz,
    size_t *const                  needed_sz)
{
    cJSON *arr;
    char * data;
    size_t n;

    INVARIANT(ps);
    INVARIANT(value);

    arr = ps->ps_jsonify(ps, value);
    if (!arr)
        return merr(ENOMEM);

    /* Ideally this would be cJSON_PrintPreallocated(), but cJSON doesn't tell
     * you about truncation via a needed size like snprintf() or strlcpy().
     * C-string based APIs rock...not :).
     */
    data = cJSON_PrintUnformatted(arr);
    n = strlcpy(buf, data, buf_sz);
    cJSON_free(data);

    if (needed_sz)
        *needed_sz = n;

    cJSON_Delete(arr);

    return 0;
}

static cJSON * HSE_NONNULL(1, 2)
mclass_policies_jsonify(const struct param_spec *const ps, const void *const value)
{
    cJSON *arr;
    const struct mclass_policy *policies;

    INVARIANT(ps);
    INVARIANT(value);

    policies = value;

    arr = cJSON_CreateArray();
    if (!arr)
        return NULL;

    for (size_t i = mclass_policy_names_cnt(); i < ps->ps_bounds.as_array.ps_max_len; i++) {
        cJSON *policy, *name, *config;
        cJSON *leaf, *leaf_k, *leaf_v;
        cJSON *root, *root_k, *root_v;

        if (!strcmp(policies[i].mc_name, HSE_MPOLICY_DEFAULT_NAME))
            return arr;

        policy = cJSON_CreateObject();
        name = cJSON_AddStringToObject(policy, "name", policies[i].mc_name);
        config = cJSON_AddObjectToObject(policy, "config");

        leaf = cJSON_AddObjectToObject(config, "leaf");
        leaf_k = cJSON_AddStringToObject(leaf, "keys",
            hse_mclass_name_get(policies[i].mc_table[HSE_MPOLICY_AGE_LEAF][HSE_MPOLICY_DTYPE_KEY]));
        leaf_v = cJSON_AddStringToObject(leaf, "values",
            hse_mclass_name_get(policies[i].mc_table[HSE_MPOLICY_AGE_LEAF][HSE_MPOLICY_DTYPE_VALUE]));
        root = cJSON_AddObjectToObject(config, "root");
        root_k = cJSON_AddStringToObject(root, "keys",
            hse_mclass_name_get(policies[i].mc_table[HSE_MPOLICY_AGE_ROOT][HSE_MPOLICY_DTYPE_KEY]));
        root_v = cJSON_AddStringToObject(root, "values",
            hse_mclass_name_get(policies[i].mc_table[HSE_MPOLICY_AGE_ROOT][HSE_MPOLICY_DTYPE_VALUE]));

        if (!policy || !name || !config || !leaf || !leaf_k || !leaf_v ||
            !root || !root_k || !root_v) {
            cJSON_Delete(policy);
            goto out;
        }

        cJSON_AddItemToArray(arr, policy);
    }

    return arr;

out:
    cJSON_Delete(arr);

    return NULL;
}

static bool HSE_NONNULL(1, 2, 3)
dur_mclass_converter(
    const struct param_spec *const ps,
    const cJSON *const             node,
    void *const                    data)
{
    const char *value;

    INVARIANT(ps);
    INVARIANT(node);
    INVARIANT(data);

    if (!cJSON_IsString(node))
        return false;

    value = cJSON_GetStringValue(node);
    if (!strcmp(value, HSE_MCLASS_AUTO_NAME)) {
        *(uint8_t *)data = HSE_MCLASS_AUTO;
        return true;
    }

    for (int i = HSE_MCLASS_BASE; i < HSE_MCLASS_COUNT; i++) {
        if (!strcmp(hse_mclass_name_get(i), value)) {
            *(uint8_t *)data = i;
            return true;
        }
    }

    log_err("Invalid value: %s, must be one of capacity or staging or pmem or auto", value);

    return false;
}

static merr_t
dur_mclass_stringify(
    const struct param_spec *const ps,
    const void *const              value,
    char *const                    buf,
    const size_t                   buf_sz,
    size_t *const                  needed_sz)
{
    uint8_t mc = *(const uint8_t *)value;
    const char *param;
    int n;

    INVARIANT(ps);
    INVARIANT(value);

    if (mc == HSE_MCLASS_AUTO)
        param = HSE_MCLASS_AUTO_NAME;
    else
        param = hse_mclass_name_get((const enum hse_mclass)mc);

    n = snprintf(buf, buf_sz, "\"%s\"", param);
    if (n < 0)
        return merr(EBADMSG);

    if (needed_sz)
        *needed_sz = n;

    return 0;
}

static cJSON * HSE_NONNULL(1, 2)
dur_mclass_jsonify(const struct param_spec *const ps, const void *const value)
{
    uint8_t mc = *(const uint8_t *)value;
    const char *name;

    INVARIANT(ps);
    INVARIANT(value);

    if (mc == HSE_MCLASS_AUTO)
        name = HSE_MCLASS_AUTO_NAME;
    else
        name = hse_mclass_name_get(*(enum hse_mclass *)value);

    return cJSON_CreateString(name);
}

static bool HSE_NONNULL(1, 2, 3)
throttle_init_policy_converter(
    const struct param_spec *const ps,
    const cJSON *const             node,
    void *const                    data)
{
    const char *value;

    assert(ps);
    assert(node);
    assert(data);

    if (!cJSON_IsString(node))
        return false;

    value = cJSON_GetStringValue(node);
    if (!strcmp(value, "auto")) {
        *(uint *)data = THROTTLE_DELAY_START_AUTO;
    } else if (!strcmp(value, "light")) {
        *(uint *)data = THROTTLE_DELAY_START_LIGHT;
    } else if (!strcmp(value, "medium")) {
        *(uint *)data = THROTTLE_DELAY_START_MEDIUM;
    } else if (!strcmp(value, "heavy") || !strcmp(value, "default")) {
        *(uint *)data = THROTTLE_DELAY_START_HEAVY;
    } else {
        log_err("Invalid value: %s, must be one of light, medium, heavy or auto", value);
        return false;
    }

    return true;
}

static merr_t
throttle_init_policy_stringify(
    const struct param_spec *const ps,
    const void *const              value,
    char *const                    buf,
    const size_t                   buf_sz,
    size_t *const                  needed_sz)
{
    size_t      n;
    const char *param;

    INVARIANT(ps);
    INVARIANT(value);

    switch (*(uint *)value) {
        case THROTTLE_DELAY_START_AUTO:
            param = "\"auto\"";
            break;
        case THROTTLE_DELAY_START_LIGHT:
            param = "\"light\"";
            break;
        case THROTTLE_DELAY_START_MEDIUM:
            param = "\"medium\"";
            break;
        case THROTTLE_DELAY_START_HEAVY:
            param = "\"heavy\"";
            break;
        default:
            abort();
    }

    n = strlcpy(buf, param, buf_sz);

    if (needed_sz)
        *needed_sz = n;

    return 0;
}

static cJSON * HSE_NONNULL(1, 2)
throttle_init_policy_jsonify(const struct param_spec *const ps, const void *const value)
{
    INVARIANT(ps);
    INVARIANT(value);

    switch (*(uint *)value) {
        case THROTTLE_DELAY_START_AUTO:
            return cJSON_CreateString("auto");
        case THROTTLE_DELAY_START_LIGHT:
            return cJSON_CreateString("light");
        case THROTTLE_DELAY_START_MEDIUM:
            return cJSON_CreateString("medium");
        case THROTTLE_DELAY_START_HEAVY:
            return cJSON_CreateString("heavy");
        default:
            abort();
    }
}

static bool HSE_NONNULL(1, 2, 3)
kvdb_open_mode_converter(
    const struct param_spec *const ps,
    const cJSON *const             node,
    void *const                    data)
{
    enum kvdb_open_mode mode;
    const char *mode_str;

    assert(ps);
    assert(node);
    assert(data);

    if (!cJSON_IsString(node))
        return false;

    mode_str = cJSON_GetStringValue(node);
    mode = kvdb_mode_string_to_value(mode_str);

    if (kvdb_mode_is_invalid(mode)) {
        log_err("Invalid value: %s, must be one of: %s", mode_str, KVDB_MODE_LIST_STR);
        return false;
    }

    *((enum kvdb_open_mode *)data) = mode;

    return true;
}

static merr_t
kvdb_open_mode_stringify(
    const struct param_spec *const ps,
    const void *const              value,
    char *const                    buf,
    const size_t                   buf_sz,
    size_t *const                  needed_sz)
{
    int n;

    INVARIANT(ps);
    INVARIANT(value);

    n = snprintf(buf, buf_sz, "\"%s\"", kvdb_mode_to_string(*((enum kvdb_open_mode *)value)));
    if (n < 0)
        return merr(EBADMSG);

    if (needed_sz)
        *needed_sz = n;

    return 0;
}

static cJSON * HSE_NONNULL(1, 2)
kvdb_open_mode_jsonify(const struct param_spec *const ps, const void *const value)
{
    INVARIANT(ps);
    INVARIANT(value);

    return cJSON_CreateString(kvdb_mode_to_string(*((enum kvdb_open_mode *)value)));
}

static const struct param_spec pspecs[] = {
    {
        .ps_name = "mode",
        .ps_description = "open mode",
        .ps_flags = 0,
        .ps_type = PARAM_TYPE_ENUM,
        .ps_offset = offsetof(struct kvdb_rparams, mode),
        .ps_size = PARAM_SZ(struct kvdb_rparams, mode),
        .ps_convert = kvdb_open_mode_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = kvdb_open_mode_stringify,
        .ps_jsonify = kvdb_open_mode_jsonify,
        .ps_default_value = {
            .as_enum = KVDB_MODE_RDWR,
        },
        .ps_bounds = {
            .as_enum = {
                .ps_min = KVDB_MODE_MIN,
                .ps_max = KVDB_MODE_MAX,
            },
        },
    },
    {
        .ps_name = "perfc.level",
        .ps_description = "set kvs perf counter enagagement level (min:0 default:2 max:9)",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL,
        .ps_type = PARAM_TYPE_U8,
        .ps_offset = offsetof(struct kvdb_rparams, perfc_level),
        .ps_size = PARAM_SZ(struct kvdb_rparams, perfc_level),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = PERFC_LEVEL_DEFAULT,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = PERFC_LEVEL_MIN,
                .ps_max = PERFC_LEVEL_MAX,
            },
        },
    },
    {
        .ps_name = "perfc_enable",
        .ps_description = "deprecated, use perfc.level",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL,
        .ps_type = PARAM_TYPE_U8,
        .ps_offset = offsetof(struct kvdb_rparams, perfc_level),
        .ps_size = PARAM_SZ(struct kvdb_rparams, perfc_level),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = 2,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = 0,
                .ps_max = 4,
            },
        },
    },
    {
        .ps_name = "c0_debug",
        .ps_description = "c0 debug flags",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL | PARAM_FLAG_WRITABLE,
        .ps_type = PARAM_TYPE_U8,
        .ps_offset = offsetof(struct kvdb_rparams, c0_debug),
        .ps_size = PARAM_SZ(struct kvdb_rparams, c0_debug),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = 0,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = 0,
                .ps_max = UINT8_MAX,
            },
        },
    },
    {
        .ps_name = "c0_diag_mode",
        .ps_description = "disable c0 spill",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL,
        .ps_type = PARAM_TYPE_BOOL,
        .ps_offset = offsetof(struct kvdb_rparams, c0_diag_mode),
        .ps_size = PARAM_SZ(struct kvdb_rparams, c0_diag_mode),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = false,
        },
    },
    {
        .ps_name = "c0_ingest_width",
        .ps_description = "set c0 kvms width",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL,
        .ps_type = PARAM_TYPE_U32,
        .ps_offset = offsetof(struct kvdb_rparams, c0_ingest_width),
        .ps_size = PARAM_SZ(struct kvdb_rparams, c0_ingest_width),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = HSE_C0_INGEST_WIDTH_DFLT,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = HSE_C0_INGEST_WIDTH_MIN,
                .ps_max = HSE_C0_INGEST_WIDTH_MAX,
            },
        },
    },
    {
        .ps_name = "txn_timeout",
        .ps_description = "transaction timeout (ms)",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL,
        .ps_type = PARAM_TYPE_U64,
        .ps_offset = offsetof(struct kvdb_rparams, txn_timeout),
        .ps_size = PARAM_SZ(struct kvdb_rparams, txn_timeout),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = 1000 * 60 * 5,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = 0,
                .ps_max = UINT64_MAX,
            },
        },
    },
    {
        .ps_name = "cndb_compact_hwm_pct",
        .ps_description = "CNDB compaction high water mark percentage",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL,
        .ps_type = PARAM_TYPE_DOUBLE,
        .ps_offset = offsetof(struct kvdb_rparams, cndb_compact_hwm_pct),
        .ps_size = PARAM_SZ(struct kvdb_rparams, cndb_compact_hwm_pct),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = HSE_CNDB_COMPACT_HWM_PCT_DEFAULT,
        },
        .ps_bounds = {
            .as_double = {
                .ps_min = 0,
                .ps_max = 100,
            },
        },
    },
    {
        .ps_name = "csched_policy",
        .ps_description = "csched (compaction scheduler) policy",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL | PARAM_FLAG_WRITABLE,
        .ps_type = PARAM_TYPE_U32,
        .ps_offset = offsetof(struct kvdb_rparams, csched_policy),
        .ps_size = PARAM_SZ(struct kvdb_rparams, csched_policy),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = csched_rp_kvset_iter_async,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = csched_rp_kvset_iter_async,
                .ps_max = csched_rp_kvset_iter_mmap,
            },
        },
    },
    {
        .ps_name = "csched_debug_mask",
        .ps_description = "csched debug (bit mask)",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL | PARAM_FLAG_WRITABLE,
        .ps_type = PARAM_TYPE_U64,
        .ps_offset = offsetof(struct kvdb_rparams, csched_debug_mask),
        .ps_size = PARAM_SZ(struct kvdb_rparams, csched_debug_mask),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = 0,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = 0,
                .ps_max = UINT64_MAX,
            },
        },
    },
    {
        .ps_name = "csched_samp_max",
        .ps_description = "csched max space amp (0x100)",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL | PARAM_FLAG_WRITABLE,
        .ps_type = PARAM_TYPE_U64,
        .ps_offset = offsetof(struct kvdb_rparams, csched_samp_max),
        .ps_size = PARAM_SZ(struct kvdb_rparams, csched_samp_max),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = 150,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = 0,
                .ps_max = UINT64_MAX,
            },
        },
    },
    {
        .ps_name = "csched_lo_th_pct",
        .ps_description = "csched low water mark percentage",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL | PARAM_FLAG_WRITABLE,
        .ps_type = PARAM_TYPE_U8,
        .ps_offset = offsetof(struct kvdb_rparams, csched_lo_th_pct),
        .ps_size = PARAM_SZ(struct kvdb_rparams, csched_lo_th_pct),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = 25,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = 0,
                .ps_max = 100,
            },
        },
    },
    {
        .ps_name = "csched_hi_th_pct",
        .ps_description = "csched hwm water mark percentage",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL | PARAM_FLAG_WRITABLE,
        .ps_type = PARAM_TYPE_U8,
        .ps_offset = offsetof(struct kvdb_rparams, csched_hi_th_pct),
        .ps_size = PARAM_SZ(struct kvdb_rparams, csched_hi_th_pct),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = 75,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = 0,
                .ps_max = 100,
            },
        },
    },
    {
        .ps_name = "csched_leaf_pct",
        .ps_description = "csched percent data in leaves",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL | PARAM_FLAG_WRITABLE,
        .ps_type = PARAM_TYPE_U8,
        .ps_offset = offsetof(struct kvdb_rparams, csched_leaf_pct),
        .ps_size = PARAM_SZ(struct kvdb_rparams, csched_leaf_pct),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = 90,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = 0,
                .ps_max = 100,
            },
        },
    },
    {
        .ps_name = "csched_gc_pct",
        .ps_description = "per-node garbage collection threshold",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL | PARAM_FLAG_WRITABLE,
        .ps_type = PARAM_TYPE_U8,
        .ps_offset = offsetof(struct kvdb_rparams, csched_gc_pct),
        .ps_size = PARAM_SZ(struct kvdb_rparams, csched_gc_pct),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = 67, /* 2/3 of the node is garbage */
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = 50,  /* half the node is garbage */
                .ps_max = 100, /* infinite garbage */
            },
        },
    },
    {
        .ps_name = "csched_max_vgroups",
        .ps_description = "leaf-scatter-remediation trigger threshold",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL | PARAM_FLAG_WRITABLE,
        .ps_type = PARAM_TYPE_U16,
        .ps_offset = offsetof(struct kvdb_rparams, csched_lscat_hwm),
        .ps_size = PARAM_SZ(struct kvdb_rparams, csched_lscat_hwm),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = 1024,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = 1,
                .ps_max = UINT16_MAX,
            },
        },
    },
    {
        .ps_name = "csched_lscat_runlen_max",
        .ps_description = "leaf-scatter-remediation kvset count limit",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL | PARAM_FLAG_WRITABLE,
        .ps_type = PARAM_TYPE_U8,
        .ps_offset = offsetof(struct kvdb_rparams, csched_lscat_runlen_max),
        .ps_size = PARAM_SZ(struct kvdb_rparams, csched_lscat_runlen_max),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = 3,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = 1,
                .ps_max = 8,
            },
        },
    },
    {
        .ps_name = "csched_qthreads",
        .ps_description = "csched queue threads",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL | PARAM_FLAG_WRITABLE,
        .ps_type = PARAM_TYPE_U64,
        .ps_offset = offsetof(struct kvdb_rparams, csched_qthreads),
        .ps_size = PARAM_SZ(struct kvdb_rparams, csched_qthreads),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = CSCHED_QTHREADS_DEFAULT,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = 0,
                .ps_max = UINT64_MAX,
            },
        },
    },
    {
        .ps_name = "csched_rspill_params",
        .ps_description = "root node spill params [min,max]",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL | PARAM_FLAG_WRITABLE,
        .ps_type = PARAM_TYPE_U64,
        .ps_offset = offsetof(struct kvdb_rparams, csched_rspill_params),
        .ps_size = PARAM_SZ(struct kvdb_rparams, csched_rspill_params),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = 0,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = 0,
                .ps_max = UINT64_MAX,
            },
        },
    },
    {
        .ps_name = "csched_leaf_comp_params",
        .ps_description = "leaf compact params",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL | PARAM_FLAG_WRITABLE,
        .ps_type = PARAM_TYPE_U64,
        .ps_offset = offsetof(struct kvdb_rparams, csched_leaf_comp_params),
        .ps_size = PARAM_SZ(struct kvdb_rparams, csched_leaf_comp_params),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = 0,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = 0,
                .ps_max = UINT64_MAX,
            },
        },
    },
    {
        .ps_name = "csched_leaf_len_params",
        .ps_description = "leaf length params [idlem,idlec,kvcompc,min,max]",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL | PARAM_FLAG_WRITABLE,
        .ps_type = PARAM_TYPE_U64,
        .ps_offset = offsetof(struct kvdb_rparams, csched_leaf_len_params),
        .ps_size = PARAM_SZ(struct kvdb_rparams, csched_leaf_len_params),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = 0,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = 0,
                .ps_max = UINT64_MAX,
            },
        },
    },
    {
        .ps_name = "csched_node_min_ttl",
        .ps_description = "Min. time-to-live for cN nodes (secs)",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL,
        .ps_type = PARAM_TYPE_U64,
        .ps_offset = offsetof(struct kvdb_rparams, csched_node_min_ttl),
        .ps_size = PARAM_SZ(struct kvdb_rparams, csched_node_min_ttl),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = 17,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = 0,
                .ps_max = UINT64_MAX,
            },
        },
    },
    {
        .ps_name = "durability.enabled",
        .ps_description = "Enable durability in the event of a crash",
        .ps_flags = 0,
        .ps_type = PARAM_TYPE_BOOL,
        .ps_offset = offsetof(struct kvdb_rparams, dur_enable),
        .ps_size = PARAM_SZ(struct kvdb_rparams, dur_enable),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_bool = true,
        },
    },
    {
        .ps_name = "durability.interval_ms",
        .ps_description = "durability lag in ms",
        .ps_flags = 0,
        .ps_type = PARAM_TYPE_U32,
        .ps_offset = offsetof(struct kvdb_rparams, dur_intvl_ms),
        .ps_size = PARAM_SZ(struct kvdb_rparams, dur_intvl_ms),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = HSE_WAL_DUR_MS_DFLT,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = HSE_WAL_DUR_MS_MIN,
                .ps_max = HSE_WAL_DUR_MS_MAX,
            },
        },
    },
    {
        .ps_name = "durability.replay.force",
        .ps_description = "Force WAL to attempt a best-effort recovery with potential data loss",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL,
        .ps_type = PARAM_TYPE_BOOL,
        .ps_offset = offsetof(struct kvdb_rparams, dur_replay_force),
        .ps_size = PARAM_SZ(struct kvdb_rparams, dur_replay_force),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_bool = false,
        },
    },
    {
        .ps_name = "durability.size_bytes",
        .ps_description = "Maximum amount of application data lost in the event of a crash",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL,
        .ps_type = PARAM_TYPE_U32,
        .ps_offset = offsetof(struct kvdb_rparams, dur_size_bytes),
        .ps_size = PARAM_SZ(struct kvdb_rparams, dur_size_bytes),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = HSE_WAL_DUR_SIZE_BYTES_DFLT,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = HSE_WAL_DUR_SIZE_BYTES_MIN,
                .ps_max = HSE_WAL_DUR_SIZE_BYTES_MAX,
            },
        },
    },
    {
        .ps_name = "durability.buffer.size",
        .ps_description = "durability buffer size in MiB",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL,
        .ps_type = PARAM_TYPE_U32,
        .ps_offset = offsetof(struct kvdb_rparams, dur_bufsz_mb),
        .ps_size = PARAM_SZ(struct kvdb_rparams, dur_bufsz_mb),
        .ps_convert = param_roundup_pow2,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = HSE_WAL_DUR_BUFSZ_MB_DFLT,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = HSE_WAL_DUR_BUFSZ_MB_MIN,
                .ps_max = HSE_WAL_DUR_BUFSZ_MB_MAX,
            },
        },
    },
    {
        .ps_name = "durability.throttling.threshold.low",
        .ps_description = "low watermark for throttling in percentage",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL,
        .ps_type = PARAM_TYPE_U8,
        .ps_offset = offsetof(struct kvdb_rparams, dur_throttle_lo_th),
        .ps_size = PARAM_SZ(struct kvdb_rparams, dur_throttle_lo_th),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = 13,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = 0,
                .ps_max = 100,
            },
        },
    },
    {
        .ps_name = "durability.throttling.threshold.high",
        .ps_description = "high watermark for throttling in percentage",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL,
        .ps_type = PARAM_TYPE_U8,
        .ps_offset = offsetof(struct kvdb_rparams, dur_throttle_hi_th),
        .ps_size = PARAM_SZ(struct kvdb_rparams, dur_throttle_hi_th),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = 87,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = 0,
                .ps_max = 100,
            },
        },
    },
    {
        .ps_name = "durability.buffer.managed",
        .ps_description = "Controls whether WAL buffers are shared with c0",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL,
        .ps_type = PARAM_TYPE_BOOL,
        .ps_offset = offsetof(struct kvdb_rparams, dur_buf_managed),
        .ps_size = PARAM_SZ(struct kvdb_rparams, dur_buf_managed),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_bool = false,
        },
    },
	{
        .ps_name = "durability.mclass",
        .ps_description = "media class to use for WAL files",
        .ps_flags = 0,
        .ps_type = PARAM_TYPE_U8,
        .ps_offset = offsetof(struct kvdb_rparams, dur_mclass),
        .ps_size = PARAM_SZ(struct kvdb_rparams, dur_mclass),
        .ps_convert = dur_mclass_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = dur_mclass_stringify,
        .ps_jsonify = dur_mclass_jsonify,
        .ps_default_value = {
            .as_uscalar = HSE_MCLASS_AUTO, /* let HSE pick */
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = HSE_MCLASS_BASE,
                .ps_max = HSE_MCLASS_AUTO,
            },
        },
    },
    {
        .ps_name = "throttle_disable",
        .ps_description = "disable sleep throttle",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL,
        .ps_type = PARAM_TYPE_BOOL,
        .ps_offset = offsetof(struct kvdb_rparams, throttle_disable),
        .ps_size = PARAM_SZ(struct kvdb_rparams, throttle_disable),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = false,
        },
    },
    {
        .ps_name = "throttle_update_ns",
        .ps_description = "throttle update sensors time in ns",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL | PARAM_FLAG_WRITABLE,
        .ps_type = PARAM_TYPE_U64,
        .ps_offset = offsetof(struct kvdb_rparams, throttle_update_ns),
        .ps_size = PARAM_SZ(struct kvdb_rparams, throttle_update_ns),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = 25 * 1000 * 1000,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = 0,
                .ps_max = UINT64_MAX,
            },
        },
    },
    {
        .ps_name = "throttle_debug",
        .ps_description = "throttle debug",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL | PARAM_FLAG_WRITABLE,
        .ps_type = PARAM_TYPE_U32,
        .ps_offset = offsetof(struct kvdb_rparams, throttle_debug),
        .ps_size = PARAM_SZ(struct kvdb_rparams, throttle_debug),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = 0,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = 0,
                .ps_max = UINT32_MAX,
            },
        },
    },
    {
        .ps_name = "throttle_debug_intvl_s",
        .ps_description = "throttle debug interval (secs)",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL | PARAM_FLAG_WRITABLE,
        .ps_type = PARAM_TYPE_U32,
        .ps_offset = offsetof(struct kvdb_rparams, throttle_debug_intvl_s),
        .ps_size = PARAM_SZ(struct kvdb_rparams, throttle_debug_intvl_s),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = 300,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = 0,
                .ps_max = UINT32_MAX,
            },
        },
    },
    {
        .ps_name = "throttling.init_policy",
        .ps_description = "throttle initialization policy",
        .ps_flags = 0,
        .ps_type = PARAM_TYPE_ENUM,
        .ps_offset = offsetof(struct kvdb_rparams, throttle_init_policy),
        .ps_size = PARAM_SZ(struct kvdb_rparams, throttle_init_policy),
        .ps_convert = throttle_init_policy_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = throttle_init_policy_stringify,
        .ps_jsonify = throttle_init_policy_jsonify,
        .ps_default_value = {
            .as_enum = THROTTLE_DELAY_START_AUTO, /* let HSE pick */
        },
        .ps_bounds = {
            .as_enum = {
                .ps_min = THROTTLE_DELAY_START_LIGHT,
                .ps_max = THROTTLE_DELAY_START_AUTO,
            },
        },
    },
    {
        .ps_name = "throttle_burst",
        .ps_description = "initial throttle burst size (bytes)",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL | PARAM_FLAG_WRITABLE,
        .ps_type = PARAM_TYPE_U64,
        .ps_offset = offsetof(struct kvdb_rparams, throttle_burst),
        .ps_size = PARAM_SZ(struct kvdb_rparams, throttle_burst),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = 1ul << 20,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = 0,
                .ps_max = UINT64_MAX,
            },
        },
    },
    {
        .ps_name = "throttle_rate",
        .ps_description = "initial throttle rate (bytes/sec)",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL | PARAM_FLAG_WRITABLE,
        .ps_type = PARAM_TYPE_U64,
        .ps_offset = offsetof(struct kvdb_rparams, throttle_rate),
        .ps_size = PARAM_SZ(struct kvdb_rparams, throttle_rate),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = 10ul << 20,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = 0,
                .ps_max = UINT64_MAX,
            },
        },
    },
    {
        .ps_name = "txn_wkth_delay",
        .ps_description = "delay for transaction worker thread",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL,
        .ps_type = PARAM_TYPE_U64,
        .ps_offset = offsetof(struct kvdb_rparams, txn_wkth_delay),
        .ps_size = PARAM_SZ(struct kvdb_rparams, txn_wkth_delay),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = 1000 * 60,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = 0,
                .ps_max = UINT64_MAX,
            },
        },
    },
    {
        .ps_name = "c0_maint_threads",
        .ps_description = "max number of maintenance threads",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL,
        .ps_type = PARAM_TYPE_U32,
        .ps_offset = offsetof(struct kvdb_rparams, c0_maint_threads),
        .ps_size = PARAM_SZ(struct kvdb_rparams, c0_maint_threads),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = HSE_C0_MAINT_THREADS_DFLT,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = HSE_C0_MAINT_THREADS_MIN,
                .ps_max = HSE_C0_MAINT_THREADS_MAX,
            },
        },
    },
    {
        .ps_name = "c0_ingest_threads",
        .ps_description = "max number of c0 ingest threads",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL,
        .ps_type = PARAM_TYPE_U32,
        .ps_offset = offsetof(struct kvdb_rparams, c0_ingest_threads),
        .ps_size = PARAM_SZ(struct kvdb_rparams, c0_ingest_threads),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = HSE_C0_INGEST_THREADS_DFLT,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = HSE_C0_INGEST_THREADS_MIN,
                .ps_max = HSE_C0_INGEST_THREADS_MAX,
            },
        },
    },
    {
        .ps_name = "cn_maint_threads",
        .ps_description = "max number of cn maintenance threads",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL,
        .ps_type = PARAM_TYPE_U16,
        .ps_offset = offsetof(struct kvdb_rparams, cn_maint_threads),
        .ps_size = PARAM_SZ(struct kvdb_rparams, cn_maint_threads),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = 32,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = 1,
                .ps_max = 256,
            },
        },
    },
    {
        .ps_name = "cn_io_threads",
        .ps_description = "max number of cn mblock i/o threads",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL,
        .ps_type = PARAM_TYPE_U16,
        .ps_offset = offsetof(struct kvdb_rparams, cn_io_threads),
        .ps_size = PARAM_SZ(struct kvdb_rparams, cn_io_threads),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = 17,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = 1,
                .ps_max = 256,
            },
        },
    },
    {
        .ps_name = "keylock_tables",
        .ps_description = "number of keylock tables",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL,
        .ps_type = PARAM_TYPE_U32,
        .ps_offset = offsetof(struct kvdb_rparams, keylock_tables),
        .ps_size = PARAM_SZ(struct kvdb_rparams, keylock_tables),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = 761,
        },
        .ps_bounds = {
            .as_uscalar = {
                .ps_min = 16,
                .ps_max = 8192,
            },
        },
    },
    {
        .ps_name = "mclass_policies",
        .ps_description = "media class policy definitions",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL | PARAM_FLAG_DEFAULT_BUILDER,
        .ps_type = PARAM_TYPE_ARRAY,
        .ps_offset = offsetof(struct kvdb_rparams, mclass_policies),
        .ps_size = PARAM_SZ(struct kvdb_rparams, mclass_policies),
        .ps_convert = mclass_policies_converter,
        .ps_validate = mclass_policies_validator,
        .ps_stringify = mclass_policies_stringify,
        .ps_jsonify = mclass_policies_jsonify,
        .ps_default_value = {
            .as_builder = mclass_policies_default_builder,
        },
        .ps_bounds = {
            .as_array = {
                .ps_max_len = HSE_MPOLICY_COUNT,
            }
        },
    },
    {
        .ps_name = "storage.capacity.directio.enabled",
        .ps_description = "Enable direct I/O for capacity mclass",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL,
        .ps_type = PARAM_TYPE_BOOL,
        .ps_offset = offsetof(struct kvdb_rparams, dio_enable[HSE_MCLASS_CAPACITY]),
        .ps_size = PARAM_SZ(struct kvdb_rparams, dio_enable[HSE_MCLASS_CAPACITY]),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = true,
        },
    },
    {
        .ps_name = "storage.staging.directio.enabled",
        .ps_description = "Enable direct I/O for staging mclass",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL,
        .ps_type = PARAM_TYPE_BOOL,
        .ps_offset = offsetof(struct kvdb_rparams, dio_enable[HSE_MCLASS_STAGING]),
        .ps_size = PARAM_SZ(struct kvdb_rparams, dio_enable[HSE_MCLASS_STAGING]),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = true,
        },
    },
    {
        .ps_name = "storage.pmem.directio.enabled",
        .ps_description = "Enable direct I/O for pmem mclass",
        .ps_flags = PARAM_FLAG_EXPERIMENTAL,
        .ps_type = PARAM_TYPE_BOOL,
        .ps_offset = offsetof(struct kvdb_rparams, dio_enable[HSE_MCLASS_PMEM]),
        .ps_size = PARAM_SZ(struct kvdb_rparams, dio_enable[HSE_MCLASS_PMEM]),
        .ps_convert = param_default_converter,
        .ps_validate = param_default_validator,
        .ps_stringify = param_default_stringify,
        .ps_jsonify = param_default_jsonify,
        .ps_default_value = {
            .as_uscalar = true,
        },
    },
};

const struct param_spec *
kvdb_rparams_pspecs_get(size_t *pspecs_sz)
{
    if (pspecs_sz)
        *pspecs_sz = NELEM(pspecs);
    return pspecs;
}

struct kvdb_rparams
kvdb_rparams_defaults()
{
    struct kvdb_rparams params;
    const struct params p = {
        .p_params = { .as_kvdb_rp = &params },
        .p_type = PARAMS_KVDB_RP,
    };

    param_default_populate(pspecs, NELEM(pspecs), &p);

    return params;
}

merr_t
kvdb_rparams_get(
    const struct kvdb_rparams *const params,
    const char *const                param,
    char *const                      buf,
    const size_t                     buf_sz,
    size_t *const                    needed_sz)
{
    const struct params p = {
        .p_params = { .as_kvdb_rp = params },
        .p_type = PARAMS_KVDB_RP,
    };

    return param_get(&p, pspecs, NELEM(pspecs), param, buf, buf_sz, needed_sz);
}

merr_t
kvdb_rparams_set(
    const struct kvdb_rparams *const params,
    const char *const                param,
    const char *const                value)
{
    struct params p = {
        .p_params = { .as_kvdb_rp = params },
        .p_type = PARAMS_KVDB_RP,
    };

    if (!params || !param || !value)
        return merr(EINVAL);

    return param_set(&p, pspecs, NELEM(pspecs), param, value);
}

cJSON *
kvdb_rparams_to_json(const struct kvdb_rparams *const params)
{
    struct params p = {
        .p_params = { .as_kvdb_rp = params },
        .p_type = PARAMS_KVDB_RP,
    };

    if (!params)
        return NULL;

    return param_to_json(&p, pspecs, NELEM(pspecs));
}
