#include "nvm.h"
#include "esphome/core/log.h"

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

// ============== NvmPartition ==============

NvmPartition::NvmPartition(NvmPlatform *parent, const PartitionConfig &config) : parent_(parent), config_(config) {}

bool NvmPartition::read(uint32_t offset, uint8_t *data, size_t len) {
  if (offset + len > config_.size) {
    ESP_LOGE(TAG, "Read out of bounds: offset=%u, len=%zu, size=%u", offset, len, config_.size);
    return false;
  }
  return parent_->read_bytes(config_.offset + offset, data, len);
}

bool NvmPartition::write(uint32_t offset, const uint8_t *data, size_t len) {
  if (offset + len > config_.size) {
    ESP_LOGE(TAG, "Write out of bounds: offset=%u, len=%zu, size=%u", offset, len, config_.size);
    return false;
  }
  return parent_->write_bytes(config_.offset + offset, data, len);
}

// ============== NvmPlatform ==============

NvmPartition *NvmPlatform::add_partition(const PartitionConfig &config) {
  if (!this->validate_partition_config(config)) {
    return nullptr;
  }

  // Check for duplicate ID
  for (const auto &part : partitions_) {
    if (part->get_id() == config.id) {
      ESP_LOGE(TAG, "Partition with ID '%s' already exists", config.id.c_str());
      return nullptr;
    }
  }

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

  NvmPartition *ptr = partition.get();
  partitions_.push_back(std::move(partition));

  ESP_LOGD(TAG, "Added partition '%s' (type=%s, offset=%u, size=%u)", config.id.c_str(),
           partition_type_to_string(config.type), config.offset, config.size);

  return ptr;
}

NvmPartition *NvmPlatform::get_partition(const std::string &id) {
  for (auto &part : partitions_) {
    if (part->get_id() == id) {
      return part.get();
    }
  }
  return nullptr;
}

bool NvmPlatform::validate_partition_config(const PartitionConfig &config) {
  // Check offset alignment (optional, could be relaxed)
  // Check size
  if (config.size == 0) {
    ESP_LOGE(TAG, "Partition size must be > 0");
    return false;
  }

  // Check bounds
  if (config.offset + config.size > this->get_total_size()) {
    ESP_LOGE(TAG, "Partition extends beyond NVM device: offset=%u, size=%u, total=%u", config.offset, config.size,
             this->get_total_size());
    return false;
  }

  // Check for overlap with existing partitions
  for (const auto &part : partitions_) {
    uint32_t existing_start = part->get_offset();
    uint32_t existing_end = existing_start + part->get_size();
    uint32_t new_start = config.offset;
    uint32_t new_end = new_start + config.size;

    if (new_start < existing_end && new_end > existing_start) {
      ESP_LOGE(TAG, "Partition '%s' overlaps with existing partition '%s'", config.id.c_str(), part->get_id().c_str());
      return false;
    }
  }

  return true;
}

void NvmPlatform::dump_config() {
  ESP_LOGCONFIG(TAG, "NVM Platform:");
  ESP_LOGCONFIG(TAG, "  Total size: %u bytes", this->get_total_size());
  ESP_LOGCONFIG(TAG, "  Partitions: %zu", partitions_.size());

  for (const auto &part : partitions_) {
    ESP_LOGCONFIG(TAG, "    '%s': type=%s, offset=0x%04X, size=%u bytes", part->get_id().c_str(),
                  partition_type_to_string(part->get_type()), part->get_offset(), part->get_size());
  }
}

// ============== KeyValuePartition ==============

int KeyValuePartition::get(const std::string &key, uint8_t *value, size_t max_len) {
  if (key.empty() || key.length() > 255) {
    ESP_LOGE(TAG, "Invalid key length");
    return -1;
  }

  // Scan through partition looking for key
  uint32_t offset = 0;
  while (offset < this->get_size()) {
    uint8_t key_len;
    if (!this->read(offset, &key_len, 1)) {
      return -1;
    }

    // Check for end marker (key_len == 0)
    if (key_len == 0) {
      break;
    }

    // Read key
    if (key_len == key.length()) {
      char stored_key[256];
      if (!this->read(offset + 1, reinterpret_cast<uint8_t *>(stored_key), key_len)) {
        return -1;
      }
      stored_key[key_len] = '\0';

      if (key == stored_key) {
        // Found key, read value length
        uint16_t value_len;
        if (!this->read(offset + 1 + key_len, reinterpret_cast<uint8_t *>(&value_len), 2)) {
          return -1;
        }

        // Read value
        size_t read_len = std::min(static_cast<size_t>(value_len), max_len);
        if (!this->read(offset + 1 + key_len + 2, value, read_len)) {
          return -1;
        }

        return read_len;
      }
    }

    // Skip to next entry
    uint16_t value_len;
    if (!this->read(offset + 1 + key_len, reinterpret_cast<uint8_t *>(&value_len), 2)) {
      return -1;
    }
    offset += 1 + key_len + 2 + value_len;
  }

  return -1;  // Key not found
}

