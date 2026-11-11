// Download mode's core (pure, host-tested with a fake transport): the request kept in NVS, one HTTPS GET at a time to
// raw.githubusercontent.com, the manifests, the guide table streamed into the free guide slot and the firmware image
// into the inactive app slot, each checked against its manifest's length and SHA-256, and the result.
#pragma once
#include <cstddef>
#include <cstdint>

#include "guide_slots.h"

namespace download {

using guide::Err;

constexpr const char* kHost = "raw.githubusercontent.com";
constexpr const char* kGuideManifestPath = "/hoverboardhavoc/eibi-binary-table/main/v1/manifest.json";
constexpr const char* kGuideTablePath = "/hoverboardhavoc/eibi-binary-table/main/v1/stations.bin";
constexpr uint32_t kManifestMax = 2048;
constexpr size_t kPathMax = 112;
// The flash layout this firmware is built for: two 1 MB app slots at 0x100000 and 0x200000. A layout that needs a
// reinstall gets its own folder on the firmware branch.
constexpr const char* kLayout = "layout1";
// The firmware's manifest and image on `branch` of hoverboardhavoc/havoc-radio, in the layout's folder.
void firmwarePaths(const char* branch, char manifest[kPathMax], char image[kPathMax]);

// What the download is for: the guide, or the firmware with the install prompt (the menu) or without it (the API).
// Stored in NVS as its value.
enum class Request : uint8_t { None, Guide, Firmware, FirmwareApi };
enum class Kind : uint8_t { Guide, Firmware };
inline Kind kindOf(Request r) { return r == Request::Guide ? Kind::Guide : Kind::Firmware; }

// One HTTPS GET at a time: open() connects and sends the request; read() returns bytes of the response (status line
// and headers included), 0 at its end, below 0 on an error or a receive timeout.
class Http {
public:
    virtual bool open(const char* path) = 0;
    virtual int read(uint8_t* buf, int n) = 0;
    virtual void close() = 0;

protected:
    ~Http() = default;
};

// The results, as stored for the normal app to show on home. A guide's Cancelled shows nothing there; a firmware
// update's shows `Update cancelled`.
enum class Result : uint8_t { None, Updated, UpToDate, NoNetwork, Failed, CheckFailed, Cancelled };
enum class Step : uint8_t { Join, Manifest, Table, Check, Prompt, Image };
const char* stepName(Step s);

class Progress {
public:
    virtual void step(Step s) = 0;
    virtual void bytes(uint32_t done, uint32_t total) = 0;

protected:
    ~Progress() = default;
};

// A response: the status line and headers, then a body of Content-Length bytes.
class Response {
public:
    explicit Response(Http& h) : h_(h) {}
    // Reads the head. True for status 200 with a Content-Length and no chunked transfer encoding.
    bool begin();
    int status() const { return status_; }
    int64_t length() const { return length_; }
    // Body bytes; 0 once Content-Length bytes have come, below 0 when the body is cut off.
    int body(uint8_t* buf, int n);

private:
    int line(char* out, int n);  // one header line without CR LF; -1 at an error
    Http& h_;
    uint8_t buf_[256];
    int pos_ = 0, fill_ = 0;
    int status_ = 0;
    int64_t length_ = -1;
    int64_t left_ = 0;
};

constexpr size_t kVersionMax = 24;
struct Manifest {
    int format = 0;
    int64_t length = -1;
    uint8_t sha256[32] = {};
    bool hasSha = false;
    char version[kVersionMax] = {};
};
// A manifest's format, length, sha256 and version from its flat JSON object; false when it is not one.
bool parseManifest(const char* json, size_t n, Manifest& out);

// Versions compared field by field as numbers (2026.47.0 is newer than 2026.46.3; a missing field counts as 0):
// 1 when a is newer than b, 0 when equal, -1 when older, -2 when either is not 1 to 4 fields of 1 to 9 digits.
int compareVersions(const char* a, const char* b);

// What a download did: the result, the step it ended in and the error there (Err::Ok when none).
struct Report {
    Result result = Result::None;
    Step step = Step::Manifest;
    Err err = Err::Ok;
};

// The guide: the manifest (up to date when its SHA-256 is the loaded table's), the table into the free slot with its
// length and SHA-256 checked against the manifest, and the table's own checks.
Report fetchGuide(Http& http, guide::Sink& sink, const guide::Info& loaded, Progress& progress);

// The inactive app slot (esp_ota_* on the radio).
class Ota {
public:
    virtual uint32_t capacity() = 0;            // the slot's size
    virtual Err begin(uint32_t length) = 0;     // erases what the image needs
    virtual Err write(const void* data, uint32_t n) = 0;
    virtual Err end() = 0;                      // the SDK's image check; Err::Image when it refuses the image
    virtual Err activate() = 0;                 // selects the slot for the next boot
    virtual void abort() = 0;                   // after begin(), on a failure; the running firmware stays selected

protected:
    ~Ota() = default;
};

// The install prompt: true to install `version`; false for a long press, the side button or 30 s without a press.
class Prompt {
public:
    virtual bool confirm(const char* version) = 0;

protected:
    ~Prompt() = default;
};

// The firmware: the manifest (up to date unless its version is newer than `running`), the prompt when given, the
// image streamed into the inactive slot with its length and SHA-256 checked against the manifest, the SDK's image
// check, then the slot selected for the next boot.
Report fetchFirmware(Http& http, Ota& ota, const char* running, const char* manifestPath, const char* imagePath,
                     Prompt* prompt, Progress& progress);

// The request in NVS: cleared before it is acted on, so a crash during the download cannot repeat it.
class RequestStore {
public:
    virtual Request get() = 0;
    virtual void set(Request r) = 0;
    virtual void clear() = 0;

protected:
    ~RequestStore() = default;
};
Request takeRequest(RequestStore& s);
// POST /api/guide/update and /api/firmware/update: without a configured network 409 and nothing stored; otherwise the
// request is stored and the answer is 202 (the radio restarts once it is sent). Returns the status and writes the body.
int requestUpdate(Request r, bool netSaved, RequestStore& s, char* body, size_t n);

// The result home shows once after the restart, as one byte: the kind in the high nibble, the result in the low one.
inline uint8_t packResult(Kind k, Result r) { return static_cast<uint8_t>(static_cast<int>(k) << 4 | static_cast<int>(r)); }
inline Kind resultKind(uint8_t v) { return static_cast<Kind>(v >> 4); }
inline Result resultOf(uint8_t v) { return static_cast<Result>(v & 15); }

// The last download since power-up, of either kind, for last_update in GET /api/guide and GET /api/firmware: its
// result, the step it ended in and the lowest free heap. Stored in NVS as these 8 bytes.
struct LastUpdate {
    Result result = Result::None;
    Step step = Step::Join;
    Kind kind = Kind::Guide;
    uint8_t reserved = 0;
    uint32_t lowestHeap = 0;
};
static_assert(sizeof(LastUpdate) == 8, "LastUpdate is stored as 8 bytes");
const char* resultName(Result r);  // "updated", "up to date", "no network", "download failed", "check failed", "cancelled"
const char* kindName(Kind k);      // "guide", "firmware"
// `"last_update":{...}` for a run, or nothing (an empty string) before one.
void lastUpdateJson(const LastUpdate& u, char* out, size_t n);

}  // namespace download
