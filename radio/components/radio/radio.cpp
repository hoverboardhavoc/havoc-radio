#include "radio.h"

#include <cstring>

#include "board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "guide.h"
#include "si4732.h"

#if HAVE_SSB_PATCH
extern const uint8_t ssb_patch_start[] asm("_binary_ssb_patch_bin_start");
extern const uint8_t ssb_patch_end[] asm("_binary_ssb_patch_bin_end");
#endif

namespace radio {

namespace {

const char* TAG = "radio";

SemaphoreHandle_t s_lock;
TaskHandle_t s_task;
State s_state;
Live s_live;
rds::Decoder s_rds;
rds::Ring* s_rdsRing;  // allocated by start()
// Requests to the task, under the lock.
int8_t s_seekReq = 0;
bool s_stopReq = false;
bool s_shutdown = false;
// The guide scan, and a new one waiting for the task.
scan::State s_scan;
bool s_scanReq = false;

class Lock {
public:
    Lock() { xSemaphoreTake(s_lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_lock); }
};

const uint8_t* patchData(size_t* n) {
#if HAVE_SSB_PATCH
    *n = static_cast<size_t>(ssb_patch_end - ssb_patch_start);
    return ssb_patch_start;
#else
    *n = 0;
    return nullptr;
#endif
}

si4732::Func funcOf(Mode m) {
    switch (m) {
        case Mode::FM: return si4732::Func::FM;
        case Mode::AM: return si4732::Func::AM;
        default: return si4732::Func::SSB;
    }
}

// Si4732 filter codes in the order of the model's bandwidth lists.
const uint8_t kAmFilter[] = {4, 5, 3, 6, 2, 1, 0};  // 1.0 1.8 2.0 2.5 3.0 4.0 6.0 kHz
const uint8_t kSsbFilter[] = {4, 5, 0, 1, 2, 3};    // 0.5 1.0 1.2 2.2 3.0 4.0 kHz

uint16_t ssbMode(int bw) {
    const uint16_t audio = kSsbFilter[bw];
    // Band-pass sideband filter up to 2.2 kHz, low-pass above; AVC on; DSP AFC off.
    const uint16_t cut = bw >= 4 ? 1 : 0;
    return static_cast<uint16_t>(audio | cut << 4 | 1u << 12 | 1u << 15);
}

// The chip as last programmed.
struct Applied {
    bool up = false;
    si4732::Func func = si4732::Func::AM;
    int32_t hz = -1;
    Mode mode = Mode::AM;
    int bw = -1;
    int volume = -1;
    int muted = -1;
    int agc = -1;
};

void setAmp(const State& s, const Applied& a) { board::ampMute(!a.up || s.muted); }

bool tuneTo(Mode m, int32_t hz, Applied& a) {
    bool ok;
    switch (m) {
        case Mode::FM: ok = si4732::tuneFm(static_cast<uint16_t>(hz / 10000)); break;
        case Mode::AM: {
            const int32_t khz = hz / 1000;
            ok = si4732::tuneAm(static_cast<uint16_t>(khz), amAntennaCap(khz));
            break;
        }
        default: {
            // Whole kHz on the chip, the rest on the BFO.
            const int32_t khz = (hz + 500) / 1000;
            ok = si4732::tuneSsb(static_cast<uint16_t>(khz), m == Mode::USB, amAntennaCap(khz)) &&
                 si4732::setProperty(si4732::prop::kSsbBfo, static_cast<uint16_t>(static_cast<int16_t>(hz - khz * 1000)));
            break;
        }
    }
    a.hz = hz;
    a.mode = m;
    return ok;
}

bool applyTune(const State& s, Applied& a) { return tuneTo(s.mode, s.hz, a); }

bool powerUpFor(const State& s, Applied& a) {
    board::ampMute(true);
    if (a.up) si4732::powerDown();
    a = Applied{};
    size_t n;
    const uint8_t* patch = patchData(&n);
    const si4732::Func f = funcOf(s.mode);
    if (!si4732::powerUp(f, patch, n)) {
        ESP_LOGE(TAG, "power up failed");
        return false;
    }
    a.up = true;
    a.func = f;
    {
        Lock lock;
        s_rds.reset();
    }
    if (f == si4732::Func::FM) {
        si4732::setProperty(si4732::prop::kFmDeemphasis, 1);  // 50 us
        si4732::rdsEnable();
    } else {
        si4732::setProperty(si4732::prop::kAmAvcMaxGain, 36 * 340);  // 36 dB, as the stock firmware's library sets
        if (f == si4732::Func::SSB) si4732::setProperty(si4732::prop::kAmSoftMuteMaxAttenuation, 0);
    }
    return true;
}

void seekBand(const State& s) {
    const bool fm = isFm(s.mode);
    const int b = bandIndex(s.hz, s.mode);
    int32_t lo, hi;  // kHz
    if (b >= 0) {
        lo = kBands[b].lo;
        hi = kBands[b].hi;
    } else if (fm) {
        lo = 64000;
        hi = 108000;
    } else {
        const int32_t k = s.hz / 1000;
        lo = k < 300 ? 150 : k < kSwLoKhz ? 300 : kSwLoKhz;
        hi = k < 300 ? 299 : k < kSwLoKhz ? kSwLoKhz - 1 : 30000;
    }
    const int32_t step = currentStep(s).hz / 1000;
    if (fm) {
        si4732::setProperty(si4732::prop::kFmSeekBottom, static_cast<uint16_t>(lo / 10));
        si4732::setProperty(si4732::prop::kFmSeekTop, static_cast<uint16_t>(hi / 10));
        si4732::setProperty(si4732::prop::kFmSeekSpacing, static_cast<uint16_t>(step / 10));
    } else {
        si4732::setProperty(si4732::prop::kAmSeekBottom, static_cast<uint16_t>(lo));
        si4732::setProperty(si4732::prop::kAmSeekTop, static_cast<uint16_t>(hi));
        si4732::setProperty(si4732::prop::kAmSeekSpacing, static_cast<uint16_t>(step > 0 ? step : 1));
    }
}

// RDS starts again after a tune or a seek: the decoder is reset and the receiver's FIFO emptied.
void rdsRestart() {
    si4732::RdsStatus st;
    si4732::rdsStatus(true, st);
    Lock lock;
    s_rds.reset();
}

// Reads the groups waiting in the receiver's FIFO, one call per group, at most kRdsReads calls.
constexpr int kRdsReads = 8;
void rdsPoll() {
    for (int i = 0; i < kRdsReads; i++) {
        si4732::RdsStatus st;
        if (!si4732::rdsStatus(false, st)) return;
        const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        Lock lock;
        s_rds.setSync(st.sync);
        if (!st.fifoUsed) return;
        const rds::Group g{{st.blk[0], st.blk[1], st.blk[2], st.blk[3]}, st.ble};
        s_rds.group(g);
        s_rdsRing->push(now, g);
    }
}

// The guide scan: the frequencies counted, then each tuned in the current mode and its signal read after kSettleMs,
// with the audio muted. The state's frequency is left alone, so the task tunes back to it afterwards, or to the result
// the UI has chosen. A stop, a cancel or a new scan ends it; a measurement in flight is then dropped.
void runScan(const State& s, Applied& a) {
    uint16_t id;
    int32_t lo, hi;
    int64_t utc;
    {
        Lock lock;
        s_scanReq = false;
        id = s_scan.id;
        lo = s_scan.loKhz;
        hi = s_scan.hiKhz;
        utc = s_scan.utcMs;
    }
    scan::Cursor c;
    uint32_t gen = 0;
    uint16_t total = 0, khz, name;
    if (!isFm(s.mode) && guide::scanFirst(c, lo, hi, utc, gen)) {
        scan::Cursor k = c;
        while (guide::scanNext(k, gen, khz, name)) total++;
    }
    {
        Lock lock;
        if (s_scan.id != id) return;
        s_scan.total = total;
        s_scan.generation = gen;
    }
    board::ampMute(true);
    si4732::setProperty(si4732::prop::kRxHardMute, 3);
    a.muted = -1;
    const int64_t t0 = esp_timer_get_time();
    unsigned n = 0;
    while (total) {
        {
            Lock lock;
            if (s_shutdown || s_scan.id != id || s_scan.phase != scan::Phase::Running) break;
        }
        if (!guide::scanNext(c, gen, khz, name)) break;
        {
            Lock lock;
            if (s_scan.id == id) s_scan.khz = khz;
        }
        tuneTo(s.mode, khz * 1000, a);
        vTaskDelay(pdMS_TO_TICKS(scan::kSettleMs));
        uint8_t rssi = 0, snr = 0;
        si4732::rsq(false, rssi, snr);
        n++;
        Lock lock;
        if (s_scan.id == id) scan::measured(s_scan, {khz, rssi, snr, name});
    }
    {
        Lock lock;
        if (s_scan.id == id) scan::stop(s_scan);
    }
    const long ms = static_cast<long>((esp_timer_get_time() - t0) / 1000);
    ESP_LOGI(TAG, "scan: %u of %u frequencies in %ld ms, %ld ms each", n, static_cast<unsigned>(total), ms,
             n ? ms / static_cast<long>(n) : 0L);
}

void storeTuned(const si4732::TuneStatus& t, bool fm, Applied& a) {
    const int32_t hz = fm ? t.freq * 10000 : t.freq * 1000;
    Lock lock;
    s_state.hz = hz;
    a.hz = hz;
}

void task(void*) {
    Applied a;
    uint32_t lastRsq = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
        State s;
        int8_t seekReq;
        bool stopReq, down, scanReq;
        {
            Lock lock;
            s = s_state;
            seekReq = s_seekReq;
            stopReq = s_stopReq;
            down = s_shutdown;
            scanReq = s_scanReq;
            s_seekReq = 0;
            s_stopReq = false;
        }
        if (down) {
            board::ampMute(true);
            if (a.up) si4732::powerDown();
            {
                Lock lock;
                s_live.ready = false;
            }
            vTaskDelete(nullptr);
        }
        if (!a.up || a.func != funcOf(s.mode)) {
            if (!powerUpFor(s, a)) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            Lock lock;
            s_live.ready = true;
        }
        const bool fm = isFm(s.mode);
        bool seeking;
        {
            Lock lock;
            seeking = s_live.seeking;
        }
        if ((stopReq || scanReq) && seeking) {
            si4732::TuneStatus t;
            if (si4732::tuneStatus(fm, true, true, t)) storeTuned(t, fm, a);
            if (fm) rdsRestart();
            Lock lock;
            s_live.seeking = false;
            seeking = false;
        }
        if (scanReq) {
            runScan(s, a);
            continue;
        }
        if (seekReq && !seeking && funcOf(s.mode) != si4732::Func::SSB) {
            seekBand(s);
            if (fm) rdsRestart();
            if (fm ? si4732::seekFm(seekReq > 0) : si4732::seekAm(seekReq > 0, amAntennaCap(s.hz / 1000))) {
                Lock lock;
                s_live.seeking = true;
                s_live.seekDir = seekReq;
                seeking = true;
            }
        }
        if (seeking) {
            si4732::TuneStatus t;
            const bool done = si4732::seekDone();
            if (si4732::tuneStatus(fm, false, done, t)) storeTuned(t, fm, a);
            if (done) {
                if (fm) rdsRestart();
                Lock lock;
                s_live.seeking = false;
            }
            continue;
        }
        if (s.hz != a.hz || s.mode != a.mode) {
            applyTune(s, a);
            if (fm) rdsRestart();
        }
        const int bw = s.bwIdx[family(s.mode)];
        if (bw != a.bw) {
            switch (family(s.mode)) {
                case kFamFM: si4732::setProperty(si4732::prop::kFmChannelFilter, static_cast<uint16_t>(bw)); break;
                case kFamAM: si4732::setProperty(si4732::prop::kAmChannelFilter, kAmFilter[bw]); break;
                default: si4732::setProperty(si4732::prop::kSsbMode, ssbMode(bw)); break;
            }
            a.bw = bw;
        }
        if (s.agc != a.agc) {
            si4732::agc(fm, s.agc);
            a.agc = s.agc;
        }
        if (s.volume != a.volume) {
            si4732::setProperty(si4732::prop::kRxVolume, s.volume);
            a.volume = s.volume;
        }
        if (s.muted != a.muted) {
            si4732::setProperty(si4732::prop::kRxHardMute, s.muted ? 3 : 0);
            a.muted = s.muted;
        }
        setAmp(s, a);
        if (fm) rdsPoll();
        const uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now - lastRsq >= 500) {
            lastRsq = now;
            uint8_t rssi, snr;
            if (si4732::rsq(fm, rssi, snr)) {
                Lock lock;
                s_live.rssi = rssi;
                s_live.snr = snr;
                s_live.bars = barsFor(rssi, fm);
            }
        }
    }
}

