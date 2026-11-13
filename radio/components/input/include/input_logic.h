// Pure input logic: quadrature decoding, the knob's press / long press / hold + turn state machine, the side
// button's short press and hold, and input scripts (POST /api/input).
// The GPIO side (interrupts and polling) is in input.cpp; this part is host-tested.
#pragma once
#include <cstddef>
#include <cstdint>

#include "ui.h"

namespace input {

// Full-step quadrature decoder. Feed the 2-bit state (A in bit 1, B in bit 0, 1 = high) on every edge.
// The encoder rests at A = B = high between detents (switches open, pull-ups); one detent is a full cycle
// of four transitions. A detent counts when the state returns to rest with at least two valid transitions
// in one direction, so a missed or bounced edge does not lose or duplicate a count.
class QuadDecoder {
public:
    int feed(uint8_t ab);  // returns +1 (clockwise, B leads), -1 (anticlockwise, A leads) or 0

private:
    uint8_t prev_ = 3;
    int8_t acc_ = 0;
};

struct EventQueue {
    static constexpr int kMax = 8;
    ui::Event ev[kMax];
    int n = 0;
    void push(ui::EventType t, int d = 0) {
        if (n < kMax) ev[n++] = {t, d};
    }
};

constexpr uint32_t kDebounceMs = 20;
constexpr uint32_t kLongPressMs = 600;
constexpr uint32_t kSideHoldMs = 1000;

class Debouncer {
public:
    // Returns the debounced level (true = pressed); `changed` is set on an accepted change.
    bool update(bool raw, uint32_t now, bool* changed);

private:
    bool stable_ = false;
    bool last_ = false;
    uint32_t since_ = 0;
};

// Called every few milliseconds with the raw button levels (true = pressed) and the detents counted since
// the last call. The side button reports Side on a release before kSideHoldMs, or SideHold at kSideHoldMs while
// still held; the release after a hold reports nothing.
class Buttons {
public:
    void step(uint32_t now, bool knob, bool side, int detents, EventQueue& out);

private:
    Debouncer knob_, side_;
    bool down_ = false, turned_ = false, longFired_ = false;
    uint32_t downAt_ = 0;
    bool sideDown_ = false, sideHeld_ = false;
    uint32_t sideAt_ = 0;
};

// One step of an input script: an event, or a pause of `n` ms.
struct Step {
    bool wait;
    ui::EventType type;
    int16_t n;  // detents for Turn, ms for a pause
};

constexpr int kScriptLines = 64;
constexpr size_t kScriptBytes = 1024;
constexpr int kWaitMaxMs = 10000;
constexpr int kTurnMax = 1000;  // detents in one `turn`

// Parses a script: lines of `turn N` (N detents, negative anticlockwise, not 0), `press`, `long`, `side`,
// `sidehold` and `wait MS` (0 to kWaitMaxMs); `#` starts a comment. Fills `out` (room for kScriptLines steps) and
// returns the number of steps, or -1 with *badLine set to the first line it does not take (from 1; line
// kScriptLines + 1 when there are more lines), or to 0 for a script over kScriptBytes.
int parseScript(const char* text, size_t len, Step* out, int* badLine);

// The steps of a script, taken by the UI loop one per pass after the hardware's events; a pause holds the rest
// back until its time is up. On the firmware confirm screen an event is taken but dropped: only the real knob
// confirms an install.
class Injector {
public:
    // False while an earlier script is still being taken.
    bool load(const Step* steps, int n);
    bool busy() const { return next_ < n_; }
    // Steps taken, pauses counting once they are over.
    int taken() const { return next_; }
    // Takes the next step when it is due and `out` has room. Returns true when this call finishes the script.
    bool take(uint32_t now, ui::View view, EventQueue& out);

private:
    Step steps_[kScriptLines];
    int n_ = 0, next_ = 0;
    bool pausing_ = false;
    uint32_t pauseAt_ = 0;
};

}  // namespace input
