// The download mode's core (pure, host-tested with a fake transport): the manifest and the table fetched from the
// data repository over one HTTPS GET at a time, the table streamed into the free slot with its SHA-256 checked
// against the manifest's, and the result.
#pragma once
#include <cstddef>
#include <cstdint>

#include "guide_core.h"

namespace guide {

constexpr const char* kHost = "raw.githubusercontent.com";
constexpr const char* kManifestPath = "/hoverboardhavoc/havoc-radio-guide/main/v1/manifest.json";
constexpr const char* kTablePath = "/hoverboardhavoc/havoc-radio-guide/main/v1/stations.bin";
constexpr uint32_t kManifestMax = 2048;

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

// Where a new table goes: the free slot (guide::Update on the radio).
class Sink {
public:
    virtual Err begin(uint32_t length) = 0;
    virtual Err add(const void* data, uint32_t n) = 0;
    virtual Err finish(const uint8_t* sha256) = 0;

protected:
    ~Sink() = default;
};

// The download's results, as stored for the normal app to show on home.
// Cancelled is recorded for GET /api/guide only; home shows nothing for it.
enum class Result : uint8_t { None, Updated, UpToDate, NoNetwork, Failed, CheckFailed, Cancelled };
enum class Step : uint8_t { Join, Manifest, Table, Check };
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

struct Manifest {
    int format = 0;
    int64_t length = -1;
    uint8_t sha256[32] = {};
    bool hasSha = false;
};
// The manifest's format, length and sha256 from its flat JSON object; false when it is not one.
bool parseManifest(const char* json, size_t n, Manifest& out);

// What the download did: the result, the step it ended in and the error there (Err::Ok when none).
struct Report {
    Result result = Result::None;
    Step step = Step::Manifest;
    Err err = Err::Ok;
};
// Steps 2 to 4 of the download: the manifest (up to date when its SHA-256 is the loaded table's), the table into the
// sink with its length and SHA-256 checked against the manifest, and the table's own checks.
Report fetch(Http& http, Sink& sink, const Info& loaded, Progress& progress);

// The download request in NVS: cleared before it is acted on, so a crash during the download cannot repeat it.
class RequestFlag {
public:
    virtual bool get() = 0;
    virtual void set() = 0;
    virtual void clear() = 0;

protected:
    ~RequestFlag() = default;
};
bool takeRequest(RequestFlag& f);
// POST /api/guide/update: without a configured network 409 and nothing stored; otherwise the request is stored and the
// answer is 202 (the radio restarts once it is sent). Returns the status and writes the body.
int requestUpdate(bool netSaved, RequestFlag& f, char* body, size_t n);

// The last download since power-up, for GET /api/guide: its result, the step it ended in and the lowest free heap.
struct LastUpdate {
    Result result = Result::None;
    Step step = Step::Join;
    uint32_t lowestHeap = 0;
};
const char* resultName(Result r);  // "updated", "up to date", "no network", "download failed", "check failed", "cancelled"
// `"last_update":{...}` for a run, or nothing (an empty string) before one.
void lastUpdateJson(const LastUpdate& u, char* out, size_t n);

}  // namespace guide
