#include "fram.h"
#include "esphome/core/log.h"

namespace esphome {
namespace nvm {
namespace fram {

static const char *const TAG = "nvm.fram";

void FramPlatform::setup() {
  // Check if FRAM is connected
  if (!this->is_connected()) {
    ESP_LOGE(TAG, "FRAM not found at address 0x%02X", this->address_);
    this->mark_failed();
    return;
  }

  ESP_LOGCONFIG(TAG, "FRAM initialized:");
  ESP_LOGCONFIG(TAG, "  Model: %u bytes", model_.size_bytes);
  ESP_LOGCONFIG(TAG, "  Address width: %u bits", model_.address_width * 8);

  // Call parent setup to initialize partitions
  NvmPlatform::setup();
}

void FramPlatform::dump_config() {
  ESP_LOGCONFIG(TAG, "FRAM Platform:");
  ESP_LOGCONFIG(TAG, "  Address: 0x%02X", this->address_);
  ESP_LOGCONFIG(TAG, "  Size: %u bytes", model_.size_bytes);
  ESP_LOGCONFIG(TAG, "  Address width: %u bits", model_.address_width * 8);

  // Call parent dump_config to show partitions
  NvmPlatform::dump_config();
}

void FramPlatform::set_model(uint32_t size_bytes) {
  for (const auto &model : FRAM_MODELS) {
    if (model.size_bytes == size_bytes) {
      model_ = model;
      return;
    }
  }
  ESP_LOGW(TAG, "Unknown FRAM size %u, using default settings", size_bytes);
  model_.size_bytes = size_bytes;
  model_.address_width = 2;  // Default to 16-bit addressing
}

bool FramPlatform::is_connected() {
  // Try to read one byte to check if device is present
  uint8_t data;
  return this->read_bytes(0, &data, 1);
}

bool FramPlatform::read_bytes(uint32_t memaddr, uint8_t *data, size_t len) {
  if (memaddr + len > model_.size_bytes) {
    ESP_LOGE(TAG, "Read out of bounds: addr=%u, len=%zu, size=%u", memaddr, len, model_.size_bytes);
    return false;
  }

  if (model_.address_width == 2) {
    return this->read_bytes_16(memaddr, data, len);
  } else {
    return this->read_bytes_ext(memaddr, data, len);
  }
}

bool FramPlatform::write_bytes(uint32_t memaddr, const uint8_t *data, size_t len) {
  if (memaddr + len > model_.size_bytes) {
    ESP_LOGE(TAG, "Write out of bounds: addr=%u, len=%zu, size=%u", memaddr, len, model_.size_bytes);
    return false;
  }

  if (model_.address_width == 2) {
    return this->write_bytes_16(memaddr, data, len);
  } else {
    return this->write_bytes_ext(memaddr, data, len);
  }
}

bool FramPlatform::read_bytes_16(uint32_t memaddr, uint8_t *data, size_t len) {
  // Standard I2C FRAM read: [device_addr+W][addr_high][addr_low] then [device_addr+R][data...]
  uint8_t addr_buf[2];
  addr_buf[0] = (memaddr >> 8) & 0xFF;
  addr_buf[1] = memaddr & 0xFF;

  // Write address, then read data
  i2c::ErrorCode err = this->write_read(addr_buf, 2, data, len);
  if (err != i2c::ERROR_OK) {
    ESP_LOGE(TAG, "Read failed at address %u: error %d", memaddr, err);
    return false;
  }

  return true;
}

bool FramPlatform::write_bytes_16(uint32_t memaddr, const uint8_t *data, size_t len) {
  // Standard I2C FRAM write: [device_addr+W][addr_high][addr_low][data...]
  // FRAM doesn't need page write delays like EEPROM

  // Prepare buffer: address + data
  std::vector<uint8_t> buf(2 + len);
  buf[0] = (memaddr >> 8) & 0xFF;
  buf[1] = memaddr & 0xFF;
  std::copy(data, data + len, buf.begin() + 2);

  i2c::ErrorCode err = this->write(buf.data(), buf.size());
  if (err != i2c::ERROR_OK) {
    ESP_LOGE(TAG, "Write failed at address %u: error %d", memaddr, err);
    return false;
  }

  return true;
}

bool FramPlatform::read_bytes_ext(uint32_t memaddr, uint8_t *data, size_t len) {
  // Extended address FRAM (17/18-bit): address high bits go into device address
  // For 17-bit: device address bits [1:0] become address bits [16:15]
  // For 18-bit: device address bits [1:0] become address bits [17:16]

  uint8_t addr_high = (memaddr >> 16) & 0x03;
  uint8_t modified_address = (this->address_ & 0xFC) | addr_high;

  uint8_t addr_buf[2];
  addr_buf[0] = (memaddr >> 8) & 0xFF;
  addr_buf[1] = memaddr & 0xFF;

  // Use modified address for this transaction
  i2c::ErrorCode err = this->bus_->write_read(modified_address, addr_buf, 2, data, len);
  if (err != i2c::ERROR_OK) {
    ESP_LOGE(TAG, "Extended read failed at address %u: error %d", memaddr, err);
    return false;
  }

  return true;
}

bool FramPlatform::write_bytes_ext(uint32_t memaddr, const uint8_t *data, size_t len) {
  // Extended address FRAM write
  uint8_t addr_high = (memaddr >> 16) & 0x03;
  uint8_t modified_address = (this->address_ & 0xFC) | addr_high;

  std::vector<uint8_t> buf(2 + len);
  buf[0] = (memaddr >> 8) & 0xFF;
  buf[1] = memaddr & 0xFF;
  std::copy(data, data + len, buf.begin() + 2);

  i2c::ErrorCode err = this->bus_->write(modified_address, buf.data(), buf.size());
  if (err != i2c::ERROR_OK) {
    ESP_LOGE(TAG, "Extended write failed at address %u: error %d", memaddr, err);
    return false;
  }

  return true;
}

}  // namespace fram
}  // namespace nvm
}  // namespace esphome
