#include "self_update.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "flash_job.h"
#include "stage.h"
#include "wifi.h"

static const char *TAG = "self-update";

#define PROJECT_NAME "installer"
// The start of every app image: the image header, the first segment's header
// and the app description at the start of that segment.
#define HEAD_LEN (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))
#define BUF_SIZE 4096
#define RESTART_DELAY_MS 1000
// A pending firmware is marked valid once the HTTP server has answered every
// check for CONFIRM_MS.
#define CONFIRM_MS 30000
#define CHECK_MS 5000

static volatile bool s_running;
static volatile uint32_t s_written;
static uint32_t s_total;

bool self_update_running(void) { return s_running; }

static bool pending_verify(void)
{
    esp_ota_img_states_t state;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

cJSON *self_update_json(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    char built[40];
    snprintf(built, sizeof(built), "%s %s", d->date, d->time);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "version", d->version);
    cJSON_AddStringToObject(o, "built", built);
    cJSON_AddStringToObject(o, "slot", esp_ota_get_running_partition()->label);
    cJSON_AddBoolToObject(o, "confirmed", !pending_verify());
    if (s_running) {
        cJSON *u = cJSON_AddObjectToObject(o, "update");
        cJSON_AddNumberToObject(u, "written", s_written);
        cJSON_AddNumberToObject(u, "total", s_total);
    } else {
        cJSON_AddNullToObject(o, "update");
    }
    return o;
}

// ---- POST /api/installer/update --------------------------------------------------

static esp_err_t reply(httpd_req_t *req, const char *status, cJSON *root)
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

static esp_err_t refuse(httpd_req_t *req, const char *status, const char *why)
{
    ESP_LOGW(TAG, "refused: %s", why);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", why);
    return reply(req, status, root);
}

static bool recv_exact(httpd_req_t *req, uint8_t *buf, size_t len)
{
    for (size_t got = 0; got < len;) {
        int n = httpd_req_recv(req, (char *)buf + got, len - got);
        if (n <= 0) {
            return false;
        }
        got += n;
    }
    return true;
}

// Read and drop the rest of the body, so the client gets the reply after
// sending all of it instead of a reset connection.
static bool drain(httpd_req_t *req, uint8_t *buf, size_t left)
{
    for (size_t n; left > 0; left -= n) {
        n = MIN(left, BUF_SIZE);
        if (!recv_exact(req, buf, n)) {
            return false;
        }
    }
    return true;
}

// Why the start of an image is not an installer app image for this chip, or
// NULL. why has room for the reason.
static const char *head_problem(const uint8_t *head, char *why, size_t len)
{
    esp_image_header_t h;
    esp_app_desc_t d;
    memcpy(&h, head, sizeof(h));
    memcpy(&d, head + sizeof(h) + sizeof(esp_image_segment_header_t), sizeof(d));
    if (h.magic != ESP_IMAGE_HEADER_MAGIC || d.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        snprintf(why, len, "not an ESP-IDF app image (send installer/build/installer.bin)");
    } else if (h.chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        snprintf(why, len, "an image for another chip (chip id %u, this is %u)", h.chip_id,
                 CONFIG_IDF_FIRMWARE_CHIP_ID);
    } else if (strncmp(d.project_name, PROJECT_NAME, sizeof(d.project_name)) != 0) {
        snprintf(why, len, "not an installer image (project \"%.32s\")", d.project_name);
    } else {
        return NULL;
    }
    return why;
}

static const char *ota_reason(esp_err_t err)
{
    return err == ESP_ERR_OTA_VALIDATE_FAILED ? "the image does not pass its check: truncated or corrupt"
                                              : esp_err_to_name(err);
}

// Receives the body into the slot and answers. Returns true when the slot is
// the new boot slot.
static bool write_slot(httpd_req_t *req, const esp_partition_t *slot, uint8_t *buf)
{
    size_t left = req->content_len;
    char why[96];
    if (!recv_exact(req, buf, HEAD_LEN)) {
        ESP_LOGW(TAG, "connection lost before the image header");
        return false;
    }
    left -= HEAD_LEN;
    if (head_problem(buf, why, sizeof(why)) != NULL) {
        if (drain(req, buf, left)) {
            refuse(req, "400 Bad Request", why);
        }
        return false;
    }
    char version[sizeof(((esp_app_desc_t *)0)->version)];
    memcpy(version, ((const esp_app_desc_t *)(buf + HEAD_LEN - sizeof(esp_app_desc_t)))->version, sizeof(version));
    version[sizeof(version) - 1] = '\0';
    ESP_LOGI(TAG, "writing installer %s, %u bytes, to %s", version, (unsigned)req->content_len, slot->label);

    esp_ota_handle_t ota;
    esp_err_t err = esp_ota_begin(slot, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        if (drain(req, buf, left)) {
            refuse(req, "500 Internal Server Error", ota_reason(err));
        }
        return false;
    }
    size_t n = HEAD_LEN;
    for (;;) {
        if ((err = esp_ota_write(ota, buf, n)) != ESP_OK) {
            esp_ota_abort(ota);
            if (drain(req, buf, left)) {
                refuse(req, "500 Internal Server Error", ota_reason(err));
            }
            return false;
        }
        s_written += n;
        if (left == 0) {
            break;
        }
        n = MIN(left, BUF_SIZE);
        if (!recv_exact(req, buf, n)) {
            esp_ota_abort(ota);
            ESP_LOGW(TAG, "connection lost after %" PRIu32 " of %" PRIu32 " bytes", s_written, s_total);
            return false;
        }
        left -= n;
    }
    if ((err = esp_ota_end(ota)) != ESP_OK) {
        refuse(req, err == ESP_ERR_OTA_VALIDATE_FAILED ? "400 Bad Request" : "500 Internal Server Error",
               ota_reason(err));
        return false;
    }
    if ((err = esp_ota_set_boot_partition(slot)) != ESP_OK) {
        refuse(req, "500 Internal Server Error", ota_reason(err));
        return false;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "version", version);
    cJSON_AddStringToObject(root, "slot", slot->label);
    reply(req, "200 OK", root);
    ESP_LOGI(TAG, "installer %s written to %s and checked; restarting", version, slot->label);
    return true;
}

