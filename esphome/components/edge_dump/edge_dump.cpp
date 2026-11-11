#include "edge_dump.h"

#include <cinttypes>
#include <string>

#include "esphome/core/log.h"

namespace esphome {
namespace edge_dump {

static const char *const TAG = "edge_dump";

void IRAM_ATTR EdgeDump::gpio_intr(EdgeDump *arg) {
  const uint32_t now = (uint32_t) esp_timer_get_time();
  arg->last_t_ = now;

  if (!arg->collecting_)
    return;

  const uint16_t n = arg->count_;
  if (n >= EDGE_DUMP_CAPACITY)
    return;

  arg->buf_[n].t_us = now;
  // Read the level rather than inferring it from an assumed alternation. On a
  // line fast enough to glitch, "every edge flips the level" is exactly the
  // assumption that silently desynchronises a reconstruction.
  arg->buf_[n].level = arg->isr_pin_.digital_read() ? 1 : 0;
  arg->count_ = n + 1;
}

void EdgeDump::setup() {
  this->pin_->setup();
  this->isr_pin_ = this->pin_->to_isr();
  this->pin_->attach_interrupt(&EdgeDump::gpio_intr, this, gpio::INTERRUPT_ANY_EDGE);

  ESP_LOGI(TAG, "Edge dump armed on GPIO%u: frame gap %" PRIu32 " us, %u frames to dump.",
           this->pin_->get_pin(), this->frame_gap_us_, this->max_frames_);
}

void EdgeDump::loop() {
  if (this->frames_done_ >= this->max_frames_)
    return;

  const uint16_t n = this->count_;
  if (n < this->min_edges_)
    return;

  // A frame is closed by silence, not by a count: the protocol emits 382 bits
  // normally but truncates to 191 when it draws only one line, so any
  // fixed-length assumption breaks on the short frame.
  const uint32_t now = (uint32_t) esp_timer_get_time();
  if ((uint32_t) (now - this->last_t_) < this->frame_gap_us_)
    return;

  this->collecting_ = false;
  this->dump_frame_(n);
  this->count_ = 0;
  this->frames_done_++;
  this->collecting_ = true;

  if (this->frames_done_ >= this->max_frames_)
    ESP_LOGI(TAG, "Done - %u frames dumped. Reset the device to capture again.", this->max_frames_);
}

void EdgeDump::dump_frame_(uint16_t count) {
  const uint32_t span = this->buf_[count - 1].t_us - this->buf_[0].t_us;

  ESP_LOGI(TAG, "=== FRAME %u: %u edges, span %" PRIu32 " us, first level %u ===",
           this->frames_done_, count, span, this->buf_[0].level);

  // Emitted as level:duration run lengths rather than absolute timestamps.
  // The run length IS the measurement - it is what gets quantised against the
  // recovered bit period - and absolute times would just be its cumulative
  // sum, costing more characters to say the same thing.
  std::string row;
  char cell[20];
  uint16_t per_line = 0;

  for (uint16_t i = 1; i < count; i++) {
    const uint32_t d = this->buf_[i].t_us - this->buf_[i - 1].t_us;
    snprintf(cell, sizeof(cell), "%u:%" PRIu32 " ", this->buf_[i - 1].level, d);
    row += cell;

    if (++per_line >= 16) {
      ESP_LOGI(TAG, "  %s", row.c_str());
      row.clear();
      per_line = 0;
    }
  }

  if (!row.empty())
    ESP_LOGI(TAG, "  %s", row.c_str());

  ESP_LOGI(TAG, "=== END FRAME %u ===", this->frames_done_);
}

void EdgeDump::dump_config() {
  ESP_LOGCONFIG(TAG, "Edge Dump:");
  LOG_PIN("  Pin: ", this->pin_);
  ESP_LOGCONFIG(TAG, "  Frame gap: %" PRIu32 " us", this->frame_gap_us_);
  ESP_LOGCONFIG(TAG, "  Min edges: %u", this->min_edges_);
  ESP_LOGCONFIG(TAG, "  Frames to dump: %u", this->max_frames_);
}

}  // namespace edge_dump
}  // namespace esphome
