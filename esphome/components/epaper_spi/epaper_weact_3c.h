#pragma once

#include <cstddef>
#include <cstdint>

#include "esphome/core/component.h"

#include "epaper_spi.h"

namespace esphome::epaper_spi {

class EPaperWeAct3C : public EPaperBase {
 public:
  EPaperWeAct3C(const char *name, uint16_t width, uint16_t height, const uint8_t *init_sequence = nullptr,
                size_t init_sequence_length = 0, DisplayType display_type = DISPLAY_TYPE_BINARY);
  ~EPaperWeAct3C();

  void fill(Color color) override;
  void clear() override;
  bool initialise(bool partial) override;

 protected:
  void HOT draw_pixel_at(int x, int y, Color color) override;
  void power_on() override;
  void power_off() override;
  void refresh_screen(bool partial) override;
  void deep_sleep() override;
  bool HOT transfer_data() override;

 private:
  // No separate red buffer - red is stored in second half of main buffer
  static constexpr size_t get_red_offset_() { return EPaperBase::buffer_length_ / 2; }

  void write_buffer_(const uint8_t *buffer, uint8_t ram_id);
  void update_display_();
};

}  // namespace esphome::epaper_spi