// Runs one update on its own task, so the HTTP server keeps answering (status,
// and 409 for installs) while the body arrives.
static void update_task(void *arg)
{
    httpd_req_t *req = arg;
    uint8_t *buf = malloc(BUF_SIZE);
    bool ok = false;
    if (buf == NULL) {
        refuse(req, "500 Internal Server Error", "out of memory");
    } else {
        ok = write_slot(req, esp_ota_get_next_update_partition(NULL), buf);
    }
    free(buf);
    httpd_req_async_handler_complete(req);
    if (ok) {
        vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));
        esp_restart();
    }
    s_running = false;
    vTaskDelete(NULL);
}

static esp_err_t h_update(httpd_req_t *req)
{
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    char why[80];
    if (req->content_len < HEAD_LEN) {
        return refuse(req, "400 Bad Request", "need the installer's app image, installer/build/installer.bin, as the body");
    }
    if (req->content_len > slot->size) {
        snprintf(why, sizeof(why), "the image is %u bytes, longer than the %" PRIu32 "-byte slot",
                 (unsigned)req->content_len, slot->size);
        return refuse(req, "413 Content Too Large", why);
    }
    if (pending_verify()) {
        return refuse(req, "409 Conflict", "the running firmware is not confirmed yet, 30 s after its start");
    }
    // httpd runs one handler at a time, so a header install cannot start
    // between this check and s_running. A WiFi install session counts its hold
    // before asking self_update_running(), and this sets s_running before
    // asking stage_held(): one of the two sees the other.
    if (flash_job_running()) {
        return refuse(req, "409 Conflict", "header install running");
    }
    if (s_running) {
        return refuse(req, "409 Conflict", "installer update running");
    }
    s_running = true;
    if (stage_held()) {
        s_running = false;
        return refuse(req, "409 Conflict", "WiFi install running");
    }
    s_written = 0;
    s_total = req->content_len;
    httpd_req_t *copy;
    if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
        s_running = false;
        return refuse(req, "500 Internal Server Error", "out of memory");
    }
    if (xTaskCreate(update_task, "self_update", 6144, copy, 5, NULL) != pdPASS) {
        httpd_req_async_handler_complete(copy);
        s_running = false;
        ESP_LOGE(TAG, "could not start the update task");
        return ESP_FAIL;  // the body is unread: close the connection
    }
    return ESP_OK;
}

void self_update_http_register(httpd_handle_t server)
{
    const httpd_uri_t uri = {.uri = "/api/installer/update", .method = HTTP_POST, .handler = h_update};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri));
}

// ---- Mark valid -------------------------------------------------------------------

// GET /status on the hotspot address answers 200: the hotspot is up and the
// HTTP server serves.
static bool http_answers(void)
{
    static const char request[] = "GET /status HTTP/1.1\r\nHost: " WIFI_HOTSPOT_IP_TEXT "\r\nConnection: close\r\n\r\n";
    static const char ok[] = "HTTP/1.1 200";
    char resp[sizeof(ok)];
    size_t got = 0;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    struct timeval tv = {.tv_sec = 5};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(80)};
    addr.sin_addr.s_addr = inet_addr(WIFI_HOTSPOT_IP_TEXT);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
        send(fd, request, sizeof(request) - 1, 0) == (ssize_t)(sizeof(request) - 1)) {
        while (got < sizeof(ok) - 1) {
            ssize_t n = recv(fd, resp + got, sizeof(ok) - 1 - got, 0);
            if (n <= 0) {
                break;
            }
            got += n;
        }
    }
    close(fd);
    return got == sizeof(ok) - 1 && memcmp(resp, ok, got) == 0;
}

static void confirm_task(void *arg)
{
    (void)arg;
    int64_t since = -1;  // the first answer of the current run of answers
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CHECK_MS));
        int64_t now = esp_timer_get_time();
        if (!http_answers()) {
            since = -1;
            continue;
        }
        if (since < 0) {
            since = now;
        }
        if (now - since >= CONFIRM_MS * 1000LL) {
            break;
        }
    }
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "the HTTP server answered for %d s: firmware marked valid", CONFIRM_MS / 1000);
    } else {
        ESP_LOGE(TAG, "marking the firmware valid: %s", esp_err_to_name(err));
    }
    vTaskDelete(NULL);
}

void self_update_start(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    bool pending = pending_verify();
    ESP_LOGI(TAG, "installer %s, built %s %s, running from %s%s", d->version, d->date, d->time,
             esp_ota_get_running_partition()->label,
             pending ? "; pending verification: marked valid once the HTTP server has answered for 30 s" : "");
    if (pending) {
        xTaskCreate(confirm_task, "confirm", 3072, NULL, 3, NULL);
    }
}
