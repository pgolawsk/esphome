#include "nvm.h"
#include "esphome/core/log.h"

#include <array>

namespace esphome {
namespace nvm {

static const char *const TAG = "nvm";

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

}  // namespace nvm
}  // namespace esphome