void wake() {
    if (s_task) xTaskNotifyGive(s_task);
}

}  // namespace

uint8_t barsFor(uint8_t rssi, bool fm) {
    const int base = fm ? 15 : 10;
    int bars = 0;
    for (int i = 0; i < 5; i++)
        if (rssi >= base + 10 * i) bars = i + 1;
    return static_cast<uint8_t>(bars);
}

void start(const State& initial) {
    s_lock = xSemaphoreCreateMutex();
    s_rdsRing = new rds::Ring();
    s_state = initial;
    size_t n;
    s_live.ssb = si4732::patchValid(patchData(&n), n);
    if (!s_live.ssb && family(s_state.mode) == kFamSSB) s_state.mode = Mode::AM;
    ESP_LOGI(TAG, "SSB patch %s", s_live.ssb ? "present" : "missing");
    xTaskCreate(task, "radio", 3072, nullptr, 6, &s_task);
}

bool submit(const Command& c) {
    bool changed = false;
    {
        Lock lock;
        switch (c.cmd) {
            case Cmd::Seek: s_seekReq = static_cast<int8_t>(c.v > 0 ? 1 : -1); break;
            case Cmd::SeekStop: s_stopReq = true; break;
            case Cmd::SavePreset: {
                // Named after the station shown on home, as the radio's display draws it.
                char ps[rds::kPsBuf];
                s_rds.ps(ps, sizeof ps, true);
                changed = apply(s_state, c, s_live.ssb, ps);
                break;
            }
            default: changed = apply(s_state, c, s_live.ssb); break;
        }
    }
    wake();
    return changed;
}

