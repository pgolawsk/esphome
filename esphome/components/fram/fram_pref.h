#pragma once

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/fram/fram.h"

namespace esphome {
namespace fram_pref {

class FramPref : public Component, public ESPPreferences {
 public:
  FramPref(fram::Fram *fram) : fram_(fram) {}

  void setup() override;
  void dump_config() override;
  // Run after FRAM (IO) but before components that use preferences (BUS)
  float get_setup_priority() const override { return setup_priority::BUS; }

  ESPPreferenceObject make_preference(size_t length, uint32_t type, bool in_flash) override;
  ESPPreferenceObject make_preference(size_t length, uint32_t type) override;
  bool sync() override;
  bool reset() override;

  void set_pool_size(uint32_t value) { this->pool_size_ = value; }
  void set_pool_start(uint32_t value) { this->pool_start_ = value; }

 protected:
  friend class FRAMPreferenceBackend;

  void ensure_initialized_();

  fram::Fram *fram_;
  uint32_t pool_size_{0};
  uint32_t pool_start_{0};
  uint32_t magic_{0};
  bool pool_cleared_{false};
  bool initialized_{false};
  uint8_t version_{1};
};

}  // namespace fram_pref
}  // namespace esphome
