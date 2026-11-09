// SSD1306 128x32 OLED over I2C, charge pump on.
#pragma once
#include <cstdint>

namespace ssd1306 {

bool init();
void contrast(uint8_t value);  // 0..255
// Pushes a 512-byte framebuffer in page order (byte x + page * 128, bit 0 at the top of the page).
bool push(const uint8_t* fb);
void display(bool on);

}  // namespace ssd1306
