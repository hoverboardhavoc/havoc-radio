// The frequency guide on the radio: the `guide` partition's two slots, read with spi_flash_read (nothing maps it),
// checked at boot and after every new table, and the lookup for the tuned frequency, shared by the UI loop and the
// web server under one lock.
#pragma once
#include <cstdint>

#include "guide_core.h"
#include "guide_slots.h"

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
