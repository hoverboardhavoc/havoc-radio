// WiFi station with the saved network; an open access point "Havoc-Radio" when none is saved or it cannot be
// joined within 15 s, while it keeps trying to rejoin. mDNS name radio.local. No outside connections.
#pragma once
#include "ui.h"

namespace net {

constexpr int kJoinTimeoutMs = 15000;
constexpr int kRejoinIntervalMs = 30000;
constexpr int kMaxNets = 12;
constexpr const char* kApSsid = "Havoc-Radio";

struct Info {
    bool joined = false;
    bool ap = false;
    char ssid[33] = {};
    char ip[16] = {};
    ui::Network nets[kMaxNets];
    int netCount = 0;
};

void start();
void join(const char* ssid, const char* password);  // saves the credentials
void scan();
void info(Info& out);

}  // namespace net
