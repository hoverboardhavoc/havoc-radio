// SSD1306 128x32 OLED over I2C, charge pump on.
#pragma once
#include <cstdint>

namespace ssd1306 {

bool init();
void contrast(uint8_t value);  // 0..255
// Pushes a 512-byte framebuffer in page order (byte x + page * 128, bit 0 at the top of the page).
bool push(const uint8_t* fb);
// Every frame pushed after this call is also copied to `copy` (512 bytes, the caller's), under a lock that
// lastFrame takes, so a reader in another task never sees half of one frame and half of the next.
void keepFrames(uint8_t* copy);
// The last frame pushed, copied to `out` (512 bytes); false before keepFrames.
bool lastFrame(uint8_t* out);
void display(bool on);

}  // namespace ssd1306
