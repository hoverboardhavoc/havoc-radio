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
bool loadWifi(char* ssid, size_t ssidLen, char* pass, size_t passLen);
void saveWifi(const char* ssid, const char* pass);

}  // namespace store
