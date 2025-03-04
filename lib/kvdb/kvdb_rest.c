/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2015-2022 Micron Technology, Inc.  All rights reserved.
 */

#include <string.h>

#include <bsd/stdlib.h>
#include <bsd/string.h>
#include <cjson/cJSON.h>
#include <cjson/cJSON_Utils.h>

#include <hse/error/merr.h>
#include <hse/experimental.h>
#include <hse/flags.h>
#include <hse/hse.h>
#include <hse/logging/logging.h>
#include <hse/rest/headers.h>
#include <hse/rest/method.h>
#include <hse/rest/params.h>
#include <hse/rest/request.h>
#include <hse/rest/response.h>
#include <hse/rest/server.h>
#include <hse/rest/status.h>
#include <hse/util/event_counter.h>
#include <hse/util/fmt.h>
#include <hse/util/printbuf.h>

#include <hse/ikvdb/ikvdb.h>
#include <hse/ikvdb/kvs.h>
#include <hse/ikvdb/kvset_view.h>
#include <hse/ikvdb/cn.h>
#include <hse/ikvdb/cn_tree_view.h>
#include <hse/ikvdb/kvset_view.h>
#include <hse/ikvdb/kvdb_cparams.h>
#include <hse/ikvdb/kvdb_rparams.h>
#include <hse/ikvdb/csched.h>
#include <hse/ikvdb/hse_gparams.h>

#include "kvdb_rest.h"
#include "kvdb_kvs.h"

#define ENDPOINT_FMT_KVDB_COMPACT  "/kvdbs/%s/compact"
#define ENDPOINT_FMT_KVDB_HOME     "/kvdbs/%s/home"
#define ENDPOINT_FMT_KVDB_KVS      "/kvdbs/%s/kvs"
#define ENDPOINT_FMT_KVDB_MCLASSES "/kvdbs/%s/mclass"
#define ENDPOINT_FMT_KVDB_MCLASS   "/kvdbs/%s/mclass/%s"
#define ENDPOINT_FMT_KVDB_PARAMS   "/kvdbs/%s/params"
#define ENDPOINT_FMT_KVDB_PERFC    "/kvdbs/%s/perfc"
#define ENDPOINT_FMT_KVS_PARAMS    "/kvdbs/%s/kvs/%s/params"
#define ENDPOINT_FMT_KVS_PERFC     "/kvdbs/%s/kvs/%s/perfc"

#define HUMAN_THRESHOLD 10000

static enum rest_status
rest_kvdb_get_kvs_names(
    const struct rest_request *const req,
    struct rest_response *const resp,
    void *const ctx)
{
    char *data;
    merr_t err;
    bool pretty;
    cJSON *root;
    size_t namec;
    char **namev;
    struct ikvdb *ikvdb;
    enum rest_status status;

    INVARIANT(req);
    INVARIANT(resp);
    INVARIANT(ctx);

    ikvdb = ctx;

    err = rest_params_get(req->rr_params, "pretty", &pretty, false);
    if (ev(err))
        return rest_response_perror(resp, REST_STATUS_BAD_REQUEST,
            "The 'pretty' query parameter must be a boolean", merr(EINVAL));

    err = ikvdb_kvs_names_get(ikvdb, &namec, &namev);
    if (ev(err))
        return rest_response_perror(resp, REST_STATUS_SERVICE_UNAVAILABLE, "Out of memory",
            merr(ENOMEM));

    root = cJSON_CreateArray();
    if (ev(!root)) {
        status = rest_response_perror(resp, REST_STATUS_SERVICE_UNAVAILABLE, "Out of memory",
            merr(ENOMEM));
        goto out;
    }

    for (size_t i = 0; i < namec; i++) {
        cJSON *kvs = cJSON_CreateString(namev[i]);
        if (ev(!kvs)) {
            status = rest_response_perror(resp, REST_STATUS_SERVICE_UNAVAILABLE, "Out of memory",
                merr(ENOMEM));
            goto out;
        }

        if (ev(!cJSON_AddItemToArray(root, kvs))) {
            status = rest_response_perror(resp, REST_STATUS_SERVICE_UNAVAILABLE, "Out of memory",
                merr(ENOMEM));
            goto out;
        }
    }

    data = (pretty ? cJSON_Print : cJSON_PrintUnformatted)(root);
    if (ev(!data)) {
        status = rest_response_perror(resp, REST_STATUS_SERVICE_UNAVAILABLE, "Out of memory",
            merr(ENOMEM));
        goto out;
    }

    fputs(data, resp->rr_stream);
    cJSON_free(data);

    rest_headers_set(resp->rr_headers, REST_HEADER_CONTENT_TYPE, REST_APPLICATION_JSON);
    status = REST_STATUS_OK;

out:
    cJSON_Delete(root);
    ikvdb_kvs_names_free(ikvdb, namev);

    return status;
}

