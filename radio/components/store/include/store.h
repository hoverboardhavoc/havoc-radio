// Settings, presets, per-band state and WiFi credentials in NVS. State writes are debounced: at most one
// every 5 s, from the UI loop, never from an interrupt.
#pragma once
#include <cstdint>

#include "radio_model.h"

namespace store {

constexpr uint32_t kMinWriteIntervalMs = 5000;

void init();
bool loadState(radio::State& s);
// Marks the state as changed; poll() writes it once the interval has passed.
void touch();
void poll(uint32_t now);
// Writes a pending state change now (before a restart).
void flush();
bool loadWifi(char* ssid, size_t ssidLen, char* pass, size_t passLen);
void saveWifi(const char* ssid, const char* pass);

// The guide download (Settings > Guide > Update guide): a request for the next boot, and the result it leaves for the
// boot after it (guide::Result as a byte).
void requestGuide();
bool takeGuideRequest();  // cleared first, so a crash during the download cannot repeat it
void setGuideResult(uint8_t result);
uint8_t takeGuideResult();  // 0 when none; cleared

}  // namespace store
