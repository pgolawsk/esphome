#include "epaper_weact_3c.h"

#include <algorithm>

#include "esphome/core/log.h"

namespace esphome::epaper_spi {

static constexpr const char *const TAG = "epaper_spi.weact_3c";

EPaperWeAct3C::EPaperWeAct3C(const char *name, uint16_t width, uint16_t height, const uint8_t *init_sequence,
                             size_t init_sequence_length, DisplayType display_type)
    : EPaperBase(name, width, height, init_sequence, init_sequence_length, display_type) {
  // For 3-color display, we need a second buffer for the red channel
}

EPaperWeAct3C::~EPaperWeAct3C() { delete[] this->red_buffer_; }

void EPaperWeAct3C::fill(Color color) {
  // Let base class handle the main buffer
  EPaperBase::fill(color);

  // Also fill the red buffer
  uint8_t fill_byte = color.is_on() ? 0xFF : 0x00;
  if (this->red_buffer_) {
    for (size_t i = 0; i < this->buffer_length_; i++) {
      this->red_buffer_[i] = fill_byte;
    }
  }
}

void EPaperWeAct3C::clear() { this->fill(COLOR_OFF); }

bool EPaperWeAct3C::initialise(bool partial) {
  ESP_LOGI(TAG, "initialise(partial=%d)", partial);

  // Initialize buffer first (this sets buffer_length_ and clears buffer)
  if (!this->init_buffer_(this->buffer_length_)) {
    ESP_LOGW(TAG, "init_buffer_ failed");
    return false;
  }

  ESP_LOGI(TAG, "width=%u, height=%u, buffer_length=%zu", this->width_, this->height_, this->buffer_length_);

  // Allocate red buffer
  this->red_buffer_ = new uint8_t[this->buffer_length_];
  for (size_t i = 0; i < this->buffer_length_; i++) {
    this->red_buffer_[i] = 0x00;  // Start with all red off
  }

  // Reset the controller
  this->reset();
  delay(10);

  // Send initialization sequence for SSD1680
  // 1. Software Reset
  this->command(0x12);  // SWReset
  delay(10);

  // Wait for busy to go low
  this->wait_for_idle_(false);

  // 2. Booster Turn-on (command 0x18 with data 0x87)
  this->cmd_data(0x18, {0x87});

  // 3. Display Update Control (command 0x21 with data 0x00)
  this->cmd_data(0x21, {0x00});

  // 4. Temperature sensor selection (internal) - command 0x4C
  this->cmd_data(0x4C, {0x00});

  // 5. Set border - command 0x3C
  this->cmd_data(0x3C, {0x05});  // Border setting

  // 6. Set display size and address
  // X address range: 0 to width-1 (in bytes, so width/8-1)
  this->cmd_data(0x44, {0x00, (uint8_t) ((this->width_ / 8) - 1)});  // Set RAM X address

  // Y address range: 0 to height-1
  this->cmd_data(0x45, {0x00, 0x00, 0x00, (uint8_t) (this->height_ - 1)});  // Set RAM Y address

  // Set RAM X address counter
  this->cmd_data(0x4E, {0x00});

  // Set RAM Y address counter
  this->cmd_data(0x4F, {0x00, 0x00});

  // 7. Display Update Control 1 (command 0x21)
  this->cmd_data(0x21, {0x40, 0x00});  // Enable clock signal

  // 8. Master Activation
  this->command(0x20);  // Master Activation
  this->wait_for_idle_(false);

  // Clear both buffers
  this->clear();

  return true;
}

void HOT EPaperWeAct3C::draw_pixel_at(int x, int y, Color color) {
  if (!this->rotate_coordinates_(x, y))
    return;

  const size_t byte_position = y * this->row_width_ + x / 8;
  const uint8_t bit_position = x % 8;
  const uint8_t pixel_bit = 0x80 >> bit_position;

  if (color.is_on()) {
    // Black pixel - set bit in main buffer, clear in red buffer
    this->buffer_[byte_position] |= pixel_bit;
    this->red_buffer_[byte_position] &= ~pixel_bit;
  } else {
    // For 3-color: OFF means white, so clear both
    this->buffer_[byte_position] &= ~pixel_bit;
    this->red_buffer_[byte_position] &= ~pixel_bit;
  }
  // Note: base class handles x_low_, x_high_, y_low_, y_high_ updates
}

void EPaperWeAct3C::power_on() { ESP_LOGD(TAG, "power_on()"); }

void EPaperWeAct3C::power_off() { ESP_LOGD(TAG, "power_off()"); }

void EPaperWeAct3C::refresh_screen(bool partial) { ESP_LOGI(TAG, "refresh_screen(partial=%d)", partial); }

void EPaperWeAct3C::deep_sleep() {
  ESP_LOGI(TAG, "deep_sleep()");

  // Enter deep sleep mode
  this->command(0x10);           // Deep sleep mode
  this->cmd_data(0x10, {0x01});  // Enter deep sleep
}

bool HOT EPaperWeAct3C::transfer_data() {
  ESP_LOGI(TAG, "transfer_data() called");

  // Transfer BLACK buffer (RAM 0x24) first
  ESP_LOGD(TAG, "transferring black buffer (RAM 0x24)");
  this->write_buffer_(nullptr, 0x24);  // nullptr means use base class buffer

  // Transfer RED buffer (RAM 0x26)
  ESP_LOGD(TAG, "transferring red buffer (RAM 0x26)");
  this->write_buffer_(this->red_buffer_, 0x26);

  // Trigger display update
  this->update_display_();

  return true;
}

void EPaperWeAct3C::write_buffer_(const uint8_t *buffer, uint8_t ram_id) {
  // RAM Address Set for X and Y
  this->cmd_data(0x44, {0x00, (uint8_t) ((this->width_ / 8) - 1)});  // Set RAM X address

  this->cmd_data(0x45, {0x00, 0x00, 0x00, (uint8_t) (this->height_ - 1)});  // Set RAM Y address

  this->cmd_data(0x4E, {0x00});        // Set RAM X counter
  this->cmd_data(0x4F, {0x00, 0x00});  // Set RAM Y counter

  // Start data transmission - send RAM ID as command
  this->command(ram_id);  // RAM access command (0x24 or 0x26)

  // Write the buffer byte by byte
  this->dc_pin_->digital_write(true);
  this->enable();

  if (buffer == nullptr) {
    // Use base class SplitBuffer
    for (size_t i = 0; i < this->buffer_length_; i++) {
      this->write_byte(this->buffer_[i]);
    }
  } else {
    // Use our own contiguous buffer
    for (size_t i = 0; i < this->buffer_length_; i++) {
      this->write_byte(buffer[i]);
    }
  }

  this->disable();
}

void EPaperWeAct3C::update_display_() {
  // Display Update Control 2
  this->cmd_data(0x22, {0xC4});  // Enable display, bypass mode

  // Master Activation
  this->command(0x20);  // Master Activation
  this->wait_for_idle_(false);
}

}  // namespace esphome::epaper_spi
