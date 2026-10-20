#pragma once

#include <initializer_list>
#include "esphome/components/display/display_buffer.h"
#include "esphome/components/spi/spi.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace pervasive_epd {

// Pervasive Displays small iTC panels (film C = black/white, film J = black/white/red "Spectra").
// Sequence: hard reset -> soft reset -> temperature + PSR -> two frames -> power on -> refresh -> DC/DC off.
// BUSY is LOW while the panel is working and HIGH when it is ready.
enum class EPDState : uint8_t {
  IDLE,
  RESET_HIGH,
  RESET_LOW,
  RESET_RELEASE,
  SOFT_RESET,
  CONFIGURE,
  POWER_ON,
  REFRESH,
  POWER_OFF,
  FINISH,
};

class PervasiveEPD : public display::DisplayBuffer,
                     public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW,
                                           spi::CLOCK_PHASE_LEADING, spi::DATA_RATE_4MHZ> {
 public:
  PervasiveEPD(const char *model, uint16_t width, uint16_t height);

  void set_dc_pin(GPIOPin *pin) { this->dc_pin_ = pin; }
  void set_reset_pin(GPIOPin *pin) { this->reset_pin_ = pin; }
  void set_busy_pin(GPIOPin *pin) { this->busy_pin_ = pin; }
  void set_psr(uint8_t psr0, uint8_t psr1) {
    this->psr_[0] = psr0;
    this->psr_[1] = psr1;
  }
  void set_temperature(uint8_t temperature) { this->temperature_ = temperature; }
  void set_mirror(bool mirror_x, bool mirror_y) {
    this->mirror_x_ = mirror_x;
    this->mirror_y_ = mirror_y;
  }

  void setup() override;
  void update() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::PROCESSOR; }

  void fill(Color color) override;
  void clear() override { this->fill(Color(255, 255, 255)); }  // blank paper is white

  display::DisplayType get_display_type() override { return display::DISPLAY_TYPE_COLOR; }
  bool is_updating() const { return this->state_ != EPDState::IDLE; }

 protected:
  enum class Ink : uint8_t { WHITE, BLACK, RED };
  static Ink color_to_ink_(Color color);

  void draw_absolute_pixel_internal(int x, int y, Color color) override;
  int get_width_internal() override { return this->width_; }
  int get_height_internal() override { return this->height_; }

  void command_(uint8_t cmd);
  void data_(const uint8_t *data, size_t length);
  void command_data_(uint8_t cmd, std::initializer_list<uint8_t> data) {
    this->command_(cmd);
    this->data_(data.begin(), data.size());
  }

  bool is_busy_();
  void start_();
  void process_state_();
  // Move to `state` after `delay_ms`. If `busy_timeout_ms` > 0, also wait (up to that long) for BUSY to release.
  void next_(EPDState state, uint32_t delay_ms, uint32_t busy_timeout_ms = 0);
  const char *state_name_() const;

  const char *model_;
  uint16_t width_;
  uint16_t height_;
  uint16_t row_bytes_;
  size_t plane_size_;

  GPIOPin *dc_pin_{nullptr};
  GPIOPin *reset_pin_{nullptr};
  GPIOPin *busy_pin_{nullptr};
  uint8_t psr_[2]{0xCF, 0x8D};
  uint8_t temperature_{25};
  bool mirror_x_{false};
  bool mirror_y_{false};

  EPDState state_{EPDState::IDLE};
  bool waiting_for_busy_{false};
  bool pending_update_{false};
  uint32_t delay_until_{0};
  uint32_t busy_deadline_{0};
  uint32_t update_start_{0};
  uint32_t refresh_start_{0};
};

}  // namespace pervasive_epd
}  // namespace esphome
