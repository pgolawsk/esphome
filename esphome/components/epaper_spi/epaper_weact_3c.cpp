#include "epaper_weact_3c.h"

#include <algorithm>
#include <cstring>

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
    : EPaperBase(name, width, height, init_sequence, init_sequence_length, display_type) {
  // buffer_length_ must be set AFTER base class constructor runs (which sets row_width_)
  // For 3-color displays, we need DOUBLE buffer length (black + red planes)
  this->buffer_length_ = this->row_width_ * this->height_ * 2;
}

void EPaperWeAct3C::fill(Color color) {
  // Let base class handle the main buffer (first half)
  EPaperBase::fill(color);

  // Clear red buffer (second half of main buffer)
  const size_t half_buffer = this->buffer_length_ / 2;
  uint8_t fill_byte = color.is_on() ? 0xFF : 0x00;
  for (size_t i = half_buffer; i < this->buffer_length_; i++) {
    this->buffer_[i] = fill_byte;
  }
}

void EPaperWeAct3C::clear() { this->fill(COLOR_OFF); }

bool EPaperWeAct3C::initialise(bool partial) {
  ESP_LOGI(TAG, "initialise(partial=%d)", partial);
  ESP_LOGI(TAG, "width=%u, height=%u, buffer_length=%zu (black=%zu, red=%zu)", this->width_, this->height_,
           this->buffer_length_, this->buffer_length_ / 2, this->buffer_length_ / 2);

  // Clear red buffer (second half of main buffer)
  const size_t half_buffer = this->buffer_length_ / 2;
  for (size_t i = half_buffer; i < this->buffer_length_; i++) {
    this->buffer_[i] = 0x00;
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

  // Red data is in second half of buffer
  const size_t red_offset = this->buffer_length_ / 2;

  // Detect red pixels (r>0, g=0, b=0)
  bool is_red = (color.red > 0) && (color.green == 0) && (color.blue == 0);

  // BLACK PLANE: 0=Black, 1=White
  // We want Black Ink if color is Active AND NOT Red
  if (color.is_on() && !is_red) {
    this->buffer_[byte_position] &= ~pixel_bit;  // Black Ink
  } else {
    this->buffer_[byte_position] |= pixel_bit;  // White Paper
  }

  // RED PLANE: 1=Red, 0=None (stored in second half of buffer)
  if (is_red) {
    this->buffer_[byte_position + red_offset] |= pixel_bit;
  } else {
    this->buffer_[byte_position + red_offset] &= ~pixel_bit;
  }
}

void EPaperWeAct3C::power_on() {
  ESP_LOGD(TAG, "power_on() - busy_pin: %s",
           this->busy_pin_ ? (this->busy_pin_->digital_read() ? "HIGH" : "LOW") : "NULL");
}

void EPaperWeAct3C::power_off() { ESP_LOGD(TAG, "power_off()"); }

void EPaperWeAct3C::refresh_screen(bool partial) {
  ESP_LOGI(TAG, "refresh_screen(partial=%d)", partial);

  // Master Activation - triggers the display refresh
  this->cmd_data(0x22, UPDATE_FULL, sizeof(UPDATE_FULL));
  this->command(ACTIVATE);
}

void EPaperWeAct3C::deep_sleep() {
  ESP_LOGI(TAG, "deep_sleep()");

  // Deep sleep mode
  this->command(0x10);
  this->cmd_data(0x10, {0x01});
}

bool HOT EPaperWeAct3C::transfer_data() {
  ESP_LOGI(TAG, "transfer_data() called");

  this->wait_for_idle_(false);

  const size_t half_buffer = this->buffer_length_ / 2;

  // RAM Address Set
  const uint8_t ram_x_range[] = {0x44, 0x00, (uint8_t) (this->width_ / 8u - 1)};
  const uint8_t ram_y_range[] = {0x45, 0x00, 0x00, (uint8_t) (this->height_ - 1), (uint8_t) ((this->height_ - 1) >> 8)};
  this->cmd_data(0x44, ram_x_range, sizeof(ram_x_range));
  this->cmd_data(0x45, ram_y_range, sizeof(ram_y_range));

  // Set RAM X counter
  this->cmd_data(0x4E, {0x00});

  // Set RAM Y counter
  this->cmd_data(0x4F, {0x00, 0x00});

  // Write RED buffer first (0x26) - second half of main buffer
  ESP_LOGD(TAG, "transferring red buffer (RAM 0x26), offset=%zu, len=%zu", half_buffer, half_buffer);
  this->command(WRITE_COLOR);
  this->dc_pin_->digital_write(true);
  this->enable();
  for (size_t i = half_buffer; i < this->buffer_length_; i++) {
    this->write_byte(this->buffer_[i]);
  }
  this->disable();

  // Reset RAM Y counter before second buffer
  this->cmd_data(0x4F, {0x00, 0x00});

  // Write BLACK buffer second (0x24) - first half of main buffer
  ESP_LOGD(TAG, "transferring black buffer (RAM 0x24), len=%zu", half_buffer);
  this->command(WRITE_BLACK);
  this->dc_pin_->digital_write(true);
  this->enable();
  for (size_t i = 0; i < half_buffer; i++) {
    this->write_byte(this->buffer_[i]);
  }
  this->disable();

  return true;
}

void EPaperWeAct3C::set_state_(EPaperState state, uint16_t delay) {
  // Override to skip waiting for busy pin in POWER_ON and REFRESH_SCREEN states
  // The base class waits for idle when state > SHOULD_WAIT, but we manage this manually
  ESP_LOGV(TAG, "set_state_: %s -> %s", this->epaper_state_to_string_(),
           state == EPaperState::POWER_ON         ? "POWER_ON"
           : state == EPaperState::REFRESH_SCREEN ? "REFRESH_SCREEN"
           : state == EPaperState::POWER_OFF      ? "POWER_OFF"
           : state == EPaperState::DEEP_SLEEP     ? "DEEP_SLEEP"
           : state == EPaperState::IDLE           ? "IDLE"
           : state == EPaperState::TRANSFER_DATA  ? "TRANSFER_DATA"
           : state == EPaperState::INITIALISE     ? "INITIALISE"
           : state == EPaperState::RESET          ? "RESET"
                                                  : "UNKNOWN");

  this->state_ = state;
  // For POWER_ON and REFRESH_SCREEN, don't wait for busy pin - we handle it manually
  if (state == EPaperState::POWER_ON || state == EPaperState::REFRESH_SCREEN || state == EPaperState::POWER_OFF ||
      state == EPaperState::DEEP_SLEEP || state == EPaperState::IDLE) {
    this->wait_for_idle_(false);
  } else {
    this->wait_for_idle_(state > EPaperState::SHOULD_WAIT);
  }
  // allow subclasses to nominate delays
  if (delay == 0)
    delay = this->next_delay_;
  this->next_delay_ = 0;
  this->delay_until_ = millis() + delay;
  ESP_LOGV(TAG, "Enter state %s, delay %u, wait_for_idle=%s",
           state == EPaperState::POWER_ON         ? "POWER_ON"
           : state == EPaperState::REFRESH_SCREEN ? "REFRESH_SCREEN"
           : state == EPaperState::POWER_OFF      ? "POWER_OFF"
           : state == EPaperState::DEEP_SLEEP     ? "DEEP_SLEEP"
           : state == EPaperState::IDLE           ? "IDLE"
           : state == EPaperState::TRANSFER_DATA  ? "TRANSFER_DATA"
           : state == EPaperState::INITIALISE     ? "INITIALISE"
           : state == EPaperState::RESET          ? "RESET"
                                                  : "UNKNOWN",
           delay, TRUEFALSE(this->waiting_for_idle_));
  if (state == EPaperState::IDLE) {
    this->disable_loop();
  }
}

}  // namespace esphome::epaper_spi
