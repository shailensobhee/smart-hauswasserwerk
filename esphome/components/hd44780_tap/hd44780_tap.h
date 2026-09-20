#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/hd44780_core/hd44780_decoder.h"

namespace esphome {
namespace hd44780_tap {

// Ring capacity in samples. Must be a power of two (the index wrap uses a mask).
// 512 * 8 B = 4 kB, enough to ride out a full-screen refresh burst while the
// main loop is busy elsewhere.
static const uint16_t RING_SIZE = 512;
static const uint16_t RING_MASK = RING_SIZE - 1;

/// One E-strobe observation: the whole GPIO input port, latched atomically.
///
/// Storing the raw port word rather than pre-extracted bits is deliberate - it
/// keeps the ISR to a single register read, and it means a change to the
/// assumed pin roles only affects decoding, not capture.
struct Sample {
  uint32_t port;  ///< GPIO_IN_REG at the falling edge of E
  uint32_t t_us;  ///< esp_timer microseconds, truncated (wraps every ~71 min)
};

class HD44780Tap : public Component {
 public:
  void set_e_pin(InternalGPIOPin *pin) { this->e_pin_ = pin; }
  void set_rs_pin(InternalGPIOPin *pin) { this->rs_pin_ = pin; }
  void set_d4_pin(InternalGPIOPin *pin) { this->d_pins_[0] = pin; }
  void set_d5_pin(InternalGPIOPin *pin) { this->d_pins_[1] = pin; }
  void set_d6_pin(InternalGPIOPin *pin) { this->d_pins_[2] = pin; }
  void set_d7_pin(InternalGPIOPin *pin) { this->d_pins_[3] = pin; }

  void set_columns(uint8_t columns) { this->columns_ = columns; }
  void set_idle_gap_us(uint32_t gap) { this->idle_gap_us_ = gap; }
  void set_raw_capture(bool enabled) { this->raw_capture_ = enabled; }

  void set_line_1(text_sensor::TextSensor *s) { this->line_1_ = s; }
  void set_line_2(text_sensor::TextSensor *s) { this->line_2_ = s; }
  void set_edge_rate(sensor::Sensor *s) { this->edge_rate_ = s; }
  void set_dropped_samples(sensor::Sensor *s) { this->dropped_samples_ = s; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  /// Falling edge of E - the instant the HD44780 latches RS and D4-D7.
  static void gpio_intr(HD44780Tap *arg);

 protected:
  void handle_sample_(const Sample &sample);
  void publish_lines_();
  void publish_diagnostics_();

  InternalGPIOPin *e_pin_{nullptr};
  InternalGPIOPin *rs_pin_{nullptr};
  InternalGPIOPin *d_pins_[4]{nullptr, nullptr, nullptr, nullptr};

  // Bit positions within the GPIO input port word, resolved once in setup().
  uint8_t rs_bit_{0};
  uint8_t d_bits_[4]{0, 0, 0, 0};

  uint8_t columns_{16};
  uint32_t idle_gap_us_{200};
  bool raw_capture_{false};

  text_sensor::TextSensor *line_1_{nullptr};
  text_sensor::TextSensor *line_2_{nullptr};
  sensor::Sensor *edge_rate_{nullptr};
  sensor::Sensor *dropped_samples_{nullptr};

  // --- ISR-shared state. Single producer (ISR), single consumer (loop). ---
  Sample ring_[RING_SIZE];
  volatile uint16_t head_{0};
  volatile uint16_t tail_{0};
  volatile uint32_t dropped_{0};
  volatile uint32_t edges_{0};

  // --- Nibble framing ---
  bool have_high_nibble_{false};
  uint8_t high_nibble_{0};
  bool pending_rs_{false};
  uint32_t last_edge_us_{0};

  /// Reconstructed controller state. Shared with bus_sweep so the speculative
  /// decodes there and the live decode here cannot disagree.
  hd44780_core::Hd44780Decoder decoder_;

  uint32_t last_publish_ms_{0};
  uint32_t last_rate_ms_{0};
  uint32_t last_rate_edges_{0};
  std::string last_line_1_;
  std::string last_line_2_;
};

}  // namespace hd44780_tap
}  // namespace esphome
