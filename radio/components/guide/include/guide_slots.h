// The guide partition's two slots (pure, host-tested with a flash fake). A new table is written to the slot not in
// use and checked there; only when it passes is the first sector of the old slot erased, so the new slot becomes the
// one read. A failed or cut-off update leaves the old table in use. At boot both slots are checked: one valid slot
// is used; with two, the one with the later EiBi date, and the other's first sector is erased.
#pragma once
#include <cstdint>

#include "guide_core.h"

namespace guide {

constexpr uint32_t kSector = 4096;
constexpr uint32_t kPartitionSize = 2 * kSlotSize;

// The partition's flash, by offset within the partition.
class Flash {
public:
    virtual bool read(uint32_t off, void* buf, uint32_t len) = 0;
    virtual bool erase(uint32_t off) = 0;  // the 4 KB sector at off
    virtual bool write(uint32_t off, const void* buf, uint32_t len) = 0;

protected:
    ~Flash() = default;
};

// Where a new table goes: the free slot (guide::Update on the radio).
class Sink {
public:
    virtual Err begin(uint32_t length) = 0;
    virtual Err add(const void* data, uint32_t n) = 0;
    virtual Err finish(const uint8_t* sha256) = 0;

protected:
    ~Sink() = default;
};

class SlotSource : public Source {
public:
    SlotSource(Flash& f, uint32_t base) : f_(f), base_(base) {}
    bool read(uint32_t off, void* buf, uint32_t len) const override {
        return off <= kSlotSize && len <= kSlotSize - off && f_.read(base_ + off, buf, len);
    }

private:
    Flash& f_;
    uint32_t base_;
};

class Slots {
public:
    explicit Slots(Flash& f) : f_(f) {}
    // Checks both slots and picks the one to read.
    void boot();
    int active() const { return active_; }  // -1 without a valid table
    const Info& info() const { return info_; }
    SlotSource source() const { return SlotSource(f_, base(active_ < 0 ? 0 : active_)); }

    // Writes a new table into the free slot in 4 KB sectors, through a caller's sector buffer, with the SHA-256
    // computed on the way. finish() checks it; commit() then makes it the table read.
    class Writer {
    public:
        Writer(Slots& s, uint8_t* sector) : s_(s), buf_(sector) {}
        Err begin(uint32_t length);
        Err add(const void* data, uint32_t n);
        // The length must have arrived; with `sha256` given it must match; then every table check.
        Err finish(const uint8_t* sha256);
        uint32_t received() const { return got_; }
        const uint8_t* sha256() const { return sha_; }
        int slot() const { return slot_; }

    private:
        Err flush();
        Slots& s_;
        uint8_t* buf_;
        int slot_ = -1;
        uint32_t length_ = 0, got_ = 0, fill_ = 0, at_ = 0;
        Sha256 hash_;
        uint8_t sha_[32] = {};
        Info info_;
        bool ready_ = false;
        friend class Slots;
    };
    // Erases the first sector of the old slot and switches to the new one. Lookups pause only for this.
    bool commit(Writer& w);

private:
    static uint32_t base(int slot) { return static_cast<uint32_t>(slot) * kSlotSize; }
    Flash& f_;
    int active_ = -1;
    Info info_;
};

}  // namespace guide
