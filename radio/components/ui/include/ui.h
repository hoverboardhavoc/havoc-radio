// The screen and navigation state machine. Pure: it takes input events, the time and the radio state,
// renders into a gfx::Frame and emits radio commands. Compiled into the firmware and the host tests.
#pragma once
#include <cstdint>

#include "gfx.h"
#include "guide_core.h"
#include "radio_model.h"

namespace ui {

// Side is a short press of the side button, SideHold its 1 s hold.
enum class EventType : uint8_t { Turn, Press, LongPress, HoldTurn, Side, SideHold };

struct Event {
    EventType type;
    int n;  // detents for Turn and HoldTurn
};

struct Network {
    char ssid[33];
    uint8_t bars;  // 1..3
};

// Everything the UI shows that is not part of the radio model.
struct Status {
    uint8_t bars = 0;        // signal, 0..5
    bool seeking = false;
    int8_t seekDir = 1;
    bool ssb = true;         // SSB patch available
    bool joined = false;     // WiFi station joined
    const char* ssid = "";   // joined network
    const char* ip = "";     // station or access point address
    const Network* nets = nullptr;
    int netCount = 0;
    int battery = 0;         // percent
    const char* version = "";
    // RDS, in FM: the PS and RadioText as the display draws them ("" when none), the programme type (0 none).
    bool rdsSync = false;
    const char* ps = "";
    const char* rt = "";
    uint8_t pty = 0;
    uint8_t rtSeq = 0;       // changes when the RadioText is cleared
    // The clock: UTC in ms since 1970 and the time since the last NTP sync, -1 until the first sync.
    int64_t utcMs = -1;
    int64_t syncAgeMs = -1;
    radio::Lamp lamp = radio::Lamp::Off;
    // The frequency guide: the loaded table (nullptr or not valid without one) and the entries for the tuned
    // frequency in AM, LSB and USB, as guide::lookup orders them.
    const guide::Info* guide = nullptr;
    const guide::Hits* hits = nullptr;
    bool netSaved = true;    // a WiFi network is configured
};

// The download mode's screen (a boot of its own, see components/download): the step, the bytes while downloading, and
// the firmware's install prompt.
struct Download {
    enum class Step : uint8_t { Join, Check, Get, Prompt };
    Step step = Step::Join;
    bool firmware = false;     // Updating firmware, else Updating guide
    const char* ssid = "";
    const char* version = "";  // the version the prompt offers
    uint32_t done = 0;
    uint32_t total = 0;
};
void drawDownload(gfx::Frame& f, int lang, const Download& d);
// The install prompt's answer to an input: a press installs; a long press or the side button (short or held)
// cancels; anything else is no answer. 30 s without a press cancels as well.
enum class PromptAnswer : uint8_t { None, Install, Cancel };
PromptAnswer promptAnswer(EventType e);
constexpr uint32_t kPromptMs = 30000;

struct Output {
    static constexpr int kMax = 8;
    radio::Command cmds[kMax];
    int n = 0;
    void push(radio::Cmd c, int32_t v = 0) {
        if (n < kMax) cmds[n++] = {c, v};
    }
};

enum class View : uint8_t { Splash, Home, List, Adjust, Info, Entry, Confirm, Message, Station, Lamp };
enum class ListId : uint8_t { Main, Bands, Modes, Presets, Steps, Bandwidth, Seek, Wifi, Settings, Language, Time,
                              Guide, GuideSettings, FirmwareSettings };
enum class AdjustId : uint8_t { Volume, Contrast };
enum class InfoId : uint8_t { Wifi, About, GuideEntry };

constexpr uint32_t kSplashMs = 2600;
constexpr uint32_t kMenuTimeoutMs = 8000;
constexpr uint32_t kConfirmTimeoutMs = 30000;
constexpr uint32_t kToastMs = 1500;
constexpr uint32_t kNameTurnMs = 3000;  // on-air names on home take turns
constexpr int kPasswordMax = 64;

constexpr int kRtRowW = 116;  // Station screen RadioText rows, from x = 10

// Word-wraps `s` into rows at most `w` px wide, breaking a word wider than a row. Calls row(i, text) for each row
// and returns the number of rows.
int wrapText(const char* s, int w, void (*row)(void* ctx, int i, const char* text), void* ctx);

// Computes the splash wheel's geometry once (about 9 KB, freed when the splash ends). Call it before
// Ui::begin so the time it takes does not come out of the animation; drawing the splash computes it if needed.
void prepareSplash();

class Ui {
public:
    void begin(uint32_t now);
    void event(const Event& e, uint32_t now, const radio::State& s, const Status& st, Output& out);
    // Time-driven transitions: splash end, menu and confirm timeouts. The splash ending, by time or by an input,
    // emits Cmd::SplashEnd once.
    void tick(uint32_t now, const radio::State& s, const Status& st, Output& out);
    void render(gfx::Frame& f, uint32_t now, const radio::State& s, const Status& st);

