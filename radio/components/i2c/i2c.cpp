#include "i2c_bus.h"

#include "board.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace i2c {

namespace {
SemaphoreHandle_t s_mutex;
constexpr i2c_port_t kPort = I2C_NUM_0;
constexpr TickType_t kTimeout = pdMS_TO_TICKS(100);

class Lock {
public:
    Lock() { xSemaphoreTake(s_mutex, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_mutex); }
};

bool run(i2c_cmd_handle_t cmd) {
    const esp_err_t err = i2c_master_cmd_begin(kPort, cmd, kTimeout);
    i2c_cmd_link_delete(cmd);
    return err == ESP_OK;
}
}  // namespace

bool init() {
    s_mutex = xSemaphoreCreateMutex();
    i2c_config_t conf = {};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = static_cast<gpio_num_t>(board::kSda);
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_io_num = static_cast<gpio_num_t>(board::kScl);
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.clk_stretch_tick = 300;
    // The ESP8266 driver is bit-banged at its fixed fastest rate, close to 400 kHz at 160 MHz.
    return i2c_driver_install(kPort, conf.mode) == ESP_OK && i2c_param_config(kPort, &conf) == ESP_OK;
}

bool writePrefixed(uint8_t addr, uint8_t prefix, const uint8_t* data, size_t n) {
    Lock lock;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, static_cast<uint8_t>(addr << 1 | I2C_MASTER_WRITE), true);
    i2c_master_write_byte(cmd, prefix, true);
    if (n) i2c_master_write(cmd, const_cast<uint8_t*>(data), n, true);
    i2c_master_stop(cmd);
    return run(cmd);
}

bool write(uint8_t addr, const uint8_t* data, size_t n) {
    Lock lock;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, static_cast<uint8_t>(addr << 1 | I2C_MASTER_WRITE), true);
    if (n) i2c_master_write(cmd, const_cast<uint8_t*>(data), n, true);
    i2c_master_stop(cmd);
    return run(cmd);
}

bool read(uint8_t addr, uint8_t* data, size_t n) {
    if (!n) return true;
    Lock lock;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, static_cast<uint8_t>(addr << 1 | I2C_MASTER_READ), true);
    if (n > 1) i2c_master_read(cmd, data, n - 1, I2C_MASTER_ACK);
    i2c_master_read_byte(cmd, data + n - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    return run(cmd);
}

}  // namespace i2c
