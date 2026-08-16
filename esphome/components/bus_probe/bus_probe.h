#pragma once

#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/text_sensor/text_sensor.h"

namespace esphome {
namespace bus_probe {

static const uint8_t MAX_PROBE_PINS = 8;

// Larger than the sniffer's ring: an any-edge probe on six lines sees roughly
// six times the events, and losing samples skews the statistics we are trying
// to read. 1024 * 8 B = 8 kB.
static const uint16_t PROBE_RING_SIZE = 1024;
static const uint16_t PROBE_RING_MASK = PROBE_RING_SIZE - 1;

struct ProbeSample {
  uint32_t port;  ///< GPIO_IN_REG at the edge
  uint32_t t_us;
};

/// Per-pin statistics for the current reporting window.
struct PinStats {
  uint8_t bit{0};
  std::string label;

  uint32_t rising{0};
  uint32_t falling{0};

  uint32_t last_change_us{0};
  bool level{false};
  bool initialised{false};

  uint32_t min_high_us{UINT32_MAX};
  uint32_t max_high_us{0};
  uint32_t min_low_us{UINT32_MAX};
  uint32_t max_low_us{0};

  uint64_t total_high_us{0};
  uint64_t total_low_us{0};

  /// How often this pin changed in the same port read as pin j. Data lines set
  /// up together and so cluster; a strobe changes on its own.
  uint32_t coincident[MAX_PROBE_PINS]{};
};

class BusProbe : public Component {
 public:
  void add_probe_pin(InternalGPIOPin *pin, const std::string &label);
  void set_report_interval(uint32_t ms) { this->report_interval_ms_ = ms; }
  void set_raw_dump(bool enabled) { this->raw_dump_ = enabled; }
  void set_summary(text_sensor::TextSensor *s) { this->summary_ = s; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  static void gpio_intr(BusProbe *arg);

 protected:
  void handle_sample_(const ProbeSample &sample);
  void report_();
  void reset_window_();

  std::vector<InternalGPIOPin *> pins_;
  PinStats stats_[MAX_PROBE_PINS];
  uint8_t pin_count_{0};

  uint32_t report_interval_ms_{10000};
  bool raw_dump_{false};
  text_sensor::TextSensor *summary_{nullptr};

  ProbeSample ring_[PROBE_RING_SIZE];
  volatile uint16_t head_{0};
  volatile uint16_t tail_{0};
  volatile uint32_t dropped_{0};

  uint32_t prev_port_{0};
  bool have_prev_{false};

  uint32_t window_start_ms_{0};
  uint32_t window_start_us_{0};
};

}  // namespace bus_probe
}  // namespace esphome
