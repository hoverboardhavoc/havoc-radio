// The body of GET /api/strings: a language's strings as one JSON object, built from the table when asked for and
// written in chunks, so no buffer holds the whole object.
#pragma once
#include <cstddef>

namespace i18n {

constexpr size_t kJsonChunk = 512;

// Writes language lang's strings as {"key":"text",...} in key order, escaped as tools/gen_strings.py's json.dumps
// does, and hands it to send in chunks of at most kJsonChunk bytes from a stack buffer. Stops at the first chunk
// send refuses; returns whether every chunk was sent.
bool writeJson(int lang, bool (*send)(void* ctx, const char* data, size_t len), void* ctx);

// One byte of a UTF-8 string as it stands in a JSON string, into out (6 bytes at most); returns its length.
// Quotes, backslashes and control characters are escaped; every other byte, non-ASCII included, is kept.
size_t jsonEscape(unsigned char b, char* out);

}  // namespace i18n