static enum rest_status
rest_kvdb_get_home(
    const struct rest_request *const req,
    struct rest_response *const resp,
    void *const ctx)
{
    merr_t err;
    bool pretty;
    const char *home;
    struct ikvdb *kvdb;

    INVARIANT(req);
    INVARIANT(resp);
    INVARIANT(ctx);

    kvdb = ctx;
    home = ikvdb_home(kvdb);

    err = rest_params_get(req->rr_params, "pretty", &pretty, false);
    if (ev(err))
        return rest_response_perror(resp, REST_STATUS_BAD_REQUEST,
            "The 'pretty' query parameter must be a boolean", merr(EINVAL));

    fprintf(resp->rr_stream, "\"%s\"", home);

    rest_headers_set(resp->rr_headers, REST_HEADER_CONTENT_TYPE, REST_APPLICATION_JSON);

    return REST_STATUS_OK;
}

static enum rest_status
rest_kvdb_get_mclass(
    const struct rest_request *const req,
    struct rest_response *const resp,
    void *const ctx)
{
    char *data;
    merr_t err;
    cJSON *root;
    bool pretty;
    struct ikvdb *kvdb;
    enum rest_status status = REST_STATUS_OK;

    INVARIANT(req);
    INVARIANT(resp);
    INVARIANT(ctx);

    kvdb = ctx;

    err = rest_params_get(req->rr_params, "pretty", &pretty, false);
    if (ev(err))
        return rest_response_perror(resp, REST_STATUS_BAD_REQUEST,
            "The 'pretty' query parameter must be a boolean", merr(EINVAL));

    root = cJSON_CreateArray();
    if (ev(!root))
        return rest_response_perror(resp, REST_STATUS_SERVICE_UNAVAILABLE, "Out of memory",
            merr(ENOMEM));

    for (int i = 0; i < HSE_MCLASS_COUNT; i++) {
        if (mpool_mclass_is_configured(ikvdb_mpool_get(kvdb), i)) {
            cJSON *mclass = cJSON_CreateString(hse_mclass_name_get(i));
            if (ev(!mclass)) {
                status = rest_response_perror(resp, REST_STATUS_SERVICE_UNAVAILABLE,
                    "Out of memory", merr(ENOMEM));
                goto out;
            }

            if (ev(!cJSON_AddItemToArray(root, mclass))) {
                status = rest_response_perror(resp, REST_STATUS_SERVICE_UNAVAILABLE,
                    "Out of memory", merr(ENOMEM));
                goto out;
            }
        }
    }

    data = (pretty ? cJSON_Print : cJSON_PrintUnformatted)(root);
    if (ev(!data)) {
        status = rest_response_perror(resp, REST_STATUS_SERVICE_UNAVAILABLE, "Out of memory",
            merr(ENOMEM));
        goto out;
    }

    fputs(data, resp->rr_stream);
    cJSON_free(data);

    rest_headers_set(resp->rr_headers, REST_HEADER_CONTENT_TYPE, REST_APPLICATION_JSON);

out:
    cJSON_Delete(root);

    return status;
}

