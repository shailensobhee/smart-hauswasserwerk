#pragma once

#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/hd44780_core/hd44780_decoder.h"

namespace esphome {
namespace bus_sweep {

// Bounded by the single-register capture: every pin must live in GPIO_IN_REG.
// Ten covers a whole HD44780 header - power rails and all - which is worth
// supporting because the operator should not have to guess which lines are
// signals before the sweep that exists to tell them.
static const uint8_t MAX_SWEEP_PINS = 10;

// How many scored hypotheses to keep and print.
static const uint8_t TOP_N = 8;

// Bit alignments tried simultaneously in the clocked sweep. A continuous
// clocked stream has no framing, so the word boundary is unknown, and a word
// `w` bits long has `w` possible phases. Sized to the longest word the sweep
// tries (see CLOCKED_WORD_BITS in the .cpp); each trial only uses the first
// `wb` slots.
static const uint8_t BIT_ALIGNMENTS = 9;

/// One edge observation: the whole GPIO input port, latched atomically.
///
/// Identical in shape to bus_probe's sample, and for the same reason - keeping
/// the raw port word means capture is role-agnostic. That is what makes
/// "capture once, replay under every hypothesis" possible at all.
struct SweepSample {
  uint32_t port;
  uint32_t t_us;
};

/// Per-pin behaviour over the whole capture. Same accounting as bus_probe's
/// PinStats, plus the two measures that turned out to be missing there.
struct SweepPinStats {
  uint8_t bit{0};
  std::string label;

  uint32_t rising{0};
  uint32_t falling{0};

  uint32_t last_change_us{0};
  bool level{false};

  uint32_t min_high_us{UINT32_MAX};
  uint32_t max_high_us{0};
  uint32_t min_low_us{UINT32_MAX};
  uint32_t max_low_us{0};

  uint64_t total_high_us{0};
  uint64_t total_low_us{0};

  uint32_t coincident[MAX_SWEEP_PINS]{};

  /// Samples in which this pin's level differed from the other pin's.
  ///
  /// This is the decisive same-wire test, and it is strictly stronger than
  /// coincident[]. Coincident edges only say two lines move together, which
  /// every synchronous bus does by construction - the measured clock and data
  /// share 399 coincident edges and are plainly different wires. Two pins on
  /// ONE physical net, by contrast, must read the same level in EVERY sample,
  /// because a sample is a single atomic port read and there is no skew to
  /// explain a difference away. So:
  ///
  ///   0 disagreements over thousands of samples  ->  one net
  ///   ~100%                                      ->  an inverted copy
  ///   anything in between                        ->  independent lines
  ///
  /// Only accumulated for lines that moved. A line sharing a net with a live
  /// signal cannot be static, and two static lines are indistinguishable to
  /// this test no matter how long the capture runs - that case belongs to the
  /// meter.
  uint32_t disagree[MAX_SWEEP_PINS]{};

  /// Samples in which this pin read HIGH, counted alongside disagree[] and in
  /// the same units.
  ///
  /// This exists because disagree[] alone produced a WRONG finding on run 10c:
  /// "SAME WIRE: D5 == D10 - 0 disagreements in 16383 samples". Both lines were
  /// event lines at rest, high for 100.0% of the capture, so of course they
  /// never disagreed - two unconnected wires that both sit high always agree.
  /// The same run claimed D6 was an inverted copy of three separate lines, for
  /// the mirror-image reason.
  ///
  /// The `active_` filter was supposed to prevent this and cannot: it asks only
  /// whether a line moved AT ALL, and eight edges in 16384 samples is a line
  /// that is static 99.95% of the time. An edge guard is the wrong guard. What
  /// the test actually needs is for each line to have SPENT time at both
  /// levels, because min(samples_high, compared - samples_high) is the number
  /// of samples that could have disagreed - the real denominator of the
  /// evidence, as opposed to the nominal one.
  uint32_t samples_high{0};

  /// Edges where this pin changed and no other tapped pin did. A strobe or a
  /// clock moves alone; data lines set up together and cluster.
  uint32_t solitary{0};

  /// Set by the report when this line's slowest cycle matches mains in both
  /// period and shape.
  bool mains{false};

  /// Log2-bucketed inter-edge gaps. Framed data is bimodal - short gaps within
  /// a byte, long ones between bytes. Noise and mains coupling are not.
  uint32_t gap_hist[16]{};
};

/// A scored interpretation of the capture.
struct Candidate {
  float score{0.0f};
  char kind[12]{};
  char desc[80]{};
  char sample[52]{};