bool KeyValuePartition::set(const std::string &key, const uint8_t *value, size_t len) {
  if (key.empty() || key.length() > 255) {
    ESP_LOGE(TAG, "Invalid key length");
    return false;
  }

  if (len > 65535) {
    ESP_LOGE(TAG, "Value too long");
    return false;
  }

  // First, try to find and delete existing key
  this->erase(key);

  // Find end of existing data
  uint32_t offset = 0;
  while (offset < this->get_size()) {
    uint8_t key_len;
    if (!this->read(offset, &key_len, 1)) {
      return false;
    }

    if (key_len == 0) {
      break;  // Found end
    }

    uint16_t value_len;
    if (!this->read(offset + 1 + key_len, reinterpret_cast<uint8_t *>(&value_len), 2)) {
      return false;
    }
    offset += 1 + key_len + 2 + value_len;
  }

  // Check if we have space
  size_t entry_size = 1 + key.length() + 2 + len;
  if (offset + entry_size + 1 > this->get_size()) {  // +1 for end marker
    ESP_LOGE(TAG, "Not enough space for key-value entry");
    return false;
  }

  // Write key length
  uint8_t key_len = static_cast<uint8_t>(key.length());
  if (!this->write(offset, &key_len, 1)) {
    return false;
  }

  // Write key
  if (!this->write(offset + 1, reinterpret_cast<const uint8_t *>(key.c_str()), key_len)) {
    return false;
  }

  // Write value length
  uint16_t value_len = static_cast<uint16_t>(len);
  if (!this->write(offset + 1 + key_len, reinterpret_cast<uint8_t *>(&value_len), 2)) {
    return false;
  }

  // Write value
  if (!this->write(offset + 1 + key_len + 2, value, len)) {
    return false;
  }

  // Write end marker
  uint8_t end_marker = 0;
  if (!this->write(offset + entry_size, &end_marker, 1)) {
    return false;
  }

  return true;
}

bool KeyValuePartition::erase(const std::string &key) {
  if (key.empty()) {
    return false;
  }

  // Find key
  uint32_t offset = 0;
  uint32_t entry_start = 0;
  bool found = false;

  while (offset < this->get_size()) {
    entry_start = offset;
    uint8_t key_len;
    if (!this->read(offset, &key_len, 1)) {
      return false;
    }

    if (key_len == 0) {
      break;
    }

    if (key_len == key.length()) {
      char stored_key[256];
      if (!this->read(offset + 1, reinterpret_cast<uint8_t *>(stored_key), key_len)) {
        return false;
      }
      stored_key[key_len] = '\0';

      if (key == stored_key) {
        found = true;
        break;
      }
    }

    uint16_t value_len;
    if (!this->read(offset + 1 + key_len, reinterpret_cast<uint8_t *>(&value_len), 2)) {
      return false;
    }
    offset += 1 + key_len + 2 + value_len;
  }

  if (!found) {
    return true;  // Key doesn't exist, nothing to erase
  }

  // Calculate entry size
  uint8_t key_len;
  this->read(entry_start, &key_len, 1);
  uint16_t value_len;
  this->read(entry_start + 1 + key_len, reinterpret_cast<uint8_t *>(&value_len), 2);
  size_t entry_size = 1 + key_len + 2 + value_len;

  // Read rest of partition
  uint32_t next_offset = entry_start + entry_size;
  std::vector<uint8_t> remaining_data;

  // Find end of data
  uint32_t end_offset = next_offset;
  while (end_offset < this->get_size()) {
    uint8_t next_key_len;
    this->read(end_offset, &next_key_len, 1);
    if (next_key_len == 0) {
      break;
    }
    uint16_t next_value_len;
    this->read(end_offset + 1 + next_key_len, reinterpret_cast<uint8_t *>(&next_value_len), 2);
    end_offset += 1 + next_key_len + 2 + next_value_len;
  }

  // Read remaining data
  size_t remaining_size = end_offset - next_offset;
  if (remaining_size > 0) {
    remaining_data.resize(remaining_size);
    this->read(next_offset, remaining_data.data(), remaining_size);
  }

  // Write remaining data over deleted entry
  if (remaining_size > 0) {
    this->write(entry_start, remaining_data.data(), remaining_size);
  }

  // Write end marker
  uint8_t end_marker = 0;
  this->write(entry_start + remaining_size, &end_marker, 1);

  return true;
}

bool KeyValuePartition::has_key(const std::string &key) {
  uint8_t dummy;
  return this->get(key, &dummy, 1) >= 0;
}

}  // namespace nvm
}  // namespace esphome