static enum rest_status
rest_kvdb_params_get(
    const struct rest_request *const req,
    struct rest_response *const resp,
    void *const ctx)
{
    merr_t err;
    bool pretty;
    struct ikvdb *kvdb;

    INVARIANT(req);
    INVARIANT(resp);
    INVARIANT(ctx);

    kvdb = ctx;

    err = rest_params_get(req->rr_params, "pretty", &pretty, false);
    if (ev(err))
        return rest_response_perror(resp, REST_STATUS_BAD_REQUEST,
            "The 'pretty' query parameter must be a boolean", merr(EINVAL));

        /* Check for single parameter or all parameters */
    if (strcmp(req->rr_matched, req->rr_actual)) {
        merr_t err;
        char sbuf[128];
        size_t needed_sz;
        const char *param;

        /* move past the final '/' */
        param = req->rr_actual + strlen(req->rr_matched) + 1;

        err = ikvdb_param_get(kvdb, param, sbuf, sizeof(sbuf), &needed_sz);
        if (ev(err)) {
            log_errx("Failed to read HSE global param (%s)", err, param);

            switch (merr_errno(err)) {
            case EINVAL:
                return rest_response_perror(resp, REST_STATUS_BAD_REQUEST, "No request body", err);
            case ENOENT:
                return rest_response_perror(resp, REST_STATUS_NOT_FOUND,
                    "Parameter does not exist", err);
            default:
                return rest_response_perror(resp, REST_STATUS_INTERNAL_SERVER_ERROR,
                    "Unhandled error", err);
            }
        }

        if (needed_sz >= sizeof(sbuf)) {
            char *dbuf;
            size_t dbuf_sz;

            dbuf_sz = needed_sz + 1;
            dbuf = malloc(dbuf_sz * sizeof(*dbuf));
            if (ev(!dbuf))
                return rest_response_perror(resp, REST_STATUS_SERVICE_UNAVAILABLE, "Out of memory",
                    merr(ENOMEM));

            err = ikvdb_param_get(kvdb, param, dbuf, dbuf_sz, NULL);
            assert(err == 0);

            fputs(dbuf, resp->rr_stream);
            free(dbuf);
        } else {
            fputs(sbuf, resp->rr_stream);
        }
    } else {
        char *data;
        struct kvdb_cparams cparams;
        cJSON *merged, *cp_json, *rp_json;

        err = ikvdb_cparams(kvdb, &cparams);
        assert(err == 0);

        cp_json = kvdb_cparams_to_json(&cparams);
        if (ev(!cp_json))
            return rest_response_perror(resp, REST_STATUS_SERVICE_UNAVAILABLE, "Out of memory",
                merr(ENOMEM));

        rp_json = kvdb_rparams_to_json(ikvdb_rparams(kvdb));
        if (ev(!rp_json)) {
            cJSON_Delete(cp_json);
            return rest_response_perror(resp, REST_STATUS_SERVICE_UNAVAILABLE, "Out of memory",
                merr(ENOMEM));
        }

        merged = cJSONUtils_MergePatchCaseSensitive(cp_json, rp_json);
        if (ev(!merged)) {
            cJSON_Delete(cp_json);
            cJSON_Delete(rp_json);
            return rest_response_perror(resp, REST_STATUS_SERVICE_UNAVAILABLE, "Out of memory",
                merr(ENOMEM));
        }

        data = (pretty ? cJSON_Print : cJSON_PrintUnformatted)(merged);
        cJSON_Delete(merged);
        cJSON_Delete(rp_json);
        if (ev(!data))
            return rest_response_perror(resp, REST_STATUS_SERVICE_UNAVAILABLE, "Out of memory",
                merr(ENOMEM));

        fputs(data, resp->rr_stream);
        cJSON_free(data);
    }

    rest_headers_set(resp->rr_headers, REST_HEADER_CONTENT_TYPE, REST_APPLICATION_JSON);

    return REST_STATUS_OK;
}

static enum rest_status
rest_kvdb_params_put(
    const struct rest_request *const req,
    struct rest_response *const resp,
    void *const ctx)
{
    merr_t err;
    bool has_param;
    const char *param;
    struct ikvdb *kvdb;
    const char *content_type;

    INVARIANT(req);
    INVARIANT(resp);
    INVARIANT(ctx);

    kvdb = ctx;
    has_param = strcmp(req->rr_matched, req->rr_actual);

    /* Check for case when no parameter is specified, /params */
    if (ev(!has_param))
        return rest_response_perror(resp, REST_STATUS_METHOD_NOT_ALLOWED,
            "Method for endpoint does not exist", merr(ENOENT));

    content_type = rest_headers_get(req->rr_headers, REST_HEADER_CONTENT_TYPE);
    if (ev(!content_type || strcmp(content_type, REST_APPLICATION_JSON) != 0))
        return rest_response_perror(resp, REST_STATUS_BAD_REQUEST,
            "Invalid '"REST_HEADER_CONTENT_TYPE"' header", merr(EINVAL));

    /* move past the final '/' */
    param = req->rr_actual + strlen(req->rr_matched) + 1;

    err = kvdb_rparams_set(ikvdb_rparams(kvdb), param, req->rr_data);
    if (ev(err)) {
        log_errx("Failed to set KVDB parameter (%s)", err, param);

        switch (merr_errno(err)) {
        case EINVAL:
            return rest_response_perror(resp, REST_STATUS_BAD_REQUEST, "No request body", err);
        case ENOENT:
            return rest_response_perror(resp, REST_STATUS_NOT_FOUND, "Parameter does not exist",
                err);
        case EROFS:
            return rest_response_perror(resp, REST_STATUS_LOCKED, "Parameter is not writable", err);
        default:
            return rest_response_perror(resp, REST_STATUS_INTERNAL_SERVER_ERROR,
                "Unhandled error", err);
        }
    }

    return REST_STATUS_CREATED;
}

