#include "bus_sweep.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <esp_attr.h>
#include <esp_timer.h>
#include <soc/gpio_reg.h>

namespace esphome {
namespace bus_sweep {

using hd44780_core::LINE_1_BASE;
using hd44780_core::LINE_2_BASE;

static const char *const TAG = "bus_sweep";

// Analysis is chunked to this budget per loop() so WiFi, the API and OTA are
// never starved. The whole sweep is a few seconds of CPU; spreading it over
// many loops costs wall-clock time and buys back responsiveness.
static const uint32_t ANALYSIS_BUDGET_MS = 10;

// Samples a line must spend at its MINORITY level before its agreement with
// another line carries information. Matches the 64-sample floor already
// applied to the capture as a whole, and for the same reason: below it,
// "never disagreed" is a statement about how little happened rather than about
// the wiring. See SweepPinStats::samples_high for the run that forced this in.
static const uint32_t NET_MIN_MINORITY = 64;

static const uint32_t STANDARD_BAUDS[] = {1200,  2400,  4800,   9600,
                                          19200, 38400, 57600,  115200};
static const uint8_t NUM_BAUDS = sizeof(STANDARD_BAUDS) / sizeof(STANDARD_BAUDS[0]);

static const uint32_t MAINS_PERIODS_US[] = {20000, 16667};

// Strings the pump is known to display, from the message table in README.md.
// Deliberately truncated to stems: the tap may join mid-refresh, and a pump
// that only rewrites changed characters never shows a full string at once.
static const char *const KNOWN_STRINGS[] = {
    "Automatic", "Standby", "TimeMode", "AlwaysOn", "PUMPOFF",
    "CheckWater", "ValveClosed", "NOTSET", "Mode",
};
static const uint8_t NUM_KNOWN = sizeof(KNOWN_STRINGS) / sizeof(KNOWN_STRINGS[0]);

/// Word lengths the clocked sweep tries.
///
/// 8 is a plain byte stream. 9 is one flag bit followed by a byte, and it is
/// here because leaving it out cost this project a wrong conclusion: the pump's
/// panel uses 9-bit words, and an 8-bit-only sweep cannot see that bus at all.
/// Every ninth bit shifts the whole stream, so text survives only in whatever
/// stretch a capture happens to start on a lucky phase - which produces a
/// confident-looking verdict carrying a fragment of real text and a decode that
/// falls apart everywhere else. The failure mode is not "scores badly", it is
/// "scores well for the wrong reason".
///
/// The payload is always the LAST eight bits of a word, whatever its length, so
/// a leading flag bit is simply never shifted in.
static const uint8_t CLOCKED_WORD_BITS[] = {8, 9};
static const uint8_t NUM_CLOCKED_WORD_BITS = sizeof(CLOCKED_WORD_BITS) / sizeof(CLOCKED_WORD_BITS[0]);

/// Distinct known stems appearing in `text`. Capped by the caller - one hit is
/// strong evidence, five is not five times stronger.
static uint8_t known_hits(const char *text) {
  if (text == nullptr || text[0] == '\0')
    return 0;
  uint8_t hits = 0;
  for (uint8_t i = 0; i < NUM_KNOWN; i++) {
    if (strstr(text, KNOWN_STRINGS[i]) != nullptr)
      hits++;
  }
  return hits;
}

/// The same search, but fed one decoded byte at a time and carrying no buffer.
///
/// The sample arrays a trial fills are display-sized - forty-odd characters,
/// enough for the report table. Searching only those means a known string that
/// lands eighty bytes into a stream is invisible, and that is not hypothetical:
/// one capture surfaced "Standby Mode" in the first forty bytes and the next
/// capture of the same lines put it past the cut and scored the interpretation
/// as noise. Whether a hypothesis is right cannot depend on where in the window
/// the interesting part happened to fall, so matching runs over every byte and
/// only the printable prefix is kept for display.
struct KnownMatcher {
  uint8_t len[NUM_KNOWN]{};
  uint16_t found{0};

  void feed(char ch) {
    for (uint8_t i = 0; i < NUM_KNOWN; i++) {
      if (this->found & (uint16_t) (1u << i))
        continue;
      const char *s = KNOWN_STRINGS[i];
      if (ch == s[this->len[i]]) {
        this->len[i]++;
        if (s[this->len[i]] == '\0') {
          this->found |= (uint16_t) (1u << i);
          this->len[i] = 0;
        }
      } else {
        // Restart from this character rather than backtrack. That is exact for
        // every string in the table except one whose own prefix repeats inside
        // it ("PUMP" in "PUMPOFF"), where an input like "PUMPUMPOFF" would be
        // missed. A KMP table is not worth carrying for that case.
        this->len[i] = (ch == s[0]) ? 1 : 0;
      }
    }
  }

  uint8_t count() const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < NUM_KNOWN; i++)
      if (this->found & (uint16_t) (1u << i))
        n++;
    return n;
  }

  /// Which strings matched. A score says a hypothesis fits; this says what it
  /// found, which is the part an operator can check against the panel.
  std::string names() const {
    std::string out;
    for (uint8_t i = 0; i < NUM_KNOWN; i++) {
      if (!(this->found & (uint16_t) (1u << i)))
        continue;
      if (!out.empty())
        out += ", ";
      out += KNOWN_STRINGS[i];
    }
    return out;
  }
};

void IRAM_ATTR BusSweep::gpio_intr(BusSweep *arg) {
  arg->isr_calls_++;
  if (!arg->capturing_)
    return;

  // One register read captures every tapped line simultaneously. Which pin
  // fired does not matter - the consumer recovers the changed lines by diffing
  // against the previous sample, so simultaneous edges arrive as one sample
  // with several changed bits.
  const uint32_t port = REG_READ(GPIO_IN_REG);
  const uint32_t now = (uint32_t) esp_timer_get_time();

  const uint32_t n = arg->count_;
  if (n >= arg->capacity_) {
    arg->overflow_++;
    return;
  }
  arg->buf_[n].port = port;
  arg->buf_[n].t_us = now;
  arg->count_ = n + 1;
}

void BusSweep::add_sweep_pin(InternalGPIOPin *pin, const std::string &label) {
  if (this->pin_count_ >= MAX_SWEEP_PINS)
    return;
  this->pins_.push_back(pin);
  this->stats_[this->pin_count_].label = label;
  this->pin_count_++;
}

