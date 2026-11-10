// The screen and navigation state machine. Pure: it takes input events, the time and the radio state,
// renders into a gfx::Frame and emits radio commands. Compiled into the firmware and the host tests.
#pragma once
#include <cstdint>

#include "gfx.h"
#include "radio_model.h"

namespace ui {

enum class EventType : uint8_t { Turn, Press, LongPress, HoldTurn, Side };

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
};

struct Output {
    static constexpr int kMax = 8;
    radio::Command cmds[kMax];
    int n = 0;
    void push(radio::Cmd c, int32_t v = 0) {
        if (n < kMax) cmds[n++] = {c, v};
    }
};

enum class View : uint8_t { Splash, Home, List, Adjust, Info, Entry, Confirm, Message, Station };
enum class ListId : uint8_t { Main, Bands, Modes, Presets, Steps, Bandwidth, Seek, Wifi, Settings, Side, Language };
enum class AdjustId : uint8_t { Volume, Contrast, Light };
enum class InfoId : uint8_t { Wifi, About };

constexpr uint32_t kSplashMs = 2600;
constexpr uint32_t kMenuTimeoutMs = 8000;
constexpr uint32_t kConfirmTimeoutMs = 30000;
constexpr uint32_t kToastMs = 1500;
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

    void toast(const char* text, uint32_t now);
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

    struct ListState {
        ListId id;
        int cursor;
        int top;
    };

private:
    void endSplash(Output& out);
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

    void drawSplash(gfx::Frame& f, uint32_t t);
    void drawHome(gfx::Frame& f, uint32_t now, const radio::State& s, const Status& st);
    void drawGutter(gfx::Frame& f, int cursor, bool withMain, const radio::State& s);
    void drawList(gfx::Frame& f, const radio::State& s, const Status& st);
    void drawAdjust(gfx::Frame& f, const radio::State& s);
    void drawInfo(gfx::Frame& f, const radio::State& s, const Status& st);
    void drawStation(gfx::Frame& f, const radio::State& s, const Status& st);
    // The Station screen goes back to the first row when the RadioText is cleared.
    void syncStation(const Status& st);
    void drawEntry(gfx::Frame& f, const radio::State& s);
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
    uint32_t toastUntil_ = 0;
    uint32_t volUntil_ = 0;
    int favIdx_ = -1;
    char entrySsid_[33] = {};
    char entry_[kPasswordMax + 1] = {};
    int entryIdx_ = 0;
    char joinSsid_[33] = {};
    char joinPwd_[kPasswordMax + 1] = {};
    char message_[64] = {};
    int stationTop_ = 0;     // first RadioText row on the Station screen
    uint8_t stationSeq_ = 0;  // Status::rtSeq of that text

    // The list currently on screen, rebuilt from the state when needed.
    enum class Act : uint8_t { None, Go, Adjust, Info, ToggleAgc, ToggleMute, Band, Mode, Preset, Step,
                               Bandwidth, Seek, Join, Side, Language, Station };
    struct Item {
        char t[48];
        char mid[14];
        char r[32];
        Act act;
        int arg;
    };
    struct Gutter {
        gfx::Icon icon;
        Act act;
        int arg;
    };
    static constexpr int kMaxItems = 48;
    Item items_[kMaxItems];
    int count_ = 0;
    Gutter gutter_[3];
    int gutterCount_ = 0;
    int cur_ = 0;
};

}  // namespace ui
