#include "epaper_weact_3c.h"

#include <algorithm>

#include "esphome/core/log.h"

namespace esphome::epaper_spi {

static constexpr const char *const TAG = "epaper_spi.weact_3c";

// Color mapping: 0=Black, 1=White, 2=Red
enum WeAct3CColor {
  WEACT_BLACK = 0,
  WEACT_WHITE = 1,
  WEACT_RED = 2,
};

uint8_t EPaperWeAct3C::color_to_bwr(Color color) {
  // Check for pure red (R > 0, G = 0, B = 0)
  if (color.r > 0 && color.g == 0 && color.b == 0) {
    return WEACT_RED;
  }

  // For other colors, determine if it's closer to black or white
  // We use luminance threshold at the middle (382 = (255*3)/2)
  if ((static_cast<int>(color.r) + color.g + color.b) > 382) {
    return WEACT_WHITE;
  }
  return WEACT_BLACK;
}

void EPaperWeAct3C::fill(Color color) {
  // If clipping is active, fall back to base implementation
  if (this->get_clipping().is_set()) {
    EPaperBase::fill(color);
    return;
  }

  auto pixel_color = color_to_bwr(color);
  uint8_t black_plane_byte, red_plane_byte;

  // Black/White plane: WeAct displays use INVERTED logic: 1=Black, 0=White
  // Red plane: 1=Red, 0=None
  switch (pixel_color) {
    case WEACT_BLACK:
      black_plane_byte = 0xFF;  // All black (inverted: 1=black)
      red_plane_byte = 0x00;    // No red
      break;
    case WEACT_WHITE:
      black_plane_byte = 0x00;  // All white (inverted: 0=white)
      red_plane_byte = 0x00;    // No red
      break;
    case WEACT_RED:
      black_plane_byte = 0x00;  // White (inverted: 0=white, not black)
      red_plane_byte = 0xFF;    // All red
      break;
    default:
      black_plane_byte = 0x00;  // Default to white (inverted)
      red_plane_byte = 0x00;
      break;
  }

  // Fill both planes
  // First half of buffer is Black/White plane, second half is Red plane
  for (size_t i = 0; i < this->plane_size_; i++) {
    this->buffer_[i] = black_plane_byte;
    this->buffer_[i + this->plane_size_] = red_plane_byte;
  }

  this->x_high_ = this->width_;
  this->y_high_ = this->height_;
  this->x_low_ = 0;
  this->y_low_ = 0;
}

void EPaperWeAct3C::clear() {
  // Clear to white (all white, no red)
  this->fill(COLOR_ON);
}

bool EPaperWeAct3C::initialise(bool partial) {
  EPaperBase::initialise(partial);
  // Additional init if needed for partial updates
  delayMicroseconds(200);  // Ensure controller processes init sequence
  return true;
}

void EPaperWeAct3C::set_window() {
  // Round to byte boundaries
  this->x_low_ &= ~7;
  this->x_high_ += 7;
  this->x_high_ &= ~7;

  uint16_t x_start = this->x_low_ / 8;
  uint16_t x_end = (this->x_high_ - 1) / 8;

  // Set RAM X range (0x44) and position (0x4E)
  this->cmd_data(0x44, {(uint8_t) x_start, (uint8_t) x_end});
  this->cmd_data(0x4E, {(uint8_t) x_start});

  // Set RAM Y range (0x45) and position (0x4F)
  this->cmd_data(0x45, {(uint8_t) this->y_low_, (uint8_t) (this->y_low_ / 256), (uint8_t) (this->y_high_ - 1),
                        (uint8_t) ((this->y_high_ - 1) / 256)});
  this->cmd_data(0x4F, {(uint8_t) this->y_low_, (uint8_t) (this->y_low_ / 256)});

  ESP_LOGV(TAG, "Set window X: %u-%u, Y: %u-%u", this->x_low_, this->x_high_, this->y_low_, this->y_high_);
  delayMicroseconds(100);  // Allow controller to process window settings
}

void HOT EPaperWeAct3C::draw_pixel_at(int x, int y, Color color) {
  if (!this->rotate_coordinates_(x, y))
    return;

  auto pixel_color = color_to_bwr(color);
  const uint32_t pos = (x + y * this->get_width_internal()) / 8u;
  const uint8_t bit = 0x80 >> (x & 0x07);

  // Black/White plane (first half of buffer)
  // WeAct displays use INVERTED logic: 1=Black, 0=White
  if (pixel_color == WEACT_BLACK) {
    this->buffer_[pos] |= bit;  // 1 = Black (inverted)
  } else {
    this->buffer_[pos] &= ~bit;  // 0 = White (inverted, for both white and red)
  }

  // Red plane (second half of buffer)
  // 1=Red, 0=None
  if (pixel_color == WEACT_RED) {
    this->buffer_[pos + this->plane_size_] |= bit;
  } else {
    this->buffer_[pos + this->plane_size_] &= ~bit;
  }
}

void EPaperWeAct3C::power_on() {
  ESP_LOGV(TAG, "Power on");
  // Empty - display is powered on during initialization
}

void EPaperWeAct3C::power_off() {
  ESP_LOGV(TAG, "Power off");
  // Empty - no power off sequence needed, avoids BUSY timeout
}

void EPaperWeAct3C::refresh_screen(bool partial) {
  ESP_LOGV(TAG, "Refresh screen");
  this->cmd_data(0x22, {0xF7});
  delayMicroseconds(200);  // Delay after display update control
  this->command(0x20);
  delayMicroseconds(200);  // Delay after activate command
  this->next_delay_ = 100;
}

void EPaperWeAct3C::deep_sleep() {
  ESP_LOGV(TAG, "Deep sleep");
  this->cmd_data(0x10, {0x01});
}

bool HOT EPaperWeAct3C::transfer_data() {
  const uint32_t start_time = App.get_loop_component_start_time();

  // First transfer: Black/White plane (command 0x24)
  if (this->current_plane_ == 0) {
    if (this->current_data_index_ == 0) {
      ESP_LOGD(TAG, "Starting Black/White plane transfer");
      this->set_window();  // Set window before sending data
      this->command(0x24);
      this->start_data_();  // Enter data mode ONCE for entire plane
    }

    size_t buf_idx = 0;
    uint8_t bytes_to_send[MAX_TRANSFER_SIZE];
    while (this->current_data_index_ < this->plane_size_) {
      bytes_to_send[buf_idx++] = this->buffer_[this->current_data_index_++];

      if (buf_idx == sizeof(bytes_to_send)) {
        this->write_array(bytes_to_send, buf_idx);
        buf_idx = 0;

        if (millis() - start_time > MAX_TRANSFER_TIME) {
          this->disable();
          return false;  // Not done yet
        }
      }
    }

    // Flush remaining bytes
    if (buf_idx > 0) {
      this->write_array(bytes_to_send, buf_idx);
    }

    this->disable();  // Exit data mode after plane complete

    // Move to red plane
    this->current_plane_ = 1;
    this->current_data_index_ = 0;
    ESP_LOGD(TAG, "Black/White plane complete, moving to Red plane");
    return false;  // Come back next loop for red plane
  }

  // Second transfer: Red plane (command 0x26)
  if (this->current_plane_ == 1) {
    if (this->current_data_index_ == 0) {
      this->command(0x26);
      this->start_data_();  // Enter data mode ONCE for entire plane
    }

    size_t buf_idx = 0;
    uint8_t bytes_to_send[MAX_TRANSFER_SIZE];
    while (this->current_data_index_ < this->plane_size_) {
      bytes_to_send[buf_idx++] = this->buffer_[this->plane_size_ + this->current_data_index_++];

      if (buf_idx == sizeof(bytes_to_send)) {
        this->write_array(bytes_to_send, buf_idx);
        buf_idx = 0;

        if (millis() - start_time > MAX_TRANSFER_TIME) {
          this->disable();
          return false;  // Not done yet
        }
      }
    }

    // Flush remaining bytes
    if (buf_idx > 0) {
      this->write_array(bytes_to_send, buf_idx);
    }

    this->disable();  // Exit data mode after plane complete

    // Reset for next update
    this->current_plane_ = 0;
    this->current_data_index_ = 0;
    ESP_LOGD(TAG, "Red plane complete");
    return true;  // Both planes done
  }

  return false;  // Should never reach here
}

}  // namespace esphome::epaper_spi
