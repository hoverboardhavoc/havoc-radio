#include "stage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "mbedtls/md5.h"
#include "mbedtls/sha256.h"

#include "flash_job.h"
#include "self_update.h"
#include "server.h"

static const char *TAG = "stage";

// A 64-region manifest in JSON is about 8.5 KB.
#define MANIFEST_JSON_MAX 12288

static const esp_partition_t *s_part;
static uint32_t s_stage_size;
static sl_manifest_t s_manifest;  // count 0: no image
static sl_presence_t s_presence;  // per stage sector, in RAM
static bool s_checked;

// WiFi install sessions holding the image, and a stage change in progress. A
// change is refused while a session holds the image; a session that starts
// during a change finds the image incomplete.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static int s_holds;
static bool s_changing;

// ---- Header ------------------------------------------------------------------

static esp_err_t write_header(void)
{
    uint8_t *sec = malloc(SL_HEADER_SIZE);
    if (sec == NULL) {
        return ESP_ERR_NO_MEM;
    }
    sl_header_encode(&s_manifest, sec);
    esp_err_t err = esp_partition_erase_range(s_part, 0, SL_HEADER_SIZE);
    if (err == ESP_OK) {
        err = esp_partition_write(s_part, 0, sec, SL_HEADER_SIZE);
    }
    free(sec);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "header write: %s", esp_err_to_name(err));
    }
    return err;
}

static bool region_sha_ok(const sl_region_t *r, uint8_t *buf)
{
    mbedtls_sha256_context ctx;
    uint8_t digest[32];
    bool ok = true;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    for (uint32_t off = 0; off < r->length && ok; off += SL_SECTOR_SIZE) {
        ok = esp_partition_read(s_part, r->stage + off, buf, SL_SECTOR_SIZE) == ESP_OK;
        mbedtls_sha256_update(&ctx, buf, SL_SECTOR_SIZE);
    }
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);
    return ok && memcmp(digest, r->sha256, sizeof(digest)) == 0;
}

// Hash each data region's staged bytes against the manifest: a matching region
// is marked present, a mismatching one missing. results[i] (when not NULL)
// gets each region's outcome. Returns whether every data region matched.
static bool check_regions(bool *results)
{
    uint8_t *buf = malloc(SL_SECTOR_SIZE);
    if (buf == NULL) {
        return false;
    }
    bool all_ok = true, changed = false;
    for (uint32_t i = 0; i < s_manifest.count; i++) {
        const sl_region_t *r = &s_manifest.regions[i];
        bool ok = sl_is_erase(r) || region_sha_ok(r, buf);
        if (!sl_is_erase(r)) {
            changed |= sl_part_mark(&s_manifest, &s_presence, i, r->stage, r->length, ok);
        }
        if (results) {
            results[i] = ok;
        }
        all_ok = all_ok && ok;
    }
    free(buf);
    if (changed) {
        write_header();
    }
    s_checked = all_ok;
    return all_ok;
}

esp_err_t stage_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "stage");
    if (s_part == NULL) {
        ESP_LOGE(TAG, "no stage partition");
        return ESP_ERR_NOT_FOUND;
    }
    s_stage_size = s_part->size < SL_STAGE_MAX ? s_part->size : SL_STAGE_MAX;

    uint8_t *sec = malloc(SL_HEADER_SIZE);
    if (sec == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_partition_read(s_part, 0, sec, SL_HEADER_SIZE);
    bool valid = err == ESP_OK && sl_header_decode(sec, &s_manifest, s_stage_size);
    free(sec);
    if (!valid) {
        ESP_LOGW(TAG, "stage %" PRIu32 " KB at 0x%" PRIx32 ": no image", s_stage_size / 1024, s_part->address);
        return ESP_OK;
    }
    sl_presence_from_flags(&s_manifest, &s_presence);
    int64_t t0 = esp_timer_get_time();
    bool ok = sl_all_flagged(&s_manifest) && check_regions(NULL);
    ESP_LOGI(TAG, "carried image \"%s\": %" PRIu32 " bytes, %" PRIu32 " regions, %" PRIu32 " of %" PRIu32
             " data regions present, %s (%lld ms)", s_manifest.label, sl_total_length(&s_manifest), s_manifest.count,
             sl_flagged_regions(&s_manifest), sl_data_regions(&s_manifest), ok ? "verified" : "incomplete",
             (esp_timer_get_time() - t0) / 1000);
    return ESP_OK;
}

bool stage_complete(void) { return s_checked; }

const sl_manifest_t *stage_manifest(void) { return &s_manifest; }

