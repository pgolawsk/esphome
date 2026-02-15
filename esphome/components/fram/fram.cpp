#include "fram.h"
#include "esphome/core/log.h"
#include <vector>

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
  ESP_LOGCONFIG(TAG, "  Address width: %u bytes", this->address_width_);
}

bool Fram::is_connected() { return this->write(nullptr, 0) == i2c::ERROR_OK; }

void Fram::write_bytes(uint32_t memaddr, const uint8_t *value, uint32_t len) {
  this->write_bytes_16(memaddr, value, len);
}

void Fram::read_bytes(uint32_t memaddr, uint8_t *value, uint32_t len) { this->read_bytes_16(memaddr, value, len); }

void Fram::write_bytes_16(uint32_t memaddr, const uint8_t *value, uint32_t len) {
  uint8_t memaddr_hi = (memaddr >> 8) & 0xFF;
  uint8_t memaddr_lo = memaddr & 0xFF;
  // FRAM requires address and data in a single I2C write transaction
  // Create a buffer with address prefix + data
  std::vector<uint8_t> buffer;
  buffer.reserve(2 + len);
  buffer.push_back(memaddr_hi);
  buffer.push_back(memaddr_lo);
  for (uint32_t i = 0; i < len; i++) {
    buffer.push_back(value[i]);
  }
  ESP_LOGD(TAG, "Write addr=0x%04X, len=%u, data[0]=0x%02X", memaddr, len, len > 0 ? value[0] : 0);
  auto err = this->bus_->write_readv(this->address_, buffer.data(), buffer.size(), nullptr, 0);
  if (err != i2c::ERROR_OK) {
    ESP_LOGW(TAG, "Write failed with error %d", err);
  }
}

void Fram::read_bytes_16(uint32_t memaddr, uint8_t *value, uint32_t len) {
  uint8_t memaddr_hi = (memaddr >> 8) & 0xFF;
  uint8_t memaddr_lo = memaddr & 0xFF;
  // Write the 16-bit address, then read the data
  uint8_t addr_buf[2] = {memaddr_hi, memaddr_lo};
  ESP_LOGD(TAG, "Read addr=0x%04X, len=%u", memaddr, len);
  auto err = this->bus_->write_readv(this->address_, addr_buf, 2, value, len);
  if (err != i2c::ERROR_OK) {
    ESP_LOGW(TAG, "Read failed with error %d", err);
  }
  ESP_LOGD(TAG, "Read data[0-3]=0x%02X 0x%02X 0x%02X 0x%02X", len > 0 ? value[0] : 0, len > 1 ? value[1] : 0,
           len > 2 ? value[2] : 0, len > 3 ? value[3] : 0);
}

}  // namespace fram
}  // namespace esphome
