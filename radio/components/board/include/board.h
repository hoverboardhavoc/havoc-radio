// Pin map and boot-strap-safe pin setup. The only place pin numbers appear.
#pragma once
#include <cstdint>

namespace board {

constexpr int kSda = 4;
constexpr int kScl = 5;
constexpr int kSiReset = 2;   // active low; boot strap, must be high at boot
constexpr int kAmpMute = 0;   // high = amplifier shut down; boot strap, high at boot
constexpr int kEncA = 14;
constexpr int kEncB = 13;
constexpr int kEncPush = 12;
constexpr int kSide = 3;      // UART0 RX, used as an input
constexpr int kLamp = 15;     // the lamp on the back (PWM); boot strap, must be low at boot

constexpr uint8_t kSi4732Addr = 0x11;
constexpr uint8_t kSsd1306Addr = 0x3C;

// Set to true if the display is mounted upside down.
constexpr bool kDisplayRotated = false;

// Battery ADC calibration (raw 0..1023 through the divider from the battery rail).
constexpr int kBatteryEmptyRaw = 750;
constexpr int kBatteryLowRaw = 850;
constexpr int kBatteryFullRaw = 990;

// Configures every pin without glitching the boot straps: amplifier muted, receiver reset released, lamp off.
void init();
void ampMute(bool muted);
void siReset();   // pulse the receiver's reset line
void lamp(int level);  // 0 (off) .. 15; only the lamp module calls it
bool knobDown();
bool sideDown();
uint8_t encoderAB();  // A in bit 1, B in bit 0, 1 = high

}  // namespace board
