#pragma once

#include "esphome/core/component.h"
#include "esphome/components/i2c/i2c.h"

namespace esphome {
namespace fram {

class Fram : public Component, public i2c::I2CDevice {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::BUS; }

  bool is_connected();
  void write_bytes(uint32_t memaddr, const uint8_t *value, uint32_t len);
  void read_bytes(uint32_t memaddr, uint8_t *value, uint32_t len);

  void set_size_bytes(uint32_t value) { this->size_bytes_ = value; }

 protected:
  void write_bytes_16(uint32_t memaddr, const uint8_t *value, uint32_t len);
  void read_bytes_16(uint32_t memaddr, uint8_t *value, uint32_t len);
  uint32_t size_bytes_{0};
};

}  // namespace fram
}  // namespace esphome
