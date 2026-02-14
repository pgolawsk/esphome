#include "fram.h"
#include "esphome/core/log.h"

namespace esphome {
namespace fram {

static const char *const TAG = "fram";

void Fram::setup() {
  if (this->is_connected()) {
    ESP_LOGCONFIG(TAG, "Found FRAM device at 0x%02X", this->address_);
  } else {
    ESP_LOGE(TAG, "FRAM device not found at 0x%02X", this->address_);
    this->mark_failed();
  }
}

void Fram::dump_config() {
  ESP_LOGCONFIG(TAG, "FRAM:");
  ESP_LOGCONFIG(TAG, "  Address: 0x%02X", this->address_);
  if (this->size_bytes_ > 0) {
    ESP_LOGCONFIG(TAG, "  Size: %u bytes", this->size_bytes_);
  }
}

bool Fram::is_connected() { return this->bus_->write(this->address_, nullptr, 0) == i2c::ERROR_OK; }

void Fram::write_bytes(uint32_t memaddr, const uint8_t *value, uint32_t len) {
  this->write_bytes_16(memaddr, value, len);
}

void Fram::read_bytes(uint32_t memaddr, uint8_t *value, uint32_t len) { this->read_bytes_16(memaddr, value, len); }

void Fram::write_bytes_16(uint32_t memaddr, const uint8_t *value, uint32_t len) {
  uint8_t memaddr_hi = (memaddr >> 8) & 0xFF;
  uint8_t memaddr_lo = memaddr & 0xFF;
  this->bus_->write_bytes(this->address_, &memaddr_hi, 1, false);
  this->bus_->write_bytes(this->address_, &memaddr_lo, 1, false);
  this->bus_->write_bytes(this->address_, value, len);
}

void Fram::read_bytes_16(uint32_t memaddr, uint8_t *value, uint32_t len) {
  uint8_t memaddr_hi = (memaddr >> 8) & 0xFF;
  uint8_t memaddr_lo = memaddr & 0xFF;
  this->bus_->write_bytes(this->address_, &memaddr_hi, 1, false);
  this->bus_->write_bytes(this->address_, &memaddr_lo, 1, false);
  this->bus_->read_bytes(this->address_, value, len);
}

}  // namespace fram
}  // namespace esphome
