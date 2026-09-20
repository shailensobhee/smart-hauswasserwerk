#include "hd44780_tap.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

#include <cinttypes>
#include <esp_attr.h>
#include <esp_timer.h>
#include <soc/gpio_reg.h>

namespace esphome {
namespace hd44780_tap {

using hd44780_core::LINE_1_BASE;
using hd44780_core::LINE_2_BASE;

static const char *const TAG = "hd44780_tap";

// Publishing is rate-limited; a full-screen refresh would otherwise emit a
// state change per character.
static const uint32_t PUBLISH_INTERVAL_MS = 250;
static const uint32_t DIAGNOSTIC_INTERVAL_MS = 10000;

// Cap on samples drained per loop() so a chatty bus cannot starve the API,
// OTA and WiFi tasks.
static const uint16_t MAX_DRAIN_PER_LOOP = 256;

void IRAM_ATTR HD44780Tap::gpio_intr(HD44780Tap *arg) {
  // Falling edge of E: the HD44780 latches RS and D4-D7 here. One register
  // read captures every tapped line simultaneously - all of them live in
  // GPIO 0-23, so there is no second register and no tearing between halves.
  const uint32_t port = REG_READ(GPIO_IN_REG);
  const uint32_t now = (uint32_t) esp_timer_get_time();

  arg->edges_++;

  const uint16_t head = arg->head_;
  const uint16_t next = (head + 1) & RING_MASK;
  if (next == arg->tail_) {
    // Full. Drop the newest rather than overwrite the oldest: a contiguous
    // run of good samples decodes, a corrupted one does not.
    arg->dropped_++;
    return;
  }
  arg->ring_[head].port = port;
  arg->ring_[head].t_us = now;
  arg->head_ = next;
}

void HD44780Tap::setup() {
  // Every tap is an input and is never driven. The component has no code path
  // that writes to these pins.
  this->e_pin_->setup();
  this->rs_pin_->setup();
  for (auto *pin : this->d_pins_)
    pin->setup();

  this->rs_bit_ = this->rs_pin_->get_pin();
  for (uint8_t i = 0; i < 4; i++)
    this->d_bits_[i] = this->d_pins_[i]->get_pin();

  // The single-register capture is only atomic while every tap is below 32.
  if (this->rs_bit_ > 31 || this->e_pin_->get_pin() > 31) {
    ESP_LOGE(TAG, "Tap pins must be GPIO0-31 for atomic capture");
    this->mark_failed();
    return;
  }
  for (uint8_t i = 0; i < 4; i++) {
    if (this->d_bits_[i] > 31) {
      ESP_LOGE(TAG, "Tap pins must be GPIO0-31 for atomic capture");
      this->mark_failed();
      return;
    }
  }

  this->decoder_.reset();

  this->e_pin_->attach_interrupt(&HD44780Tap::gpio_intr, this, gpio::INTERRUPT_FALLING_EDGE);

  this->last_rate_ms_ = millis();
  this->last_publish_ms_ = this->last_rate_ms_;
}

void HD44780Tap::loop() {
  uint16_t drained = 0;
  while (this->tail_ != this->head_ && drained < MAX_DRAIN_PER_LOOP) {
    const Sample sample = this->ring_[this->tail_];
    this->tail_ = (this->tail_ + 1) & RING_MASK;
    drained++;
    this->handle_sample_(sample);
  }

  const uint32_t now = millis();
  if (this->decoder_.dirty && now - this->last_publish_ms_ >= PUBLISH_INTERVAL_MS) {
    this->last_publish_ms_ = now;
    this->decoder_.dirty = false;
    this->publish_lines_();
  }
  if (now - this->last_rate_ms_ >= DIAGNOSTIC_INTERVAL_MS)
    this->publish_diagnostics_();
}

void HD44780Tap::handle_sample_(const Sample &sample) {
  const bool rs = (sample.port >> this->rs_bit_) & 1;
  uint8_t nibble = 0;
  for (uint8_t i = 0; i < 4; i++)
    nibble |= ((sample.port >> this->d_bits_[i]) & 1) << i;

  const uint32_t gap = sample.t_us - this->last_edge_us_;
  this->last_edge_us_ = sample.t_us;

  if (this->raw_capture_) {
    ESP_LOGV(TAG, "edge dt=%" PRIu32 "us rs=%u nibble=0x%X port=0x%08" PRIx32, gap, rs ? 1 : 0,
             nibble, sample.port);
  }

  // Resync. Nibbles carry no framing, so a mid-stream start can sit
  // permanently off by one and still decode into plausible-looking bytes.
  // Two independent boundary signals recover from that:
  //   1. RS is held for a whole byte, so any RS change must land on a byte
  //      boundary. A change while a high nibble is pending means we are
  //      misaligned.
  //   2. A long idle gap means the previous byte finished. This is the only
  //      signal available during a long run of same-RS character writes.
  if (this->have_high_nibble_ && (rs != this->pending_rs_ || gap > this->idle_gap_us_)) {
    ESP_LOGD(TAG, "Resync: discarding orphan nibble 0x%X (rs %u->%u, dt=%" PRIu32 "us)",
             this->high_nibble_, this->pending_rs_ ? 1 : 0, rs ? 1 : 0, gap);
    this->have_high_nibble_ = false;
  }

  if (!this->have_high_nibble_) {
    this->high_nibble_ = nibble;
    this->pending_rs_ = rs;
    this->have_high_nibble_ = true;
    return;
  }

  this->have_high_nibble_ = false;
  const uint8_t value = (this->high_nibble_ << 4) | nibble;
  if (this->raw_capture_) {
    const char printable = (value >= 0x20 && value <= 0x7E) ? (char) value : '.';
    ESP_LOGD(TAG, "byte rs=%u 0x%02X '%c'", this->pending_rs_ ? 1 : 0, value, printable);
  }
  this->decoder_.feed_byte(this->pending_rs_, value);
}

void HD44780Tap::publish_lines_() {
  if (this->line_1_ != nullptr) {
    const std::string text = this->decoder_.render_line(LINE_1_BASE, this->columns_);
    if (!text.empty() && text != this->last_line_1_) {
      this->last_line_1_ = text;
      this->line_1_->publish_state(text);
    }
  }
  if (this->line_2_ != nullptr) {
    const std::string text = this->decoder_.render_line(LINE_2_BASE, this->columns_);
    if (!text.empty() && text != this->last_line_2_) {
      this->last_line_2_ = text;
      this->line_2_->publish_state(text);
    }
  }
}

void HD44780Tap::publish_diagnostics_() {
  const uint32_t now = millis();
  const uint32_t edges = this->edges_;
  const uint32_t elapsed = now - this->last_rate_ms_;

  if (this->edge_rate_ != nullptr && elapsed > 0) {
    const float rate = (float) (edges - this->last_rate_edges_) * 1000.0f / (float) elapsed;
    this->edge_rate_->publish_state(rate);
  }
  if (this->dropped_samples_ != nullptr)
    this->dropped_samples_->publish_state((float) this->dropped_);

  this->last_rate_ms_ = now;
  this->last_rate_edges_ = edges;
}

void HD44780Tap::dump_config() {
  ESP_LOGCONFIG(TAG, "HD44780 bus tap (listen-only):");
  LOG_PIN("  E pin:  ", this->e_pin_);
  LOG_PIN("  RS pin: ", this->rs_pin_);
  LOG_PIN("  D4 pin: ", this->d_pins_[0]);
  LOG_PIN("  D5 pin: ", this->d_pins_[1]);
  LOG_PIN("  D6 pin: ", this->d_pins_[2]);
  LOG_PIN("  D7 pin: ", this->d_pins_[3]);
  ESP_LOGCONFIG(TAG, "  Columns: %u", this->columns_);
  ESP_LOGCONFIG(TAG, "  Idle gap: %" PRIu32 " us", this->idle_gap_us_);
  ESP_LOGCONFIG(TAG, "  Raw capture: %s", YESNO(this->raw_capture_));
  if (this->is_failed())
    ESP_LOGE(TAG, "  Setup failed - see errors above");
}

}  // namespace hd44780_tap
}  // namespace esphome

#endif  // USE_ESP32
