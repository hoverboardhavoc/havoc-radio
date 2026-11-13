// Fakes for the guide tests: the partition's NOR flash.
#pragma once
#include <cstring>
#include <string>
#include <vector>

#include "guide_slots.h"

// NOR flash: erase sets a sector to 0xFF, a write can only clear bits.
struct FakeFlash : guide::Flash {
    std::vector<uint8_t> m = std::vector<uint8_t>(guide::kPartitionSize, 0xFF);
    int erases = 0, writes = 0;
    bool failWrites = false;
    bool read(uint32_t off, void* buf, uint32_t len) override {
        if (off > m.size() || len > m.size() - off) return false;
        std::memcpy(buf, m.data() + off, len);
        return true;
    }
    bool erase(uint32_t off) override {
        if (off % guide::kSector || off >= m.size()) return false;
        std::memset(m.data() + off, 0xFF, guide::kSector);
        erases++;
        return true;
    }
    bool write(uint32_t off, const void* buf, uint32_t len) override {
        if (failWrites || off > m.size() || len > m.size() - off) return false;
        for (uint32_t i = 0; i < len; i++) m[off + i] &= static_cast<const uint8_t*>(buf)[i];
        writes++;
        return true;
    }
    void place(int slot, const std::string& t) {
        std::memcpy(m.data() + slot * guide::kSlotSize, t.data(), t.size());
    }
};
