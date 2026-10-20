#include "pervasive_epd.h"
#include <cinttypes>
#include <cstring>
#include "esphome/core/log.h"

namespace esphome {
namespace pervasive_epd {

static const char *const TAG = "pervasive_epd";

static constexpr uint32_t BUSY_TIMEOUT_MS = 5000;
static constexpr uint32_t REFRESH_TIMEOUT_MS = 60000;
static constexpr uint32_t NO_BUSY_REFRESH_WAIT_MS = 20000;  // used only when no busy_pin is configured
static constexpr uint32_t NO_BUSY_STEP_WAIT_MS = 200;

PervasiveEPD::PervasiveEPD(const char *model, uint16_t width, uint16_t height)
    : model_(model), width_(width), height_(height) {
  this->row_bytes_ = (width + 7) / 8;
  this->plane_size_ = static_cast<size_t>(this->row_bytes_) * height;
}

void PervasiveEPD::setup() {
  // Two 1-bit planes: [black plane][red plane]
  this->init_internal_(this->plane_size_ * 2);
  if (this->buffer_ == nullptr) {
    this->mark_failed();
    return;
  }
  this->dc_pin_->setup();
  this->dc_pin_->digital_write(false);
  this->reset_pin_->setup();
  this->reset_pin_->digital_write(true);
  if (this->busy_pin_ != nullptr)
    this->busy_pin_->setup();
  this->spi_setup();
  this->disable_loop();
}

void PervasiveEPD::dump_config() {
  LOG_DISPLAY("", "Pervasive Displays iTC e-paper", this);
  ESP_LOGCONFIG(TAG,
                "  Model: %s (%ux%u native)\n"
                "  PSR: 0x%02X 0x%02X\n"
                "  Temperature: %u C\n"
                "  Mirror X/Y: %s/%s",
                this->model_, this->width_, this->height_, this->psr_[0], this->psr_[1], this->temperature_,
                YESNO(this->mirror_x_), YESNO(this->mirror_y_));
  LOG_PIN("  CS Pin: ", this->cs_);
  LOG_PIN("  DC Pin: ", this->dc_pin_);
  LOG_PIN("  Reset Pin: ", this->reset_pin_);
  LOG_PIN("  Busy Pin: ", this->busy_pin_);
  LOG_UPDATE_INTERVAL(this);
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

PervasiveEPD::Ink PervasiveEPD::color_to_ink_(Color color) {
  const int r = color.r, g = color.g, b = color.b;
  if (r > 127 && r > g + b)
    return Ink::RED;
  if (r + g + b >= 382)
    return Ink::WHITE;
  return Ink::BLACK;
}

void PervasiveEPD::fill(Color color) {
  if (this->get_clipping().is_set()) {
    display::Display::fill(color);
    return;
  }
  const Ink ink = color_to_ink_(color);
  // Black plane: 1 = black. Red plane: 1 = red.
  memset(this->buffer_, ink == Ink::BLACK ? 0xFF : 0x00, this->plane_size_);
  memset(this->buffer_ + this->plane_size_, ink == Ink::RED ? 0xFF : 0x00, this->plane_size_);
}

void HOT PervasiveEPD::draw_absolute_pixel_internal(int x, int y, Color color) {
  if (x < 0 || y < 0 || x >= this->width_ || y >= this->height_)
    return;
  if (this->mirror_x_)
    x = this->width_ - 1 - x;
  if (this->mirror_y_)
    y = this->height_ - 1 - y;

  const size_t pos = static_cast<size_t>(y) * this->row_bytes_ + (x >> 3);
  const uint8_t bit = 0x80 >> (x & 0x07);
  uint8_t &black = this->buffer_[pos];
  uint8_t &red = this->buffer_[this->plane_size_ + pos];

  switch (color_to_ink_(color)) {
    case Ink::RED:
      black &= ~bit;
      red |= bit;
      break;
    case Ink::BLACK:
      black |= bit;
      red &= ~bit;
      break;
    case Ink::WHITE:
    default:
      black &= ~bit;
      red &= ~bit;
      break;
  }
}

// ---------------------------------------------------------------------------
// Low-level SPI (CS is released between the command byte and its data, as in the reference driver)
// ---------------------------------------------------------------------------

void PervasiveEPD::command_(uint8_t cmd) {
  this->dc_pin_->digital_write(false);
  this->enable();
  this->write_byte(cmd);
  this->disable();
}

void PervasiveEPD::data_(const uint8_t *data, size_t length) {
  if (length == 0)
    return;
  this->dc_pin_->digital_write(true);
  this->enable();
  this->write_array(data, length);
  this->disable();
}

bool PervasiveEPD::is_busy_() {
  if (this->busy_pin_ == nullptr)
    return false;
  // Pervasive iTC: LOW = busy, HIGH = ready. (Set `inverted: true` on the pin to flip this.)
  return !this->busy_pin_->digital_read();
}

// ---------------------------------------------------------------------------
// Non-blocking update state machine
// ---------------------------------------------------------------------------

void PervasiveEPD::update() {
  if (this->is_failed())
    return;
  if (this->state_ != EPDState::IDLE) {
    // A refresh takes ~15 s on tri-colour panels; remember the request and redraw when done.
    this->pending_update_ = true;
    ESP_LOGD(TAG, "Refresh in progress, update queued");
    return;
  }
  this->do_update_();
  this->start_();
}

void PervasiveEPD::start_() {
  this->update_start_ = millis();
  this->pending_update_ = false;
  this->next_(EPDState::RESET_HIGH, 5);
  this->enable_loop();
}

void PervasiveEPD::next_(EPDState state, uint32_t delay_ms, uint32_t busy_timeout_ms) {
  this->state_ = state;
  this->waiting_for_busy_ = false;
  if (busy_timeout_ms > 0) {
    if (this->busy_pin_ != nullptr) {
      this->waiting_for_busy_ = true;
      this->busy_deadline_ = millis() + delay_ms + busy_timeout_ms;
    } else {
      delay_ms += busy_timeout_ms > BUSY_TIMEOUT_MS ? NO_BUSY_REFRESH_WAIT_MS : NO_BUSY_STEP_WAIT_MS;
    }
  }
  this->delay_until_ = millis() + delay_ms;
}

void PervasiveEPD::loop() {
  if (this->state_ == EPDState::IDLE) {
    this->disable_loop();
    return;
  }
  const uint32_t now = millis();
  if (static_cast<int32_t>(now - this->delay_until_) < 0)
    return;
  if (this->waiting_for_busy_) {
    if (this->is_busy_()) {
      if (static_cast<int32_t>(now - this->busy_deadline_) < 0)
        return;
      ESP_LOGW(TAG,
               "Panel still reports busy after timeout (before state %s); continuing. Check the busy pin; "
               "if it is wired correctly but reads the other way round, add 'inverted: true' to busy_pin.",
               this->state_name_());
    }
    this->waiting_for_busy_ = false;
  }
  this->process_state_();
}

void PervasiveEPD::process_state_() {
  switch (this->state_) {
    case EPDState::IDLE:
      this->disable_loop();
      break;

    // Hard reset, as in the reference driver: high 5 ms, low 10 ms, high 5 ms (+5 ms settle)
    case EPDState::RESET_HIGH:
      this->reset_pin_->digital_write(true);
      this->next_(EPDState::RESET_LOW, 5);
      break;
    case EPDState::RESET_LOW:
      this->reset_pin_->digital_write(false);
      this->next_(EPDState::RESET_RELEASE, 10);
      break;
    case EPDState::RESET_RELEASE:
      this->reset_pin_->digital_write(true);
      this->next_(EPDState::SOFT_RESET, 10);
      break;

    case EPDState::SOFT_RESET:
      if (this->busy_pin_ != nullptr) {
        ESP_LOGD(TAG, "BUSY after reset reads '%s' (expected 'ready')", this->is_busy_() ? "busy" : "ready");
      }
      this->command_data_(0x00, {0x0E});  // soft reset
      this->next_(EPDState::CONFIGURE, 5, BUSY_TIMEOUT_MS);
      break;

    case EPDState::CONFIGURE:
      this->command_data_(0xE5, {this->temperature_});  // input temperature
      this->command_data_(0xE0, {0x02});                // activate temperature
      this->command_data_(0x00, {this->psr_[0], this->psr_[1]});  // panel settings (PSR)
      this->command_(0x10);  // first frame: black plane
      this->data_(this->buffer_, this->plane_size_);
      this->command_(0x13);  // second frame: red plane
      this->data_(this->buffer_ + this->plane_size_, this->plane_size_);
      this->next_(EPDState::POWER_ON, 0, BUSY_TIMEOUT_MS);
      break;

    case EPDState::POWER_ON:
      this->command_(0x04);  // DC/DC power on
      this->next_(EPDState::REFRESH, 0, BUSY_TIMEOUT_MS);
      break;

    case EPDState::REFRESH:
      this->command_(0x12);  // display refresh
      this->refresh_start_ = millis();
      this->next_(EPDState::POWER_OFF, 5, REFRESH_TIMEOUT_MS);
      break;

    case EPDState::POWER_OFF: {
      const uint32_t refresh_ms = millis() - this->refresh_start_;
      if (this->busy_pin_ != nullptr && refresh_ms < 1000) {
        ESP_LOGW(TAG,
                 "Refresh finished in only %" PRIu32 " ms; a tri-colour refresh normally takes ~15 s. "
                 "BUSY may not be connected, or its polarity is wrong.",
                 refresh_ms);
      } else {
        ESP_LOGD(TAG, "Panel refresh took %" PRIu32 " ms", refresh_ms);
      }
      this->command_(0x02);  // DC/DC off
      this->next_(EPDState::FINISH, 0, BUSY_TIMEOUT_MS);
      break;
    }

    case EPDState::FINISH:
      // Park control lines low, as the application note recommends after power-off.
      this->dc_pin_->digital_write(false);
      this->reset_pin_->digital_write(false);
      this->state_ = EPDState::IDLE;
      ESP_LOGD(TAG, "Display update took %" PRIu32 " ms", millis() - this->update_start_);
      if (this->pending_update_) {
        this->pending_update_ = false;
        this->do_update_();
        this->start_();
      } else {
        this->disable_loop();
      }
      break;
  }
}

const char *PervasiveEPD::state_name_() const {
  switch (this->state_) {
    case EPDState::IDLE:
      return "IDLE";
    case EPDState::RESET_HIGH:
      return "RESET_HIGH";
    case EPDState::RESET_LOW:
      return "RESET_LOW";
    case EPDState::RESET_RELEASE:
      return "RESET_RELEASE";
    case EPDState::SOFT_RESET:
      return "SOFT_RESET";
    case EPDState::CONFIGURE:
      return "CONFIGURE";
    case EPDState::POWER_ON:
      return "POWER_ON";
    case EPDState::REFRESH:
      return "REFRESH";
    case EPDState::POWER_OFF:
      return "POWER_OFF";
    case EPDState::FINISH:
      return "FINISH";
  }
  return "UNKNOWN";
}

}  // namespace pervasive_epd
}  // namespace esphome
