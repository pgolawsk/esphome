#include "nvm.h"
#include "esphome/core/log.h"

#include <array>

namespace esphome {
namespace nvm {

static const char *const TAG = "nvm";

// Static member definitions for PreferencesPartition
const uint32_t PreferencesPartition::MAGIC;
const uint8_t PreferencesPartition::VERSION;

const char *partition_type_to_string(PartitionType type) {
  switch (type) {
    case PartitionType::PREFERENCES:
      return "preferences";
    case PartitionType::RAW:
      return "raw";
    case PartitionType::KEY_VALUE:
      return "key_value";
    default:
      return "unknown";
  }
}

// ========== NvmPartition ==========

NvmPartition::NvmPartition(NvmPlatform *parent, const PartitionConfig &config) : parent_(parent), config_(config) {}

bool NvmPartition::read(uint32_t offset, uint8_t *data, size_t len) {
  if (offset + len > config_.size) {
    ESP_LOGE(TAG, "Partition '%s' read out of bounds: offset=%u, len=%zu, size=%u", config_.id.c_str(), offset, len,
             config_.size);
    return false;
  }
  return parent_->read_bytes(config_.offset + offset, data, len);
}

bool NvmPartition::write(uint32_t offset, const uint8_t *data, size_t len) {
  if (offset + len > config_.size) {
    ESP_LOGE(TAG, "Partition '%s' write out of bounds: offset=%u, len=%zu, size=%u", config_.id.c_str(), offset, len,
             config_.size);
    return false;
  }
  return parent_->write_bytes(config_.offset + offset, data, len);
}

// ========== NvmPlatform ==========

void NvmPlatform::setup() {
  // Calculate automatic offsets for partitions
  this->calculate_partition_offsets();

  // Validate all partitions
  for (const auto &partition : partitions_) {
    if (!this->validate_partition_config(partition->config_)) {
      this->mark_failed();
      return;
    }
  }

  ESP_LOGCONFIG(TAG, "NVM Platform initialized with %zu partitions", partitions_.size());
}

void NvmPlatform::dump_config() {
  ESP_LOGCONFIG(TAG, "NVM Platform:");
  ESP_LOGCONFIG(TAG, "  Total size: %u bytes", this->get_total_size());
  ESP_LOGCONFIG(TAG, "  Partitions: %zu", partitions_.size());

  for (const auto &partition : partitions_) {
    ESP_LOGCONFIG(TAG, "    '%s': type=%s, offset=0x%04X, size=%u bytes", partition->get_id().c_str(),
                  partition_type_to_string(partition->get_type()), partition->get_offset(), partition->get_size());
  }
}

NvmPartition *NvmPlatform::add_partition(const PartitionConfig &config) {
  // Create appropriate partition type
  std::unique_ptr<NvmPartition> partition;

  switch (config.type) {
    case PartitionType::PREFERENCES:
      partition = std::make_unique<PreferencesPartition>(this, config);
      break;
    case PartitionType::RAW:
      partition = std::make_unique<RawPartition>(this, config);
      break;
    case PartitionType::KEY_VALUE:
      partition = std::make_unique<KeyValuePartition>(this, config);
      break;
    default:
      ESP_LOGE(TAG, "Unknown partition type: %d", static_cast<int>(config.type));
      return nullptr;
  }

  partitions_.push_back(std::move(partition));
  return partitions_.back().get();
}

NvmPartition *NvmPlatform::get_partition(const std::string &id) {
  for (const auto &partition : partitions_) {
    if (partition->get_id() == id) {
      return partition.get();
    }
  }
  return nullptr;
}

bool NvmPlatform::validate_partition_config(const PartitionConfig &config) {
  // Check size
  if (config.size == 0) {
    ESP_LOGE(TAG, "Partition '%s' has zero size", config.id.c_str());
    return false;
  }

  // Check bounds
  if (config.offset + config.size > this->get_total_size()) {
    ESP_LOGE(TAG, "Partition '%s' exceeds device size: offset=%u, size=%u, device_size=%u", config.id.c_str(),
             config.offset, config.size, this->get_total_size());
    return false;
  }

  // Check for overlap
  if (!this->check_partition_overlap(config)) {
    return false;
  }

  return true;
}

bool NvmPlatform::check_partition_overlap(const PartitionConfig &config) {
  for (const auto &partition : partitions_) {
    // Skip the partition being validated (for updates)
    if (partition->get_id() == config.id) {
      continue;
    }

    // Check for overlap
    uint32_t existing_end = partition->get_offset() + partition->get_size();
    uint32_t new_end = config.offset + config.size;

    if (config.offset < existing_end && new_end > partition->get_offset()) {
      ESP_LOGE(TAG, "Partition '%s' overlaps with partition '%s'", config.id.c_str(), partition->get_id().c_str());
      return false;
    }
  }
  return true;
}

void NvmPlatform::calculate_partition_offsets() {
  uint32_t current_offset = 0;

  for (auto &partition : partitions_) {
    // If offset is 0, auto-calculate
    if (partition->config_.offset == 0) {
      partition->config_.offset = current_offset;
    }

    // Update current offset for next partition
    current_offset = partition->config_.offset + partition->config_.size;
  }
}

// ========== KeyValuePartition ==========

int KeyValuePartition::get(const std::string &key, uint8_t *value, size_t max_len) {
  uint32_t offset, value_offset;
  uint16_t value_len;

  if (!this->find_key(key, offset, value_offset, value_len)) {
    return -1;  // Key not found
  }

  // Limit read length
  size_t read_len = std::min(static_cast<size_t>(value_len), max_len);
  if (!this->read(value_offset, value, read_len)) {
    return -1;
  }

  return static_cast<int>(read_len);
}

bool KeyValuePartition::set(const std::string &key, const uint8_t *value, size_t len) {
  // Check if key already exists
  uint32_t existing_offset, value_offset;
  uint16_t existing_len;

  if (this->find_key(key, existing_offset, value_offset, existing_len)) {
    // Key exists - check if new value fits
    if (len <= existing_len) {
      // Overwrite in place
      return this->write(value_offset, value, len);
    } else {
      // Need to erase and rewrite
      this->erase(key);
    }
  }

  // Find end of storage
  uint32_t write_offset = 0;
  uint8_t marker;
  while (write_offset < this->get_size()) {
    if (!this->read(write_offset, &marker, 1)) {
      break;
    }
    if (marker == 0xFF || marker == 0x00) {
      // Empty slot found
      break;
    }
    // Skip entry: key_len(1) + key + value_len(2) + value
    uint8_t key_len = marker;
    uint16_t value_len;
    this->read(write_offset + 1 + key_len, reinterpret_cast<uint8_t *>(&value_len), 2);
    write_offset += 1 + key_len + 2 + value_len;
  }

  // Check if we have space
  size_t entry_size = 1 + key.size() + 2 + len;
  if (write_offset + entry_size > this->get_size()) {
    ESP_LOGE(TAG, "KeyValue partition '%s' full, cannot store key '%s'", this->get_id().c_str(), key.c_str());
    return false;
  }

  // Write entry
  uint8_t key_len = static_cast<uint8_t>(key.size());
  uint16_t value_len = static_cast<uint16_t>(len);

  this->write(write_offset, &key_len, 1);
  this->write(write_offset + 1, reinterpret_cast<const uint8_t *>(key.c_str()), key.size());
  this->write(write_offset + 1 + key.size(), reinterpret_cast<uint8_t *>(&value_len), 2);
  this->write(write_offset + 1 + key.size() + 2, value, len);

  return true;
}

bool KeyValuePartition::erase(const std::string &key) {
  uint32_t offset, value_offset;
  uint16_t value_len;

  if (!this->find_key(key, offset, value_offset, value_len)) {
    return false;  // Key not found
  }

  // Mark as deleted by zeroing key length
  uint8_t zero = 0;
  this->write(offset, &zero, 1);

  return true;
}

bool KeyValuePartition::has_key(const std::string &key) {
  uint32_t offset, value_offset;
  uint16_t value_len;
  return this->find_key(key, offset, value_offset, value_len);
}

std::string KeyValuePartition::get_string(const std::string &key, const std::string &default_value) {
  // Use std::array for compile-time known size (avoid STL vector overhead)
  std::array<uint8_t, 256> buffer{};
  int len = this->get(key, buffer.data(), buffer.size());
  if (len < 0) {
    return default_value;
  }
  return std::string(buffer.begin(), buffer.begin() + len);
}

bool KeyValuePartition::set_string(const std::string &key, const std::string &value) {
  return this->set(key, reinterpret_cast<const uint8_t *>(value.c_str()), value.size());
}

bool KeyValuePartition::find_key(const std::string &key, uint32_t &offset, uint32_t &value_offset,
                                 uint16_t &value_len) {
  uint32_t current_offset = 0;

  while (current_offset < this->get_size()) {
    uint8_t key_len;
    if (!this->read(current_offset, &key_len, 1)) {
      break;
    }

    // Check for empty slot
    if (key_len == 0xFF || key_len == 0x00) {
      break;
    }

    // Check if key matches
    if (key_len == key.size()) {
      // Use unique_ptr for runtime-sized buffer (avoid STL vector overhead)
      auto stored_key = std::make_unique<uint8_t[]>(key_len);
      this->read(current_offset + 1, stored_key.get(), key_len);

      if (std::string(stored_key.get(), stored_key.get() + key_len) == key) {
        // Found it!
        offset = current_offset;
        this->read(current_offset + 1 + key_len, reinterpret_cast<uint8_t *>(&value_len), 2);
        value_offset = current_offset + 1 + key_len + 2;
        return true;
      }
    }

    // Skip to next entry
    uint16_t entry_value_len;
    this->read(current_offset + 1 + key_len, reinterpret_cast<uint8_t *>(&entry_value_len), 2);
    current_offset += 1 + key_len + 2 + entry_value_len;
  }

  return false;
}

void KeyValuePartition::compact() {
  // TODO: Implement compaction to remove deleted entries
  // This would read all valid entries, erase the partition, and rewrite them
}

// ========== PreferencesPartition ==========

void PreferencesPartition::setup() {
  ESP_LOGVV(TAG, "PreferencesPartition setup: size=%u", this->get_size());

  // Initialize the pool
  this->ensure_initialized_();

  // Save the original NVS preferences before replacing
  // This is needed to delegate the boot counter key which must stay in NVS
  // because safe_mode reads it before NVM preferences is initialized
  this->nvs_preferences_ = global_preferences;

  // Set global preferences pointer
  global_preferences = this;

  ESP_LOGVV(TAG, "PreferencesPartition setup complete, initialized=%d", this->initialized_);
}

void PreferencesPartition::dump_config() {
  ESP_LOGCONFIG(TAG, "Preferences Partition:");
  ESP_LOGCONFIG(TAG, "  Size: %u bytes", this->get_size());
  if (this->pool_used_ > 0) {
    float usage_percent = (this->pool_used_ * 100.0f) / this->get_size();
    ESP_LOGCONFIG(TAG, "  Used: %u bytes (%.1f%%)", this->pool_used_, usage_percent);
  }
  if (this->pool_cleared_) {
    ESP_LOGCONFIG(TAG, "  Pool was cleared");
  }
  ESP_LOGCONFIG(TAG, "  Boot counter: delegated to NVS");
}

ESPPreferenceObject PreferencesPartition::make_preference(size_t length, uint32_t type, bool in_flash) {
  return this->make_preference(length, type);
}

ESPPreferenceObject PreferencesPartition::make_preference(size_t length, uint32_t type) {
  // Delegate boot counter to NVS - safe_mode reads it before NVM is initialized
  if (type == safe_mode::RTC_KEY && this->nvs_preferences_ != nullptr) {
    ESP_LOGV(TAG, "Delegating boot counter key %u to NVS", type);
    return this->nvs_preferences_->make_preference(length, type);
  }
  return ESPPreferenceObject(new NvmPreferenceBackend(this, type));
}

bool PreferencesPartition::sync() {
  // Also sync NVS preferences (for delegated keys like boot counter)
  if (this->nvs_preferences_ != nullptr) {
    this->nvs_preferences_->sync();
  }
  return true;
}

bool PreferencesPartition::reset() {
  this->pool_cleared_ = true;
  uint32_t pool_size = this->get_size();

  // Clear the entire pool area
  for (uint32_t i = 0; i < pool_size; i += 32) {
    std::array<uint8_t, 32> zeros{};
    uint32_t len = (i + 32 > pool_size) ? (pool_size - i) : 32;
    this->write(i, zeros.data(), len);
  }
  // Write magic, version, and pool_size
  uint32_t value = MAGIC;
  this->write(0, reinterpret_cast<uint8_t *>(&value), 4);
  this->write(4, &VERSION, 1);
  this->write(5, reinterpret_cast<uint8_t *>(&pool_size), 4);
  this->pool_used_ = POOL_HEADER_SIZE;
  this->warned_80_percent_ = false;
  ESP_LOGD(TAG, "Factory reset: NVM preferences cleared");
  return true;
}

void PreferencesPartition::ensure_initialized_() {
  if (this->initialized_) {
    return;
  }

  uint32_t pool_size = this->get_size();
  ESP_LOGVV(TAG, "Lazy initialization - pool_size=%u", pool_size);

  uint32_t magic = 0;
  this->read(0, reinterpret_cast<uint8_t *>(&magic), 4);
  ESP_LOGVV(TAG, "Read magic: 0x%08X, expected: 0x%08X", magic, MAGIC);

  bool needs_clear = false;

  if (magic != MAGIC) {
    ESP_LOGW(TAG, "Magic mismatch, initializing pool");
    needs_clear = true;
  } else {
    uint8_t version_from_nvm = 0;
    this->read(4, &version_from_nvm, 1);
    ESP_LOGVV(TAG, "Version: %u, expected: %u", version_from_nvm, VERSION);
    if (version_from_nvm != VERSION) {
      ESP_LOGW(TAG, "NVM preferences version mismatch. Clearing preferences.");
      needs_clear = true;
    } else {
      // Validate first key slot - should be 0 or a valid key with reasonable size
      uint32_t first_key = 0;
      uint32_t first_size = 0;
      this->read(POOL_HEADER_SIZE, reinterpret_cast<uint8_t *>(&first_key), 4);
      this->read(POOL_HEADER_SIZE + 4, reinterpret_cast<uint8_t *>(&first_size), 4);
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
    this->read(5, reinterpret_cast<uint8_t *>(&stored_pool_size), 4);
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
      this->write(i, zeros.data(), len);
    }
    // Write magic, version, and pool_size
    uint32_t value = MAGIC;
    this->write(0, reinterpret_cast<uint8_t *>(&value), 4);
    this->write(4, &VERSION, 1);
    this->write(5, reinterpret_cast<uint8_t *>(&pool_size), 4);

    // Verify write
    uint32_t verify_magic = 0;
    this->read(0, reinterpret_cast<uint8_t *>(&verify_magic), 4);
    ESP_LOGVV(TAG, "Verify magic after write: 0x%08X", verify_magic);

    // Verify first key slot is zero
    uint32_t verify_key = 0;
    this->read(POOL_HEADER_SIZE, reinterpret_cast<uint8_t *>(&verify_key), 4);
    ESP_LOGVV(TAG, "Verify first key slot: 0x%08X", verify_key);

    this->pool_used_ = POOL_HEADER_SIZE;
  }

  this->initialized_ = true;
}

uint32_t PreferencesPartition::calculate_pool_used_() {
  uint32_t pool_size = this->get_size();
  uint32_t addr = POOL_HEADER_SIZE;
  uint32_t iterations = 0;
  const uint32_t max_iterations = 100;

  while (addr < pool_size && iterations < max_iterations) {
    iterations++;
    uint32_t key = 0;
    this->read(addr, reinterpret_cast<uint8_t *>(&key), 4);

    if (key == 0) {
      // Empty slot - this is where the pool ends
      return addr;
    }

    uint32_t size = 0;
    this->read(addr + 4, reinterpret_cast<uint8_t *>(&size), 4);

    // Invalid size - return current position
    if (size > 1024 || size == 0xFFFFFFFF) {
      return addr;
    }

    addr += 8 + size + 4;  // key + size + data + hash
  }

  return addr;
}

// ========== NvmPreferenceBackend ==========

uint32_t NvmPreferenceBackend::find_key_(uint32_t key_hash) {
  // Ensure pool is initialized before searching
  this->partition_->ensure_initialized_();

  if (!this->partition_->initialized_) {
    ESP_LOGW(TAG, "find_key_: Pool not initialized");
    return 0;
  }

  uint32_t pool_size = this->partition_->get_size();
  uint32_t addr = PreferencesPartition::POOL_HEADER_SIZE;
  uint32_t iterations = 0;
  const uint32_t max_iterations = 100;  // Safety limit

  ESP_LOGV(TAG, "find_key_: looking for 0x%08X, pool_size=%u", key_hash, pool_size);

  while (addr < pool_size && iterations < max_iterations) {
    iterations++;
    uint32_t key_from_nvm = 0;
    this->partition_->read(addr, reinterpret_cast<uint8_t *>(&key_from_nvm), 4);

    if (key_from_nvm == key_hash) {
      ESP_LOGV(TAG, "Found key 0x%08X at offset %u (iteration %u)", key_hash, addr, iterations);
      return addr;
    }

    if (key_from_nvm == 0) {
      ESP_LOGV(TAG, "Empty slot at offset %u, storing key 0x%08X (iteration %u)", addr, key_hash, iterations);
      this->partition_->write(addr, reinterpret_cast<uint8_t *>(&key_hash), 4);
      return addr;
    }

    uint32_t size_from_nvm = 0;
    this->partition_->read(addr + 4, reinterpret_cast<uint8_t *>(&size_from_nvm), 4);

    // Safety check: if size is unreasonably large, clear remaining pool and use this slot
    if (size_from_nvm > 1024 || size_from_nvm == 0xFFFFFFFF) {
      ESP_LOGW(TAG, "Invalid size %u at offset %u, clearing remaining pool", size_from_nvm, addr);
      // Clear from this address to end of pool
      for (uint32_t i = addr; i < pool_size; i += 32) {
        std::array<uint8_t, 32> zeros{};
        uint32_t len = (i + 32 > pool_size) ? (pool_size - i) : 32;
        this->partition_->write(i, zeros.data(), len);
      }
      // Now use this slot
      this->partition_->write(addr, reinterpret_cast<uint8_t *>(&key_hash), 4);
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
  uint32_t key_hash = fnv1_hash(std::to_string(this->type_));
  ESP_LOGV(TAG, "Save: type=%u, key_hash=0x%08X, len=%u", this->type_, key_hash, len);

  uint32_t addr = this->find_key_(key_hash);

  if (addr == 0) {
    ESP_LOGW(TAG, "Save: Could not find/allocate slot for key 0x%08X (pool may be full)", key_hash);
    return false;
  }

  // Check if this is an update (key already exists at this address)
  uint32_t existing_key = 0;
  this->partition_->read(addr, reinterpret_cast<uint8_t *>(&existing_key), 4);
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
  this->partition_->write(addr + 4, buffer.get(), 4 + len + 4);

  // Update pool usage tracking
  uint32_t entry_size = 4 + 4 + len + 4;  // key + size + data + hash
  uint32_t new_used = addr + entry_size;
  if (new_used > this->partition_->pool_used_) {
    this->partition_->pool_used_ = new_used;
  }

  // Check for 80% warning
  float usage_percent = (this->partition_->pool_used_ * 100.0f) / this->partition_->get_size();
  if (usage_percent > 80.0f && !this->partition_->warned_80_percent_) {
    ESP_LOGW(TAG, "Pool is %.0f%% full (%u/%u bytes). Consider increasing partition size", usage_percent,
             this->partition_->pool_used_, this->partition_->get_size());
    this->partition_->warned_80_percent_ = true;
  }

  return true;
}

bool NvmPreferenceBackend::load(uint8_t *data, size_t len) {
  uint32_t key_hash = fnv1_hash(std::to_string(this->type_));
  ESP_LOGV(TAG, "Load: type=%u, key_hash=0x%08X, len=%u", this->type_, key_hash, len);

  uint32_t addr = this->find_key_(key_hash);

  if (addr == 0) {
    ESP_LOGV(TAG, "Load: Key 0x%08X not found", key_hash);
    return false;
  }

  uint32_t size_from_nvm = 0;
  this->partition_->read(addr + 4, reinterpret_cast<uint8_t *>(&size_from_nvm), 4);
  ESP_LOGVV(TAG, "Load: addr=%u, size_from_nvm=%u, expected=%u", addr, size_from_nvm, len);

  if (size_from_nvm != len) {
    ESP_LOGVV(TAG, "Load: Size mismatch (got %u, expected %u) - key may be from old config", size_from_nvm, len);
    return false;
  }

  this->partition_->read(addr + 8, data, len);
  uint32_t hash_from_nvm = 0;
  this->partition_->read(addr + 8 + len, reinterpret_cast<uint8_t *>(&hash_from_nvm), 4);

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
