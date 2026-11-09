// Single owner of the I2C bus (SDA/SCL from board.h). Every transfer takes the bus mutex.
#pragma once
#include <cstddef>
#include <cstdint>

namespace i2c {

bool init();
// START, address + W, optional prefix byte, data, STOP.
bool write(uint8_t addr, const uint8_t* data, size_t n);
bool writePrefixed(uint8_t addr, uint8_t prefix, const uint8_t* data, size_t n);
// START, address + R, n bytes (ACK all but the last), STOP.
bool read(uint8_t addr, uint8_t* data, size_t n);

}  // namespace i2c
