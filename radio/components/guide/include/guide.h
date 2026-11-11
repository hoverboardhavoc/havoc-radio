// The frequency guide on the radio: the `guide` partition's two slots, read with spi_flash_read (nothing maps it),
// checked at boot and after every new table, and the lookup for the tuned frequency, shared by the UI loop and the
// web server under one lock.
#pragma once
#include <cstdint>

#include "guide_core.h"
#include "guide_slots.h"
#include "scan_core.h"

namespace guide {

// Finds the partition and checks both slots.
void start();
// The loaded table (Info::valid false without one).
Info info();
// The UI loop: the entries for `hz` in AM, LSB or USB (`am`), at `utcMs` (-1 without the clock), looked up again
// when the frequency, the minute or the table changes. Not in AM: no entries.
const Hits& refresh(int32_t hz, bool am, int64_t utcMs);
// Runs fn with the last lookup under the lock (the web server). `am` is false in FM.
void withHits(void (*fn)(const Hits& hits, bool am, void* arg), void* arg);

// The guide scan's frequencies (scan_core.h), read a step at a time under the lock. scanFirst positions the cursor on
// the loaded table and gives its generation; false without a table.
bool scanFirst(scan::Cursor& c, int32_t loKhz, int32_t hiKhz, int64_t utcMs, uint32_t& generation);
// The next frequency; false at the end, or when the table changed since `generation`.
bool scanNext(scan::Cursor& c, uint32_t generation, uint16_t& khz, uint16_t& name);
// String `idx` of the table of `generation` into out (kStrMax + 1 bytes); false, with out empty, when the table
// changed since.
bool stringOf(uint32_t generation, uint16_t idx, char* out);

// A new table written to the free slot in 4 KB sectors (web upload, download mode). Lookups continue on the old
// table and pause only for the switch.
class Update : public Sink {
public:
    Update();
    ~Update();
    Update(const Update&) = delete;
    Update& operator=(const Update&) = delete;
    Err begin(uint32_t length) override;
    Err add(const void* data, uint32_t n) override;
    // Checks the SHA-256 when given and every table check; on Ok the new table is the one read.
    Err finish(const uint8_t* sha256) override;
    uint32_t received() const;

private:
    uint8_t* sector_;
    Slots::Writer* writer_;
};

}  // namespace guide
