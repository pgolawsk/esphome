#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include <vector>
#include <memory>

namespace esphome {
namespace nvm {

/// Partition types supported by NVM
enum class PartitionType : uint8_t {
  PREFERENCES = 0,  ///< ESPHome preferences backend
  RAW = 1,          ///< Raw byte storage
  KEY_VALUE = 2,    ///< Key-value store
};

/// Convert PartitionType to string for logging
const char *partition_type_to_string(PartitionType type);

/// Partition configuration
struct PartitionConfig {
  std::string id;      ///< User-defined partition ID
  PartitionType type;  ///< Partition type
  uint32_t offset;     ///< Offset in NVM device
  uint32_t size;       ///< Size in bytes
};

/// Forward declaration
class NvmPlatform;

/// Base class for NVM partitions
class NvmPartition {
 public:
  NvmPartition(NvmPlatform *parent, const PartitionConfig &config);
  virtual ~NvmPartition() = default;

  /// Read data from partition
  /// @param offset Offset within partition (not absolute NVM offset)
  /// @param data Buffer to read into
  /// @param len Number of bytes to read
  /// @return true on success
  bool read(uint32_t offset, uint8_t *data, size_t len);

  /// Write data to partition
  /// @param offset Offset within partition (not absolute NVM offset)
  /// @param data Data to write
  /// @param len Number of bytes to write
  /// @return true on success
  bool write(uint32_t offset, const uint8_t *data, size_t len);

  /// Get partition ID
  const std::string &get_id() const { return config_.id; }

  /// Get partition type
  PartitionType get_type() const { return config_.type; }

  /// Get partition offset (absolute in NVM device)
  uint32_t get_offset() const { return config_.offset; }

  /// Get partition size
  uint32_t get_size() const { return config_.size; }

  /// Get parent NVM platform
  NvmPlatform *get_parent() const { return parent_; }

 protected:
  NvmPlatform *parent_;
  PartitionConfig config_;
};

/// Base class for NVM platforms (FRAM, EEPROM, etc.)
class NvmPlatform : public Component {
 public:
  NvmPlatform() = default;
  ~NvmPlatform() override = default;

  /// Read bytes from NVM device
  /// @param memaddr Absolute memory address
  /// @param data Buffer to read into
  /// @param len Number of bytes to read
  /// @return true on success
  virtual bool read_bytes(uint32_t memaddr, uint8_t *data, size_t len) = 0;

  /// Write bytes to NVM device
  /// @param memaddr Absolute memory address
  /// @param data Data to write
  /// @param len Number of bytes to write
  /// @return true on success
  virtual bool write_bytes(uint32_t memaddr, const uint8_t *data, size_t len) = 0;

  /// Get total size of NVM device in bytes
  virtual uint32_t get_total_size() const = 0;

  /// Add a partition to this NVM device
  /// @param config Partition configuration
  /// @return Pointer to created partition, or nullptr on failure
  NvmPartition *add_partition(const PartitionConfig &config);

  /// Get partition by ID
  /// @param id Partition ID
  /// @return Pointer to partition, or nullptr if not found
  NvmPartition *get_partition(const std::string &id);

  /// Get all partitions
  const std::vector<std::unique_ptr<NvmPartition>> &get_partitions() const { return partitions_; }

  /// Validate partition configuration
  /// @param config Partition configuration to validate
  /// @return true if valid
  bool validate_partition_config(const PartitionConfig &config);

  // ========== Component methods ==========
  void dump_config() override;

 protected:
  std::vector<std::unique_ptr<NvmPartition>> partitions_;
};

/// Specialized partition for preferences storage
class PreferencesPartition : public NvmPartition {
 public:
  using NvmPartition::NvmPartition;

  /// Get the ESPPreferences backend for this partition
  /// This is used by the preferences system to store/restore values
  /// @return Pointer to preferences backend
  void *get_preferences_backend() { return preferences_backend_; }

  void set_preferences_backend(void *backend) { preferences_backend_ = backend; }

 protected:
  void *preferences_backend_{nullptr};
};

/// Specialized partition for raw data storage
class RawPartition : public NvmPartition {
 public:
  using NvmPartition::NvmPartition;
};

/// Specialized partition for key-value storage
class KeyValuePartition : public NvmPartition {
 public:
  using NvmPartition::NvmPartition;

  /// Get value by key
  /// @param key Key to look up
  /// @param value Buffer to store value
  /// @param max_len Maximum bytes to read
  /// @return Actual bytes read, or -1 on error
  int get(const std::string &key, uint8_t *value, size_t max_len);

  /// Set value by key
  /// @param key Key to set
  /// @param value Value to store
  /// @param len Length of value
  /// @return true on success
  bool set(const std::string &key, const uint8_t *value, size_t len);

  /// Delete key
  /// @param key Key to delete
  /// @return true on success
  bool erase(const std::string &key);

  /// Check if key exists
  /// @param key Key to check
  /// @return true if key exists
  bool has_key(const std::string &key);

 protected:
  // Key-value storage format:
  // [key_len: 1 byte][key: N bytes][value_len: 2 bytes][value: M bytes]
  // Keys are stored sequentially, no indexing
};

}  // namespace nvm
}  // namespace esphome
