#pragma once

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/nvm/nvm.h"
#include "esphome/components/safe_mode/safe_mode.h"

namespace esphome {
namespace nvm {

/// NVM-based preferences backend for ESPHome
///
/// This component integrates with ESPHome's preferences system to store
/// global variables and other preferences in external NVM (FRAM/EEPROM)
/// instead of internal flash.
///
/// Usage:
/// ```yaml
/// nvm:
///   - platform: fram_i2c
///     id: my_fram
///     partitions:
///       - id: pref_store
///         type: preferences
///         size: 4KB
///
/// nvm_pref:
///   partition: pref_store
/// ```
class NvmPreferences : public Component, public ESPPreferences {
 public:
  NvmPreferences() = default;

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::BUS; }

  /// Set the preferences partition to use
  void set_partition(PreferencesPartition *partition) { partition_ = partition; }

  // ========== ESPPreferences interface ==========
  ESPPreferenceObject make_preference(size_t length, uint32_t type, bool in_flash) override;
  ESPPreferenceObject make_preference(size_t length, uint32_t type) override;
  bool sync() override;
  bool reset() override;

 protected:
  friend class NvmPreferenceBackend;

  /// Initialize the preferences pool
  void ensure_initialized_();

  /// Calculate pool usage
  uint32_t calculate_pool_used_();

  PreferencesPartition *partition_{nullptr};
  ESPPreferences *nvs_preferences_{nullptr};  ///< Original NVS preferences for delegated keys
  bool initialized_{false};
  bool pool_cleared_{false};
  uint32_t pool_used_{0};
  bool warned_80_percent_{false};

  // Pool header constants
  static const uint32_t POOL_HEADER_SIZE = 9;  ///< magic(4) + version(1) + pool_size(4)
  static const uint32_t MAGIC = 0xDEADBEEF;
  static const uint8_t VERSION = 3;  ///< Version 3 for NVM preferences format
};

/// Backend for individual preference objects
class NvmPreferenceBackend : public ESPPreferenceBackend {
 public:
  NvmPreferenceBackend(NvmPreferences *prefs, uint32_t type) : prefs_(prefs), type_(type) {}

  bool save(const uint8_t *data, size_t len) override;
  bool load(uint8_t *data, size_t len) override;

 protected:
  /// Find or allocate a key slot
  uint32_t find_key_(uint32_t key_hash);

  NvmPreferences *prefs_;
  uint32_t type_;
};

}  // namespace nvm
}  // namespace esphome