bool edit(bool (*fn)(State&, void*), void* arg) {
    bool changed;
    {
        Lock lock;
        changed = fn(s_state, arg);
    }
    wake();
    return changed;
}

void snapshot(State& out) {
    Lock lock;
    out = s_state;
}

Live live() {
    Lock lock;
    return s_live;
}

void rdsState(rds::Decoder& out) {
    Lock lock;
    out = s_rds;
}

int rdsGroups(rds::Raw* out, int max) {
    Lock lock;
    if (!s_rdsRing) return 0;
    const int n = s_rdsRing->count() < max ? s_rdsRing->count() : max;
    for (int i = 0; i < n; i++) out[i] = s_rdsRing->at(s_rdsRing->count() - n + i);
    return n;
}

ScanStart startScan(scan::Scope scope, int64_t utcMs) {
    {
        Lock lock;
        if (s_scan.phase == scan::Phase::Running) return ScanStart::Busy;
        if (isFm(s_state.mode)) return ScanStart::NotAm;
        int band;
        int32_t lo, hi;
        if (!scanRange(s_state.hz, s_state.mode, scope == scan::Scope::All, band, lo, hi)) return ScanStart::NoBand;
        scan::start(s_scan, scope, band, lo, hi, s_state.hz, utcMs);
        s_scanReq = true;
    }
    wake();
    return ScanStart::Ok;
}

void stopScan(bool cancel) {
    Lock lock;
    if (s_scan.phase != scan::Phase::Running) return;
    if (cancel)
        scan::cancel(s_scan);
    else
        scan::stop(s_scan);
}

void scanState(scan::State& out) {
    Lock lock;
    out = s_scan;
}

void shutdown() {
    {
        Lock lock;
        s_shutdown = true;
    }
    wake();
}

}  // namespace radio