esp_err_t stage_read(uint32_t addr, void *buf, size_t len)
{
    return esp_partition_read(s_part, addr, buf, len);
}

esp_err_t stage_region_md5(const sl_region_t *r, char hex[33])
{
    uint8_t *buf = malloc(SL_SECTOR_SIZE);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    mbedtls_md5_context ctx;
    uint8_t digest[16];
    esp_err_t err = ESP_OK;
    memset(buf, 0xFF, SL_SECTOR_SIZE);
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts(&ctx);
    for (uint32_t off = 0; off < r->length && err == ESP_OK; off += SL_SECTOR_SIZE) {
        if (!sl_is_erase(r)) {
            err = esp_partition_read(s_part, r->stage + off, buf, SL_SECTOR_SIZE);
        }
        mbedtls_md5_update(&ctx, buf, SL_SECTOR_SIZE);
    }
    mbedtls_md5_finish(&ctx, digest);
    mbedtls_md5_free(&ctx);
    free(buf);
    for (int i = 0; i < 16; i++) {
        sprintf(hex + 2 * i, "%02x", digest[i]);
    }
    return err;
}

// ---- WiFi install --------------------------------------------------------------

bool stage_flat_read(uint32_t offset, uint8_t *buf, size_t len)
{
    while (len > 0) {
        sl_span_t s = sl_flat_span(&s_manifest, offset, (uint32_t)len);
        if (s.len == 0) {
            return false;
        }
        if (s.fill) {
            memset(buf, 0xFF, s.len);
        } else if (esp_partition_read(s_part, s.stage, buf, s.len) != ESP_OK) {
            return false;
        }
        buf += s.len;
        offset += s.len;
        len -= s.len;
    }
    return true;
}

const char *stage_hold(uint32_t *len)
{
    taskENTER_CRITICAL(&s_lock);
    s_holds++;
    // Counted before asking: an installer update that starts now sees this hold.
    const char *problem = self_update_running() ? "installer update running"
                          : s_checked && !s_changing ? NULL : "no complete image";
    taskEXIT_CRITICAL(&s_lock);
    *len = sl_total_length(&s_manifest);
    return problem;
}

bool stage_held(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool held = s_holds > 0;
    taskEXIT_CRITICAL(&s_lock);
    return held;
}

void stage_release(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_holds--;
    taskEXIT_CRITICAL(&s_lock);
}

// ---- HTTP helpers --------------------------------------------------------------

static esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *root)
{
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", msg);
    return send_json(req, status, root);
}

// Start a change of the stage, or answer 409 and return false while a flash
// job or a WiFi install uses it.
static bool begin_change(httpd_req_t *req)
{
    const char *busy = NULL;
    taskENTER_CRITICAL(&s_lock);
    if (flash_job_running()) {
        busy = "flash job running";
    } else if (s_holds > 0) {
        busy = "WiFi install running";
    } else {
        s_changing = true;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (busy) {
        send_error(req, "409 Conflict", busy);
        return false;
    }
    return true;
}

static void end_change(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_changing = false;
    taskEXIT_CRITICAL(&s_lock);
}

// Receive exactly len body bytes.
static bool recv_exact(httpd_req_t *req, uint8_t *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        int n = httpd_req_recv(req, (char *)buf + got, len - got);
        if (n <= 0) {
            return false;
        }
        got += n;
    }
    return true;
}

static bool json_u32(const cJSON *obj, const char *key, uint32_t *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > 4294967295.0 ||
        v->valuedouble != (double)(uint32_t)v->valuedouble) {
        return false;
    }
    *out = (uint32_t)v->valuedouble;
    return true;
}

static bool query_u32(httpd_req_t *req, const char *key, uint32_t *out)
{
    char q[64];
    char val[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, key, val, sizeof(val)) != ESP_OK || val[0] == '\0') {
        return false;
    }
    char *end;
    unsigned long v = strtoul(val, &end, 0);
    if (*end != '\0' || v > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

// The layout and, per region, which 64 KB parts are present.
static cJSON *stage_state_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "stage_size", s_stage_size);
    cJSON_AddStringToObject(root, "label", s_manifest.label);
    cJSON *regions = cJSON_AddArrayToObject(root, "regions");
    for (uint32_t i = 0; i < s_manifest.count; i++) {
        const sl_region_t *r = &s_manifest.regions[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "target", r->target);
        cJSON_AddNumberToObject(o, "length", r->length);
        cJSON_AddBoolToObject(o, "erase", sl_is_erase(r));
        cJSON_AddNumberToObject(o, "stage", r->stage);
        cJSON *parts = cJSON_AddArrayToObject(o, "parts");
        for (uint32_t k = 0; k < sl_part_count(r); k++) {
            cJSON_AddItemToArray(parts, cJSON_CreateBool(sl_part_present(&s_manifest, &s_presence, i, k)));
        }
        cJSON_AddItemToArray(regions, o);
    }
    return root;
}

