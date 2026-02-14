
#include "fram_pref.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace fram_pref {

static const char *const TAG = "fram_pref";

class FRAMPreferenceBackend : public ESPPreferenceBackend {
 public:
  FRAMPreferenceBackend(FramPref *comp, uint32_t type) : comp_(comp), type_(type) {}

  bool save(const uint8_t *data, size_t len) override;
  bool load(uint8_t *data, size_t len) override;

 protected:
  uint32_t find_key_(uint32_t key_hash);

  FramPref *comp_;
  uint32_t type_;
};

uint32_t FRAMPreferenceBackend::find_key_(uint32_t key_hash) {
  uint32_t addr = this->comp_->pool_start_ + 5;  // 4 bytes for magic, 1 for version
  uint32_t end = this->comp_->pool_start_ + this->comp_->pool_size_;
  while (addr < end) {
    uint32_t key_from_fram = 0;
    this->comp_->fram_->read_bytes(addr, (uint8_t *) &key_from_fram, 4);
    if (key_from_fram == key_hash) {
      return addr;
    }
    if (key_from_fram == 0) {
      this->comp_->fram_->write_bytes(addr, (uint8_t *) &key_hash, 4);
      return addr;
    }
    uint32_t size_from_fram = 0;
    this->comp_->fram_->read_bytes(addr + 4, (uint8_t *) &size_from_fram, 4);
    addr += 8 + size_from_fram + 4;  // key + size + data + crc
  }
  return 0;
}

bool FRAMPreferenceBackend::save(const uint8_t *data, size_t len) {
  if (!this->comp_->fram_->is_connected()) {
    return false;
  }
  uint32_t key_hash = fnv1_hash(std::to_string(this->type_));
  uint32_t addr = this->find_key_(key_hash);

  if (addr == 0) {
    return false;
  }

  this->comp_->fram_->write_bytes(addr + 4, (uint8_t *) &len, 4);
  this->comp_->fram_->write_bytes(addr + 8, data, len);
  uint32_t crc = crc32(data, len);
  this->comp_->fram_->write_bytes(addr + 8 + len, (uint8_t *) &crc, 4);
  return true;
}

bool FRAMPreferenceBackend::load(uint8_t *data, size_t len) {
  if (!this->comp_->fram_->is_connected()) {
    return false;
  }
  uint32_t key_hash = fnv1_hash(std::to_string(this->type_));
  uint32_t addr = this->find_key_(key_hash);

  if (addr == 0) {
    return false;
  }

  uint32_t size_from_fram = 0;
  this->comp_->fram_->read_bytes(addr + 4, (uint8_t *) &size_from_fram, 4);
  if (size_from_fram != len) {
    return false;
  }

  this->comp_->fram_->read_bytes(addr + 8, data, len);
  uint32_t crc_from_fram = 0;
  this->comp_->fram_->read_bytes(addr + 8 + len, (uint8_t *) &crc_from_fram, 4);
  return crc32(data, len) == crc_from_fram;
}

void FramPref::setup() {
  if (!this->fram_->is_connected()) {
    ESP_LOGW(TAG, "FRAM device not found, using default preferences.");
    this->mark_failed();
    return;
  }

  this->magic_ = 0xDEADBEEF;
  uint32_t magic = 0;
  this->fram_->read_bytes(this->pool_start_, (uint8_t *) &magic, 4);

  if (magic != this->magic_) {
    this->pool_cleared_ = true;
    uint32_t value = this->magic_;
    this->fram_->write_bytes(this->pool_start_, (uint8_t *) &value, 4);
    this->fram_->write_bytes(this->pool_start_ + 4, &this->version_, 1);
  } else {
    uint8_t version_from_fram = 0;
    this->fram_->read_bytes(this->pool_start_ + 4, &version_from_fram, 1);
    if (version_from_fram != this->version_) {
      ESP_LOGW(TAG, "FRAM preferences version mismatch. Clearing preferences.");
      this->pool_cleared_ = true;
      uint32_t value = this->magic_;
      this->fram_->write_bytes(this->pool_start_, (uint8_t *) &value, 4);
      this->fram_->write_bytes(this->pool_start_ + 4, &this->version_, 1);
    }
  }

  global_preferences = this;
}

void FramPref::dump_config() {
  ESP_LOGCONFIG(TAG, "FRAM Preferences:");
  if (this->pool_cleared_) {
    ESP_LOGCONFIG(TAG, "  Pool was cleared");
  }
}

ESPPreferenceObject FramPref::make_preference(size_t length, uint32_t type, bool in_flash) {
  return this->make_preference(length, type);
}

ESPPreferenceObject FramPref::make_preference(size_t length, uint32_t type) {
  return ESPPreferenceObject(new FRAMPreferenceBackend(this, type));
}

bool FramPref::sync() { return true; }

bool FramPref::reset() {
  this->pool_cleared_ = true;
  uint32_t value = this->magic_;
  this->fram_->write_bytes(this->pool_start_, (uint8_t *) &value, 4);
  this->fram_->write_bytes(this->pool_start_ + 4, &this->version_, 1);
  return true;
}

}  // namespace fram_pref
}  // namespace esphome
