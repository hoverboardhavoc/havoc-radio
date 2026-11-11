// The frequency guide table (docs/guide-format.md): its checks, the lookup for a frequency and the on-air rule.
// Pure: it reads the table through a Source, so the host tests run it on tables made by the converter. Every byte
// of a table is untrusted: check() runs on the whole table before any lookup uses it, and lookups index only within
// the bounds it checked.
#pragma once
#include <cstddef>
#include <cstdint>

namespace guide {

constexpr uint32_t kHeaderLen = 48;
constexpr uint32_t kEntryLen = 14;
constexpr uint32_t kSlotSize = 0x78000;  // a table is at most one slot of the partition
constexpr uint16_t kFormat = 1;
constexpr int kStrMax = 64;              // bytes of UTF-8, without the NUL
constexpr int kMaxHits = 16;             // entries a lookup returns at most
constexpr uint8_t kIrregular = 0x80;
constexpr uint8_t kEveryDay = 0x7F;
constexpr int32_t kNearHz = 1000;        // entries within 1 kHz of the tuned frequency

enum class Err : uint8_t {
    Ok, Read, Short, Magic, Format, HeaderLen, Length, Crc, Layout, StringTable, String, Entry, Index, Order,
    // Uploads and downloads.
    TooLong, Cut, Hash, Write,
};
const char* errText(Err e);

// Reads `len` bytes at `off` of a table; false when the read fails.
class Source {
public:
    virtual bool read(uint32_t off, void* buf, uint32_t len) const = 0;

protected:
    ~Source() = default;
};

// A table in memory (host tests, and uploads checked before they are written).
class MemSource : public Source {
public:
    MemSource(const void* data, uint32_t size) : p_(static_cast<const uint8_t*>(data)), size_(size) {}
    bool read(uint32_t off, void* buf, uint32_t len) const override;

private:
    const uint8_t* p_;
    uint32_t size_;
};

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t n);
    void finish(uint8_t out[32]);

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    uint64_t len_ = 0;
    uint32_t fill_ = 0;
};

uint32_t crc32(uint32_t crc, const void* data, size_t n);  // IEEE, as zlib; start with 0

// A checked table.
struct Info {
    bool valid = false;
    char season[5] = {};
    uint32_t entries = 0;
    uint32_t length = 0;
    uint32_t modified = 0;    // EiBi's Last-Modified, Unix time
    uint32_t strings = 0;     // string count
    uint32_t stringsOff = 0;  // offset of the string table
    uint8_t sha256[32] = {};
};

// Runs every check on a table of at most `size` bytes. On Ok, `out` describes it and is valid.
Err check(const Source& src, uint32_t size, Info& out);

struct Entry {
    uint16_t khz;
    uint8_t tenths;
    uint8_t days;   // bit 0 Monday .. bit 6 Sunday; kIrregular
    uint16_t start;  // minutes, 0..1439
    uint16_t end;    // minutes, 1..1440; before the start: runs past midnight
    uint16_t name, lang, target;
    int32_t hz() const { return (khz * 10 + tenths) * 100; }
};

bool readEntry(const Source& src, uint32_t i, Entry& out);
// A string of a checked table, NUL-terminated, at most kStrMax bytes plus the NUL.
bool readString(const Source& src, const Info& info, uint16_t idx, char out[kStrMax + 1]);

// On air at `utcMs` (ms since 1970, negative when the clock is not set): the window holds the current UTC minute,
// wrapping past midnight, and the day bit of the day the window started is set. Irregular entries never are.
bool onAir(const Entry& e, int64_t utcMs);
// EiBi's form of the day bits: ranges of three or more days (`Mo-Fr`, wrapping `We-Mo`) and single days joined with
// commas (`Tu,Th`). Every day and irregular are left to the caller (`daily`, `irr`).
void daysText(uint8_t days, char out[24]);

// The lookup for one frequency: up to kMaxHits entries within 1 kHz. On-air entries come first, in table order;
// the rest follow in start time order from the current UTC minute (from 0000 without the clock).
struct Hit {
    Entry e;
    bool onAir;
    const char* name;  // in Hits::arena
    const char* lang;
    const char* target;
};
struct Hits {
    static constexpr size_t kArena = 1024;
    int n = 0;
    Hit hit[kMaxHits];
    char arena[kArena];
    size_t used = 0;
};
void lookup(const Source& src, const Info& info, int32_t hz, int64_t utcMs, Hits& out);

}  // namespace guide