cJSON *stage_status_json(void)
{
    uint32_t len = sl_total_length(&s_manifest);
    bool complete = s_checked;
    const char *problem = complete ? server_image_problem(len) : "no complete image";
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "label", s_manifest.label);
    cJSON_AddNumberToObject(o, "length", len);
    cJSON_AddBoolToObject(o, "complete", complete);
    cJSON_AddNumberToObject(o, "regions", s_manifest.count);
    cJSON_AddNumberToObject(o, "data_regions", sl_data_regions(&s_manifest));
    cJSON_AddNumberToObject(o, "present", sl_flagged_regions(&s_manifest));
    cJSON_AddBoolToObject(o, "wifi_ready", problem == NULL);
    cJSON_AddStringToObject(o, "wifi_problem", problem ? problem : "");
    cJSON_AddNumberToObject(o, "wifi_max", US_IMAGE_MAX);
    return o;
}

// ---- POST /stage ---------------------------------------------------------------

static const char *parse_manifest(const char *body, sl_manifest_t *m)
{
    memset(m, 0, sizeof(*m));
    cJSON *root = cJSON_Parse(body);
    const char *err = NULL;
    const cJSON *regions = cJSON_GetObjectItemCaseSensitive(root, "regions");
    const cJSON *label = cJSON_GetObjectItemCaseSensitive(root, "label");
    if (!cJSON_IsArray(regions) || (label != NULL && !cJSON_IsString(label))) {
        err = "need {\"label\": text, \"regions\": [...]}";
        goto out;
    }
    sl_set_label(m, label ? label->valuestring : "");
    int n = cJSON_GetArraySize(regions);
    if (n <= 0 || n > (int)SL_MAX_REGIONS) {
        err = sl_err_str(SL_ERR_COUNT);
        goto out;
    }
    const cJSON *item;
    cJSON_ArrayForEach(item, regions) {
        sl_region_t *r = &m->regions[m->count++];
        const cJSON *sha = cJSON_GetObjectItemCaseSensitive(item, "sha256");
        const cJSON *erase = cJSON_GetObjectItemCaseSensitive(item, "erase");
        if (!json_u32(item, "target", &r->target) || !json_u32(item, "length", &r->length) ||
            !cJSON_IsString(sha) || !sl_hex_decode(sha->valuestring, r->sha256, sizeof(r->sha256)) ||
            !cJSON_IsBool(erase)) {
            err = "each region needs target, length, sha256 (hex) and erase (bool)";
            goto out;
        }
        r->flags = cJSON_IsTrue(erase) ? SL_FLAG_ERASE : 0;
    }
out:
    cJSON_Delete(root);
    return err;
}

static esp_err_t stage_post(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > MANIFEST_JSON_MAX) {
        return send_error(req, "400 Bad Request", "manifest body missing or too large");
    }
    char *body = malloc(req->content_len + 1);
    sl_manifest_t *m = malloc(sizeof(*m));
    if (body == NULL || m == NULL) {
        free(body);
        free(m);
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    if (!recv_exact(req, (uint8_t *)body, req->content_len)) {
        free(body);
        free(m);
        return ESP_FAIL;
    }
    body[req->content_len] = '\0';

    const char *err = parse_manifest(body, m);
    free(body);
    sl_err_t lerr = SL_OK;
    if (err == NULL && (lerr = sl_layout(m, s_stage_size)) != SL_OK) {
        err = sl_err_str(lerr);
    }
    if (err != NULL) {
        free(m);
        return send_error(req, "400 Bad Request", err);
    }

    bool kept = s_manifest.count != 0 && sl_manifest_equal(m, &s_manifest);
    if (!kept) {
        s_manifest = *m;  // every region absent
        sl_presence_clear(&s_presence);
        s_checked = false;
        ESP_LOGI(TAG, "upload: new image \"%s\", %" PRIu32 " regions, %" PRIu32 " bytes", s_manifest.label,
                 s_manifest.count, sl_total_length(&s_manifest));
    } else {
        ESP_LOGI(TAG, "upload: same regions as the carried image, %" PRIu32 " of %" PRIu32 " data regions present",
                 sl_flagged_regions(&s_manifest), sl_data_regions(&s_manifest));
    }
    bool relabel = strcmp(m->label, s_manifest.label) != 0;
    if (relabel) {
        memcpy(s_manifest.label, m->label, sizeof(s_manifest.label));
    }
    free(m);
    if ((!kept || relabel) && write_header() != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "stage header write failed");
    }
    cJSON *root = stage_state_json();
    cJSON_AddBoolToObject(root, "kept", kept);
    return send_json(req, "200 OK", root);
}

