#include "epaper_weact_3c.h"

#include <algorithm>

#include "esphome/core/log.h"

namespace esphome::epaper_spi {

static constexpr const char *const TAG = "epaper_spi.weact_3c";

// General Commands
static const uint8_t SW_RESET = 0x12;
static const uint8_t ACTIVATE = 0x20;
static const uint8_t WRITE_BLACK = 0x24;
static const uint8_t WRITE_COLOR = 0x26;

// Configuration commands
static const uint8_t DATA_ENTRY[] = {0x11, 0x03};            // data entry mode
static const uint8_t BORDER_FULL[] = {0x3C, 0x05};           // border waveform
static const uint8_t TEMP_SENS[] = {0x18, 0x80};             // use internal temp sensor
static const uint8_t DISPLAY_UPDATE[] = {0x21, 0x00, 0x80};  // display update control
static const uint8_t UPDATE_FULL[] = {0x22, 0xF7};           // full update control

EPaperWeAct3C::EPaperWeAct3C(const char *name, uint16_t width, uint16_t height, const uint8_t *init_sequence,
                             size_t init_sequence_length, DisplayType display_type)
    : EPaperBase(name, width, height, init_sequence, init_sequence_length, display_type) {}

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
  this->command(SW_RESET);
  delay(10);

  // Wait for busy to go low
  this->wait_for_idle_(false);

  // 2. Driver Output Control
  const uint8_t drv_out_ctl[] = {0x01, (uint8_t) ((this->height_ - 1) & 0xFF),
                                 (uint8_t) (((this->height_ - 1) >> 8) & 0xFF), 0x00};
  this->cmd_data(0x01, drv_out_ctl, sizeof(drv_out_ctl));

  // 3. Data Entry Mode
  this->cmd_data(0x11, DATA_ENTRY, sizeof(DATA_ENTRY));

  // 4. Border Setting
  this->cmd_data(0x3C, BORDER_FULL, sizeof(BORDER_FULL));

  // 5. Temperature Sensor
  this->cmd_data(0x18, TEMP_SENS, sizeof(TEMP_SENS));

  // 6. Display Update Control
  this->cmd_data(0x21, DISPLAY_UPDATE, sizeof(DISPLAY_UPDATE));

  return true;
}

void HOT EPaperWeAct3C::draw_pixel_at(int x, int y, Color color) {
  if (!this->rotate_coordinates_(x, y))
    return;

  const size_t byte_position = y * this->row_width_ + x / 8;
  const uint8_t bit_position = x % 8;
  const uint8_t pixel_bit = 0x80 >> bit_position;

  // Detect red pixels (r>0, g=0, b=0)
  bool is_red = (color.red > 0) && (color.green == 0) && (color.blue == 0);

  // BLACK PLANE: 0=Black, 1=White
  // We want Black Ink if color is Active AND NOT Red
  if (color.is_on() && !is_red) {
    this->buffer_[byte_position] &= ~pixel_bit;  // Black Ink
  } else {
    this->buffer_[byte_position] |= pixel_bit;  // White Paper
  }

  // RED PLANE: 1=Red, 0=None
  if (is_red) {
    this->red_buffer_[byte_position] |= pixel_bit;
  } else {
    this->red_buffer_[byte_position] &= ~pixel_bit;
  }
}

void EPaperWeAct3C::power_on() { ESP_LOGD(TAG, "power_on()"); }

void EPaperWeAct3C::power_off() { ESP_LOGD(TAG, "power_off()"); }

void EPaperWeAct3C::refresh_screen(bool partial) { ESP_LOGI(TAG, "refresh_screen(partial=%d)", partial); }

void EPaperWeAct3C::deep_sleep() {
  ESP_LOGI(TAG, "deep_sleep()");

  // Deep sleep mode
  this->command(0x10);
  this->cmd_data(0x10, {0x01});
}

bool HOT EPaperWeAct3C::transfer_data() {
  ESP_LOGI(TAG, "transfer_data() called");

  this->wait_for_idle_(false);

  // RAM Address Set
  const uint8_t ram_x_range[] = {0x44, 0x00, (uint8_t) (this->width_ / 8u - 1)};
  const uint8_t ram_y_range[] = {0x45, 0x00, 0x00, (uint8_t) (this->height_ - 1), (uint8_t) ((this->height_ - 1) >> 8)};
  this->cmd_data(0x44, ram_x_range, sizeof(ram_x_range));
  this->cmd_data(0x45, ram_y_range, sizeof(ram_y_range));

  // Set RAM X counter
  this->cmd_data(0x4E, {0x00});

  // Set RAM Y counter
  this->cmd_data(0x4F, {0x00, 0x00});

  // Write RED buffer first (0x26)
  ESP_LOGD(TAG, "transferring red buffer (RAM 0x26)");
  this->command(WRITE_COLOR);
  this->dc_pin_->digital_write(true);
  this->enable();
  for (size_t i = 0; i < this->buffer_length_; i++) {
    this->write_byte(this->red_buffer_[i]);
  }
  this->disable();

  // Reset RAM Y counter before second buffer
  this->cmd_data(0x4F, {0x00, 0x00});

  // Write BLACK buffer second (0x24)
  ESP_LOGD(TAG, "transferring black buffer (RAM 0x24)");
  this->command(WRITE_BLACK);
  this->dc_pin_->digital_write(true);
  this->enable();
  for (size_t i = 0; i < this->buffer_length_; i++) {
    this->write_byte(this->buffer_[i]);
  }
  this->disable();

  // Trigger display update
  this->cmd_data(0x22, UPDATE_FULL, sizeof(UPDATE_FULL));
  this->command(ACTIVATE);

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
  this->cmd_data(0x22, UPDATE_FULL, sizeof(UPDATE_FULL));  // Enable display

  // Master Activation
  this->command(ACTIVATE);  // Master Activation
  this->wait_for_idle_(false);
}

}  // namespace esphome::epaper_spi
