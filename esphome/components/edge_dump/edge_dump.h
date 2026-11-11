#pragma once

// Dump the raw edge timing of ONE line, so clock recovery can be attempted
// off-device.
//
// Why this exists: the display's DATA line was identified statistically (duty
// cycle, frame period, edge rate) but the CLOCK is not wired, so nothing could
// be decoded and the identification could not be confirmed by reading text.
// Statistics are weaker evidence than text, and the gap mattered enough to
// build a tool for.
//
// A synchronous serial data line holds each bit for one clock period, so the
// interval between its transitions is an integer multiple of that period. If
// the period is stable, the bit stream can be reconstructed WITHOUT the clock
// wire. If it is not stable - a bit-banged driver with variable delays - this
// will fail, and failing visibly is the point. It is a test, not a promise.
//
// Everything here is deliberately dumb: capture edges, print them, analyse on
// the PC. Putting the recovery algorithm on the device would mean a reflash
// per iteration, and the algorithm is exactly the part that needs iterating.

#include "esphome/core/component.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace edge_dump {

// ~140 edges per frame was measured; 2048 holds a dozen frames and costs 16 kB.
static const uint16_t EDGE_DUMP_CAPACITY = 2048;

struct Edge {
  uint32_t t_us;
  uint8_t level;
};

class EdgeDump : public Component {
 public:
  void set_pin(InternalGPIOPin *pin) { this->pin_ = pin; }
  void set_frame_gap_us(uint32_t v) { this->frame_gap_us_ = v; }
  void set_min_edges(uint16_t v) { this->min_edges_ = v; }
  void set_max_frames(uint16_t v) { this->max_frames_ = v; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  static void IRAM_ATTR gpio_intr(EdgeDump *arg);

 protected:
  void dump_frame_(uint16_t count);

  InternalGPIOPin *pin_{nullptr};
  ISRInternalGPIOPin isr_pin_;

  uint32_t frame_gap_us_{1000};
  uint16_t min_edges_{40};
  uint16_t max_frames_{4};
  uint16_t frames_done_{0};

  // Gates the ISR while loop() is walking the buffer, so a frame cannot be
  // overwritten mid-dump. Cheaper and less disruptive than detaching the
  // interrupt, and a dropped frame costs nothing here - the panel sends
  // another one 90 ms later.
  volatile bool collecting_{true};
  volatile uint16_t count_{0};
  volatile uint32_t last_t_{0};

  Edge buf_[EDGE_DUMP_CAPACITY];
};

}  // namespace edge_dump
}  // namespace esphome