  /// Set bit per stats_ index this interpretation reads from. Lets the verdict
  /// ask whether a hypothesis actually depends on a line that turned out to be
  /// mains-coupled, instead of discarding every hypothesis because some
  /// unrelated line in the tap was.
  uint16_t pins{0};
};

enum class SweepPhase : uint8_t {
  SETTLING,   ///< Letting WiFi and the API come up before touching timing
  CAPTURING,  ///< ISR filling the buffer
  ANALYSING,  ///< Replaying the buffer under every hypothesis, chunked
  DONE,
};

class BusSweep : public Component {
 public:
  void add_sweep_pin(InternalGPIOPin *pin, const std::string &label);
  void set_capacity(uint32_t n) { this->capacity_ = n; }
  void set_settle_delay(uint32_t ms) { this->settle_delay_ms_ = ms; }
  void set_capture_timeout(uint32_t ms) { this->capture_timeout_ms_ = ms; }
  void set_idle_gap_us(uint32_t us) { this->idle_gap_us_ = us; }
  void set_columns(uint8_t c) { this->columns_ = c; }
  void set_verdict(text_sensor::TextSensor *s) { this->verdict_ = s; }

  /// Re-arm from a lambda. Lets the operator provoke the board (press MODE,
  /// change mode) and capture again without a reflash.
  void start_capture();

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  static void gpio_intr(BusSweep *arg);

 protected:
  void begin_analysis_();
  /// Runs one unit of analysis work. Returns false when everything is done.
  bool analysis_step_(uint32_t step);
  void classify_();
  void run_hd44780_trial_(uint32_t idx);
  void run_uart_trial_(uint32_t idx);
  void run_clocked_trial_(uint32_t idx);
  void report_();

  bool decode_hd_trial_(uint32_t idx, uint8_t &e, uint8_t &rs, uint8_t d[4]) const;
  void offer_(const Candidate &c);

  std::vector<InternalGPIOPin *> pins_;
  SweepPinStats stats_[MAX_SWEEP_PINS];
  uint8_t pin_count_{0};

  /// Indices into stats_ of the lines that actually changed during the
  /// capture. Every hypothesis is enumerated over these rather than over all
  /// tapped pins: a line that never moved cannot be a strobe, a clock or a TX,
  /// and on a bus being actively refreshed it is not a data bit either. This
  /// is what keeps a ten-pin header tractable - the ordered HD44780 space over
  /// ten lines is 151 200 trials, over the six that move it is 720.
  uint8_t active_[MAX_SWEEP_PINS]{};
  uint8_t active_count_{0};

  /// Denominator for the disagreement percentages, counted where they are
  /// accumulated rather than inferred from count_ in the report. The two would
  /// only differ by one today, but a report that derives its own denominator
  /// is a report that can be silently wrong about how sure it is.
  uint32_t compared_{0};

  uint32_t capacity_{4096};
  uint32_t settle_delay_ms_{10000};
  uint32_t capture_timeout_ms_{60000};
  uint32_t idle_gap_us_{200};
  uint8_t columns_{16};
  text_sensor::TextSensor *verdict_{nullptr};

  SweepSample *buf_{nullptr};
  volatile uint32_t count_{0};
  volatile uint32_t overflow_{0};
  volatile bool capturing_{false};

  /// Incremented before the capturing_ gate, so "the ISR never fired" and "the
  /// ISR fired while disarmed" are distinguishable. Never reset.
  volatile uint32_t isr_calls_{0};

  /// Set bit per tapped GPIO, so a port word can be masked down to just the
  /// lines under test.
  uint32_t pin_mask_{0};

  // Polled cross-check on the interrupt path. loop() reads the port directly
  // during capture and counts changes it sees for itself. "No edges" and "the
  // ISR never fired" produce identical buffers, and only the first is a
  // finding - without this the component cannot tell them apart and would
  // report a confident NO TRAFFIC for its own broken capture.
  uint32_t poll_prev_port_{0};
  uint32_t poll_changes_{0};
  bool poll_seeded_{false};
  uint32_t last_progress_ms_{0};

  SweepPhase phase_{SweepPhase::SETTLING};
  uint32_t phase_start_ms_{0};
  uint32_t step_{0};

  // Trial-space sizes, computed once the pin count is known.
  uint32_t hd_trials_{0};
  uint32_t uart_trials_{0};
  uint32_t clocked_trials_{0};

  Candidate top_[TOP_N];
  uint8_t top_count_{0};

  /// One decoder instance reused across every HD44780 trial. reset() is total,
  /// so there is no carry-over between trials and no per-trial allocation.
  hd44780_core::Hd44780Decoder decoder_;
};

}  // namespace bus_sweep
}  // namespace esphome
