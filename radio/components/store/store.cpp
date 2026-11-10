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
// Bump when radio::State changes layout; a stored blob of another version or size is ignored, except version 1.
constexpr uint32_t kVersion = 2;

struct Blob {
    uint32_t version;
    radio::State state;
};

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
    if (read && n == kV1Size && b.version == 1) {
        b.state.timeSync = d.timeSync;
        std::strcpy(b.state.ntpServer, d.ntpServer);
    } else if (!read || n != sizeof b || b.version != kVersion) {
        return false;
    }
    s = b.state;
    // Values out of range in a stored blob fall back to the defaults.
    if (s.presetCount > radio::kMaxPresets) s.presetCount = 0;
    if (s.volume > radio::kVolumeMax) s.volume = d.volume;
    if (s.contrast < radio::kContrastMin || s.contrast > radio::kContrastMax) s.contrast = d.contrast;
    if (s.light > radio::kLightMax) s.light = 0;
    if (static_cast<int>(s.side) > static_cast<int>(radio::Side::Back)) s.side = radio::Side::Mute;
    if (static_cast<int>(s.mode) > static_cast<int>(radio::Mode::USB)) s = d;
    s.hz = radio::clampHz(s.hz, s.mode);
    for (int i = 0; i < radio::kFamilies; i++) {
        int n;
        radio::steps(static_cast<radio::Family>(i), &n);
        if (s.stepIdx[i] >= n) s.stepIdx[i] = d.stepIdx[i];
        radio::bandwidths(static_cast<radio::Family>(i), &n);
        if (s.bwIdx[i] >= n) s.bwIdx[i] = d.bwIdx[i];
    }
    for (int i = 0; i < s.presetCount; i++) s.presets[i].name[radio::kPresetName - 1] = 0;
    s.ntpServer[radio::kNtpServerMax] = 0;
    if (!s.ntpServer[0] || !radio::validNtpServer(s.ntpServer)) std::strcpy(s.ntpServer, d.ntpServer);
    return true;
}

void touch() { s_dirty = true; }

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