static enum rest_status
rest_kvdb_get_perfc(
    const struct rest_request *const req,
    struct rest_response *const resp,
    void *const ctx)
{
    char *data;
    merr_t err;
    cJSON *root;
    bool pretty;
    bool filtered;
    const char *alias;
    const char *filter;
    struct ikvdb *kvdb;
    enum rest_status status;
    char dt_path[DT_PATH_MAX];

    INVARIANT(req);
    INVARIANT(resp);
    INVARIANT(ctx);

    kvdb = ctx;
    alias = ikvdb_alias(kvdb);
    filtered = strcmp(req->rr_actual, req->rr_matched);
    filter = filtered ? req->rr_actual + strlen(req->rr_matched) + 1 : NULL;

    err = rest_params_get(req->rr_params, "pretty", &pretty, false);
    if (ev(err))
        return rest_response_perror(resp, REST_STATUS_BAD_REQUEST,
            "The 'pretty' query parameter must be a boolean", merr(EINVAL));

    snprintf(dt_path, sizeof(dt_path), PERFC_DT_PATH "/kvdbs/%s%s%s",
        alias, filtered ? "/" : "", filtered ? filter : "");

    err = dt_emit(dt_path, &root);
    if (ev(err)) {
        switch (merr_errno(err)) {
        case ENAMETOOLONG:
            /* Impossible to get this error unless the path doesn't exist
             * because it would be impossible tt register a data tree path
             * that is too long.
             */
        case ENOENT:
            return rest_response_perror(resp, REST_STATUS_NOT_FOUND,
                "Data tree element does not exist", err);
        default:
            return rest_response_perror(resp, REST_STATUS_INTERNAL_SERVER_ERROR,
                "Unhandled error", err);
        }
    }

    data = (pretty ? cJSON_Print : cJSON_PrintUnformatted)(root);
    if (ev(!data)) {
        status = rest_response_perror(resp, REST_STATUS_SERVICE_UNAVAILABLE, "Out of memory",
            merr(ENOMEM));
        goto out;
    }

    fputs(data, resp->rr_stream);
    cJSON_free(data);

    rest_headers_set(resp->rr_headers, REST_HEADER_CONTENT_TYPE, REST_APPLICATION_JSON);
    status = REST_STATUS_OK;

out:
    cJSON_Delete(root);

    return status;
}

static enum rest_status
rest_kvs_params_get(
    const struct rest_request *const req,
    struct rest_response *const resp,
    void *const ctx)
{
    merr_t err;
    bool pretty;
    struct kvdb_kvs *kvs;

    INVARIANT(req);
    INVARIANT(resp);
    INVARIANT(ctx);

    kvs = ctx;

    err = rest_params_get(req->rr_params, "pretty", &pretty, false);
    if (ev(err))
        return rest_response_perror(resp, REST_STATUS_BAD_REQUEST,
            "The 'pretty' query parameter must be a boolean", merr(EINVAL));

        /* Check for single parameter or all parameters */
    if (strcmp(req->rr_matched, req->rr_actual)) {
        merr_t err;
        char sbuf[128];
        size_t needed_sz;
        const char *param;

        /* move past the final '/' */
        param = req->rr_actual + strlen(req->rr_matched) + 1;

        err = ikvdb_kvs_param_get((struct hse_kvs *)kvs, param, sbuf, sizeof(sbuf), &needed_sz);
        if (ev(err)) {
            log_errx("Failed to read HSE global param (%s)", err, param);

            switch (merr_errno(err)) {
            case EINVAL:
                return rest_response_perror(resp, REST_STATUS_BAD_REQUEST, "No request body", err);
            case ENOENT:
                return rest_response_perror(resp, REST_STATUS_NOT_FOUND,
                    "Parameter does not exist", err);
            default:
                return rest_response_perror(resp, REST_STATUS_INTERNAL_SERVER_ERROR,
                    "Unhandled error", err);
            }
        }

        if (needed_sz >= sizeof(sbuf)) {
            char *dbuf;
            size_t dbuf_sz;

            dbuf_sz = needed_sz + 1;
            dbuf = malloc(dbuf_sz * sizeof(*dbuf));
            if (ev(!dbuf))
                return rest_response_perror(resp, REST_STATUS_SERVICE_UNAVAILABLE, "Out of memory",
                    merr(ENOMEM));

            err = ikvdb_kvs_param_get((struct hse_kvs *)kvs, param, dbuf, dbuf_sz, NULL);
            assert(err == 0);

            fputs(dbuf, resp->rr_stream);
            free(dbuf);
        } else {
            fputs(sbuf, resp->rr_stream);
        }
    } else {
        char *data;
        cJSON *merged, *cp_json, *rp_json;

        cp_json = kvs_cparams_to_json(kvs->kk_cparams);
        if (ev(!cp_json))
            return rest_response_perror(resp, REST_STATUS_SERVICE_UNAVAILABLE, "Out of memory",
                merr(ENOMEM));

        rp_json = kvs_rparams_to_json(&kvs->kk_ikvs->ikv_rp);
        if (ev(!rp_json)) {
            cJSON_Delete(cp_json);
            return rest_response_perror(resp, REST_STATUS_SERVICE_UNAVAILABLE, "Out of memory",
                merr(ENOMEM));
        }

        merged = cJSONUtils_MergePatchCaseSensitive(cp_json, rp_json);
        if (ev(!merged)) {
            cJSON_Delete(cp_json);
            cJSON_Delete(rp_json);
            return rest_response_perror(resp, REST_STATUS_SERVICE_UNAVAILABLE, "Out of memory",
                merr(ENOMEM));
        }

        data = (pretty ? cJSON_Print : cJSON_PrintUnformatted)(merged);
        cJSON_Delete(merged);
        cJSON_Delete(rp_json);
        if (ev(!data))
            return rest_response_perror(resp, REST_STATUS_SERVICE_UNAVAILABLE, "Out of memory",
                merr(ENOMEM));

        fputs(data, resp->rr_stream);
        cJSON_free(data);
    }

    rest_headers_set(resp->rr_headers, REST_HEADER_CONTENT_TYPE, REST_APPLICATION_JSON);

    return REST_STATUS_OK;
}

