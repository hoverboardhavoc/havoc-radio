#include "guide_slots.h"

#include <cstring>

namespace guide {

void Slots::boot() {
    Info a, b;
    const bool okA = check(SlotSource(f_, base(0)), kSlotSize, a) == Err::Ok;
    const bool okB = check(SlotSource(f_, base(1)), kSlotSize, b) == Err::Ok;
    active_ = -1;
    info_ = Info();
    if (okA && okB) {
        // Power was lost between a new table's check and the erase of the old one: the later EiBi date wins.
        const int keep = b.modified > a.modified ? 1 : 0;
        f_.erase(base(1 - keep));
        active_ = keep;
        info_ = keep ? b : a;
    } else if (okA || okB) {
        active_ = okA ? 0 : 1;
        info_ = okA ? a : b;
    }
}

Err Slots::Writer::begin(uint32_t length) {
    ready_ = false;
    got_ = fill_ = at_ = 0;
    hash_ = Sha256();
    if (length < kHeaderLen) return Err::Short;
    if (length > kSlotSize) return Err::TooLong;
    length_ = length;
    slot_ = s_.active_ == 0 ? 1 : 0;
    return Err::Ok;
}

Err Slots::Writer::flush() {
    if (!fill_) return Err::Ok;
    const uint32_t off = base(slot_) + at_;
    // Whole words: the tail of the last sector is padded with 0xFF.
    while (fill_ % 4) buf_[fill_++] = 0xFF;
    if (!s_.f_.erase(off) || !s_.f_.write(off, buf_, fill_)) return Err::Write;
    at_ += kSector;
    fill_ = 0;
    return Err::Ok;
}

Err Slots::Writer::add(const void* data, uint32_t n) {
    if (slot_ < 0) return Err::Write;
    if (n > length_ - got_) return Err::TooLong;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    hash_.update(p, n);
    got_ += n;
    while (n) {
        const uint32_t k = kSector - fill_ < n ? kSector - fill_ : n;
        std::memcpy(buf_ + fill_, p, k);
        fill_ += k, p += k, n -= k;
        if (fill_ == kSector) {
            const Err e = flush();
            if (e != Err::Ok) return e;
        }
    }
    return Err::Ok;
}

Err Slots::Writer::finish(const uint8_t* sha256) {
    if (slot_ < 0) return Err::Write;
    if (got_ != length_) return Err::Cut;
    const Err fe = flush();
    if (fe != Err::Ok) return fe;
    hash_.finish(sha_);
    if (sha256 && std::memcmp(sha256, sha_, sizeof sha_)) return Err::Hash;
    const Err e = check(SlotSource(s_.f_, base(slot_)), length_, info_);
    if (e != Err::Ok) return e;
    if (info_.length != length_) return Err::Length;
    ready_ = true;
    return Err::Ok;
}

bool Slots::commit(Writer& w) {
    if (!w.ready_) return false;
    if (active_ >= 0 && active_ != w.slot_) f_.erase(base(active_));
    active_ = w.slot_;
    info_ = w.info_;
    w.ready_ = false;
    return true;
}

}  // namespace guide
