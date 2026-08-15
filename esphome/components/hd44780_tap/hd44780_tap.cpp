#include "hd44780_tap.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

#include <cinttypes>
#include <esp_attr.h>
#include <esp_timer.h>
#include <soc/gpio_reg.h>

namespace esphome {
namespace hd44780_tap {

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
  if (this->dirty_ && now - this->last_publish_ms_ >= PUBLISH_INTERVAL_MS) {
    this->last_publish_ms_ = now;
    this->dirty_ = false;
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
  this->handle_byte_(this->pending_rs_, (this->high_nibble_ << 4) | nibble);
}

void HD44780Tap::handle_byte_(bool rs, uint8_t value) {
  if (this->raw_capture_) {
    const char printable = (value >= 0x20 && value <= 0x7E) ? (char) value : '.';
    ESP_LOGD(TAG, "byte rs=%u 0x%02X '%c'", rs ? 1 : 0, value, printable);
  }
  if (rs) {
    this->handle_data_(value);
  } else {
    this->handle_command_(value);
  }
}

void HD44780Tap::handle_command_(uint8_t cmd) {
  // Tested from the most significant set bit down - that is how the HD44780
  // decodes its instruction set.
  if (cmd & 0x80) {  // Set DDRAM Address
    this->address_ = cmd & 0x7F;
    this->cgram_mode_ = false;
    return;
  }
  if (cmd & 0x40) {
    // Set CGRAM Address. The address counter is shared, so every following
    // RS=1 write goes to the character generator, not to the screen. Treating
    // those as display data would smear custom-glyph bytes across a row.
    this->cgram_mode_ = true;
    return;
  }
  if (cmd & 0x20)  // Function Set - bus width / lines / font, nothing to model
    return;

  if (cmd & 0x10) {  // Cursor or Display Shift
    const bool display = cmd & 0x08;
    const bool right = cmd & 0x04;
    if (display) {
      // The window moves, so a DDRAM address no longer maps to a fixed column.
      this->shift_ = (this->shift_ + (right ? LINE_LEN - 1 : 1)) % LINE_LEN;
      this->dirty_ = true;
    } else {
      const bool saved = this->increment_;
      this->increment_ = right;
      this->advance_address_();
      this->increment_ = saved;
    }
    return;
  }
  if (cmd & 0x08)  // Display / cursor / blink on-off
    return;
  if (cmd & 0x04) {  // Entry Mode Set
    this->increment_ = cmd & 0x02;
    return;
  }
  if (cmd & 0x02) {  // Return Home
    this->address_ = 0;
    this->shift_ = 0;
    this->cgram_mode_ = false;
    this->dirty_ = true;
    return;
  }
  if (cmd & 0x01)  // Clear Display
    this->clear_display_();
  // 0x00 is not an instruction; it also appears during 4-bit initialisation.
}

void HD44780Tap::handle_data_(uint8_t value) {
  if (this->cgram_mode_)
    return;  // Character generator write - never reaches the screen as text

  if (this->address_ < DDRAM_SIZE) {
    this->ddram_[this->address_] = value;
    this->written_[this->address_] = true;
    this->dirty_ = true;
  }
  this->advance_address_();
}

void HD44780Tap::advance_address_() {
  // DDRAM is split: 0x00-0x27 then 0x40-0x67. The counter skips the hole.
  if (this->increment_) {
    if (this->address_ == 0x27) {
      this->address_ = LINE_2_BASE;
    } else if (this->address_ == 0x67) {
      this->address_ = LINE_1_BASE;
    } else {
      this->address_++;
    }
  } else {
    if (this->address_ == LINE_2_BASE) {
      this->address_ = 0x27;
    } else if (this->address_ == LINE_1_BASE) {
      this->address_ = 0x67;
    } else {
      this->address_--;
    }
  }
}

void HD44780Tap::clear_display_() {
  for (uint8_t i = 0; i < DDRAM_SIZE; i++) {
    this->ddram_[i] = ' ';
    this->written_[i] = true;
  }
  this->address_ = 0;
  this->shift_ = 0;
  this->increment_ = true;  // Clear Display forces I/D = 1
  this->cgram_mode_ = false;
  this->dirty_ = true;
}

std::string HD44780Tap::render_line_(uint8_t base) const {
  // A cell that has never been written is not a space - it is unknown. The
  // pump's firmware may only refresh the fields that change, so a sniffer that
  // starts mid-run never sees the static parts. Report unknown cells as '~' so
  // a partial view is never mistaken for a blank display.
  bool any = false;
  std::string out;
  out.reserve(this->columns_);
  for (uint8_t i = 0; i < this->columns_; i++) {
    const uint8_t idx = base + ((i + this->shift_) % LINE_LEN);
    if (idx >= DDRAM_SIZE)
      continue;
    if (!this->written_[idx]) {
      out.push_back('~');
      continue;
    }
    any = true;
    const uint8_t c = this->ddram_[idx];
    out.push_back((c >= 0x20 && c <= 0x7E) ? (char) c : '.');
  }
  if (!any)
    return "";  // Nothing observed yet - unknown, not empty
  while (!out.empty() && out.back() == ' ')
    out.pop_back();
  return out;
}

void HD44780Tap::publish_lines_() {
  if (this->line_1_ != nullptr) {
    const std::string text = this->render_line_(LINE_1_BASE);
    if (!text.empty() && text != this->last_line_1_) {
      this->last_line_1_ = text;
      this->line_1_->publish_state(text);
    }
  }
  if (this->line_2_ != nullptr) {
    const std::string text = this->render_line_(LINE_2_BASE);
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