static enum rest_status
rest_kvs_params_put(
    const struct rest_request *const req,
    struct rest_response *const resp,
    void *const ctx)
{
    merr_t err;
    bool has_param;
    const char *param;
    struct kvdb_kvs *kvs;
    const char *content_type;

    INVARIANT(req);
    INVARIANT(resp);
    INVARIANT(ctx);

    kvs = ctx;
    has_param = strcmp(req->rr_matched, req->rr_actual);

    /* Check for case when no parameter is specified, /params */
    if (ev(!has_param))
        return rest_response_perror(resp, REST_STATUS_METHOD_NOT_ALLOWED,
            "Method for endpoint does not exist", merr(ENOENT));

    content_type = rest_headers_get(req->rr_headers, REST_HEADER_CONTENT_TYPE);
    if (ev(!content_type || strcmp(content_type, REST_APPLICATION_JSON) != 0))
        return rest_response_perror(resp, REST_STATUS_BAD_REQUEST,
            "Invalid '"REST_HEADER_CONTENT_TYPE"' header", merr(EINVAL));

    /* move past the final '/' */
    param = req->rr_actual + strlen(req->rr_matched) + 1;

    err = kvs_rparams_set(&kvs->kk_ikvs->ikv_rp, param, req->rr_data);
    if (ev(err)) {
        log_errx("Failed to set KVS parameter (%s)", err, param);

        switch (merr_errno(err)) {
        case EINVAL:
            return rest_response_perror(resp, REST_STATUS_BAD_REQUEST, "No request body", err);
        case ENOENT:
            return rest_response_perror(resp, REST_STATUS_NOT_FOUND, "Parameter does not exist",
                err);
        case EROFS:
            return rest_response_perror(resp, REST_STATUS_LOCKED, "Parameter is not writable", err);
        default:
            return rest_response_perror(resp, REST_STATUS_INTERNAL_SERVER_ERROR,
                "Unhandled error", err);
        }
    }

    return REST_STATUS_CREATED;
}

static enum rest_status
rest_kvs_get_perfc(
    const struct rest_request *const req,
    struct rest_response *const resp,
    void *const ctx)
{
    char *data;
    merr_t err;
    cJSON *root;
    bool pretty;
    bool filtered;
    const char *alias;
    const char *filter;
    struct ikvdb *kvdb;
    struct kvdb_kvs *kvs;
    enum rest_status status;
    char dt_path[DT_PATH_MAX];

    INVARIANT(req);
    INVARIANT(resp);
    INVARIANT(ctx);

    kvs = ctx;
    kvdb = ikvdb_kvdb_handle(kvs->kk_parent);
    filtered = strcmp(req->rr_actual, req->rr_matched);
    filter = filtered ? req->rr_actual + strlen(req->rr_matched) + 1 : NULL;
    alias = ikvdb_alias(kvdb);

    err = rest_params_get(req->rr_params, "pretty", &pretty, false);
    if (ev(err))
        return rest_response_perror(resp, REST_STATUS_BAD_REQUEST,
            "The 'pretty' query parameter must be a boolean", merr(EINVAL));

    snprintf(dt_path, sizeof(dt_path), PERFC_DT_PATH "/kvdbs/%s/kvs/%s%s%s",
        alias, kvs->kk_ikvs->ikv_kvs_name,
        filtered ? "/" : "", filtered ? filter : "");

    err = dt_emit(dt_path, &root);
    if (ev(err)) {
        switch (merr_errno(err)) {
        case ENAMETOOLONG:
            /* Impossible to get this error unless the path doesn't exist
             * because it would be impossible tt register a data tree path
             * that is too long.
             */
        case ENOENT:
            return rest_response_perror(resp, REST_STATUS_NOT_FOUND,
                "Data tree element does not exist", err);
        default:
            return rest_response_perror(resp, REST_STATUS_INTERNAL_SERVER_ERROR,
                "Unhandled error", err);
        }
    }

    data = (pretty ? cJSON_Print : cJSON_PrintUnformatted)(root);
    if (ev(!data)) {
        status = rest_response_perror(resp, REST_STATUS_SERVICE_UNAVAILABLE, "Out of memory",
            merr(ENOMEM));
        goto out;
    }

    fputs(data, resp->rr_stream);
    cJSON_free(data);

    rest_headers_set(resp->rr_headers, REST_HEADER_CONTENT_TYPE, REST_APPLICATION_JSON);
    status = REST_STATUS_OK;

out:
    cJSON_Delete(root);

    return status;
}