void BusSweep::setup() {
  for (uint8_t i = 0; i < this->pin_count_; i++) {
    InternalGPIOPin *pin = this->pins_[i];
    pin->setup();
    if (pin->get_pin() > 31) {
      ESP_LOGE(TAG, "Sweep pins must be GPIO0-31 for atomic capture");
      this->mark_failed();
      return;
    }
    this->stats_[i].bit = pin->get_pin();
    this->pin_mask_ |= 1UL << pin->get_pin();
  }

  this->buf_ = new (std::nothrow) SweepSample[this->capacity_];
  if (this->buf_ == nullptr) {
    ESP_LOGE(TAG, "Could not allocate %" PRIu32 " samples (%" PRIu32 " bytes)", this->capacity_,
             this->capacity_ * (uint32_t) sizeof(SweepSample));
    this->mark_failed();
    return;
  }

  // Attached now but gated by capturing_, so the ISR is inert until the settle
  // delay expires. Keeping the attachment static avoids any detach/attach race
  // around the analysis phase - the flag alone makes the buffer stable.
  for (uint8_t i = 0; i < this->pin_count_; i++)
    this->pins_[i]->attach_interrupt(&BusSweep::gpio_intr, this, gpio::INTERRUPT_ANY_EDGE);

  this->phase_ = SweepPhase::SETTLING;
  this->phase_start_ms_ = millis();
}

void BusSweep::start_capture() {
  if (this->is_failed())
    return;

  // classify_() accumulates into stats_, so a re-arm has to clear them or the
  // second run reports the sum of both captures. Label and bit are identity,
  // not measurement, and are preserved.
  for (uint8_t i = 0; i < this->pin_count_; i++) {
    SweepPinStats fresh;
    fresh.bit = this->stats_[i].bit;
    fresh.label = this->stats_[i].label;
    this->stats_[i] = fresh;
  }

  this->count_ = 0;
  this->overflow_ = 0;
  this->top_count_ = 0;
  this->step_ = 0;
  this->compared_ = 0;
  this->poll_changes_ = 0;
  this->poll_seeded_ = false;
  this->phase_ = SweepPhase::CAPTURING;
  this->phase_start_ms_ = millis();
  this->last_progress_ms_ = this->phase_start_ms_;
  this->capturing_ = true;
  ESP_LOGI(TAG, "Capture armed: %" PRIu32 " sample buffer, %" PRIu32 " s timeout.",
           this->capacity_, this->capture_timeout_ms_ / 1000);
  ESP_LOGI(TAG, "Exercise the board NOW - press buttons, change modes. A header that");
  ESP_LOGI(TAG, "only speaks on events is indistinguishable from a dead one if idle.");
}

void BusSweep::loop() {
  const uint32_t now = millis();

  switch (this->phase_) {
    case SweepPhase::SETTLING:
      if (now - this->phase_start_ms_ >= this->settle_delay_ms_)
        this->start_capture();
      break;

    case SweepPhase::CAPTURING: {
      // Polled cross-check. This samples far too slowly to capture a bus, and
      // is not trying to - it only has to prove that *something* moved, so
      // that a zero-sample buffer can be attributed to a quiet header rather
      // than to a dead interrupt.
      const uint32_t port = REG_READ(GPIO_IN_REG) & this->pin_mask_;
      if (!this->poll_seeded_) {
        this->poll_prev_port_ = port;
        this->poll_seeded_ = true;
      } else if (port != this->poll_prev_port_) {
        this->poll_changes_++;
        this->poll_prev_port_ = port;
      }

      if (now - this->last_progress_ms_ >= 10000) {
        this->last_progress_ms_ = now;
        ESP_LOGI(TAG,
                 "capturing: %" PRIu32 " samples, %" PRIu32 " isr calls, %" PRIu32
                 " polled changes, %" PRIu32 " s left",
                 this->count_, this->isr_calls_, this->poll_changes_,
                 (this->capture_timeout_ms_ - (now - this->phase_start_ms_)) / 1000);
      }

      if (this->count_ >= this->capacity_ ||
          now - this->phase_start_ms_ >= this->capture_timeout_ms_)
        this->begin_analysis_();
      break;
    }

    case SweepPhase::ANALYSING: {
      const uint32_t budget_start = millis();
      while (millis() - budget_start < ANALYSIS_BUDGET_MS) {
        if (!this->analysis_step_(this->step_)) {
          this->report_();
          this->phase_ = SweepPhase::DONE;
          break;
        }
        this->step_++;
      }
      break;
    }

    case SweepPhase::DONE:
      break;
  }
}

void BusSweep::begin_analysis_() {
  this->capturing_ = false;

  const uint32_t count = this->count_;

  ESP_LOGI(TAG, "--- capture complete: %" PRIu32 " samples, %" PRIu32 " overflowed, %.1f s ---",
           count, this->overflow_, (millis() - this->phase_start_ms_) / 1000.0f);

  // Which lines actually moved. One XOR scan over the buffer, before any
  // hypothesis is enumerated.
  //
  // This is what makes a whole ten-pin header sweepable. Every hypothesis here
  // needs its pins to carry edges - E strobes, a UART start bit, a clock - and
  // a line that never changed contributes a constant to every decode, so those
  // permutations score zero no matter how many of them are tried. Enumerating
  // them anyway would turn 720 trials into 151 200 and buy nothing. The other
  // half of the argument is the false-positive rate: a larger search space
  // means more chances for noise to fit, so pruning provably-dead branches
  // makes the surviving winner more believable, not less.
  uint32_t moved_mask = 0;
  for (uint32_t k = 1; k < count; k++)
    moved_mask |= this->buf_[k].port ^ this->buf_[k - 1].port;
  moved_mask &= this->pin_mask_;

  this->active_count_ = 0;
  for (uint8_t i = 0; i < this->pin_count_; i++)
    if ((moved_mask >> this->stats_[i].bit) & 1)
      this->active_[this->active_count_++] = i;

  const uint8_t n = this->active_count_;

  if (n < this->pin_count_) {
    std::string moved, still;
    for (uint8_t i = 0; i < this->pin_count_; i++) {
      std::string &dst = ((moved_mask >> this->stats_[i].bit) & 1) ? moved : still;
      if (!dst.empty())
        dst += " ";
      dst += this->stats_[i].label;
    }
    ESP_LOGI(TAG, "Active lines: %s", moved.empty() ? "(none)" : moved.c_str());
    ESP_LOGI(TAG, "Static lines, excluded from every permutation: %s", still.c_str());
    ESP_LOGI(TAG, "  (a line that never changed cannot be a strobe, clock or TX,");
    ESP_LOGI(TAG, "   and decodes to a constant - those trials would score zero.)");
  }

  // HD44780 needs six distinct roles; with fewer moving lines the hypothesis
  // cannot even be expressed. P(n-2, 4) ordered data assignments per (E, RS).
  if (n >= 6) {
    const uint32_t m = n - 2;
    const uint32_t nd = m * (m - 1) * (m - 2) * (m - 3);
    this->hd_trials_ = (uint32_t) n * (n - 1) * nd;
  } else {
    this->hd_trials_ = 0;
  }
  this->uart_trials_ = (uint32_t) n * NUM_BAUDS;
  // clock x data x polarity x bit order x word length. The bit alignments are
  // scored inside each trial rather than multiplying the trial count; word
  // length cannot be, because it changes how many alignments there are.
  this->clocked_trials_ =
      n >= 2 ? (uint32_t) n * (n - 1) * 2 * 2 * NUM_CLOCKED_WORD_BITS : 0;

  ESP_LOGI(TAG, "Analysing %u active of %u tapped lines: %" PRIu32 " HD44780 + %" PRIu32
                " UART + %" PRIu32 " clocked hypotheses",
           this->active_count_, this->pin_count_, this->hd_trials_, this->uart_trials_,
           this->clocked_trials_);

  this->step_ = 0;
  this->phase_ = SweepPhase::ANALYSING;
}