static esp_err_t h_stage_post(httpd_req_t *req)
{
    if (!begin_change(req)) {
        return ESP_OK;
    }
    esp_err_t err = stage_post(req);
    end_change();
    return err;
}

static esp_err_t h_stage_get(httpd_req_t *req)
{
    return send_json(req, "200 OK", stage_state_json());
}

// ---- PUT /stage/part -------------------------------------------------------------

static esp_err_t stage_part(httpd_req_t *req)
{
    uint32_t region, offset, addr;
    if (!query_u32(req, "region", &region) || !query_u32(req, "offset", &offset)) {
        return send_error(req, "400 Bad Request", "need region=<i>&offset=<n>");
    }
    sl_err_t lerr = sl_part_check(&s_manifest, region, offset, req->content_len, &addr);
    if (lerr != SL_OK) {
        return send_error(req, "400 Bad Request", sl_err_str(lerr));
    }

    // Mark the sectors missing before touching them: a reboot mid-write never
    // leaves a half-written region flagged present.
    s_checked = false;
    if (sl_part_mark(&s_manifest, &s_presence, region, addr, req->content_len, false) &&
        write_header() != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "stage header write failed");
    }

    uint8_t *buf = malloc(2 * SL_SECTOR_SIZE);
    if (buf == NULL) {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    uint8_t *back = buf + SL_SECTOR_SIZE;
    const char *err = NULL;
    for (uint32_t off = 0; off < req->content_len; off += SL_SECTOR_SIZE) {
        if (!recv_exact(req, buf, SL_SECTOR_SIZE)) {
            free(buf);
            return ESP_FAIL;  // connection gone; the uploader retries the part
        }
        if (esp_partition_erase_range(s_part, addr + off, SL_SECTOR_SIZE) != ESP_OK ||
            esp_partition_write(s_part, addr + off, buf, SL_SECTOR_SIZE) != ESP_OK ||
            esp_partition_read(s_part, addr + off, back, SL_SECTOR_SIZE) != ESP_OK) {
            err = "stage flash write failed";
            break;
        }
        if (memcmp(buf, back, SL_SECTOR_SIZE) != 0) {
            err = "stage read-back mismatch";
            break;
        }
    }
    free(buf);
    if (err != NULL) {
        return send_error(req, "500 Internal Server Error", err);
    }

    if (sl_part_mark(&s_manifest, &s_presence, region, addr, req->content_len, true) &&
        write_header() != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "stage header write failed");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "region", region);
    cJSON_AddNumberToObject(root, "offset", offset);
    cJSON_AddNumberToObject(root, "length", req->content_len);
    return send_json(req, "200 OK", root);
}

static esp_err_t h_stage_part(httpd_req_t *req)
{
    if (!begin_change(req)) {
        return ESP_OK;
    }
    esp_err_t err = stage_part(req);
    end_change();
    return err;
}

// ---- POST /stage/check -----------------------------------------------------------

static esp_err_t h_stage_check(httpd_req_t *req)
{
    if (!begin_change(req)) {
        return ESP_OK;
    }
    if (s_manifest.count == 0) {
        end_change();
        return send_error(req, "409 Conflict", "no manifest staged");
    }
    bool results[SL_MAX_REGIONS];
    bool all_ok = check_regions(results);
    end_change();
    cJSON *root = cJSON_CreateObject();
    cJSON *regions = cJSON_AddArrayToObject(root, "regions");
    for (uint32_t i = 0; i < s_manifest.count; i++) {
        cJSON_AddItemToArray(regions, cJSON_CreateString(results[i] ? "ok" : "mismatch"));
    }
    cJSON_AddBoolToObject(root, "ok", all_ok);
    ESP_LOGI(TAG, "upload: check of \"%s\" %s", s_manifest.label, all_ok ? "passed, now the carried image" : "mismatch");
    return send_json(req, "200 OK", root);
}

void stage_http_register(httpd_handle_t server)
{
    const httpd_uri_t uris[] = {
        {.uri = "/stage", .method = HTTP_POST, .handler = h_stage_post},
        {.uri = "/stage", .method = HTTP_GET, .handler = h_stage_get},
        {.uri = "/stage/part", .method = HTTP_PUT, .handler = h_stage_part},
        {.uri = "/stage/check", .method = HTTP_POST, .handler = h_stage_check},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uris[i]));
    }
}
