
#include "fram_pref.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include <vector>

namespace esphome {
namespace fram_pref {

static const char *const TAG = "fram_pref";

// Pool layout:
// Offset 0: 4 bytes magic (0xDEADBEEF)
// Offset 4: 1 byte version
// Offset 5: 4 bytes pool_size (stored for validation)
// Offset 9: key slots begin
static const uint32_t POOL_HEADER_SIZE = 9;  // magic(4) + version(1) + pool_size(4)

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

uint32_t FramPref::calculate_pool_used_() {
  uint32_t addr = this->pool_start_ + POOL_HEADER_SIZE;
  uint32_t end = this->pool_start_ + this->pool_size_;
  uint32_t iterations = 0;
  const uint32_t max_iterations = 100;

  while (addr < end && iterations < max_iterations) {
    iterations++;
    uint32_t key = 0;
    this->fram_->read_bytes(addr, (uint8_t *) &key, 4);

    if (key == 0) {
      // Empty slot - this is where the pool ends
      return addr - this->pool_start_;
    }

    uint32_t size = 0;
    this->fram_->read_bytes(addr + 4, (uint8_t *) &size, 4);

    // Invalid size - return current position
    if (size > 1024 || size == 0xFFFFFFFF) {
      return addr - this->pool_start_;
    }

    addr += 8 + size + 4;  // key + size + data + hash
  }

  return addr - this->pool_start_;
}

void FramPref::ensure_initialized_() {
  if (this->initialized_) {
    return;
  }

  ESP_LOGVV(TAG, "Lazy initialization - pool_start=%u, pool_size=%u", this->pool_start_, this->pool_size_);

  if (!this->fram_->is_connected()) {
    ESP_LOGW(TAG, "FRAM device not found");
    return;
  }

  this->magic_ = 0xDEADBEEF;
  uint32_t magic = 0;
  this->fram_->read_bytes(this->pool_start_, (uint8_t *) &magic, 4);
  ESP_LOGVV(TAG, "Read magic: 0x%08X, expected: 0x%08X", magic, this->magic_);

  bool needs_clear = false;

  if (magic != this->magic_) {
    ESP_LOGW(TAG, "Magic mismatch, initializing pool");
    needs_clear = true;
  } else {
    uint8_t version_from_fram = 0;
    this->fram_->read_bytes(this->pool_start_ + 4, &version_from_fram, 1);
    ESP_LOGVV(TAG, "Version: %u, expected: %u", version_from_fram, this->version_);
    if (version_from_fram != this->version_) {
      ESP_LOGW(TAG, "FRAM preferences version mismatch. Clearing preferences.");
      needs_clear = true;
    } else {
      // Validate first key slot - should be 0 or a valid key with reasonable size
      uint32_t first_key = 0;
      uint32_t first_size = 0;
      this->fram_->read_bytes(this->pool_start_ + POOL_HEADER_SIZE, (uint8_t *) &first_key, 4);
      this->fram_->read_bytes(this->pool_start_ + POOL_HEADER_SIZE + 4, (uint8_t *) &first_size, 4);
      ESP_LOGVV(TAG, "First key at addr %u: 0x%08X, size: %u", POOL_HEADER_SIZE, first_key, first_size);

      // If first key is non-zero, validate the size is reasonable
      if (first_key != 0) {
        // Size should be reasonable (less than pool size and not garbage)
        if (first_size > this->pool_size_ || first_size > 10000) {
          ESP_LOGW(TAG, "First key slot has invalid size %u, clearing pool", first_size);
          needs_clear = true;
        }
      }
    }
  }

  if (!needs_clear) {
    // Calculate pool usage before checking pool size change
    this->pool_used_ = this->calculate_pool_used_();

    // Check if pool size decreased and data doesn't fit
    uint32_t stored_pool_size = 0;
    this->fram_->read_bytes(this->pool_start_ + 5, (uint8_t *) &stored_pool_size, 4);
    ESP_LOGVV(TAG, "Stored pool size: %u, current: %u, used: %u", stored_pool_size, this->pool_size_, this->pool_used_);

    if (stored_pool_size != 0 && this->pool_size_ < stored_pool_size) {
      // Pool size decreased - check if data still fits
      if (this->pool_used_ > this->pool_size_) {
        ESP_LOGW(TAG, "Pool size decreased from %u to %u and data (%u bytes) doesn't fit! Clearing pool.",
                 stored_pool_size, this->pool_size_, this->pool_used_);
        needs_clear = true;
      } else {
        ESP_LOGW(TAG, "Pool size decreased from %u to %u. Data (%u bytes) fits, preserving.", stored_pool_size,
                 this->pool_size_, this->pool_used_);
      }
    }
  }

  if (!needs_clear) {
    ESP_LOGD(TAG, "FRAM preferences pool restored successfully");
    float usage_percent = (this->pool_used_ * 100.0f) / this->pool_size_;
    ESP_LOGD(TAG, "Pool usage: %u/%u bytes (%.1f%%)", this->pool_used_, this->pool_size_, usage_percent);

    // Warn if pool is too small (over 90% at startup)
    if (usage_percent > 90.0f) {
      ESP_LOGW(TAG, "Pool is %.0f%% full! Consider increasing pool_size", usage_percent);
    } else if (usage_percent > 80.0f) {
      ESP_LOGW(TAG, "Pool is %.0f%% full. Consider increasing pool_size soon", usage_percent);
      this->warned_80_percent_ = true;
    }
  }

  if (needs_clear) {
    this->pool_cleared_ = true;
    ESP_LOGI(TAG, "Clearing pool area...");
    // Clear the pool area by writing zeros
    for (uint32_t i = 0; i < this->pool_size_; i += 32) {
      uint8_t zeros[32] = {0};
      uint32_t len = (i + 32 > this->pool_size_) ? (this->pool_size_ - i) : 32;
      this->fram_->write_bytes(this->pool_start_ + i, zeros, len);
    }
    // Write magic, version, and pool_size
    uint32_t value = this->magic_;
    this->fram_->write_bytes(this->pool_start_, (uint8_t *) &value, 4);
    this->fram_->write_bytes(this->pool_start_ + 4, &this->version_, 1);
    this->fram_->write_bytes(this->pool_start_ + 5, (uint8_t *) &this->pool_size_, 4);

    // Verify write
    uint32_t verify_magic = 0;
    this->fram_->read_bytes(this->pool_start_, (uint8_t *) &verify_magic, 4);
    ESP_LOGVV(TAG, "Verify magic after write: 0x%08X", verify_magic);

    // Verify first key slot is zero
    uint32_t verify_key = 0;
    this->fram_->read_bytes(this->pool_start_ + POOL_HEADER_SIZE, (uint8_t *) &verify_key, 4);
    ESP_LOGVV(TAG, "Verify first key slot: 0x%08X", verify_key);

    this->pool_used_ = POOL_HEADER_SIZE;
  }

  this->initialized_ = true;
}

uint32_t FRAMPreferenceBackend::find_key_(uint32_t key_hash) {
  // Ensure pool is initialized before searching
  this->comp_->ensure_initialized_();

  if (!this->comp_->initialized_) {
    ESP_LOGW(TAG, "find_key_: Pool not initialized");
    return 0;
  }

  uint32_t addr = this->comp_->pool_start_ + POOL_HEADER_SIZE;
  uint32_t end = this->comp_->pool_start_ + this->comp_->pool_size_;
  uint32_t iterations = 0;
  const uint32_t max_iterations = 100;  // Safety limit

  ESP_LOGV(TAG, "find_key_: looking for 0x%08X, pool_start=%u, pool_size=%u", key_hash, this->comp_->pool_start_,
           this->comp_->pool_size_);

  while (addr < end && iterations < max_iterations) {
    iterations++;
    uint32_t key_from_fram = 0;
    this->comp_->fram_->read_bytes(addr, (uint8_t *) &key_from_fram, 4);

    if (key_from_fram == key_hash) {
      ESP_LOGV(TAG, "Found key 0x%08X at addr %u (iteration %u)", key_hash, addr, iterations);
      return addr;
    }

    if (key_from_fram == 0) {
      ESP_LOGV(TAG, "Empty slot at addr %u, storing key 0x%08X (iteration %u)", addr, key_hash, iterations);
      this->comp_->fram_->write_bytes(addr, (uint8_t *) &key_hash, 4);
      return addr;
    }

    uint32_t size_from_fram = 0;
    this->comp_->fram_->read_bytes(addr + 4, (uint8_t *) &size_from_fram, 4);

    // Safety check: if size is unreasonably large, clear remaining pool and use this slot
    if (size_from_fram > 1024 || size_from_fram == 0xFFFFFFFF) {
      ESP_LOGW(TAG, "Invalid size %u at addr %u, clearing remaining pool", size_from_fram, addr);
      // Clear from this address to end of pool
      for (uint32_t i = addr; i < end; i += 32) {
        uint8_t zeros[32] = {0};
        uint32_t len = (i + 32 > end) ? (end - i) : 32;
        this->comp_->fram_->write_bytes(i, zeros, len);
      }
      // Now use this slot
      this->comp_->fram_->write_bytes(addr, (uint8_t *) &key_hash, 4);
      return addr;
    }

    addr += 8 + size_from_fram + 4;  // key + size + data + crc
  }

  if (iterations >= max_iterations) {
    ESP_LOGW(TAG, "Max iterations reached in find_key_");
  }

  return 0;
}

bool FRAMPreferenceBackend::save(const uint8_t *data, size_t len) {
  if (!this->comp_->fram_->is_connected()) {
    ESP_LOGW(TAG, "Save: FRAM not connected");
    return false;
  }

  uint32_t key_hash = fnv1_hash(std::to_string(this->type_));
  ESP_LOGV(TAG, "Save: type=%u, key_hash=0x%08X, len=%u", this->type_, key_hash, len);

  uint32_t addr = this->find_key_(key_hash);

  if (addr == 0) {
    ESP_LOGW(TAG, "Save: Could not find/allocate slot for key 0x%08X (pool may be full)", key_hash);
    return false;
  }

  // Check if this is an update (key already exists at this address)
  uint32_t existing_key = 0;
  this->comp_->fram_->read_bytes(addr, (uint8_t *) &existing_key, 4);
  bool is_update = (existing_key == key_hash);
  ESP_LOGV(TAG, "Save: addr=%u, existing_key=0x%08X, is_update=%d", addr, existing_key, is_update);

  // Use FNV-1a hash for data integrity (CRC32 not available in ESPHome)
  uint32_t hash = FNV1_OFFSET_BASIS;
  for (size_t i = 0; i < len; i++) {
    hash ^= data[i];
    hash *= FNV1_PRIME;
  }

  // Combine size + data + hash into a single buffer for one I2C transaction
  // This reduces I2C overhead and prevents blocking for too long
  std::vector<uint8_t> buffer;
  buffer.reserve(4 + len + 4);
  // Size (4 bytes)
  buffer.push_back(len & 0xFF);
  buffer.push_back((len >> 8) & 0xFF);
  buffer.push_back((len >> 16) & 0xFF);
  buffer.push_back((len >> 24) & 0xFF);
  // Data
  for (size_t i = 0; i < len; i++) {
    buffer.push_back(data[i]);
  }
  // Hash (4 bytes)
  buffer.push_back(hash & 0xFF);
  buffer.push_back((hash >> 8) & 0xFF);
  buffer.push_back((hash >> 16) & 0xFF);
  buffer.push_back((hash >> 24) & 0xFF);

  ESP_LOGVV(TAG, "Save: Writing to addr %u, hash=0x%08X", addr, hash);
  this->comp_->fram_->write_bytes(addr + 4, buffer.data(), buffer.size());

  // Update pool usage tracking
  uint32_t entry_size = 4 + 4 + len + 4;  // key + size + data + hash
  uint32_t new_used = (addr - this->comp_->pool_start_) + entry_size;
  if (new_used > this->comp_->pool_used_) {
    this->comp_->pool_used_ = new_used;
  }

  // Check for 80% warning
  float usage_percent = (this->comp_->pool_used_ * 100.0f) / this->comp_->pool_size_;
  if (usage_percent > 80.0f && !this->comp_->warned_80_percent_) {
    ESP_LOGW(TAG, "Pool is %.0f%% full (%u/%u bytes). Consider increasing pool_size", usage_percent,
             this->comp_->pool_used_, this->comp_->pool_size_);
    this->comp_->warned_80_percent_ = true;
  }

  return true;
}

bool FRAMPreferenceBackend::load(uint8_t *data, size_t len) {
  if (!this->comp_->fram_->is_connected()) {
    ESP_LOGW(TAG, "Load: FRAM not connected");
    return false;
  }

  uint32_t key_hash = fnv1_hash(std::to_string(this->type_));
  ESP_LOGV(TAG, "Load: type=%u, key_hash=0x%08X, len=%u", this->type_, key_hash, len);

  uint32_t addr = this->find_key_(key_hash);

  if (addr == 0) {
    ESP_LOGV(TAG, "Load: Key 0x%08X not found", key_hash);
    return false;
  }

  uint32_t size_from_fram = 0;
  this->comp_->fram_->read_bytes(addr + 4, (uint8_t *) &size_from_fram, 4);
  ESP_LOGVV(TAG, "Load: addr=%u, size_from_fram=%u, expected=%u", addr, size_from_fram, len);

  if (size_from_fram != len) {
    ESP_LOGVV(TAG, "Load: Size mismatch (got %u, expected %u) - key may be from old config", size_from_fram, len);
    return false;
  }

  this->comp_->fram_->read_bytes(addr + 8, data, len);
  uint32_t hash_from_fram = 0;
  this->comp_->fram_->read_bytes(addr + 8 + len, (uint8_t *) &hash_from_fram, 4);
  // Use FNV-1a hash for data integrity (CRC32 not available in ESPHome)
  uint32_t hash = FNV1_OFFSET_BASIS;
  for (size_t i = 0; i < len; i++) {
    hash ^= data[i];
    hash *= FNV1_PRIME;
  }

  ESP_LOGVV(TAG, "Load: computed hash=0x%08X, stored hash=0x%08X, match=%d", hash, hash_from_fram,
            hash == hash_from_fram);
  return hash == hash_from_fram;
}

void FramPref::setup() {
  ESP_LOGVV(TAG, "Setup: pool_start=%u, pool_size=%u", this->pool_start_, this->pool_size_);

  // Initialize the pool
  this->ensure_initialized_();

  // Set global preferences pointer
  global_preferences = this;

  ESP_LOGVV(TAG, "Setup complete, initialized=%d", this->initialized_);
}

void FramPref::dump_config() {
  ESP_LOGCONFIG(TAG, "FRAM Preferences:");
  ESP_LOGCONFIG(TAG, "  Pool start: %u", this->pool_start_);
  ESP_LOGCONFIG(TAG, "  Pool size: %u bytes", this->pool_size_);
  if (this->pool_used_ > 0) {
    float usage_percent = (this->pool_used_ * 100.0f) / this->pool_size_;
    ESP_LOGCONFIG(TAG, "  Pool used: %u bytes (%.1f%%)", this->pool_used_, usage_percent);
  }
  if (this->pool_cleared_) {
    ESP_LOGCONFIG(TAG, "  Pool was cleared");
  }

  // Count number of keys in pool
  uint32_t key_count = 0;
  uint32_t addr = this->pool_start_ + POOL_HEADER_SIZE;
  uint32_t end = this->pool_start_ + this->pool_size_;
  while (addr < end) {
    uint32_t key = 0;
    this->fram_->read_bytes(addr, (uint8_t *) &key, 4);
    if (key == 0) {
      break;  // End of used pool
    }
    key_count++;
    uint32_t size = 0;
    this->fram_->read_bytes(addr + 4, (uint8_t *) &size, 4);
    if (size > 1024 || size == 0xFFFFFFFF) {
      break;  // Invalid size
    }
    addr += 8 + size + 4;  // key + size + data + hash
  }
  ESP_LOGCONFIG(TAG, "  Keys stored: %u", key_count);
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
  // Clear the entire pool area
  for (uint32_t i = 0; i < this->pool_size_; i += 32) {
    uint8_t zeros[32] = {0};
    uint32_t len = (i + 32 > this->pool_size_) ? (this->pool_size_ - i) : 32;
    this->fram_->write_bytes(this->pool_start_ + i, zeros, len);
  }
  // Write magic, version, and pool_size
  uint32_t value = this->magic_;
  this->fram_->write_bytes(this->pool_start_, (uint8_t *) &value, 4);
  this->fram_->write_bytes(this->pool_start_ + 4, &this->version_, 1);
  this->fram_->write_bytes(this->pool_start_ + 5, (uint8_t *) &this->pool_size_, 4);
  this->pool_used_ = POOL_HEADER_SIZE;
  this->warned_80_percent_ = false;
  ESP_LOGD(TAG, "Factory reset: FRAM preferences cleared");
  return true;
}

}  // namespace fram_pref
}  // namespace esphome
