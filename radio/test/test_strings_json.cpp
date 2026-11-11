// GET /api/strings: the body built from the table, for every language, is the JSON object the generator writes
// with json.dumps, byte for byte, sent in chunks of at most kJsonChunk bytes.
#include <sstream>

#include "i18n_json.h"
#include "i18n_strings.h"
#include "util.h"

namespace {

struct Body {
    std::string text;
    size_t chunks = 0;
    size_t largest = 0;
};

bool collect(void* ctx, const char* data, size_t len) {
    auto* b = static_cast<Body*>(ctx);
    b->text.append(data, len);
    b->chunks++;
    if (len > b->largest) b->largest = len;
    return true;
}

bool refuseSecond(void* ctx, const char*, size_t) { return ++*static_cast<int*>(ctx) < 2; }

std::string escaped(unsigned char b) {
    char out[6];
    return std::string(out, i18n::jsonEscape(b, out));
}

}  // namespace

TEST(strings_json_matches_generator) {
    std::istringstream want(readFile(t::root() + "/radio/build/host/gen/i18n_strings.jsonl"));
    std::string line;
    int l = 0;
    for (; std::getline(want, line); l++) {
        if (l >= i18n::kLangCount) break;
        Body b;
        CHECK(i18n::writeJson(l, collect, &b));
        if (b.text != line) t::fail(__FILE__, __LINE__, std::string(i18n::kLangCode[l]) + ": body differs");
        CHECK(b.largest <= i18n::kJsonChunk);
        CHECK_EQ(b.chunks, (line.size() + i18n::kJsonChunk - 1) / i18n::kJsonChunk);
    }
    CHECK_EQ(l, i18n::kLangCount);
}

TEST(strings_json_stops_when_send_fails) {
    int calls = 0;
    CHECK(!i18n::writeJson(0, refuseSecond, &calls));
    CHECK_EQ(calls, 2);
}

TEST(strings_json_escapes) {
    CHECK_STR(escaped('"'), "\\\"");
    CHECK_STR(escaped('\\'), "\\\\");
    CHECK_STR(escaped('\n'), "\\n");
    CHECK_STR(escaped('\t'), "\\t");
    CHECK_STR(escaped(0x01), "\\u0001");
    CHECK_STR(escaped(0x1f), "\\u001f");
    CHECK_STR(escaped('a'), "a");
    CHECK_STR(escaped(0x7f), "\x7f");
    CHECK_STR(escaped(0xc3), "\xc3");
}