bool BusSweep::analysis_step_(uint32_t step) {
  if (step == 0) {
    this->classify_();
    return true;
  }
  uint32_t idx = step - 1;

  if (idx < this->hd_trials_) {
    this->run_hd44780_trial_(idx);
    return true;
  }
  idx -= this->hd_trials_;

  if (idx < this->uart_trials_) {
    this->run_uart_trial_(idx);
    return true;
  }
  idx -= this->uart_trials_;

  if (idx < this->clocked_trials_) {
    this->run_clocked_trial_(idx);
    return true;
  }
  return false;
}

// --- Phase 2: classification ----------------------------------------------

void BusSweep::classify_() {
  const uint32_t count = this->count_;
  if (count < 2)
    return;

  for (uint8_t i = 0; i < this->pin_count_; i++) {
    SweepPinStats &s = this->stats_[i];
    s.level = (this->buf_[0].port >> s.bit) & 1;
    s.last_change_us = this->buf_[0].t_us;
  }

  uint32_t prev_port = this->buf_[0].port;
  for (uint32_t k = 1; k < count; k++) {
    const uint32_t port = this->buf_[k].port;
    const uint32_t changed = port ^ prev_port;
    prev_port = port;

    // Net identity, accumulated before the changed==0 skip below. Identical
    // consecutive port words are not noise to be discarded here - they are the
    // signature of the very thing being looked for. Two pins on one wire have
    // two separate GPIO interrupts, so a single edge fires the ISR twice and
    // the second call stores a sample with no change in it. Skipping those
    // would throw away the strongest evidence for the case they indicate.
    //
    // Scoped to active_ - see SweepPinStats::disagree for why static lines are
    // excluded - which also keeps this cheap: 8 active lines is 28 pairs.
    this->compared_++;
    for (uint8_t a = 0; a < this->active_count_; a++) {
      const uint8_t ia = this->active_[a];
      const uint32_t la = (port >> this->stats_[ia].bit) & 1;
      // Sample-weighted, unlike total_high_us, and that is the point - the
      // guard in report_() has to be in the same units as the disagreement
      // count it qualifies. See SweepPinStats::samples_high.
      if (la)
        this->stats_[ia].samples_high++;
      for (uint8_t b = a + 1; b < this->active_count_; b++) {
        const uint8_t ib = this->active_[b];
        if (la != ((port >> this->stats_[ib].bit) & 1)) {
          this->stats_[ia].disagree[ib]++;
          this->stats_[ib].disagree[ia]++;
        }
      }
    }

    if (changed == 0)
      continue;

    uint8_t moved[MAX_SWEEP_PINS];
    uint8_t moved_count = 0;

    for (uint8_t i = 0; i < this->pin_count_; i++) {
      SweepPinStats &s = this->stats_[i];
      if (!((changed >> s.bit) & 1))
        continue;

      const uint32_t held = this->buf_[k].t_us - s.last_change_us;
      if (s.level) {
        s.total_high_us += held;
        if (held < s.min_high_us)
          s.min_high_us = held;
        if (held > s.max_high_us)
          s.max_high_us = held;
        s.falling++;
      } else {
        s.total_low_us += held;
        if (held < s.min_low_us)
          s.min_low_us = held;
        if (held > s.max_low_us)
          s.max_low_us = held;
        s.rising++;
      }

      // Log2 bucket of the gap since this pin last moved.
      uint8_t bucket = 0;
      uint32_t g = held;
      while (g > 1 && bucket < 15) {
        g >>= 1;
        bucket++;
      }
      s.gap_hist[bucket]++;

      s.level = !s.level;
      s.last_change_us = this->buf_[k].t_us;
      moved[moved_count++] = i;
    }

    if (moved_count == 1) {
      this->stats_[moved[0]].solitary++;
    } else {
      for (uint8_t a = 0; a < moved_count; a++)
        for (uint8_t b = 0; b < moved_count; b++)
          if (a != b)
            this->stats_[moved[a]].coincident[moved[b]]++;
    }
  }
}

// --- Phase 3a: HD44780 permutations ---------------------------------------

bool BusSweep::decode_hd_trial_(uint32_t idx, uint8_t &e, uint8_t &rs, uint8_t d[4]) const {
  // Enumerated over the active lines only; the slots below index active_, and
  // every role is mapped back to a stats_ index on the way out.
  const uint8_t n = this->active_count_;
  if (n < 6)
    return false;

  const uint32_t m = n - 2;
  const uint32_t nd = m * (m - 1) * (m - 2) * (m - 3);
  const uint32_t per_e = (uint32_t) (n - 1) * nd;

  const uint8_t e_slot = (uint8_t) (idx / per_e);
  uint32_t r = idx % per_e;
  const uint32_t rs_slot = r / nd;
  uint32_t d_sel = r % nd;

  uint8_t rest[MAX_SWEEP_PINS];
  uint8_t rc = 0;
  for (uint8_t i = 0; i < n; i++)
    if (i != e_slot)
      rest[rc++] = i;
  const uint8_t rs_pick = rest[rs_slot];

  uint8_t pool[MAX_SWEEP_PINS];
  uint8_t pc = 0;
  for (uint8_t i = 0; i < n; i++)
    if (i != e_slot && i != rs_pick)
      pool[pc++] = i;

  // Factorial-base decode of an ordered 4-of-m selection.
  uint32_t div = nd;
  for (uint8_t j = 0; j < 4; j++) {
    div /= (m - j);
    const uint32_t sel = d_sel / div;
    d_sel %= div;
    d[j] = this->active_[pool[sel]];
    for (uint8_t t = (uint8_t) sel; t + 1 < (uint8_t) (m - j); t++)
      pool[t] = pool[t + 1];
  }

  e = this->active_[e_slot];
  rs = this->active_[rs_pick];
  return true;
}

