#include "ssd1306.h"

#include <cstring>

#include "board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "i2c_bus.h"

namespace ssd1306 {

namespace {
constexpr uint8_t kCommand = 0x00;
constexpr uint8_t kData = 0x40;
constexpr size_t kFrameBytes = 512;

uint8_t* s_copy;
SemaphoreHandle_t s_copyLock;

bool cmds(const uint8_t* c, int n) { return i2c::writePrefixed(board::kSsd1306Addr, kCommand, c, n); }
}  // namespace

bool init() {
    const uint8_t seq[] = {
        0xAE,                                  // display off
        0xD5, 0x80,                            // clock divide / oscillator
        0xA8, 0x1F,                            // multiplex: 32 rows
        0xD3, 0x00,                            // display offset 0
        0x40,                                  // start line 0
        0x8D, 0x14,                            // charge pump on
        0x20, 0x00,                            // horizontal addressing
        static_cast<uint8_t>(board::kDisplayRotated ? 0xA0 : 0xA1),  // segment remap
        static_cast<uint8_t>(board::kDisplayRotated ? 0xC0 : 0xC8),  // COM scan direction
        0xDA, 0x02,                            // COM pins: sequential, for 128x32
        0x81, 0x8F,                            // contrast
        0xD9, 0xF1,                            // pre-charge for the internal charge pump
        0xDB, 0x40,                            // VCOMH deselect level
        0xA4,                                  // display follows RAM
        0xA6,                                  // normal, not inverted
        0xAF,                                  // display on
    };
    return cmds(seq, sizeof seq);
}

void contrast(uint8_t value) {
    const uint8_t c[] = {0x81, value};
    cmds(c, sizeof c);
}

bool push(const uint8_t* fb) {
    const uint8_t window[] = {0x21, 0, 127, 0x22, 0, 3};
    const bool ok = cmds(window, sizeof window) && i2c::writePrefixed(board::kSsd1306Addr, kData, fb, kFrameBytes);
    if (s_copy) {
        xSemaphoreTake(s_copyLock, portMAX_DELAY);
        std::memcpy(s_copy, fb, kFrameBytes);
        xSemaphoreGive(s_copyLock);
    }
    return ok;
}

void keepFrames(uint8_t* copy) {
    s_copyLock = xSemaphoreCreateMutex();
    if (s_copyLock) s_copy = copy;
}

bool lastFrame(uint8_t* out) {
    if (!s_copy) return false;
    xSemaphoreTake(s_copyLock, portMAX_DELAY);
    std::memcpy(out, s_copy, kFrameBytes);
    xSemaphoreGive(s_copyLock);
    return true;
}

void display(bool on) {
    const uint8_t c = on ? 0xAF : 0xAE;
    cmds(&c, 1);
}

}  // namespace ssd1306
