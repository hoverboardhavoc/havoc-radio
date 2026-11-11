#include "store.h"

#include <cstddef>
#include <cstring>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "radio.h"

namespace store {

namespace {

const char* TAG = "store";
const char* kNamespace = "radio";
// Bump when radio::State changes layout; a stored blob of another version or size is ignored, except versions 1
// and 2.
constexpr uint32_t kVersion = 3;

struct Blob {
    uint32_t version;
    radio::State state;
};

// Version 2 had the side button's action, one byte, between the light level and the language. The language byte
// therefore sat where version 3 has padding, and every field after it is where it was.
constexpr size_t kV2Lang = offsetof(radio::State, lang) + 1;
static_assert(kV2Lang + 1 <= offsetof(radio::State, bandLast), "the fields after the language moved");
// Version 1 is version 2 without the time sync setting and the NTP server at the end: its settings and presets
// are kept and the new fields take their defaults.
constexpr size_t kV1Size = offsetof(Blob, state) + offsetof(radio::State, timeSync);

bool s_dirty = false;
uint32_t s_lastWrite = 0;

void writeState() {
    Blob b;
    b.version = kVersion;
    radio::snapshot(b.state);
    nvs_handle h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, "state", &b, sizeof b) == ESP_OK) nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "state saved");
}

}  // namespace

void init() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "nvs init failed: %d", err);
}

bool loadState(radio::State& s) {
    nvs_handle h;
    if (nvs_open(kNamespace, NVS_READONLY, &h) != ESP_OK) return false;
    Blob b;
    size_t n = sizeof b;
    const bool read = nvs_get_blob(h, "state", &b, &n) == ESP_OK;
    nvs_close(h);
    radio::State d;
    radio::defaults(d);
    const bool v1 = read && n == kV1Size && b.version == 1, v2 = read && n == sizeof b && b.version == 2;
    if (v1) {
        b.state.timeSync = d.timeSync;
        std::strcpy(b.state.ntpServer, d.ntpServer);
    } else if (!v2 && (!read || n != sizeof b || b.version != kVersion)) {
        return false;
    }
    if (v1 || v2) std::memcpy(&b.state.lang, reinterpret_cast<const uint8_t*>(&b.state) + kV2Lang, 1);
    s = b.state;
    radio::repair(s);
    return true;
}

void touch() { s_dirty = true; }

void flush() {
    if (!s_dirty) return;
    s_dirty = false;
    writeState();
}

void poll(uint32_t now) {
    if (!s_dirty || now - s_lastWrite < kMinWriteIntervalMs) return;
    s_dirty = false;
    s_lastWrite = now;
    writeState();
}

bool loadWifi(char* ssid, size_t ssidLen, char* pass, size_t passLen) {
    nvs_handle h;
    if (nvs_open(kNamespace, NVS_READONLY, &h) != ESP_OK) return false;
    const bool ok = nvs_get_str(h, "ssid", ssid, &ssidLen) == ESP_OK && nvs_get_str(h, "pass", pass, &passLen) == ESP_OK;
    nvs_close(h);
    return ok && ssid[0];
}

void saveWifi(const char* ssid, const char* pass) {
    nvs_handle h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    nvs_commit(h);
    nvs_close(h);
}

}  // namespace store