void BusSweep::run_hd44780_trial_(uint32_t idx) {
  uint8_t e, rs, d[4];
  if (!this->decode_hd_trial_(idx, e, rs, d))
    return;

  const uint32_t count = this->count_;
  if (count < 4)
    return;

  const uint8_t e_bit = this->stats_[e].bit;
  const uint8_t rs_bit = this->stats_[rs].bit;
  uint8_t d_bits[4];
  for (uint8_t i = 0; i < 4; i++)
    d_bits[i] = this->stats_[d[i]].bit;

  this->decoder_.reset();

  bool have_high = false;
  uint8_t high = 0;
  bool pend_rs = false;
  uint32_t last_t = this->buf_[0].t_us;
  uint32_t prev_port = this->buf_[0].port;
  uint32_t strobes = 0, orphans = 0;

  // Sized to fit Candidate::sample without truncation. Display only - the
  // known-string search runs over every byte via the matcher.
  char text[48];
  uint8_t tlen = 0;
  KnownMatcher km;

  for (uint32_t k = 1; k < count; k++) {
    const uint32_t port = this->buf_[k].port;
    const bool fell = ((prev_port >> e_bit) & 1) && !((port >> e_bit) & 1);
    prev_port = port;
    if (!fell)
      continue;

    strobes++;
    const bool rs_v = (port >> rs_bit) & 1;
    uint8_t nib = 0;
    for (uint8_t i = 0; i < 4; i++)
      nib |= (uint8_t) (((port >> d_bits[i]) & 1) << i);

    const uint32_t gap = this->buf_[k].t_us - last_t;
    last_t = this->buf_[k].t_us;

    // Same two resync signals the live tap uses: RS is held for a whole byte,
    // and an idle gap ends one.
    if (have_high && (rs_v != pend_rs || gap > this->idle_gap_us_)) {
      have_high = false;
      orphans++;
    }
    if (!have_high) {
      high = nib;
      pend_rs = rs_v;
      have_high = true;
      continue;
    }
    have_high = false;

    const uint8_t val = (uint8_t) ((high << 4) | nib);
    this->decoder_.feed_byte(pend_rs, val);
    if (pend_rs) {
      km.feed((char) val);
      if (tlen < sizeof(text) - 1)
        text[tlen++] = (val >= 0x20 && val <= 0x7E) ? (char) val : '.';
    }
  }
  text[tlen] = '\0';

  // Too few strobes and every ratio below is dominated by luck.
  if (strobes < 16)
    return;

  const hd44780_core::DecodeStats &st = this->decoder_.stats;
  float score = 0.0f;

  // The strongest single discriminator. Real firmware addresses 0x00-0x27 and
  // 0x40-0x67; a wrong assignment scatters uniformly over 0x00-0x7F, so about
  // a quarter of its addresses land in the hole between the two lines.
  if (st.ddram_addr_cmds >= 2)
    score += 40.0f * ((float) st.ddram_addr_valid / (float) st.ddram_addr_cmds);
  if (st.data_bytes >= 4)
    score += 30.0f * ((float) st.printable / (float) st.data_bytes);
  if (st.saw_2line_function_set)
    score += 10.0f;
  if (st.clear_display > 0)
    score += 3.0f;
  score -= 25.0f * ((float) orphans / (float) strobes);

  const std::string l1 = this->decoder_.render_line(LINE_1_BASE, this->columns_);
  const std::string l2 = this->decoder_.render_line(LINE_2_BASE, this->columns_);

  uint8_t hits = km.count();
  if (!l1.empty())
    hits += known_hits(l1.c_str());
  if (!l2.empty())
    hits += known_hits(l2.c_str());
  if (hits > 2)
    hits = 2;
  score += 20.0f * (float) hits;

  if (score <= 0.0f)
    return;

  Candidate c;
  c.score = score;
  c.pins = (uint16_t) ((1u << e) | (1u << rs) | (1u << d[0]) | (1u << d[1]) | (1u << d[2]) |
                       (1u << d[3]));
  snprintf(c.kind, sizeof(c.kind), "HD44780");
  snprintf(c.desc, sizeof(c.desc), "E=%s RS=%s D4-7=%s,%s,%s,%s", this->stats_[e].label.c_str(),
           this->stats_[rs].label.c_str(), this->stats_[d[0]].label.c_str(),
           this->stats_[d[1]].label.c_str(), this->stats_[d[2]].label.c_str(),
           this->stats_[d[3]].label.c_str());
  if (!l1.empty() || !l2.empty()) {
    snprintf(c.sample, sizeof(c.sample), "[%s|%s]", l1.c_str(), l2.c_str());
  } else {
    snprintf(c.sample, sizeof(c.sample), "%s", text);
  }
  this->offer_(c);
}

// --- Phase 3b: async serial -----------------------------------------------

void BusSweep::run_uart_trial_(uint32_t idx) {
  const uint8_t slot = (uint8_t) (idx / NUM_BAUDS);
  const uint32_t baud = STANDARD_BAUDS[idx % NUM_BAUDS];
  const uint32_t count = this->count_;
  if (count < 8 || slot >= this->active_count_)
    return;
  const uint8_t p = this->active_[slot];

  const SweepPinStats &ps = this->stats_[p];
  if (ps.rising + ps.falling < 8)
    return;

  const uint8_t bit = ps.bit;
  const float bit_us = 1000000.0f / (float) baud;
  const uint32_t last_t = this->buf_[count - 1].t_us;

  // The capture holds only transitions, so the level at an arbitrary instant is
  // the level of the most recent sample at or before it. Both the sampling
  // instants within a frame and the frames themselves advance monotonically, so
  // a single forward-only cursor is enough - no search per bit.
  uint32_t cursor = 0;
  auto level_at = [&](uint32_t t) -> bool {
    while (cursor + 1 < count && this->buf_[cursor + 1].t_us <= t)
      cursor++;
    return (this->buf_[cursor].port >> bit) & 1;
  };

  uint32_t frames = 0, errs = 0, printable = 0;
  uint32_t next_ok = 0;
  bool have_next_ok = false;
  // Sized to fit Candidate::sample without truncation. Display only - the
  // known-string search runs over every byte via the matcher.
  char text[48];
  uint8_t tlen = 0;
  KnownMatcher km;

  for (uint32_t k = 1; k < count; k++) {
    const bool fell = ((this->buf_[k - 1].port >> bit) & 1) && !((this->buf_[k].port >> bit) & 1);
    if (!fell)
      continue;

    const uint32_t t0 = this->buf_[k].t_us;
    if (have_next_ok && (int32_t) (t0 - next_ok) < 0)
      continue;  // Still inside the previous frame - not a start bit.

    const uint32_t frame_end = t0 + (uint32_t) (9.5f * bit_us);
    if (frame_end > last_t)
      break;  // Frame runs past the end of the capture; nothing to sample.

    uint8_t val = 0;
    for (uint8_t b = 0; b < 8; b++) {
      // 8N1, LSB first, sampled mid-bit.
      if (level_at(t0 + (uint32_t) ((1.5f + (float) b) * bit_us)))
        val |= (uint8_t) (1 << b);
    }
    const bool stop_ok = level_at(frame_end);

    frames++;
    if (!stop_ok)
      errs++;
    km.feed((char) val);
    if (val >= 0x20 && val <= 0x7E) {
      printable++;
      if (tlen < sizeof(text) - 1)
        text[tlen++] = (char) val;
    } else if (tlen < sizeof(text) - 1) {
      text[tlen++] = '.';
    }

    next_ok = t0 + (uint32_t) (10.0f * bit_us);
    have_next_ok = true;
  }
  text[tlen] = '\0';

  if (frames < 8)
    return;

  // The stop-bit check does most of the work here. Noise can fake printable
  // bytes at some baud rate; it very rarely fakes valid framing as well.
  float score = 45.0f * ((float) printable / (float) frames) -
                40.0f * ((float) errs / (float) frames);
  uint8_t hits = km.count();
  if (hits > 2)
    hits = 2;
  score += 20.0f * (float) hits;

  if (score <= 0.0f)
    return;

  Candidate c;
  c.score = score;
  c.pins = (uint16_t) (1u << p);
  snprintf(c.kind, sizeof(c.kind), "UART");
  snprintf(c.desc, sizeof(c.desc), "%s @ %" PRIu32 " 8N1 (%" PRIu32 " frames, %" PRIu32 " err)",
           ps.label.c_str(), baud, frames, errs);
  snprintf(c.sample, sizeof(c.sample), "%s", text);
  this->offer_(c);
}

