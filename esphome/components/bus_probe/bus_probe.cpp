#include "bus_probe.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <esp_attr.h>
#include <esp_timer.h>
#include <soc/gpio_reg.h>

namespace esphome {
namespace bus_probe {

static const char *const TAG = "bus_probe";

static const uint16_t MAX_DRAIN_PER_LOOP = 512;

// Used only to annotate the report. A pin whose shortest pulse matches one of
// these is worth trying as async serial at that rate.
static const uint32_t STANDARD_BAUDS[] = {1200,  2400,  4800,   9600,
                                          19200, 38400, 57600,  115200};

// 50 Hz and 60 Hz full periods. A pin whose slowest cycle matches one of these
// is following the mains, which means it is not referenced to our ground.
static const uint32_t MAINS_PERIODS_US[] = {20000, 16667};

void IRAM_ATTR BusProbe::gpio_intr(BusProbe *arg) {
  // Every configured pin shares this handler. Which pin fired does not matter:
  // the whole port is captured, and the consumer recovers the changed lines by
  // diffing against the previous sample. Simultaneous edges therefore arrive
  // as one sample with several changed bits, which is exactly what the
  // coincidence matrix needs.
  const uint32_t port = REG_READ(GPIO_IN_REG);
  const uint32_t now = (uint32_t) esp_timer_get_time();

  const uint16_t head = arg->head_;
  const uint16_t next = (head + 1) & PROBE_RING_MASK;
  if (next == arg->tail_) {
    arg->dropped_++;
    return;
  }
  arg->ring_[head].port = port;
  arg->ring_[head].t_us = now;
  arg->head_ = next;
}

void BusProbe::add_probe_pin(InternalGPIOPin *pin, const std::string &label) {
  if (this->pin_count_ >= MAX_PROBE_PINS)
    return;
  this->pins_.push_back(pin);
  this->stats_[this->pin_count_].label = label;
  this->pin_count_++;
}

void BusProbe::setup() {
  for (uint8_t i = 0; i < this->pin_count_; i++) {
    InternalGPIOPin *pin = this->pins_[i];
    pin->setup();
    if (pin->get_pin() > 31) {
      ESP_LOGE(TAG, "Probe pins must be GPIO0-31 for atomic capture");
      this->mark_failed();
      return;
    }
    this->stats_[i].bit = pin->get_pin();
  }

  for (uint8_t i = 0; i < this->pin_count_; i++)
    this->pins_[i]->attach_interrupt(&BusProbe::gpio_intr, this, gpio::INTERRUPT_ANY_EDGE);

  this->window_start_ms_ = millis();
  this->window_start_us_ = (uint32_t) esp_timer_get_time();
}

void BusProbe::loop() {
  uint16_t drained = 0;
  while (this->tail_ != this->head_ && drained < MAX_DRAIN_PER_LOOP) {
    const ProbeSample sample = this->ring_[this->tail_];
    this->tail_ = (this->tail_ + 1) & PROBE_RING_MASK;
    drained++;
    this->handle_sample_(sample);
  }

  if (millis() - this->window_start_ms_ >= this->report_interval_ms_)
    this->report_();
}

void BusProbe::handle_sample_(const ProbeSample &sample) {
  if (!this->have_prev_) {
    // Seed levels and timers from the first edge; durations only become
    // meaningful from the second change onward.
    this->prev_port_ = sample.port;
    this->have_prev_ = true;
    for (uint8_t i = 0; i < this->pin_count_; i++) {
      PinStats &s = this->stats_[i];
      s.level = (sample.port >> s.bit) & 1;
      s.last_change_us = sample.t_us;
      s.initialised = true;
    }
    return;
  }

  const uint32_t changed = sample.port ^ this->prev_port_;
  this->prev_port_ = sample.port;
  if (changed == 0)
    return;  // Another pin's ISR for an edge we already captured

  // Which of our pins moved in this sample - needed for the coincidence matrix.
  uint8_t moved[MAX_PROBE_PINS];
  uint8_t moved_count = 0;

  for (uint8_t i = 0; i < this->pin_count_; i++) {
    PinStats &s = this->stats_[i];
    if (!((changed >> s.bit) & 1))
      continue;

    const uint32_t held = sample.t_us - s.last_change_us;
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
    s.level = !s.level;
    s.last_change_us = sample.t_us;
    moved[moved_count++] = i;
  }

  for (uint8_t a = 0; a < moved_count; a++)
    for (uint8_t b = 0; b < moved_count; b++)
      if (a != b)
        this->stats_[moved[a]].coincident[moved[b]]++;

  if (this->raw_dump_) {
    char bits[MAX_PROBE_PINS + 1];
    for (uint8_t i = 0; i < this->pin_count_; i++)
      bits[i] = ((sample.port >> this->stats_[i].bit) & 1) ? '1' : '0';
    bits[this->pin_count_] = '\0';
    ESP_LOGD(TAG, "t=%" PRIu32 "us %s", sample.t_us, bits);
  }
}

void BusProbe::report_() {
  const uint32_t now_ms = millis();
  const uint32_t now_us = (uint32_t) esp_timer_get_time();
  const uint32_t elapsed_ms = now_ms - this->window_start_ms_;
  const uint32_t elapsed_us = now_us - this->window_start_us_;
  if (elapsed_ms == 0)
    return;

  // Read the port directly rather than relying on captured samples: a pin that
  // never moved has never fired its ISR, and a static high/low is exactly the
  // signature of a supply rail or a strapped line.
  const uint32_t port = REG_READ(GPIO_IN_REG);

  ESP_LOGI(TAG, "--- bus probe: %.1f s window, %" PRIu32 " dropped ---",
           elapsed_ms / 1000.0f, this->dropped_);
  ESP_LOGI(TAG, " # label  now  edges  edge/s   high%%   min_hi   min_lo   max_hi   max_lo  ~baud");

  std::string compact;
  uint8_t mains_pins = 0;
  for (uint8_t i = 0; i < this->pin_count_; i++) {
    PinStats &s = this->stats_[i];
    const uint32_t edges = s.rising + s.falling;
    const uint8_t now_level = (port >> s.bit) & 1;
    const float rate = (float) edges * 1000.0f / (float) elapsed_ms;

    // Duty is only meaningful once the pin has actually toggled.
    const uint64_t accounted = s.total_high_us + s.total_low_us;
    char duty[12];
    if (accounted > 0) {
      snprintf(duty, sizeof(duty), "%5.1f%%", (double) s.total_high_us * 100.0 / (double) accounted);
    } else {
      snprintf(duty, sizeof(duty), "%6s", now_level ? "100%" : "0%");
    }

    char min_hi[12], min_lo[12], max_hi[12], max_lo[12];
    snprintf(min_hi, sizeof(min_hi), "%8" PRIu32, s.min_high_us == UINT32_MAX ? 0 : s.min_high_us);
    snprintf(min_lo, sizeof(min_lo), "%8" PRIu32, s.min_low_us == UINT32_MAX ? 0 : s.min_low_us);
    snprintf(max_hi, sizeof(max_hi), "%8" PRIu32, s.max_high_us);
    snprintf(max_lo, sizeof(max_lo), "%8" PRIu32, s.max_low_us);

    // Shortest pulse ~ one bit time for async serial.
    uint32_t shortest = UINT32_MAX;
    if (s.min_high_us != UINT32_MAX)
      shortest = s.min_high_us;
    if (s.min_low_us != UINT32_MAX && s.min_low_us < shortest)
      shortest = s.min_low_us;

    char baud[16] = "     -";

    // Longest high plus longest low is the slowest full cycle the pin went
    // through. When that lands on a mains period the pin is not carrying data
    // at all: it is a high-impedance node following the line frequency, and
    // every other number in the row is noise statistics. Check this before the
    // baud guess, which would otherwise report the threshold-crossing chatter
    // as a wildly implausible bit rate.
    const uint64_t slow_period = (uint64_t) s.max_high_us + (uint64_t) s.max_low_us;
    bool mains = false;
    for (uint32_t mains_period : MAINS_PERIODS_US) {
      if (slow_period == 0)
        continue;
      const float err = fabsf((float) slow_period - (float) mains_period) / (float) mains_period;
      if (err < 0.10f) {
        snprintf(baud, sizeof(baud), "%5.0fHz!", 1000000.0f / (float) mains_period);
        mains = true;
        break;
      }
    }

    if (!mains && shortest != UINT32_MAX && shortest > 0) {
      const float implied = 1000000.0f / (float) shortest;
      uint32_t best = 0;
      float best_err = 1e9f;
      for (uint32_t candidate : STANDARD_BAUDS) {
        const float err = fabsf(implied - (float) candidate) / (float) candidate;
        if (err < best_err) {
          best_err = err;
          best = candidate;
        }
      }
      if (best_err < 0.15f) {
        snprintf(baud, sizeof(baud), "%6" PRIu32, best);
      } else {
        snprintf(baud, sizeof(baud), "%6.0f?", implied);
      }
    }
    if (mains)
      mains_pins++;

    ESP_LOGI(TAG, "%2u %-6s  %u %6" PRIu32 " %7.1f  %s %s %s %s %s %s", i, s.label.c_str(),
             now_level, edges, rate, duty, min_hi, min_lo, max_hi, max_lo, baud);

    char entry[24];
    snprintf(entry, sizeof(entry), "%s:%u/%.0f ", s.label.c_str(), now_level, rate);
    compact += entry;
  }

  // Only worth printing once something is moving.
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
  } else {
    ESP_LOGW(TAG, "No edges on any pin. If these are LCD lines the panel may only");
    ESP_LOGW(TAG, "refresh on change - press a button on the pump and watch again.");
  }