static enum rest_status
rest_kvdb_mclass_info_get(
    const struct rest_request *const req,
    struct rest_response *const resp,
    void *const ctx)
{
    char *data;
    merr_t err;
    bool pretty;
    cJSON *root;
    bool bad = false;
    enum rest_status status;
    struct hse_mclass_info mc_info;
    enum hse_mclass mclass = HSE_MCLASS_INVALID;

    INVARIANT(req);
    INVARIANT(resp);
    INVARIANT(ctx);

    for (int i = HSE_MCLASS_BASE; i < HSE_MCLASS_COUNT; i++) {
        if (strstr(req->rr_actual, hse_mclass_name_get(i))) {
            mclass = i;
            break;
        }
    }
    assert(mclass != HSE_MCLASS_INVALID);

    err = rest_params_get(req->rr_params, "pretty", &pretty, false);
    if (ev(err))
        return rest_response_perror(resp, REST_STATUS_BAD_REQUEST,
            "The 'pretty' query parameter must be a boolean", merr(EINVAL));

    err = ikvdb_mclass_info_get((struct ikvdb *)ctx, mclass, &mc_info);
    if (ev(err)) {
        switch (merr_errno(err)) {
        case ENOENT:
            return rest_response_perror(resp, REST_STATUS_NOT_FOUND,
                "Media class is not configured", err);
        default:
            return rest_response_perror(resp, REST_STATUS_INTERNAL_SERVER_ERROR,
                "Unhandled error", err);
        }
    }

    root = cJSON_CreateObject();
    if (ev(!root))
        return rest_response_perror(resp, REST_STATUS_SERVICE_UNAVAILABLE, "Out of memory",
            merr(ENOMEM));

    bad |= !cJSON_AddNumberToObject(root, "allocated_bytes", mc_info.mi_allocated_bytes);
    bad |= !cJSON_AddNumberToObject(root, "used_bytes", mc_info.mi_used_bytes);
    bad |= !cJSON_AddStringToObject(root, "path", mc_info.mi_path);

    if (ev(bad)) {
        status = rest_response_perror(resp, REST_STATUS_SERVICE_UNAVAILABLE, "Out of memory",
            merr(ENOMEM));
        goto out;
    }

    data = (pretty ? cJSON_Print : cJSON_PrintUnformatted)(root);
    if (ev(!data)) {
        status = rest_response_perror(resp, REST_STATUS_SERVICE_UNAVAILABLE, "Out of memory",
            merr(ENOMEM));
        goto out;
    }

    fputs(data, resp->rr_stream);
    cJSON_free(data);

    rest_headers_set(resp->rr_headers, REST_HEADER_CONTENT_TYPE, REST_APPLICATION_JSON);
    status = REST_STATUS_OK;

out:
    cJSON_Delete(root);

    return status;
}

static enum rest_status
rest_kvdb_compact_request(
    const struct rest_request *const req,
    struct rest_response *const resp,
    void *const ctx)
{
    struct ikvdb *kvdb;
    const int flags = HSE_KVDB_COMPACT_SAMP_LWM;

    INVARIANT(req);
    INVARIANT(resp);
    INVARIANT(ctx);

    kvdb = ctx;

    ikvdb_compact(kvdb, flags);

    return REST_STATUS_ACCEPTED;
}

static enum rest_status
rest_kvdb_compact_cancel(
    const struct rest_request *const req,
    struct rest_response *const resp,
    void *const ctx)
{
    struct ikvdb *kvdb;
    const int flags = HSE_KVDB_COMPACT_CANCEL;

    INVARIANT(req);
    INVARIANT(resp);
    INVARIANT(ctx);

    kvdb = ctx;

    ikvdb_compact(kvdb, flags);

    return REST_STATUS_ACCEPTED;
}