// --- Phase 3c: clocked / synchronous --------------------------------------

void BusSweep::run_clocked_trial_(uint32_t idx) {
  const uint8_t n = this->active_count_;
  const uint32_t count = this->count_;
  if (count < 16 || n < 2)
    return;

  uint32_t t = idx;
  const uint8_t wb = CLOCKED_WORD_BITS[t % NUM_CLOCKED_WORD_BITS];
  t /= NUM_CLOCKED_WORD_BITS;
  const uint8_t msb_first = (uint8_t) (t % 2);
  t /= 2;
  const uint8_t on_rising = (uint8_t) (t % 2);
  t /= 2;
  const uint8_t c_slot = (uint8_t) (t / (n - 1));
  const uint32_t dslot = t % (n - 1);
  if (c_slot >= n)
    return;

  uint8_t d_slot = 0;
  uint32_t seen = 0;
  for (uint8_t i = 0; i < n; i++) {
    if (i == c_slot)
      continue;
    if (seen == dslot) {
      d_slot = i;
      break;
    }
    seen++;
  }

  const uint8_t c = this->active_[c_slot];
  const uint8_t data_pin = this->active_[d_slot];

  const uint8_t c_bit = this->stats_[c].bit;
  const uint8_t d_bit = this->stats_[data_pin].bit;

  // A continuous clocked stream carries no framing, so the word boundary is
  // unknown. All `wb` alignments are accumulated in one pass and the best is
  // taken - cheaper than replaying the buffer once per phase.
  uint32_t acc[BIT_ALIGNMENTS] = {};
  uint32_t nbytes[BIT_ALIGNMENTS] = {};
  uint32_t nprint[BIT_ALIGNMENTS] = {};
  // Longer than Candidate::sample on purpose: a hit anywhere in the stream is
  // worth printing in full, and truncating to the report column would hide the
  // context that makes it checkable against the panel.
  char texts[BIT_ALIGNMENTS][96];
  uint8_t tlens[BIT_ALIGNMENTS] = {};
  KnownMatcher km[BIT_ALIGNMENTS];
  uint32_t bitidx = 0;

  for (uint32_t k = 1; k < count; k++) {
    const uint32_t prev = this->buf_[k - 1].port;
    const uint32_t port = this->buf_[k].port;
    const bool was = (prev >> c_bit) & 1;
    const bool is = (port >> c_bit) & 1;
    const bool match = on_rising ? (!was && is) : (was && !is);
    if (!match)
      continue;

    const uint8_t b = (uint8_t) ((port >> d_bit) & 1);
    for (uint8_t a = 0; a < wb; a++) {
      if (bitidx < a)
        continue;
      const uint32_t pos = (bitidx - a) % wb;
      // Only the last eight bits of a word are payload. On a 9-bit word that
      // drops the leading flag bit, which is the whole point: shifting it in
      // is what makes an 8-bit reading of a 9-bit bus decode to noise.
      if (pos >= (uint32_t) (wb - 8)) {
        if (msb_first) {
          acc[a] = (acc[a] << 1) | b;
        } else {
          acc[a] |= (uint32_t) b << (pos - (wb - 8));
        }
      }
      if (pos == (uint32_t) (wb - 1)) {
        const uint8_t v = (uint8_t) (acc[a] & 0xFF);
        acc[a] = 0;
        nbytes[a]++;
        km[a].feed((char) v);
        if (v >= 0x20 && v <= 0x7E) {
          nprint[a]++;
          if (tlens[a] < sizeof(texts[a]) - 1)
            texts[a][tlens[a]++] = (char) v;
        } else if (tlens[a] < sizeof(texts[a]) - 1) {
          texts[a][tlens[a]++] = '.';
        }
      }
    }
    bitidx++;
  }

  // Pick the byte boundary by its full score, not by printable ratio alone.
  //
  // Ratio-first is wrong whenever a wrong alignment produces more bytes that
  // merely happen to land in 0x20-0x7E than the right one does. That is not a
  // corner case: a shifted alignment smears each real character across two
  // output bytes and lands in the printable range about a third of the time,
  // which is enough to beat a correct alignment carrying spaces and control
  // bytes. An alignment that spells out a string the display is known to show
  // is the right one, whatever its ratio, so hits have to be weighed while the
  // choice is being made rather than after it.
  //
  // Weighted below HD44780 and UART on purpose. This hypothesis has the most
  // free parameters - clock, data line, polarity, bit order, alignment - so it
  // has the most opportunity to fit noise by coincidence.
  uint8_t best = 0;
  float best_score = -1.0f;
  for (uint8_t a = 0; a < wb; a++) {
    texts[a][tlens[a]] = '\0';
    if (nbytes[a] < 8)
      continue;
    uint8_t h = km[a].count();
    if (h > 2)
      h = 2;
    const float s = 35.0f * ((float) nprint[a] / (float) nbytes[a]) + 20.0f * (float) h;
    if (s > best_score) {
      best_score = s;
      best = a;
    }
  }
  if (best_score < 0.0f)
    return;

  const float score = best_score;
  if (score <= 0.0f)
    return;

  if (km[best].count() > 0) {
    ESP_LOGI(TAG, "  hit: CLK=%s%s DATA=%s %s %ub align%u matched [%s]",
             this->stats_[c].label.c_str(), on_rising ? "/rise" : "/fall",
             this->stats_[data_pin].label.c_str(), msb_first ? "MSB" : "LSB", wb, best,
             km[best].names().c_str());
    ESP_LOGI(TAG, "       %s", texts[best]);
  }

  Candidate cand;
  cand.score = score;
  cand.pins = (uint16_t) ((1u << c) | (1u << data_pin));
  snprintf(cand.kind, sizeof(cand.kind), "CLOCKED");
  snprintf(cand.desc, sizeof(cand.desc), "CLK=%s%s DATA=%s %s %ub align%u (%" PRIu32 " B)",
           this->stats_[c].label.c_str(), on_rising ? "/rise" : "/fall",
           this->stats_[data_pin].label.c_str(), msb_first ? "MSB" : "LSB", wb, best,
           nbytes[best]);
  snprintf(cand.sample, sizeof(cand.sample), "%s", texts[best]);
  this->offer_(cand);
}

// --- Ranking ---------------------------------------------------------------