    void toast(const char* text, uint32_t now, uint32_t ms = kToastMs);
    // A download's result, shown in home's top row until the next input; during the splash it waits for the splash
    // to end.
    void result(const char* text);
    // A firmware image is waiting: ask for a knob press.
    void confirmInstall(uint32_t now);
    // A fixed full-screen message that ignores input (battery empty).
    void message(const char* text);

    View view() const { return view_; }
    ListId listId() const { return list_.id; }
    int cursor() const { return list_.cursor; }
    int top() const { return list_.top; }
    int depth() const { return depth_; }
    int entryIndex() const { return entryIdx_; }
    const char* entryText() const { return entry_; }
    // Credentials of the last Join command.
    const char* joinSsid() const { return joinSsid_; }
    const char* joinPassword() const { return joinPwd_; }
    // The server of the last NtpServer command, as typed.
    const char* serverText() const { return server_; }

    struct ListState {
        ListId id;
        int cursor;
        int top;
    };

private:
    void endSplash(Output& out);
    void showResult(const char* text);
    void goHome();
    void popTo();
    void openList(ListId id, bool push, const radio::State& s, const Status& st);
    void build(ListId id, const radio::State& s, const Status& st);
    void scrollTo();
    void activate(const radio::State& s, const Status& st, Output& out, uint32_t now);
    void adjustDone();
    int adjustValue(const radio::State& s) const;
    void startSeek(int dir, const radio::State& s, Output& out, uint32_t now);
    void stopSeek(const Status& st, Output& out);
    // The lamp screen, over a list as an adjust screen or in place of another screen; it turns the lamp on.
    void openLamp(const Status& st, Output& out);

    void drawSplash(gfx::Frame& f, uint32_t t);
    void drawHome(gfx::Frame& f, uint32_t now, const radio::State& s, const Status& st);
    void drawGutter(gfx::Frame& f, int cursor, bool withMain, const radio::State& s);
    void drawList(gfx::Frame& f, const radio::State& s, const Status& st);
    void drawAdjust(gfx::Frame& f, const radio::State& s);
    void drawLamp(gfx::Frame& f, const radio::State& s, const Status& st);
    void drawInfo(gfx::Frame& f, const radio::State& s, const Status& st);
    void drawStation(gfx::Frame& f, const radio::State& s, const Status& st);
    // The Station screen goes back to the first row when the RadioText is cleared.
    void syncStation(const Status& st);
    void drawEntry(gfx::Frame& f, const radio::State& s);
    void startEntry(const char* ssid, const char* value, int idx);
    void drawConfirm(gfx::Frame& f, const radio::State& s);
    void drawMessage(gfx::Frame& f);

    View view_ = View::Splash;
    uint32_t splashStart_ = 0;
    uint32_t lastInput_ = 0;
    uint32_t confirmStart_ = 0;
    ListState list_{ListId::Main, 0, 0};
    ListState stack_[4];
    int depth_ = 0;
    AdjustId adjust_ = AdjustId::Volume;
    InfoId info_ = InfoId::Wifi;
    char toast_[96] = {};
    char result_[96] = {};
    uint32_t toastUntil_ = 0;
    bool resultShown_ = false;  // toast_ holds a download result, shown until the next input
    uint32_t volUntil_ = 0;
    char entrySsid_[33] = {};
    bool entryServer_ = false;  // text entry for the NTP server, else a WiFi password
    char entry_[kPasswordMax + 1] = {};
    int entryIdx_ = 0;
    char joinSsid_[33] = {};
    char joinPwd_[kPasswordMax + 1] = {};
    char server_[radio::kNtpServerMax + 1] = {};
    char message_[64] = {};
    int stationTop_ = 0;     // first RadioText row on the Station screen
    uint8_t stationSeq_ = 0;  // Status::rtSeq of that text
    // The guide entry whose details are shown.
    struct GuideEntry {
        guide::Entry e;
        char name[guide::kStrMax + 1], lang[guide::kStrMax + 1], target[guide::kStrMax + 1];
    } guideEntry_{};

    // The list currently on screen, rebuilt from the state when needed.
    enum class Act : uint8_t { None, Go, Adjust, Info, ToggleAgc, ToggleMute, Band, Mode, Preset, Step,
                               Bandwidth, Seek, Join, Language, Station, ToggleTimeSync, Server, Lamp, GuideEntry,
                               GuideUpdate, FirmwareUpdate };
    struct Item {
        char t[48];
        char mid[14];
        char r[48];  // a cut NTP server: up to 42 one-pixel characters and the ellipsis
        Act act;
        int arg;
        bool skip;  // shown, not selectable
        bool mark;  // a guide entry on air
    };
    struct Gutter {
        gfx::Icon icon;
        Act act;
        int arg;
    };
    static constexpr int kMaxItems = 48;
    Item items_[kMaxItems];
    int count_ = 0;
    Gutter gutter_[1];
    int gutterCount_ = 0;
    int cur_ = 0;
};

}  // namespace ui
