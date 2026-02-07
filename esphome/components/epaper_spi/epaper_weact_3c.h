#pragma once

#include "epaper_spi.h"

namespace esphome::epaper_spi {

/**
 * Class for WeAct 3-color (Black/White/Red) e-paper displays.
 * These displays use SSD1680 controller and have two memory planes:
 * - Black/White plane (0x24): 0=Black, 1=White (inverted logic)
 * - Red plane (0x26): 1=Red, 0=None
 */
class EPaperWeAct3C : public EPaperBase {
 public:
  EPaperWeAct3C(const char *name, uint16_t width, uint16_t height, const uint8_t *init_sequence,
                size_t init_sequence_length)
      : EPaperBase(name, width, height, init_sequence, init_sequence_length, DISPLAY_TYPE_COLOR) {
    // Two planes: Black/White + Red, each is width*height/8 bytes
    this->buffer_length_ = (width + 7) / 8 * height * 2;
    this->plane_size_ = (width + 7) / 8 * height;
  }

  void fill(Color color) override;
  void clear() override;

 protected:
  bool initialise(bool partial) override;
  void set_window();
  void refresh_screen(bool partial) override;
  void power_on() override;
  void power_off() override;
  void deep_sleep() override;
  void draw_pixel_at(int x, int y, Color color) override;

  bool transfer_data() override;

  /**
   * Convert a Color to the display's 3-color format.
   * Returns: 0=Black, 1=White, 2=Red
   */
  static uint8_t color_to_bwr(Color color);

  size_t plane_size_{};       // Size of one plane in bytes
  uint8_t current_plane_{0};  // 0=Black/White plane, 1=Red plane
};

}  // namespace esphome::epaper_spi