static enum rest_status
rest_kvdb_compact_status_get(
    const struct rest_request *const req,
    struct rest_response *const resp,
    void *const ctx)
{
    bool bad;
    char *data;
    merr_t err;
    cJSON *root;
    bool pretty;
    struct ikvdb *kvdb;
    enum rest_status status;
    struct hse_kvdb_compact_status compact_status = { 0 };

    INVARIANT(req);
    INVARIANT(resp);
    INVARIANT(ctx);

    kvdb = ctx;

    err = rest_params_get(req->rr_params, "pretty", &pretty, false);
    if (ev(err))
        return rest_response_perror(resp, REST_STATUS_BAD_REQUEST,
            "The 'pretty' query parameter must be a boolean", merr(EINVAL));

    ikvdb_compact_status_get(kvdb, &compact_status);

    root = cJSON_CreateObject();
    if (ev(!root))
        return rest_response_perror(resp, REST_STATUS_SERVICE_UNAVAILABLE, "Out of memory",
            merr(ENOMEM));

    bad = !cJSON_AddNumberToObject(root, "samp_lwm_pct", compact_status.kvcs_samp_lwm);
    bad |= !cJSON_AddNumberToObject(root, "samp_hwm_pct", compact_status.kvcs_samp_hwm);
    bad |= !cJSON_AddNumberToObject(root, "samp_hwm_pct", compact_status.kvcs_samp_hwm);
    bad |= !cJSON_AddNumberToObject(root, "samp_curr_pct", compact_status.kvcs_samp_curr);
    bad |= !cJSON_AddBoolToObject(root, "active", compact_status.kvcs_active);
    bad |= !cJSON_AddBoolToObject(root, "canceled", compact_status.kvcs_canceled);

    if (ev(bad)) {
        status = rest_response_perror(resp, REST_STATUS_SERVICE_UNAVAILABLE, "Out of memory",
            merr(ENOMEM));
        goto out;
    }

    data = (pretty ? cJSON_Print : cJSON_PrintUnformatted)(root);
    if (ev(!data)) {
        status = rest_response_perror(resp, REST_STATUS_SERVICE_UNAVAILABLE, "Out of memory",
            merr(ENOMEM));
        goto out;
    }

    fputs(data, resp->rr_stream);
    cJSON_free(data);

    rest_headers_set(resp->rr_headers, REST_HEADER_CONTENT_TYPE, REST_APPLICATION_JSON);
    status = REST_STATUS_OK;

out:
    cJSON_Delete(root);

    return status;
}

merr_t
kvdb_rest_add_endpoints(struct ikvdb *const kvdb)
{
    static rest_handler *handlers[][REST_METHOD_COUNT] = {
        {
            [REST_METHOD_GET] = rest_kvdb_compact_status_get,
            [REST_METHOD_POST] = rest_kvdb_compact_request,
            [REST_METHOD_DELETE] = rest_kvdb_compact_cancel,
        },
        {
            [REST_METHOD_GET] = rest_kvdb_get_home,
        },
        {
            [REST_METHOD_GET] = rest_kvdb_get_kvs_names,
        },
        {
            [REST_METHOD_GET] = rest_kvdb_get_mclass,
        },
        {
            [REST_METHOD_GET] = rest_kvdb_mclass_info_get,
        },
        {
            [REST_METHOD_GET] = rest_kvdb_params_get,
            [REST_METHOD_PUT] = rest_kvdb_params_put,
        },
        {
            [REST_METHOD_GET] = rest_kvdb_get_perfc,
        },
    };

    merr_t err = 0;
    const char *alias;

    if (ev(!kvdb))
        return merr(EINVAL);

    alias = ikvdb_alias(kvdb);

    err = rest_server_add_endpoint(0, handlers[0], kvdb, ENDPOINT_FMT_KVDB_COMPACT, alias);
    if (err) {
        log_errx("Failed to add REST endpoint (" ENDPOINT_FMT_KVDB_COMPACT ")", err, alias);
        return err;
    }

    err = rest_server_add_endpoint(REST_ENDPOINT_EXACT, handlers[1], kvdb, ENDPOINT_FMT_KVDB_HOME,
        alias);
    if (err) {
        log_errx("Failed to add REST endpoint (" ENDPOINT_FMT_KVDB_HOME ")", err, alias);
        return err;
    }

    err = rest_server_add_endpoint(REST_ENDPOINT_EXACT, handlers[2], kvdb, ENDPOINT_FMT_KVDB_KVS,
        alias);
    if (err) {
        log_errx("Failed to add REST endpoint (" ENDPOINT_FMT_KVDB_KVS ")", err, alias);
        return err;
    }

    err = rest_server_add_endpoint(
        REST_ENDPOINT_EXACT, handlers[3], kvdb, ENDPOINT_FMT_KVDB_MCLASSES, alias);
    if (err) {
        log_errx("Failed to add REST endpoint (" ENDPOINT_FMT_KVDB_MCLASSES ")", err, alias);
        return err;
    }

    for (int i = HSE_MCLASS_BASE; i < HSE_MCLASS_COUNT; i++) {
        const char *mclass = hse_mclass_name_get(i);

        err = rest_server_add_endpoint(REST_ENDPOINT_EXACT, handlers[4], kvdb,
            ENDPOINT_FMT_KVDB_MCLASS, alias, mclass);
        if (err) {
            log_errx("Failed to add REST endpoint (" ENDPOINT_FMT_KVDB_MCLASS ")", err,
                alias, mclass);
            return err;
        }
    }

    err = rest_server_add_endpoint(0, handlers[5], kvdb, ENDPOINT_FMT_KVDB_PARAMS, alias);
    if (err) {
        log_errx("Failed to add REST endpoint (" ENDPOINT_FMT_KVDB_PARAMS ")", err, alias);
        return err;
    }

    err = rest_server_add_endpoint(0, handlers[6], kvdb, ENDPOINT_FMT_KVDB_PERFC, alias);
    if (err) {
        log_errx("Failed to add REST endpoint (" ENDPOINT_FMT_KVDB_PERFC ")", err, alias);
        return err;
    }

    return 0;
}

