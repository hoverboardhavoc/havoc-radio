#include "input_logic.h"

#include <cstring>

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

namespace {

bool blank(char c) { return c == ' ' || c == '\t' || c == '\r'; }

bool is(const char* w, size_t n, const char* name) { return std::strlen(name) == n && !std::memcmp(w, name, n); }

// A decimal integer with an optional sign, the whole word; false past six digits.
bool number(const char* w, size_t n, long* v) {
    size_t i = w[0] == '-' || w[0] == '+' ? 1 : 0;
    if (i == n || n - i > 6) return false;
    long x = 0;
    for (; i < n; i++) {
        if (w[i] < '0' || w[i] > '9') return false;
        x = x * 10 + (w[i] - '0');
    }
    *v = w[0] == '-' ? -x : x;
    return true;
}

struct Op {
    const char* name;
    ui::EventType type;
};
constexpr Op kOps[] = {{"press", ui::EventType::Press},
                       {"long", ui::EventType::LongPress},
                       {"side", ui::EventType::Side},
                       {"sidehold", ui::EventType::SideHold}};

}  // namespace

int parseScript(const char* text, size_t len, Step* out, int* badLine) {
    *badLine = 0;
    if (len > kScriptBytes) return -1;
    const char* const end = text + len;
    int n = 0, line = 0;
    for (const char* p = text; p < end;) {
        const char* eol = static_cast<const char*>(std::memchr(p, '\n', end - p));
        if (!eol) eol = end;
        const char* hash = static_cast<const char*>(std::memchr(p, '#', eol - p));
        const char* const stop = hash ? hash : eol;
        if (++line > kScriptLines) {
            *badLine = line;
            return -1;
        }
        // The words before the comment: at most two are used, a third makes the line bad.
        const char* w[3];
        size_t wl[3];
        int words = 0;
        for (const char* q = p; q < stop && words < 3;) {
            while (q < stop && blank(*q)) q++;
            if (q == stop) break;
            w[words] = q;
            while (q < stop && !blank(*q)) q++;
            wl[words] = q - w[words];
            words++;
        }
        p = eol < end ? eol + 1 : end;
        if (!words) continue;
        Step st{false, ui::EventType::Turn, 0};
        long v = 0;
        bool ok = false;
        if (is(w[0], wl[0], "turn")) {
            ok = words == 2 && number(w[1], wl[1], &v) && v && v >= -kTurnMax && v <= kTurnMax;
        } else if (is(w[0], wl[0], "wait")) {
            st.wait = true;
            ok = words == 2 && number(w[1], wl[1], &v) && v >= 0 && v <= kWaitMaxMs;
        } else {
            for (const Op& op : kOps)
                if (words == 1 && is(w[0], wl[0], op.name)) {
                    st.type = op.type;
                    ok = true;
                }
        }
        if (!ok) {
            *badLine = line;
            return -1;
        }
        st.n = static_cast<int16_t>(v);
        out[n++] = st;
    }
    return n;
}

bool Injector::load(const Step* steps, int n) {
    if (busy()) return false;
    for (int i = 0; i < n; i++) steps_[i] = steps[i];
    n_ = n;
    next_ = 0;
    pausing_ = false;
    return true;
}

bool Injector::take(uint32_t now, ui::View view, EventQueue& out) {
    if (!busy()) return false;
    const Step& st = steps_[next_];
    if (st.wait) {
        if (!pausing_) {
            pausing_ = true;
            pauseAt_ = now;
        }
        if (now - pauseAt_ < static_cast<uint32_t>(st.n)) return false;
        pausing_ = false;
    } else {
        if (out.n >= EventQueue::kMax) return false;
        if (view != ui::View::Confirm) out.push(st.type, st.n);
    }
    return ++next_ == n_;
}

}  // namespace input