void BusSweep::offer_(const Candidate &c) {
  if (this->top_count_ < TOP_N) {
    this->top_[this->top_count_++] = c;
  } else {
    // Replace the weakest, if this beats it.
    uint8_t worst = 0;
    for (uint8_t i = 1; i < TOP_N; i++)
      if (this->top_[i].score < this->top_[worst].score)
        worst = i;
    if (this->top_[worst].score >= c.score)
      return;
    this->top_[worst] = c;
  }
  // Insertion sort, descending. The table is eight entries; nothing subtler is
  // worth the code.
  for (uint8_t i = 1; i < this->top_count_; i++) {
    Candidate key = this->top_[i];
    int8_t j = (int8_t) i - 1;
    while (j >= 0 && this->top_[j].score < key.score) {
      this->top_[j + 1] = this->top_[j];
      j--;
    }
    this->top_[j + 1] = key;
  }
}

// --- Phase 4: report -------------------------------------------------------

void BusSweep::report_() {
  const uint32_t count = this->count_;

  ESP_LOGI(TAG, "==================== SWEEP REPORT ====================");

  // Read the port directly: a pin that never moved has never fired its ISR, and
  // a static level is exactly the signature of a rail or a strapped line.
  const uint32_t port = REG_READ(GPIO_IN_REG);

  uint32_t span_us = 0;
  if (count >= 2)
    span_us = this->buf_[count - 1].t_us - this->buf_[0].t_us;

  ESP_LOGI(TAG, "%" PRIu32 " samples over %.2f s (%" PRIu32 " overflowed)", count,
           span_us / 1000000.0f, this->overflow_);

  ESP_LOGI(TAG, " # label  now  edges  edge/s   high%%   min_hi   min_lo   max_hi   max_lo  solo%%");

  uint8_t static_pins = 0, mains_pins = 0;
  std::string compact;

  for (uint8_t i = 0; i < this->pin_count_; i++) {
    SweepPinStats &s = this->stats_[i];
    const uint32_t edges = s.rising + s.falling;
    const uint8_t now_level = (port >> s.bit) & 1;
    const float rate = span_us > 0 ? (float) edges * 1000000.0f / (float) span_us : 0.0f;

    const uint64_t accounted = s.total_high_us + s.total_low_us;
    char duty[12];
    if (accounted > 0) {
      snprintf(duty, sizeof(duty), "%5.1f%%",
               (double) s.total_high_us * 100.0 / (double) accounted);
    } else {
      snprintf(duty, sizeof(duty), "%6s", now_level ? "100%" : "0%");
    }

    char solo[12];
    if (edges > 0) {
      snprintf(solo, sizeof(solo), "%5.0f%%", (double) s.solitary * 100.0 / (double) edges);
    } else {
      snprintf(solo, sizeof(solo), "%6s", "-");
    }

    ESP_LOGI(TAG, "%2u %-6s  %u %6" PRIu32 " %7.1f  %s %8" PRIu32 " %8" PRIu32 " %8" PRIu32
                  " %8" PRIu32 "  %s",
             i, s.label.c_str(), now_level, edges, rate, duty,
             s.min_high_us == UINT32_MAX ? 0 : s.min_high_us,
             s.min_low_us == UINT32_MAX ? 0 : s.min_low_us, s.max_high_us, s.max_low_us, solo);

    if (edges == 0) {
      static_pins++;
      ESP_LOGI(TAG, "     %s: static %s for the whole capture - rail, ground, strap,",
               s.label.c_str(), now_level ? "HIGH" : "LOW");
      ESP_LOGI(TAG, "     or an idle programming/debug line. Cross-check with the meter.");
    }

    // Mains detection needs both the period AND the shape. Summing the longest
    // high and the longest low lands near 20 ms on a floating input often
    // enough to matter - a 2.9 ms high next to a 17.8 ms low sums to 20.7 ms
    // and is not mains at all. A sine crossing a fixed threshold splits the
    // period far more evenly than that even with a healthy DC offset; the two
    // genuinely mains-locked pins measured on this board came in at 52/48 and
    // 44/56. Requiring each half to hold a quarter of the period keeps those
    // and rejects the lopsided noise, which matters because this verdict
    // short-circuits every other hypothesis and sends the operator back to
    // re-check a ground bond that may be perfectly good.
    const uint64_t slow_period = (uint64_t) s.max_high_us + (uint64_t) s.max_low_us;
    for (uint32_t mains_period : MAINS_PERIODS_US) {
      if (slow_period == 0)
        continue;
      const float err = fabsf((float) slow_period - (float) mains_period) / (float) mains_period;
      if (err >= 0.10f)
        continue;

      const float high_share = (float) s.max_high_us / (float) slow_period;
      if (high_share < 0.25f || high_share > 0.75f) {
        ESP_LOGI(TAG, "     %s: slowest cycle %.1f ms is mains-like, but the halves split",
                 s.label.c_str(), slow_period / 1000.0f);
        ESP_LOGI(TAG, "     %.0f/%.0f - too lopsided for a sine. Reads as a floating input.",
                 high_share * 100.0f, (1.0f - high_share) * 100.0f);
        break;
      }

      mains_pins++;
      s.mains = true;
      ESP_LOGW(TAG, "     %s: slowest cycle %.1f ms -> %0.f Hz, halves %.0f/%.0f. MAINS, not data.",
               s.label.c_str(), slow_period / 1000.0f, 1000000.0f / (float) mains_period,
               high_share * 100.0f, (1.0f - high_share) * 100.0f);
      break;
    }

    char entry[24];
    snprintf(entry, sizeof(entry), "%s:%u/%.0f ", s.label.c_str(), now_level, rate);
    compact += entry;
  }

  // Gap histograms, but only for pins that actually moved. Bimodal gaps mean
  // framed data: short within a byte, long between bytes.
  for (uint8_t i = 0; i < this->pin_count_; i++) {
    SweepPinStats &s = this->stats_[i];
    if (s.rising + s.falling == 0)
      continue;
    std::string row;
    char lead[16];
    snprintf(lead, sizeof(lead), "%-6s gaps:", s.label.c_str());
    row += lead;
    for (uint8_t b = 0; b < 16; b++) {
      char cell[12];
      snprintf(cell, sizeof(cell), " %" PRIu32, s.gap_hist[b]);
      row += cell;
    }
    ESP_LOGI(TAG, "%s", row.c_str());
  }
  ESP_LOGI(TAG, "(gap buckets are log2 microseconds: 1,2,4,8,...,32768+)");

  bool any_edges = false;
  for (uint8_t i = 0; i < this->pin_count_; i++)
    if (this->stats_[i].rising + this->stats_[i].falling > 0)
      any_edges = true;

  if (any_edges) {
    ESP_LOGI(TAG, "coincident edges (same port read):");
    std::string header = "        ";
    for (uint8_t j = 0; j < this->pin_count_; j++) {
      char cell[10];
      snprintf(cell, sizeof(cell), "%7s", this->stats_[j].label.c_str());
      header += cell;
    }
    ESP_LOGI(TAG, "%s", header.c_str());
    for (uint8_t i = 0; i < this->pin_count_; i++) {
      std::string row;
      char lead[10];
      snprintf(lead, sizeof(lead), "%-8s", this->stats_[i].label.c_str());
      row += lead;
      for (uint8_t j = 0; j < this->pin_count_; j++) {
        char cell[10];
        if (i == j) {
          snprintf(cell, sizeof(cell), "%7s", "-");
        } else {
          snprintf(cell, sizeof(cell), "%7" PRIu32, this->stats_[i].coincident[j]);
        }
        row += cell;
      }
      ESP_LOGI(TAG, "%s", row.c_str());
    }
  }

  // --- Net identity ---------------------------------------------------------
  //
  // The question this answers is "are any two of these wires the same wire?",
  // which the coincident-edge matrix above cannot settle - see
  // SweepPinStats::disagree. A matrix is something to read; the findings
  // printed under it are something to act on, so both are emitted and the
  // findings go out at WARN so they survive a report that is otherwise a wall
  // of INFO.
  std::string net_finding;
  if (this->active_count_ >= 2 && this->compared_ > 0) {
    ESP_LOGI(TAG, "level disagreement (%% of %" PRIu32 " samples, moving lines only):",
             this->compared_);
    std::string header = "        ";
    for (uint8_t a = 0; a < this->active_count_; a++) {
      char cell[10];
      snprintf(cell, sizeof(cell), "%7s", this->stats_[this->active_[a]].label.c_str());
      header += cell;
    }
    ESP_LOGI(TAG, "%s", header.c_str());
    for (uint8_t a = 0; a < this->active_count_; a++) {
      const uint8_t ia = this->active_[a];
      std::string row;
      char lead[10];
      snprintf(lead, sizeof(lead), "%-8s", this->stats_[ia].label.c_str());
      row += lead;
      for (uint8_t b = 0; b < this->active_count_; b++) {
        char cell[10];
        if (a == b) {
          snprintf(cell, sizeof(cell), "%7s", "-");
        } else {
          const uint8_t ib = this->active_[b];
          snprintf(cell, sizeof(cell), "%6.1f%%",
                   (double) this->stats_[ia].disagree[ib] * 100.0 / (double) this->compared_);
        }
        row += cell;
      }
      ESP_LOGI(TAG, "%s", row.c_str());
    }

    // Too few samples and "never disagreed" is not evidence of anything - two
    // unrelated lines agree by chance a quarter of the time over two samples.
    if (this->compared_ < 64) {
      ESP_LOGW(TAG, "Only %" PRIu32 " samples compared - too few to call any pair same-wire.",
               this->compared_);
    } else {
      uint8_t too_static = 0;
      for (uint8_t a = 0; a < this->active_count_; a++) {
        for (uint8_t b = a + 1; b < this->active_count_; b++) {
          const uint8_t ia = this->active_[a], ib = this->active_[b];
          const uint32_t d = this->stats_[ia].disagree[ib];
          const float pct = (float) d * 100.0f / (float) this->compared_;
          const char *la = this->stats_[ia].label.c_str();
          const char *lb = this->stats_[ib].label.c_str();

          // The real denominator. compared_ is how many samples were LOOKED at;
          // this is how many could have carried information, which is what the
          // claim below is actually resting on. A line that sits high for the
          // whole capture agrees with every other high line trivially, and run
          // 10c turned that into a confident and wrong "SAME WIRE: D5 == D10".
          //
          // Checked per PAIR rather than per line, and both sides must pass: a
          // shared net has one duty cycle by definition, so one busy line does
          // not license a claim about a static partner.
          const uint32_t ha = this->stats_[ia].samples_high, hb = this->stats_[ib].samples_high;
          const uint32_t mv_a = std::min(ha, this->compared_ - ha);
          const uint32_t mv_b = std::min(hb, this->compared_ - hb);
          if (std::min(mv_a, mv_b) < NET_MIN_MINORITY) {
            too_static++;
            continue;
          }

          if (d == 0) {
            ESP_LOGW(TAG, "SAME WIRE: %s == %s - 0 disagreements in %" PRIu32 " samples.", la, lb,
                     this->compared_);
          } else if (pct < 0.1f) {
            ESP_LOGW(TAG, "SAME WIRE: %s == %s - %" PRIu32 " disagreements in %" PRIu32
                          " samples (%.3f%%), glitch-level.",
                     la, lb, d, this->compared_, pct);
          } else if (pct > 99.0f) {
            ESP_LOGW(TAG, "INVERTED: %s is an inverted copy of %s (%.1f%% disagreement).", la, lb,
                     pct);
          } else {
            continue;
          }

          // Keep the verdict string short; the log above has the detail. HA
          // truncates long states and the point here is the pin pair.
          if (net_finding.size() < 80) {
            if (!net_finding.empty())
              net_finding += ", ";
            char frag[24];
            snprintf(frag, sizeof(frag), "%s%s%s", la, pct > 99.0f ? "=!" : "==", lb);
            net_finding += frag;
          }
        }
      }
      // Reported rather than silently skipped. "No pair was testable" and "no
      // pair shared a net" are different results, and only the second is a
      // finding - collapsing them is how the test came to assert D5 == D10.
      if (too_static > 0)
        ESP_LOGW(TAG,
                 "%u pair(s) too static to test: a line needs >= %u samples at "
                 "its minority level before agreement means anything.",
                 too_static, NET_MIN_MINORITY);
      if (net_finding.empty() && too_static == 0)
        ESP_LOGI(TAG, "No two moving lines share a net.");
    }
  }

  ESP_LOGI(TAG, "------------------- hypotheses -------------------");
  if (this->top_count_ == 0) {
    ESP_LOGI(TAG, "  (none scored above zero)");
  } else {
    for (uint8_t i = 0; i < this->top_count_; i++) {
      ESP_LOGI(TAG, "%5.1f  %-8s %-40s %s", this->top_[i].score, this->top_[i].kind,
               this->top_[i].desc, this->top_[i].sample);
    }
  }

  // --- Verdict ---
  //
  // Thresholds are deliberately blunt. The useful distinction is "one
  // interpretation stands clear of the field" versus "everything is in the
  // noise", and a margin over the runner-up says that better than an absolute
  // score does.
  std::string verdict;
  const float best = this->top_count_ > 0 ? this->top_[0].score : 0.0f;

  // The runner-up is the best hypothesis that reads a DIFFERENT set of lines.
  //
  // The margin test is asking whether some other explanation of the data fits
  // just as well. Entries that differ only in clock polarity, bit order, word
  // length or bit alignment are not other explanations - they are the same wiring
  // decoded slightly differently, and on a clock whose data is stable across
  // both edges the rising and falling variants necessarily score within a
  // point of each other. Counting one of those as the rival makes a finding
  // look contested by itself and drops a clean result to AMBIGUOUS every time.
  // The pin set is what the operator actually has to act on, so that is what
  // has to differ for a disagreement to be real.
  float runner = 0.0f;
  for (uint8_t i = 1; i < this->top_count_; i++) {
    if (this->top_[i].pins != this->top_[0].pins) {
      runner = this->top_[i].score;
      break;
    }
  }

  uint16_t mains_mask = 0;
  for (uint8_t i = 0; i < this->pin_count_; i++)
    if (this->stats_[i].mains)
      mains_mask |= (uint16_t) (1u << i);

  // A clear winner that reads no mains-coupled line survives the mains check.
  //
  // Mains used to veto everything, on the reasoning that a bad ground makes
  // every level meaningless. That is true of the pin that is coupled, and of
  // any decode that reads it - not of the whole tap. Once a header is sampled
  // whole, an unused line left floating is ordinary, and letting it suppress a
  // byte-exact decode on two other lines throws away the only real finding in
  // the report. The narrower rule keeps the original protection: if the
  // winning hypothesis touches a coupled line, its bytes are still suspect and
  // the mains verdict still fires.
  const bool clear_winner = best >= 45.0f && best >= runner + 12.0f;
  const bool winner_is_clean = clear_winner && (this->top_[0].pins & mains_mask) == 0;

  if (count == 0 && this->poll_changes_ > 0) {
    // The slow poll in loop() saw the port move while the ISR recorded
    // nothing. Whatever is wrong is on our side, and every "static line"
    // above is an artefact. This has to outrank NO TRAFFIC: a broken capture
    // that reports a confident negative is worse than one that reports
    // nothing, because the negative ends the investigation.
    verdict = "CAPTURE BROKEN - interrupts recorded nothing";
    ESP_LOGE(TAG, "VERDICT: the polled cross-check saw %" PRIu32 " port changes, but the",
             this->poll_changes_);
    ESP_LOGE(TAG, "interrupt handler recorded 0 samples. The lines are NOT static -");
    ESP_LOGE(TAG, "the capture path is broken. Ignore the per-pin table above; it is");
    ESP_LOGE(TAG, "describing an empty buffer, not the hardware.");
  } else if (count < 32) {
    verdict = "NO TRAFFIC - nothing to decode";
    ESP_LOGW(TAG, "VERDICT: only %" PRIu32 " edges captured (%" PRIu32 " polled changes).", count,
             this->poll_changes_);
    if (static_pins == this->pin_count_) {
      ESP_LOGW(TAG, "Every line was static for the entire window. That is what an idle");
      ESP_LOGW(TAG, "ISP/SWD/ICSP programming header looks like - VCC, GND, RESET and");
      ESP_LOGW(TAG, "programming pins only move while a programmer is attached.");
      ESP_LOGW(TAG, "If you exercised the board during capture, this header carries no");
      ESP_LOGW(TAG, "runtime data and the LCD header is the target instead.");
    } else {
      ESP_LOGW(TAG, "Re-run and exercise the board during the capture window.");
    }
  } else if (winner_is_clean) {
    char line[128];
    snprintf(line, sizeof(line), "%s: %s", this->top_[0].kind, this->top_[0].desc);
    verdict = line;
    ESP_LOGI(TAG, "VERDICT: %s wins by %.1f over the runner-up.", this->top_[0].kind,
             best - runner);
    ESP_LOGI(TAG, "  %s", this->top_[0].desc);
    ESP_LOGI(TAG, "  decoded: %s", this->top_[0].sample);
    if (mains_pins > 0) {
      ESP_LOGW(TAG, "Note: %u other pin(s) are mains-coupled and their levels mean nothing.",
               mains_pins);
      ESP_LOGW(TAG, "The winning decode does not read any of them, so it stands - but those");
      ESP_LOGW(TAG, "lines are either floating or on the wrong side of the ground bond.");
    }
  } else if (mains_pins > 0) {
    verdict = "MAINS COUPLING - ground reference is not valid";
    ESP_LOGW(TAG, "VERDICT: %u pin(s) cycling at the mains frequency.", mains_pins);
    if (clear_winner) {
      ESP_LOGW(TAG, "The top hypothesis reads one of them, so its bytes are not evidence.");
    }
    ESP_LOGW(TAG, "No level on a coupled pin can be trusted. Either the line is floating,");
    ESP_LOGW(TAG, "or the ground bond did not take. If it went to the LCD backlight");
    ESP_LOGW(TAG, "cathode, check continuity to LCD header pin 4 - many panels switch the");
    ESP_LOGW(TAG, "cathode low-side, which reads ~0 V but is not ground.");
  } else if (best >= 45.0f) {
    verdict = "AMBIGUOUS - several hypotheses fit equally";
    ESP_LOGW(TAG, "VERDICT: top score %.1f but the runner-up is within %.1f.", best, best - runner);
    ESP_LOGW(TAG, "Capture longer, or exercise the board harder, and re-run.");
  } else {
    verdict = "NO MATCH - traffic present but no hypothesis fits";
    ESP_LOGW(TAG, "VERDICT: there is activity, but nothing decoded above noise (best %.1f).",
             best);
    ESP_LOGW(TAG, "This header is doing something other than HD44780, 8N1 serial, or a");
    ESP_LOGW(TAG, "simple clocked stream. The per-pin table above is the evidence.");
    if (this->active_count_ < 6) {
      // Say this out loud rather than let a silent zero read as a rejection.
      ESP_LOGW(TAG, "Note: only %u line(s) moved, so HD44780 was never tried - it needs six.",
               this->active_count_);
      ESP_LOGW(TAG, "If this is an LCD bus, some of its lines are not in the tap.");
    }
  }

  ESP_LOGI(TAG, "======================================================");

  // A same-wire result is independent of which hypothesis won, and is usually
  // the more actionable of the two - it says which wires can be dropped. It
  // must not be lost because the decode happened to land on AMBIGUOUS.
  if (!net_finding.empty())
    verdict = "SAME WIRE " + net_finding + " | " + verdict;

  if (this->verdict_ != nullptr)
    this->verdict_->publish_state(verdict);
}

void BusSweep::dump_config() {
  ESP_LOGCONFIG(TAG, "Bus sweep (listen-only, %u pins):", this->pin_count_);
  for (uint8_t i = 0; i < this->pin_count_; i++) {
    ESP_LOGCONFIG(TAG, "  [%u] %s:", i, this->stats_[i].label.c_str());
    LOG_PIN("    ", this->pins_[i]);
  }
  ESP_LOGCONFIG(TAG, "  Capacity: %" PRIu32 " samples (%" PRIu32 " bytes)", this->capacity_,
                this->capacity_ * (uint32_t) sizeof(SweepSample));
  ESP_LOGCONFIG(TAG, "  Settle delay: %" PRIu32 " ms", this->settle_delay_ms_);
  ESP_LOGCONFIG(TAG, "  Capture timeout: %" PRIu32 " ms", this->capture_timeout_ms_);
  ESP_LOGCONFIG(TAG, "  Idle gap: %" PRIu32 " us", this->idle_gap_us_);
  if (this->is_failed())
    ESP_LOGE(TAG, "  Setup failed - see errors above");
}

}  // namespace bus_sweep
}  // namespace esphome

#endif  // USE_ESP32