void
kvdb_rest_remove_endpoints(struct ikvdb *const kvdb)
{
    const char *alias;

    INVARIANT(kvdb);

    alias = ikvdb_alias(kvdb);

    rest_server_remove_endpoint(ENDPOINT_FMT_KVDB_COMPACT, alias);
    rest_server_remove_endpoint(ENDPOINT_FMT_KVDB_HOME, alias);
    rest_server_remove_endpoint(ENDPOINT_FMT_KVDB_KVS, alias);
    rest_server_remove_endpoint(ENDPOINT_FMT_KVDB_MCLASSES, alias);
    for (int i = HSE_MCLASS_BASE; i < HSE_MCLASS_COUNT; i++)
        rest_server_remove_endpoint(ENDPOINT_FMT_KVDB_MCLASS, alias, hse_mclass_name_get(i));
    rest_server_remove_endpoint(ENDPOINT_FMT_KVDB_PARAMS, alias);
    rest_server_remove_endpoint(ENDPOINT_FMT_KVDB_PERFC, alias);
}

merr_t
kvs_rest_add_endpoints(struct ikvdb *const kvdb, struct kvdb_kvs *const kvs)
{
    static rest_handler *handlers[][REST_METHOD_COUNT] = {
        {
            [REST_METHOD_GET] = rest_kvs_params_get,
            [REST_METHOD_PUT] = rest_kvs_params_put,
        },
        {
            [REST_METHOD_GET] = rest_kvs_get_perfc,
        },
    };

    merr_t err;
    const char *alias;

    INVARIANT(kvdb);
    INVARIANT(kvs);

    alias = ikvdb_alias(kvdb);

    err = rest_server_add_endpoint(0, handlers[0], kvs, ENDPOINT_FMT_KVS_PARAMS,
        alias, kvs->kk_name);
    if (err) {
        log_errx("Failed to add REST endpoint (" ENDPOINT_FMT_KVS_PARAMS ")", err,
            alias, kvs->kk_name);
        goto out;
    }

    err = rest_server_add_endpoint(0, handlers[1], kvs, ENDPOINT_FMT_KVS_PERFC,
        alias, kvs->kk_name);
    if (err) {
        log_errx("Failed to add REST endpoint (" ENDPOINT_FMT_KVS_PERFC ")", err,
            alias, kvs->kk_name);
        goto out;
    }

out:
    if (err) {
        kvs_rest_remove_endpoints(kvdb, kvs);
    } else {
        atomic_inc(&kvs->kk_refcnt);
    }

    return err;
}

void
kvs_rest_remove_endpoints(struct ikvdb *const kvdb, struct kvdb_kvs *const kvs)
{
    const char *alias;

    INVARIANT(kvdb);
    INVARIANT(kvs);

    alias = ikvdb_alias(kvdb);

    rest_server_remove_endpoint(ENDPOINT_FMT_KVS_PARAMS, alias, kvs->kk_name);
    rest_server_remove_endpoint(ENDPOINT_FMT_KVS_PERFC, alias, kvs->kk_name);

    atomic_dec(&kvs->kk_refcnt);
}
