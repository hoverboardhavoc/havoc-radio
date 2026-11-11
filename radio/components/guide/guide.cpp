#include "guide.h"

#include <cstdlib>
#include <new>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_spi_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace guide {

namespace {

const char* TAG = "guide";
constexpr esp_partition_subtype_t kSubtype = static_cast<esp_partition_subtype_t>(0x40);

class PartitionFlash : public Flash {
public:
    const esp_partition_t* part = nullptr;
    bool read(uint32_t off, void* buf, uint32_t len) override {
        return part && off <= part->size && len <= part->size - off && spi_flash_read(part->address + off, buf, len) == ESP_OK;
    }
    bool erase(uint32_t off) override {
        return part && off < part->size && spi_flash_erase_sector((part->address + off) / kSector) == ESP_OK;
    }
    bool write(uint32_t off, const void* buf, uint32_t len) override {
        return part && off <= part->size && len <= part->size - off &&
               spi_flash_write(part->address + off, const_cast<void*>(buf), len) == ESP_OK;
    }
};

PartitionFlash s_flash;
Slots s_slots(s_flash);
SemaphoreHandle_t s_lock;
volatile uint32_t s_generation = 1;  // changes with every switch

Hits s_hits;
bool s_am = false;
struct Key {
    int32_t hz;
    int32_t minute;
    uint32_t generation;
    bool am;
} s_key{0, -2, 0, false};

class Lock {
public:
    Lock() { xSemaphoreTake(s_lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_lock); }
};

}  // namespace

void start() {
    s_lock = xSemaphoreCreateMutex();
    s_flash.part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, kSubtype, "guide");
    if (!s_flash.part || s_flash.part->size < kPartitionSize) {
        ESP_LOGE(TAG, "no guide partition");
        s_flash.part = nullptr;
        return;
    }
    s_slots.boot();
    const Info& i = s_slots.info();
    if (i.valid)
        ESP_LOGI(TAG, "slot %d: EiBi %s, %u entries", s_slots.active(), i.season, static_cast<unsigned>(i.entries));
    else
        ESP_LOGI(TAG, "no guide");
}

Info info() {
    Lock lock;
    return s_slots.info();
}

const Hits& refresh(int32_t hz, bool am, int64_t utcMs) {
    const Key k{hz, utcMs >= 0 ? static_cast<int32_t>(utcMs / 60000) : -1, s_generation, am};
    if (k.hz == s_key.hz && k.minute == s_key.minute && k.generation == s_key.generation && k.am == s_key.am)
        return s_hits;
    Lock lock;
    s_key = k;
    s_am = am;
    if (am && s_slots.active() >= 0)
        lookup(s_slots.source(), s_slots.info(), hz, utcMs, s_hits);
    else
        s_hits.n = 0;
    return s_hits;
}

void withHits(void (*fn)(const Hits&, bool, void*), void* arg) {
    Lock lock;
    fn(s_hits, s_am, arg);
}

Update::Update() : sector_(static_cast<uint8_t*>(std::malloc(kSector))), writer_(nullptr) {
    if (sector_) writer_ = new (std::nothrow) Slots::Writer(s_slots, sector_);
}

Update::~Update() {
    delete writer_;
    std::free(sector_);
}

Err Update::begin(uint32_t length) {
    if (!writer_ || !s_flash.part) return Err::Write;
    return writer_->begin(length);
}

Err Update::add(const void* data, uint32_t n) {
    if (!writer_) return Err::Write;
    return writer_->add(data, n);
}

Err Update::finish(const uint8_t* sha256) {
    if (!writer_) return Err::Write;
    const Err e = writer_->finish(sha256);
    if (e != Err::Ok) return e;
    Lock lock;
    s_slots.commit(*writer_);
    s_generation = s_generation + 1;
    ESP_LOGI(TAG, "slot %d: EiBi %s, %u entries", s_slots.active(), s_slots.info().season,
             static_cast<unsigned>(s_slots.info().entries));
    return Err::Ok;
}

uint32_t Update::received() const { return writer_ ? writer_->received() : 0; }

}  // namespace guide