  if (mains_pins > 0) {
    ESP_LOGW(TAG, "%u pin(s) are cycling at the mains frequency. That is not data:", mains_pins);
    ESP_LOGW(TAG, "those nodes are floating with respect to our ground, so no level");
    ESP_LOGW(TAG, "on ANY pin here can be trusted. Fix the ground reference first -");
    ESP_LOGW(TAG, "and if the tapped board has no isolated supply, do not fix it by");
    ESP_LOGW(TAG, "bonding it to a USB-earthed ground. Isolate instead.");
  }

  if (this->summary_ != nullptr && !compact.empty()) {
    compact.pop_back();
    this->summary_->publish_state(compact);
  }

  (void) elapsed_us;
  this->reset_window_();
}

void BusProbe::reset_window_() {
  // Levels and last_change_us carry over so a pulse straddling the boundary is
  // still measured; only the counters restart.
  for (uint8_t i = 0; i < this->pin_count_; i++) {
    PinStats &s = this->stats_[i];
    s.rising = 0;
    s.falling = 0;
    s.min_high_us = UINT32_MAX;
    s.max_high_us = 0;
    s.min_low_us = UINT32_MAX;
    s.max_low_us = 0;
    s.total_high_us = 0;
    s.total_low_us = 0;
    for (uint8_t j = 0; j < MAX_PROBE_PINS; j++)
      s.coincident[j] = 0;
  }
  this->dropped_ = 0;
  this->window_start_ms_ = millis();
  this->window_start_us_ = (uint32_t) esp_timer_get_time();
}

void BusProbe::dump_config() {
  ESP_LOGCONFIG(TAG, "Bus probe (listen-only, %u pins):", this->pin_count_);
  for (uint8_t i = 0; i < this->pin_count_; i++) {
    ESP_LOGCONFIG(TAG, "  [%u] %s:", i, this->stats_[i].label.c_str());
    LOG_PIN("    ", this->pins_[i]);
  }
  ESP_LOGCONFIG(TAG, "  Report interval: %" PRIu32 " ms", this->report_interval_ms_);
  ESP_LOGCONFIG(TAG, "  Raw dump: %s", YESNO(this->raw_dump_));
}

}  // namespace bus_probe
}  // namespace esphome

#endif  // USE_ESP32
