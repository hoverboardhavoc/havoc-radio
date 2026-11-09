#include "wifi.h"

#include <string.h>

#include "freertos/FreeRTOS.h"

#include "dhcpserver/dhcpserver.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs.h"

#include "activity.h"

static const char *TAG = "wifi";

#define AP_SSID "Havoc"
#define AP_CHANNEL 1
#define AP_MAX_STA 4
// The SuperMini's antenna drops stations at full TX power; 8.5 dBm (34 x 0.25 dBm) keeps them up.
#define TX_POWER_QDBM 34
#define RETRY_US (15 * 1000000LL)
#define NVS_NS "wifi"
#define MDNS_HOSTNAME "havoc-radio-installer"
#define HTTP_PORT 80
#define SOCKET_PORT 3333

typedef enum { HOME_OFF, HOME_JOINING, HOME_JOINED } home_state_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_ssid[33];
static char s_pass[65];
static home_state_t s_state;
static char s_ip[16];
static esp_timer_handle_t s_retry;

static void set_state(home_state_t st, const char *ip)
{
    taskENTER_CRITICAL(&s_lock);
    s_state = st;
    strlcpy(s_ip, ip, sizeof(s_ip));
    taskEXIT_CRITICAL(&s_lock);
}

// Every RETRY_US while a home network is set and not joined, start a join. A
// join scans every channel and takes the hotspot off its channel for a moment,
// so none starts while a radio downloads.
static void retry(void *arg)
{
    (void)arg;
    if (s_ssid[0] == '\0' || s_state == HOME_JOINED || activity_session_open()) {
        return;
    }
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "home network %s: connect: %s", s_ssid, esp_err_to_name(err));
    }
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    switch (id) {
    case WIFI_EVENT_AP_STACONNECTED: {
        wifi_event_ap_staconnected_t *e = data;
        ESP_LOGI(TAG, "station joined: " MACSTR, MAC2STR(e->mac));
        activity_join(e->mac);
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        wifi_event_ap_stadisconnected_t *e = data;
        ESP_LOGI(TAG, "station left: " MACSTR " (reason %d)", MAC2STR(e->mac), e->reason);
        activity_leave(e->mac);
        break;
    }
    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *e = data;
        if (s_ssid[0] == '\0') {
            set_state(HOME_OFF, "");
            break;
        }
        if (s_state == HOME_JOINED) {
            ESP_LOGW(TAG, "home network %s lost (reason %d)", s_ssid, e->reason);
        } else {
            ESP_LOGW(TAG, "home network %s: join failed (reason %d), retrying", s_ssid, e->reason);
        }
        set_state(HOME_JOINING, "");
        break;
    }
    default:
        break;
    }
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == IP_EVENT_AP_STAIPASSIGNED) {
        ip_event_ap_staipassigned_t *e = data;
        uint8_t ip[4];
        memcpy(ip, &e->ip.addr, 4);
        ESP_LOGI(TAG, "station " MACSTR " got " IPSTR, MAC2STR(e->mac), IP2STR(&e->ip));
        activity_ip(e->mac, ip);
    } else if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&e->ip_info.ip));
        set_state(HOME_JOINED, ip);
        ESP_LOGI(TAG, "home network %s joined, address %s, %s.local", s_ssid, ip, MDNS_HOSTNAME);
    }
}

static void load_home(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t n = sizeof(s_ssid);
    if (nvs_get_str(h, "ssid", s_ssid, &n) != ESP_OK) {
        s_ssid[0] = '\0';
    }
    n = sizeof(s_pass);
    if (nvs_get_str(h, "pass", s_pass, &n) != ESP_OK) {
        s_pass[0] = '\0';
    }
    nvs_close(h);
}

static void sta_config(void)
{
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, s_ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, s_pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = s_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
}

static void mdns_start(void)
{
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(MDNS_HOSTNAME));
    ESP_ERROR_CHECK(mdns_instance_name_set("Havoc Radio Installer"));
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", HTTP_PORT, NULL, 0));
    // The raw UART bridge has no standard service type.
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_esp-uart", "_tcp", SOCKET_PORT, NULL, 0));
}

void wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    // 192.168.4.1/24, and the DHCP server offers 192.168.4.1 as DNS server.
    esp_netif_ip_info_t ip = {0};
    ip.ip.addr = ESP_IP4TOADDR(192, 168, 4, 1);
    ip.gw.addr = ESP_IP4TOADDR(192, 168, 4, 1);
    ip.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);
    esp_netif_dhcps_stop(ap);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap, &ip));
    uint8_t offer = OFFER_DNS;
    ESP_ERROR_CHECK(esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof offer));
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ip.ip.addr;
    ESP_ERROR_CHECK(esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap));

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip, NULL, NULL));
    const esp_timer_create_args_t targs = {.callback = retry, .name = "wifi_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_retry, RETRY_US));

    load_home();
    wifi_config_t wc = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = sizeof AP_SSID - 1,
            .channel = AP_CHANNEL,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = AP_MAX_STA,
            .pmf_cfg = {.required = false},
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(s_ssid[0] ? WIFI_MODE_APSTA : WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    if (s_ssid[0]) {
        sta_config();
        set_state(HOME_JOINING, "");
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    if (s_ssid[0]) {
        esp_wifi_connect();
    }
    // Must follow esp_wifi_start().
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(TX_POWER_QDBM));
    // Modem sleep on the station side delays answers by up to 700 ms and breaks uploads.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "hotspot %s on channel %d, %s; home network %s", AP_SSID, AP_CHANNEL, WIFI_HOTSPOT_IP_TEXT,
             s_ssid[0] ? s_ssid : "not set");
    mdns_start();
}

esp_err_t wifi_set_home(const char *ssid, const char *password)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (ssid[0]) {
        err = nvs_set_str(h, "ssid", ssid);
        if (err == ESP_OK) {
            err = nvs_set_str(h, "pass", password);
        }
    } else {
        nvs_erase_all(h);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        return err;
    }

    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_pass, password, sizeof(s_pass));
    if (!ssid[0]) {
        ESP_LOGI(TAG, "home network forgotten");
        set_state(HOME_OFF, "");
        esp_wifi_disconnect();
        return esp_wifi_set_mode(WIFI_MODE_AP);
    }
    ESP_LOGI(TAG, "home network set to %s, joining", ssid);
    set_state(HOME_JOINING, "");
    // Connect here, after the station has its configuration. (Connecting from the
    // STA_START event that the switch to AP+STA posts can run first, in the
    // higher-priority event task, fail with no event and leave the state at
    // joining.) The periodic retry covers a connect that fails.
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode != WIFI_MODE_APSTA) {
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) {
            return err;
        }
    } else {
        esp_wifi_disconnect();
    }
    sta_config();
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "home network %s: connect: %s", ssid, esp_err_to_name(err));
    }
    return ESP_OK;
}

cJSON *wifi_home_json(void)
{
    static const char *const names[] = {"off", "joining", "joined"};
    cJSON *o = cJSON_CreateObject();
    taskENTER_CRITICAL(&s_lock);
    home_state_t st = s_state;
    char ip[16];
    strlcpy(ip, s_ip, sizeof(ip));
    taskEXIT_CRITICAL(&s_lock);
    cJSON_AddStringToObject(o, "ssid", s_ssid);
    cJSON_AddStringToObject(o, "state", names[st]);
    cJSON_AddStringToObject(o, "ip", ip);
    return o;
}
