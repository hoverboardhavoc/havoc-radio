// Download mode: the boot that downloads a new frequency guide table (Settings > Guide > Update guide).
#pragma once
#include <cstdint>

namespace download {

// Runs the download with only the display, the knob and WiFi started, stores the result and restarts.
[[noreturn]] void run(int lang);
// The text home shows once for a stored result (guide::Result), or nullptr for none.
const char* resultText(int lang, uint8_t result);

}  // namespace download
