#include "input_logic.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"  // the decoder runs in the encoder interrupt: code in IRAM, table in DRAM
#else
#define IRAM_ATTR
#define DRAM_ATTR
#endif

namespace input {

namespace {
// Gray-code transition table indexed by (previous << 2) | current: +1 clockwise (B changes before A),
// -1 anticlockwise (A before B), 0 none/invalid. The stock firmware's AiEsp32RotaryEncoder on the same pins
// counts up for B before A and turns the right way.
DRAM_ATTR int8_t kStep[16] = {0, 1, -1, 0, -1, 0, 0, 1, 1, 0, 0, -1, 0, -1, 1, 0};
}  // namespace

int IRAM_ATTR QuadDecoder::feed(uint8_t ab) {
    ab &= 3;
    acc_ = static_cast<int8_t>(acc_ + kStep[(prev_ << 2) | ab]);
    prev_ = ab;
    if (ab != 3) return 0;
    const int a = acc_;
    acc_ = 0;
    return a >= 2 ? 1 : a <= -2 ? -1 : 0;
}

bool Debouncer::update(bool raw, uint32_t now, bool* changed) {
    *changed = false;
    if (raw != last_) {
        last_ = raw;
        since_ = now;
    } else if (raw != stable_ && now - since_ >= kDebounceMs) {
        stable_ = raw;
        *changed = true;
    }
    return stable_;
}

void Buttons::step(uint32_t now, bool knob, bool side, int detents, EventQueue& out) {
    bool changed;
    const bool k = knob_.update(knob, now, &changed);
    if (changed && k) {
        down_ = true;
        turned_ = false;
        longFired_ = false;
        downAt_ = now;
    }
    if (detents) {
        if (down_) {
            turned_ = true;
            out.push(ui::EventType::HoldTurn, detents);
        } else {
            out.push(ui::EventType::Turn, detents);
        }
    }
    if (down_ && !turned_ && !longFired_ && now - downAt_ >= kLongPressMs) {
        longFired_ = true;
        out.push(ui::EventType::LongPress);
    }
    if (changed && !k) {
        if (down_ && !longFired_ && !turned_) out.push(ui::EventType::Press);
        down_ = false;
    }
    const bool sd = side_.update(side, now, &changed);
    if (changed && sd) {
        sideDown_ = true;
        sideHeld_ = false;
        sideAt_ = now;
    }
    if (sideDown_ && !sideHeld_ && now - sideAt_ >= kSideHoldMs) {
        sideHeld_ = true;
        out.push(ui::EventType::SideHold);
    }
    if (changed && !sd) {
        if (sideDown_ && !sideHeld_) out.push(ui::EventType::Side);
        sideDown_ = false;
    }
}

}  // namespace input
