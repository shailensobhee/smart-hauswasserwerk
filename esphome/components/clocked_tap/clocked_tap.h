#pragma once

#include <string>

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/text_sensor/text_sensor.h"

namespace esphome {
namespace clocked_tap {

// Ring capacity in samples. Must be a power of two (the index wrap uses a
// mask). 2048 * 8 B = 16 kB, about 0.5 s of headroom at the measured 3.8
// kedges/s - enough to ride out an OTA push or a WiFi reconnect without
// losing a frame.
static const uint16_t RING_SIZE = 2048;
static const uint16_t RING_MASK = RING_SIZE - 1;

// The measured frame is 376 bits. Double it, so a frame gap set too wide
// merges two frames without silently truncating the second one away.
static const uint16_t MAX_FRAME_BITS = 768;
static const uint16_t FRAME_BYTES = MAX_FRAME_BITS / 8;

static const uint8_t MAX_LINES = 4;
static const uint8_t MAX_COLUMNS = 40;

/// One clock-edge observation: the whole GPIO input port, latched atomically.
///
/// Same shape as the other components here, and for the same reason: the ISR
/// stays a single register read, and changing which line is data affects
/// decoding only, not capture.
struct Sample {
  uint32_t port;
  uint32_t t_us;
};

class ClockedTap : public Component {
 public:
  void set_clock_pin(InternalGPIOPin *pin) { this->clock_pin_ = pin; }
  void set_data_pin(InternalGPIOPin *pin) { this->data_pin_ = pin; }
  void set_on_rising(bool v) { this->on_rising_ = v; }
  void set_msb_first(bool v) { this->msb_first_ = v; }
  void set_frame_gap_us(uint32_t v) { this->frame_gap_us_ = v; }
  void set_word_bits(uint8_t v) { this->word_bits_ = v; }
  void set_marker(uint8_t a, uint8_t b) {
    this->marker_a_ = a;
    this->marker_b_ = b;
  }
  void set_columns(uint8_t v) { this->columns_ = v; }
  void set_stable_frames(uint8_t v) { this->stable_frames_ = v; }
  void set_blank_hold_ms(uint32_t v) { this->blank_hold_ms_ = v; }
  void set_log_repeats(bool v) { this->log_repeats_ = v; }
  void set_line_1(text_sensor::TextSensor *s) { this->line_1_ = s; }
  void set_line_2(text_sensor::TextSensor *s) { this->line_2_ = s; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  static void gpio_intr(ClockedTap *arg);

 protected:
  void handle_sample_(const Sample &sample);
  void end_frame_();
  void decode_frame_();
  void publish_lines_();
  void flush_blanks_();
  void log_hex_() const;
  uint8_t bit_at_(uint16_t i) const { return (uint8_t) ((this->bits_[i >> 3] >> (7 - (i & 7))) & 1u); }
  uint8_t word_data_(uint16_t start) const;

  InternalGPIOPin *clock_pin_{nullptr};
  InternalGPIOPin *data_pin_{nullptr};
  uint8_t data_bit_{0};

  bool on_rising_{true};
  bool msb_first_{true};
  uint32_t frame_gap_us_{1000};
  uint8_t word_bits_{9};
  uint8_t marker_a_{0x7C};
  uint8_t marker_b_{0x40};
  uint8_t columns_{16};
  uint8_t stable_frames_{2};
  uint32_t blank_hold_ms_{1500};
  bool log_repeats_{false};
  text_sensor::TextSensor *line_1_{nullptr};
  text_sensor::TextSensor *line_2_{nullptr};

  // --- ISR-shared state. Single producer (ISR), single consumer (loop). ---
  Sample ring_[RING_SIZE];
  volatile uint16_t head_{0};
  volatile uint16_t tail_{0};
  volatile uint32_t dropped_{0};
  volatile uint32_t edges_{0};

  // --- Frame assembly ---
  //
  // Bits, not bytes. The two display lines in one frame are 2 bits out of
  // phase with each other, so there is no byte alignment that renders both:
  // the decoder has to be free to resync anywhere, which means keeping the
  // frame as a bit string.
  uint8_t bits_[FRAME_BYTES];
  uint16_t nbits_{0};
  bool in_frame_{false};
  bool overflowed_{false};
  uint32_t last_edge_us_{0};
  uint32_t last_edge_ms_{0};

  // --- Decode results ---
  char lines_[MAX_LINES][MAX_COLUMNS + 1]{};
  uint8_t nlines_{0};
  char prev_screen_[MAX_LINES * (MAX_COLUMNS + 1)]{};

  // A screen has to decode identically `stable_frames_` times running before
  // it counts as the new state. A single flipped bit renders as a one-cell
  // difference, which is otherwise indistinguishable from a real change and
  // would log a spurious line every time - at 10 frames/s the debounce costs
  // 100 ms of latency and removes every single-frame glitch.
  char pending_screen_[MAX_LINES * (MAX_COLUMNS + 1)]{};
  uint8_t pending_count_{0};

  // Repeat suppression keys on the DECODED text, not the raw bits. Single-bit
  // corruption makes almost every raw frame unique - 61 distinct frames in one
  // capture that were all the same two lines - so raw dedup would suppress
  // nothing and the actual screen change would be lost in the scroll.
  uint32_t repeats_{0};
  uint32_t last_log_ms_{0};

  // --- Published state, which is deliberately NOT what the log shows ---------
  //
  // The log prints the panel verbatim, padding and all, because protocol work
  // needs to see reality. Home Assistant wants the opposite: trimmed text, and
  // no entity that rewrites itself twice a second forever.
  //
  // `blank_hold_ms_` is what buys the second property. The Always On screen
  // blinks its warning at 1 Hz - 500 ms of "Not Recommended", 500 ms of blank,
  // indefinitely - and that blink is real, so the stable_frames debounce
  // cannot touch it. Publishing it straight through would write two state
  // changes a second into HA's recorder for as long as the pump sits in that
  // mode. So a line going blank is held: it only publishes once it has STAYED
  // blank longer than the blink period. A real blank (Standby Mode's second
  // line) survives the hold and publishes; a blink never does.
  std::string published_[2];
  bool have_published_[2]{};
  uint32_t blank_since_[2]{};

  uint32_t frames_{0};
  uint32_t undecoded_{0};
  uint32_t garbled_{0};
  uint32_t last_stats_ms_{0};
  uint32_t last_stats_frames_{0};
};

}  // namespace clocked_tap
}  // namespace esphome
