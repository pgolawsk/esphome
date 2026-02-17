#include "nvm_pref.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include <memory>

namespace esphome {
namespace nvm {

static const char *const TAG = "nvm_pref";

// ========== NvmPreferences ==========

void NvmPreferences::setup() {
  ESP_LOGVV(TAG, "Setup: partition=%p", this->partition_);

  if (this->partition_ == nullptr) {
    ESP_LOGE(TAG, "No partition configured");
    this->mark_failed();
    return;
  }

  // Initialize the pool
  this->ensure_initialized_();

  // Save the original NVS preferences before replacing
  // This is needed to delegate the boot counter key which must stay in NVS
  // because safe_mode reads it before NVM preferences is initialized
  this->nvs_preferences_ = global_preferences;

  // Set global preferences pointer
  global_preferences = this;

  ESP_LOGVV(TAG, "Setup complete, initialized=%d", this->initialized_);
}

void NvmPreferences::dump_config() {
  ESP_LOGCONFIG(TAG, "NVM Preferences:");
  if (this->partition_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Partition: %s", this->partition_->get_id().c_str());
    ESP_LOGCONFIG(TAG, "  Size: %u bytes", this->partition_->get_size());
    if (this->pool_used_ > 0) {
      float usage_percent = (this->pool_used_ * 100.0f) / this->partition_->get_size();
      ESP_LOGCONFIG(TAG, "  Used: %u bytes (%.1f%%)", this->pool_used_, usage_percent);
    }
    if (this->pool_cleared_) {
      ESP_LOGCONFIG(TAG, "  Pool was cleared");
    }
    ESP_LOGCONFIG(TAG, "  Boot counter: delegated to NVS");
  }
}

void NvmPreferences::ensure_initialized_() {
  if (this->initialized_) {
    return;
  }

  uint32_t pool_size = this->partition_->get_size();
  ESP_LOGVV(TAG, "Lazy initialization - pool_size=%u", pool_size);

  uint32_t magic = 0;
  this->partition_->read(0, reinterpret_cast<uint8_t *>(&magic), 4);
  ESP_LOGVV(TAG, "Read magic: 0x%08X, expected: 0x%08X", magic, MAGIC);

  bool needs_clear = false;

  if (magic != MAGIC) {
    ESP_LOGW(TAG, "Magic mismatch, initializing pool");
    needs_clear = true;
  } else {
    uint8_t version_from_nvm = 0;
    this->partition_->read(4, &version_from_nvm, 1);
    ESP_LOGVV(TAG, "Version: %u, expected: %u", version_from_nvm, VERSION);
    if (version_from_nvm != VERSION) {
      ESP_LOGW(TAG, "NVM preferences version mismatch. Clearing preferences.");
      needs_clear = true;
    } else {
      // Validate first key slot - should be 0 or a valid key with reasonable size
      uint32_t first_key = 0;
      uint32_t first_size = 0;
      this->partition_->read(POOL_HEADER_SIZE, reinterpret_cast<uint8_t *>(&first_key), 4);
      this->partition_->read(POOL_HEADER_SIZE + 4, reinterpret_cast<uint8_t *>(&first_size), 4);
      ESP_LOGVV(TAG, "First key at offset %u: 0x%08X, size: %u", POOL_HEADER_SIZE, first_key, first_size);

      // If first key is non-zero, validate the size is reasonable
      if (first_key != 0) {
        // Size should be reasonable (less than pool size and not garbage)
        if (first_size > pool_size || first_size > 10000) {
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
    this->partition_->read(5, reinterpret_cast<uint8_t *>(&stored_pool_size), 4);
    ESP_LOGVV(TAG, "Stored pool size: %u, current: %u, used: %u", stored_pool_size, pool_size, this->pool_used_);

    if (stored_pool_size != 0 && pool_size < stored_pool_size) {
      // Pool size decreased - check if data still fits
      if (this->pool_used_ > pool_size) {
        ESP_LOGW(TAG, "Pool size decreased from %u to %u and data (%u bytes) doesn't fit! Clearing pool.",
                 stored_pool_size, pool_size, this->pool_used_);
        needs_clear = true;
      } else {
        ESP_LOGW(TAG, "Pool size decreased from %u to %u. Data (%u bytes) fits, preserving.", stored_pool_size,
                 pool_size, this->pool_used_);
      }
    }
  }

  if (!needs_clear) {
    ESP_LOGD(TAG, "NVM preferences pool restored successfully");
    float usage_percent = (this->pool_used_ * 100.0f) / pool_size;
    ESP_LOGD(TAG, "Pool usage: %u/%u bytes (%.1f%%)", this->pool_used_, pool_size, usage_percent);

    // Warn if pool is too small (over 90% at startup)
    if (usage_percent > 90.0f) {
      ESP_LOGW(TAG, "Pool is %.0f%% full! Consider increasing partition size", usage_percent);
    } else if (usage_percent > 80.0f) {
      ESP_LOGW(TAG, "Pool is %.0f%% full. Consider increasing partition size soon", usage_percent);
      this->warned_80_percent_ = true;
    }
  }

  if (needs_clear) {
    this->pool_cleared_ = true;
    ESP_LOGI(TAG, "Clearing pool area...");
    // Clear the pool area by writing zeros
    for (uint32_t i = 0; i < pool_size; i += 32) {
      std::array<uint8_t, 32> zeros{};
      uint32_t len = (i + 32 > pool_size) ? (pool_size - i) : 32;
      this->partition_->write(i, zeros.data(), len);
    }
    // Write magic, version, and pool_size
    uint32_t value = MAGIC;
    this->partition_->write(0, reinterpret_cast<uint8_t *>(&value), 4);
    this->partition_->write(4, &VERSION, 1);
    this->partition_->write(5, reinterpret_cast<uint8_t *>(&pool_size), 4);

    // Verify write
    uint32_t verify_magic = 0;
    this->partition_->read(0, reinterpret_cast<uint8_t *>(&verify_magic), 4);
    ESP_LOGVV(TAG, "Verify magic after write: 0x%08X", verify_magic);

    // Verify first key slot is zero
    uint32_t verify_key = 0;
    this->partition_->read(POOL_HEADER_SIZE, reinterpret_cast<uint8_t *>(&verify_key), 4);
    ESP_LOGVV(TAG, "Verify first key slot: 0x%08X", verify_key);

    this->pool_used_ = POOL_HEADER_SIZE;
  }

  this->initialized_ = true;
}

uint32_t NvmPreferences::calculate_pool_used_() {
  uint32_t pool_size = this->partition_->get_size();
  uint32_t addr = POOL_HEADER_SIZE;
  uint32_t iterations = 0;
  const uint32_t max_iterations = 100;

  while (addr < pool_size && iterations < max_iterations) {
    iterations++;
    uint32_t key = 0;
    this->partition_->read(addr, reinterpret_cast<uint8_t *>(&key), 4);

    if (key == 0) {
      // Empty slot - this is where the pool ends
      return addr;
    }

    uint32_t size = 0;
    this->partition_->read(addr + 4, reinterpret_cast<uint8_t *>(&size), 4);

    // Invalid size - return current position
    if (size > 1024 || size == 0xFFFFFFFF) {
      return addr;
    }

    addr += 8 + size + 4;  // key + size + data + hash
  }

  return addr;
}

ESPPreferenceObject NvmPreferences::make_preference(size_t length, uint32_t type, bool in_flash) {
  return this->make_preference(length, type);
}

ESPPreferenceObject NvmPreferences::make_preference(size_t length, uint32_t type) {
  // Delegate boot counter to NVS - safe_mode reads it before NVM is initialized
  if (type == safe_mode::RTC_KEY && this->nvs_preferences_ != nullptr) {
    ESP_LOGV(TAG, "Delegating boot counter key %u to NVS", type);
    return this->nvs_preferences_->make_preference(length, type);
  }
  return ESPPreferenceObject(new NvmPreferenceBackend(this, type));
}

bool NvmPreferences::sync() {
  // Also sync NVS preferences (for delegated keys like boot counter)
  if (this->nvs_preferences_ != nullptr) {
    this->nvs_preferences_->sync();
  }
  return true;
}

bool NvmPreferences::reset() {
  if (this->partition_ == nullptr) {
    return false;
  }

  this->pool_cleared_ = true;
  uint32_t pool_size = this->partition_->get_size();

  // Clear the entire pool area
  for (uint32_t i = 0; i < pool_size; i += 32) {
    std::array<uint8_t, 32> zeros{};
    uint32_t len = (i + 32 > pool_size) ? (pool_size - i) : 32;
    this->partition_->write(i, zeros.data(), len);
  }
  // Write magic, version, and pool_size
  uint32_t value = MAGIC;
  this->partition_->write(0, reinterpret_cast<uint8_t *>(&value), 4);
  this->partition_->write(4, &VERSION, 1);
  this->partition_->write(5, reinterpret_cast<uint8_t *>(&pool_size), 4);
  this->pool_used_ = POOL_HEADER_SIZE;
  this->warned_80_percent_ = false;
  ESP_LOGD(TAG, "Factory reset: NVM preferences cleared");
  return true;
}

// ========== NvmPreferenceBackend ==========

uint32_t NvmPreferenceBackend::find_key_(uint32_t key_hash) {
  // Ensure pool is initialized before searching
  this->prefs_->ensure_initialized_();

  if (!this->prefs_->initialized_) {
    ESP_LOGW(TAG, "find_key_: Pool not initialized");
    return 0;
  }

  uint32_t pool_size = this->prefs_->partition_->get_size();
  uint32_t addr = NvmPreferences::POOL_HEADER_SIZE;
  uint32_t iterations = 0;
  const uint32_t max_iterations = 100;  // Safety limit

  ESP_LOGV(TAG, "find_key_: looking for 0x%08X, pool_size=%u", key_hash, pool_size);

  while (addr < pool_size && iterations < max_iterations) {
    iterations++;
    uint32_t key_from_nvm = 0;
    this->prefs_->partition_->read(addr, reinterpret_cast<uint8_t *>(&key_from_nvm), 4);

    if (key_from_nvm == key_hash) {
      ESP_LOGV(TAG, "Found key 0x%08X at offset %u (iteration %u)", key_hash, addr, iterations);
      return addr;
    }

    if (key_from_nvm == 0) {
      ESP_LOGV(TAG, "Empty slot at offset %u, storing key 0x%08X (iteration %u)", addr, key_hash, iterations);
      this->prefs_->partition_->write(addr, reinterpret_cast<uint8_t *>(&key_hash), 4);
      return addr;
    }

    uint32_t size_from_nvm = 0;
    this->prefs_->partition_->read(addr + 4, reinterpret_cast<uint8_t *>(&size_from_nvm), 4);

    // Safety check: if size is unreasonably large, clear remaining pool and use this slot
    if (size_from_nvm > 1024 || size_from_nvm == 0xFFFFFFFF) {
      ESP_LOGW(TAG, "Invalid size %u at offset %u, clearing remaining pool", size_from_nvm, addr);
      // Clear from this address to end of pool
      for (uint32_t i = addr; i < pool_size; i += 32) {
        std::array<uint8_t, 32> zeros{};
        uint32_t len = (i + 32 > pool_size) ? (pool_size - i) : 32;
        this->prefs_->partition_->write(i, zeros.data(), len);
      }
      // Now use this slot
      this->prefs_->partition_->write(addr, reinterpret_cast<uint8_t *>(&key_hash), 4);
      return addr;
    }

    addr += 8 + size_from_nvm + 4;  // key + size + data + crc
  }

  if (iterations >= max_iterations) {
    ESP_LOGW(TAG, "Max iterations reached in find_key_");
  }

  return 0;
}

bool NvmPreferenceBackend::save(const uint8_t *data, size_t len) {
  if (this->prefs_->partition_ == nullptr) {
    ESP_LOGW(TAG, "Save: No partition configured");
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
  this->prefs_->partition_->read(addr, reinterpret_cast<uint8_t *>(&existing_key), 4);
  bool is_update = (existing_key == key_hash);
  ESP_LOGV(TAG, "Save: addr=%u, existing_key=0x%08X, is_update=%d", addr, existing_key, is_update);

  // Use FNV-1a hash for data integrity
  uint32_t hash = FNV1_OFFSET_BASIS;
  for (size_t i = 0; i < len; i++) {
    hash ^= data[i];
    hash *= FNV1_PRIME;
  }

  // Write size + data + hash
  // Use unique_ptr for runtime-sized buffer (avoid STL vector overhead)
  auto buffer = std::make_unique<uint8_t[]>(4 + len + 4);
  // Size (4 bytes)
  buffer[0] = len & 0xFF;
  buffer[1] = (len >> 8) & 0xFF;
  buffer[2] = (len >> 16) & 0xFF;
  buffer[3] = (len >> 24) & 0xFF;
  // Data
  std::memcpy(buffer.get() + 4, data, len);
  // Hash (4 bytes)
  buffer[4 + len] = hash & 0xFF;
  buffer[4 + len + 1] = (hash >> 8) & 0xFF;
  buffer[4 + len + 2] = (hash >> 16) & 0xFF;
  buffer[4 + len + 3] = (hash >> 24) & 0xFF;

  ESP_LOGVV(TAG, "Save: Writing to offset %u, hash=0x%08X", addr, hash);
  this->prefs_->partition_->write(addr + 4, buffer.get(), 4 + len + 4);

  // Update pool usage tracking
  uint32_t entry_size = 4 + 4 + len + 4;  // key + size + data + hash
  uint32_t new_used = addr + entry_size;
  if (new_used > this->prefs_->pool_used_) {
    this->prefs_->pool_used_ = new_used;
  }

  // Check for 80% warning
  float usage_percent = (this->prefs_->pool_used_ * 100.0f) / this->prefs_->partition_->get_size();
  if (usage_percent > 80.0f && !this->prefs_->warned_80_percent_) {
    ESP_LOGW(TAG, "Pool is %.0f%% full (%u/%u bytes). Consider increasing partition size", usage_percent,
             this->prefs_->pool_used_, this->prefs_->partition_->get_size());
    this->prefs_->warned_80_percent_ = true;
  }

  return true;
}

bool NvmPreferenceBackend::load(uint8_t *data, size_t len) {
  if (this->prefs_->partition_ == nullptr) {
    ESP_LOGW(TAG, "Load: No partition configured");
    return false;
  }

  uint32_t key_hash = fnv1_hash(std::to_string(this->type_));
  ESP_LOGV(TAG, "Load: type=%u, key_hash=0x%08X, len=%u", this->type_, key_hash, len);

  uint32_t addr = this->find_key_(key_hash);

  if (addr == 0) {
    ESP_LOGV(TAG, "Load: Key 0x%08X not found", key_hash);
    return false;
  }

  uint32_t size_from_nvm = 0;
  this->prefs_->partition_->read(addr + 4, reinterpret_cast<uint8_t *>(&size_from_nvm), 4);
  ESP_LOGVV(TAG, "Load: addr=%u, size_from_nvm=%u, expected=%u", addr, size_from_nvm, len);

  if (size_from_nvm != len) {
    ESP_LOGVV(TAG, "Load: Size mismatch (got %u, expected %u) - key may be from old config", size_from_nvm, len);
    return false;
  }

  this->prefs_->partition_->read(addr + 8, data, len);
  uint32_t hash_from_nvm = 0;
  this->prefs_->partition_->read(addr + 8 + len, reinterpret_cast<uint8_t *>(&hash_from_nvm), 4);

  // Use FNV-1a hash for data integrity
  uint32_t hash = FNV1_OFFSET_BASIS;
  for (size_t i = 0; i < len; i++) {
    hash ^= data[i];
    hash *= FNV1_PRIME;
  }

  ESP_LOGVV(TAG, "Load: computed hash=0x%08X, stored hash=0x%08X, match=%d", hash, hash_from_nvm,
            hash == hash_from_nvm);
  return hash == hash_from_nvm;
}

}  // namespace nvm
}  // namespace esphome
