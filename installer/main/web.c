#include "web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "lwip/sockets.h"

#include "activity.h"
#include "flash_job.h"
#include "self_update.h"
#include "server.h"
#include "stage.h"
#include "uart_bridge.h"
#include "wifi.h"

static const char *TAG = "web";

// installer/page/index.html with the wiring diagram, gzipped by tools/make_page.py.
extern const uint8_t page_start[] asm("_binary_page_html_gz_start");
extern const uint8_t page_end[] asm("_binary_page_html_gz_end");

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
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

// The IPv4 address of either end of the request's connection, as text. lwIP
// shows IPv4 peers of the server's IPv6 socket as IPv4-mapped addresses.
static void sock_ip(httpd_req_t *req, bool local, char out[16])
{
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    int fd = httpd_req_to_sockfd(req);
    int r = local ? getsockname(fd, (struct sockaddr *)&ss, &len) : getpeername(fd, (struct sockaddr *)&ss, &len);
    const uint8_t *a = NULL;
    if (r == 0 && ss.ss_family == AF_INET) {
        a = (const uint8_t *)&((struct sockaddr_in *)&ss)->sin_addr.s_addr;
    } else if (r == 0 && ss.ss_family == AF_INET6) {
        a = ((struct sockaddr_in6 *)&ss)->sin6_addr.s6_addr + 12;
    }
    if (a == NULL) {
        out[0] = '\0';
        return;
    }
    snprintf(out, 16, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
}

static esp_err_t h_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)page_start, page_end - page_start);
}

// The OS connectivity checks (/hotspot-detect.html, /generate_204, /gen_204,
// /connecttest.txt, /ncsi.txt, /success.txt) and every other unknown path get a
// redirect to the page, so phones open it on joining the hotspot.
static esp_err_t h_redirect(httpd_req_t *req, httpd_err_code_t code)
{
    (void)code;
    char ip[16];
    char url[32];
    sock_ip(req, true, ip);
    snprintf(url, sizeof(url), "http://%s/", ip[0] ? ip : WIFI_HOTSPOT_IP_TEXT);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", url);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t h_api_status(httpd_req_t *req)
{
    char client[16];
    sock_ip(req, false, client);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "now", platform_now_ms());
    cJSON_AddStringToObject(root, "client", client);
    cJSON_AddItemToObject(root, "image", stage_status_json());
    activity_json(root);
    cJSON_AddItemToObject(root, "job", flash_job_json());
    cJSON_AddNumberToObject(root, "console_clients", bridge_console_clients());
    cJSON_AddBoolToObject(root, "socket_client", bridge_socket_client());
    cJSON_AddNumberToObject(root, "uart_baud", bridge_baud());
    cJSON_AddItemToObject(root, "home", wifi_home_json());
    cJSON_AddItemToObject(root, "installer", self_update_json());
    return send_json(req, "200 OK", root);
}

static esp_err_t wifi_error(httpd_req_t *req, const char *status, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", msg);
    return send_json(req, status, root);
}

static esp_err_t h_api_wifi(httpd_req_t *req)
{
    char body[256];
    if (req->content_len == 0 || req->content_len >= sizeof(body)) {
        return wifi_error(req, "400 Bad Request", "need {\"ssid\": s, \"password\": s}");
    }
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) {
            return ESP_FAIL;
        }
        got += n;
    }
    body[got] = '\0';
    cJSON *root = cJSON_Parse(body);
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(root, "password");
    const char *err = NULL;
    if (!cJSON_IsString(ssid) || (pass != NULL && !cJSON_IsString(pass))) {
        err = "need {\"ssid\": s, \"password\": s}";
    } else if (strlen(ssid->valuestring) > 32) {
        err = "the network name is longer than 32 bytes";
    } else if (pass != NULL && strlen(pass->valuestring) > 64) {
        err = "the password is longer than 64 characters";
    } else if (pass != NULL && pass->valuestring[0] && strlen(pass->valuestring) < 8) {
        err = "a WPA password has at least 8 characters";
    }
    esp_err_t werr = ESP_OK;
    if (err == NULL) {
        werr = wifi_set_home(ssid->valuestring, pass ? pass->valuestring : "");
    }
    cJSON_Delete(root);
    if (err != NULL) {
        return wifi_error(req, "400 Bad Request", err);
    }
    if (werr != ESP_OK) {
        return wifi_error(req, "500 Internal Server Error", esp_err_to_name(werr));
    }
    cJSON *ok = cJSON_CreateObject();
    cJSON_AddBoolToObject(ok, "ok", true);
    return send_json(req, "200 OK", ok);
}

// The bridge's status: home network, serial baud and line levels.
static esp_err_t h_status(httpd_req_t *req)
{
    cJSON *home = wifi_home_json();
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(home, "state");
    const cJSON *ip = cJSON_GetObjectItemCaseSensitive(home, "ip");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "wifi", strcmp(state->valuestring, "joined") == 0 ? "connected" : "disconnected");
    cJSON_AddStringToObject(root, "ip", ip->valuestring[0] ? ip->valuestring : "0.0.0.0");
    cJSON_AddNumberToObject(root, "baud", bridge_baud());
    cJSON_AddStringToObject(root, "io0", bridge_io0_low() ? "low" : "released");
    cJSON_AddStringToObject(root, "en", bridge_en_low() ? "low" : "released");
    cJSON_Delete(home);
    return send_json(req, "200 OK", root);
}

httpd_handle_t web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 20;
    cfg.max_open_sockets = 10;
    // Stage handlers hash and verify flash with 4 KB buffers.
    cfg.stack_size = 8192;
    // A WiFi link stalls for up to 11 s; let a part upload ride that out.
    cfg.recv_wait_timeout = 20;
    cfg.send_wait_timeout = 20;
    // Drop half-open connections from timed-out clients instead of refusing new ones.
    cfg.lru_purge_enable = true;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));

    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_page},
        {.uri = "/api/status", .method = HTTP_GET, .handler = h_api_status},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = h_api_wifi},
        {.uri = "/status", .method = HTTP_GET, .handler = h_status},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uris[i]));
    }
    ESP_ERROR_CHECK(httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, h_redirect));
    ESP_LOGI(TAG, "http on tcp/80, page %u bytes gzipped", (unsigned)(page_end - page_start));
    return server;
}
