#include "clocked_tap.h"

#include <cstdio>
#include <cstring>

#include "esphome/core/log.h"
#include "esphome/core/hal.h"

#include "hal/gpio_ll.h"
#include "soc/gpio_reg.h"

namespace esphome {
namespace clocked_tap {

static const char *const TAG = "clocked_tap";

// Bounded drain per loop(): the same treatment the other components give this,
// for the same reason. An unbounded drain starves WiFi/API/OTA on a single-core
// part, and losing OTA is losing the ability to fix the thing remotely.
static const uint16_t MAX_DRAIN_PER_LOOP = 512;

// A frame ends on a clock gap, but a gap is only observable on the *next* edge.
// If the panel stops mid-refresh - or stops entirely - the last frame would sit
// in the buffer forever. loop() closes it out after this long with no edges.
static const uint32_t IDLE_FLUSH_MS = 40;

static const uint32_t STATS_INTERVAL_MS = 30000;

// Repeat suppression has a failure mode: attach a log viewer to a board whose
// display has not changed and you see nothing at all, which is
// indistinguishable from a dead tap. Re-printing the unchanged screen this
// often keeps the current screen on screen without turning the log into a wall.
static const uint32_t REPRINT_INTERVAL_MS = 10000;

void IRAM_ATTR ClockedTap::gpio_intr(ClockedTap *arg) {
  // Latch the whole port in one instruction: the data line's level at this
  // clock edge is the entire measurement, and reading it separately would
  // sample it microseconds late.
  const uint32_t port = REG_READ(GPIO_IN_REG);
  const uint32_t now = (uint32_t) esp_timer_get_time();

  arg->edges_++;

  const uint16_t head = arg->head_;
  const uint16_t next = (head + 1) & RING_MASK;
  if (next == arg->tail_) {
    // Drop the newest rather than overwrite the oldest. A contiguous run of
    // edges with a known hole at the end can still be framed; a ring that
    // silently rotates under the consumer corrupts a frame with no trace.
    arg->dropped_++;
    return;
  }
  arg->ring_[head].port = port;
  arg->ring_[head].t_us = now;
  arg->head_ = next;
}

void ClockedTap::setup() {
  ESP_LOGCONFIG(TAG, "Setting up clocked tap...");

  // The single-register capture in the ISR only sees GPIO0-31. A pin above
  // that would read as a constant zero and decode into convincing garbage, so
  // refuse rather than publish nonsense.
  const uint8_t clk = this->clock_pin_->get_pin();
  const uint8_t dat = this->data_pin_->get_pin();
  if (clk > 31 || dat > 31) {
    ESP_LOGE(TAG, "Pins must be GPIO0-31 (single-register capture); got CLK=%u DATA=%u", clk, dat);
    this->mark_failed();
    return;
  }
  this->data_bit_ = dat;

  this->clock_pin_->setup();
  this->data_pin_->setup();

  this->clock_pin_->attach_interrupt(&ClockedTap::gpio_intr, this,
                                     this->on_rising_ ? gpio::INTERRUPT_RISING_EDGE
                                                      : gpio::INTERRUPT_FALLING_EDGE);

  this->last_edge_ms_ = millis();
  this->last_stats_ms_ = this->last_edge_ms_;

  this->restore_persist_();
}

// Persist under a fixed hash so the slot is stable across reboots. Restore any
// saved lines and publish them immediately, so a device that rebooted while a
// warning stood comes up showing that warning instead of blank.
void ClockedTap::restore_persist_() {
  this->pref_ = global_preferences->make_preference<PersistState>(0x7C40C0DEu);
  PersistState st{};
  if (!this->pref_.load(&st))
    return;
  text_sensor::TextSensor *sensors[2] = {this->line_1_, this->line_2_};
  for (uint8_t l = 0; l < 2; l++) {
    if (!st.valid[l])
      continue;
    st.line[l][MAX_COLUMNS] = '\0';  // defensive: guarantee termination
    std::string text(st.line[l]);
    this->published_[l] = text;
    this->have_published_[l] = true;
    if (sensors[l] != nullptr) {
      sensors[l]->publish_state(text);
      ESP_LOGI(TAG, "Restored line %u from flash: '%s'", (unsigned) (l + 1), text.c_str());
    }
  }
}

// Called only when a published line actually changes, never per frame.
void ClockedTap::save_persist_() {
  PersistState st{};
  for (uint8_t l = 0; l < 2; l++) {
    st.valid[l] = this->have_published_[l] ? 1u : 0u;
    std::strncpy(st.line[l], this->published_[l].c_str(), MAX_COLUMNS);
    st.line[l][MAX_COLUMNS] = '\0';
  }
  this->pref_.save(&st);
}

void ClockedTap::loop() {
  uint16_t drained = 0;
  while (this->tail_ != this->head_ && drained < MAX_DRAIN_PER_LOOP) {
    Sample s = this->ring_[this->tail_];
    this->tail_ = (this->tail_ + 1) & RING_MASK;
    drained++;
    this->handle_sample_(s);
  }

  const uint32_t now = millis();
  if (drained > 0)
    this->last_edge_ms_ = now;
  else if (this->in_frame_ && (now - this->last_edge_ms_) > IDLE_FLUSH_MS)
    this->end_frame_();

  this->flush_blanks_();

  if ((now - this->last_stats_ms_) >= STATS_INTERVAL_MS) {
    const uint32_t elapsed = now - this->last_stats_ms_;
    const uint32_t df = this->frames_ - this->last_stats_frames_;
    ESP_LOGD(TAG,
             "%.1f frames/s, %u edges, %u dropped, %u undecoded, %u garbled, %u slipped, %u "
             "untagged",
             df * 1000.0f / (float) elapsed, (unsigned) this->edges_, (unsigned) this->dropped_,
             (unsigned) this->undecoded_, (unsigned) this->garbled_, (unsigned) this->slipped_,
             (unsigned) this->untagged_);
    if (this->dropped_ > 0)
      ESP_LOGW(TAG, "%u samples dropped - frames spanning a drop are corrupt", (unsigned) this->dropped_);
    this->last_stats_ms_ = now;
    this->last_stats_frames_ = this->frames_;
  }
}

void ClockedTap::handle_sample_(const Sample &sample) {
  const uint32_t gap = sample.t_us - this->last_edge_us_;
  this->last_edge_us_ = sample.t_us;

  // The stream carries no framing bits, so the boundary has to come from
  // somewhere outside the data. Here it is the idle gap between refreshes:
  // measured intra-frame clock gaps top out around 64us while frames are ~90ms
  // apart, three orders of magnitude of daylight to put a threshold in.
  if (this->in_frame_ && gap >= this->frame_gap_us_)
    this->end_frame_();

  if (!this->in_frame_) {
    this->in_frame_ = true;
    this->nbits_ = 0;
    this->overflowed_ = false;
  }

  if (this->nbits_ >= MAX_FRAME_BITS) {
    this->overflowed_ = true;
    return;
  }

  const uint8_t bit = (uint8_t) ((sample.port >> this->data_bit_) & 1u);
  const uint16_t i = this->nbits_++;
  if (bit)
    this->bits_[i >> 3] |= (uint8_t) (1u << (7 - (i & 7)));
  else
    this->bits_[i >> 3] &= (uint8_t) ~(1u << (7 - (i & 7)));
}

/// Pull the 8 data bits of the word starting at bit `start`.
///
/// A 9-bit word is one flag bit followed by eight data bits, so the payload is
/// always the LAST eight bits of the word whatever the word length. That makes
/// the same expression correct for a plain 8-bit stream too.
uint8_t ClockedTap::word_data_(uint16_t start) const {
  const uint16_t b = start + this->word_bits_ - 8;
  uint8_t v = 0;
  if (this->msb_first_) {
    for (uint8_t k = 0; k < 8; k++)
      v = (uint8_t) ((v << 1) | this->bit_at_(b + k));
  } else {
    for (uint8_t k = 0; k < 8; k++)
      v = (uint8_t) ((v >> 1) | (this->bit_at_(b + k) << 7));
  }
  return v;
}

/// THE ONE PRINTABLE MISALIGNMENT: a '@'-only line is a bit-slipped BLANK.
///
/// Read a field of spaces one bit late and every cell comes back as '@':
///
///     0x20 << 1 == 0x40 == '@'
///
/// That single fact is the whole bug. Of the nine alignments a 9-bit word can
/// be read at, a run of 0x20 renders as ' ' at +0, '@' at +1, and a value
/// outside 0x20..0x7E (so, '?') at +2 through +8. So +1 is the ONLY slip on
/// this panel that produces printable text, which means it is the one and only
/// blind spot in the '?' garbled-cell indicator below - it is not counted as
/// garbled, it does not look corrupt, and it reaches Home Assistant as a line
/// of '@' sitting on an otherwise blank Standby screen. Observed live
/// 2026-11-11 ("Standby" on line 1, "@@@@@" on line 2).
///
/// It is NOT a false marker match, which was the first theory: 0x7C is
/// 01111100 and needs five consecutive 1 bits, while a stream of spaces
/// (000100000 repeated) only ever has isolated 1s. The marker cannot be
/// synthesised out of blank cells. The slip happens after a GENUINE marker.
///
/// The rule is safe in both directions. No screen in this pump's whole mode
/// cycle contains a literal '@', so reading it as "that cell is a space and we
/// were one bit late" is the only interpretation with a physical meaning
/// behind it. It is applied per CELL in decode_frame_() below, which also
/// covers a PARTIAL slip - a line whose text decodes correctly and whose
/// trailing spaces do not ("NOT SET @@@@", seen live at 21:38:55).

/// Find each line marker in the frame and read the characters after it.
///
/// Scanning for the marker at every BIT position, rather than decoding from a
/// fixed offset, is the thing that makes this work at all: the two lines in one
/// frame are two bits out of phase with each other, so no single alignment
/// renders both. Resyncing on content is also what makes the decode survive the
/// occasional slipped clock edge - a corrupted line costs that line, not the
/// rest of the frame.
///
/// LINES ARE SLOTTED BY THE ROW TAG, NOT BY ORDER AND NOT BY BIT OFFSET.
///
/// Never by bit offset: one frame in captures/2026-11-11-modewalk.txt carries
/// its two markers at bits 1777 and 1968 instead of the usual 27 and 218 - an
/// idle gap inside the frame - and any absolute-offset rule drops a line there.
/// tools/walk.py::read_slots() was fixed for exactly this.
///
/// And no longer by order either, which this code did until 2026-11-11. THE
/// PANEL DOES NOT ALWAYS SEND ROW 1 FIRST. On the Time edit screen (press SET
/// from `Time Now`) it sends row 2 first, so Home Assistant showed the screen
/// upside down - `Next + -` on top of `Time Now 00:00`. Reported by the user
/// reading the glass, and visible in captures/live_log.txt from 04:15:10 on.
///
/// The panel says which row it means. The 9-bit word immediately BEFORE each
/// `7C 40` marker is a command word (flag bit 1) whose payload is the row:
///
///     0x00 -> row 1          0x80 -> row 2
///
/// Verified by tools/rowtag.py over every frame in captures/live_log.txt:
/// 563/563 two-marker frames carry a valid and DISTINCT tag on each half - no
/// unrecognised tags, no frame claiming the same row twice - and the 51 frames
/// whose halves arrive in the reverse order are exactly, and only, the `Next
/// + -` edit screen. So this is not a heuristic; it is what the bus says.
///
/// The tag also fixes the single-line 191-bit SHORT frame, which the old code
/// could only ever file as row 1. If the panel refreshes row 2 alone, the tag
/// puts it in row 2 instead of overwriting row 1 with it.
///
/// If any tag in the frame is unrecognised, or two halves claim the same row,
/// the WHOLE frame falls back to the old append-in-order behaviour, so an
/// unexpected screen degrades to the previous (mostly-correct) rule rather
/// than losing a line. See the comment on pass 1 for why the fallback has to
/// be all-or-nothing per frame rather than per half.
void ClockedTap::decode_frame_() {
  this->nlines_ = 0;
  for (uint8_t l = 0; l < MAX_LINES; l++)
    this->present_[l] = false;
  const uint8_t wb = this->word_bits_;
  const uint16_t marker_span = (uint16_t) (2 * wb);

  // PASS 1: locate the half-frames and read each one's row tag. Nothing is
  // placed yet, because slotting has to be decided for the frame as a WHOLE.
  //
  // Deciding per half is not good enough, and that is measured, not theoretical:
  // 0x00 and 0x80 differ by a single bit, so ONE flipped bit in a tag makes
  // half 1 claim row 2. A per-half rule then puts half 1 in row 2 and, finding
  // row 2 taken, pushes half 2 into row 1 - both lines wrong, i.e. a one-bit
  // error turns into a full swap. That happened on the very first live run of
  // this code (21:33:46, `[NOT SET][Time Mode]`, untagged=1), so the frame is
  // now judged as a unit: trust the tags only when they are ALL valid and all
  // distinct, otherwise fall back to transmission order, which is what this
  // decoder used before and is right on every screen except the Time edit.
  uint16_t offs[MAX_LINES];
  uint8_t slots[MAX_LINES];
  uint8_t nhalves = 0;
  bool tags_usable = true;
  bool claimed[MAX_LINES] = {};

  uint16_t i = 0;
  while (i + marker_span <= this->nbits_ && nhalves < MAX_LINES) {
    if (this->word_data_(i) != this->marker_a_ || this->word_data_(i + wb) != this->marker_b_) {
      i++;
      continue;
    }

    uint8_t slot = 0xFF;
    if (i >= wb) {
      const uint8_t tag = this->word_data_(i - wb);
      if (tag == ROW_TAG_1)
        slot = 0;
      else if (tag == ROW_TAG_2)
        slot = 1;
    }
    if (slot == 0xFF || claimed[slot])
      tags_usable = false;  // unrecognised, or two halves claiming one row
    else
      claimed[slot] = true;

    offs[nhalves] = i;
    slots[nhalves] = slot;
    nhalves++;
    i += marker_span + (uint16_t) (this->columns_ * wb);
  }

  this->tags_usable_ = tags_usable;
  if (!tags_usable)
    this->untagged_++;

  // PASS 2: place each half and decode its characters.
  for (uint8_t h = 0; h < nhalves; h++) {
    const uint8_t slot = tags_usable ? slots[h] : h;
    if (slot >= MAX_LINES)
      continue;

    uint16_t j = offs[h] + marker_span;
    char *dst = this->lines_[slot];
    uint8_t n = 0;
    while (n < this->columns_ && j + wb <= this->nbits_) {
      const uint8_t d = this->word_data_(j);
      // '@' IS A SPACE READ ONE BIT LATE - see the block comment above. Recover
      // it rather than showing it, because on a +1 slip '@' can ONLY have come
      // from a space: it needs c << 1 == 0x40, so c == 0x20, and no other cell
      // value on this panel can produce it. (A -1 slip would need c >> 1 ==
      // 0x40, i.e. c == 0x80 or 0x81, neither of which is a character the
      // panel draws.) No screen on this pump contains a literal '@', so
      // rendering it as the space it really is loses nothing and keeps a
      // partial slip - "NOT SET @@@@", observed live at 21:38:55 - from
      // looking like a screen change and being published as one.
      //
      // '?' for anything else, rather than dropping the character: a corrupted
      // cell should stay visible and in its column, so a one-bit glitch reads
      // as a glitch instead of silently shortening the line.
      if (d == 0x40) {
        dst[n] = ' ';
        this->slipped_++;
      } else if (d >= 0x20 && d < 0x7F) {
        dst[n] = (char) d;
      } else {
        dst[n] = '?';
        this->garbled_++;
      }
      n++;
      j += wb;
    }
    dst[n] = '\0';
    this->present_[slot] = true;
    this->nlines_++;
  }
}

void ClockedTap::end_frame_() {
  this->in_frame_ = false;

  if (this->nbits_ == 0)
    return;

  this->frames_++;
  this->decode_frame_();

  if (this->nlines_ == 0) {
    this->undecoded_++;
    ESP_LOGV(TAG, "no line marker in %u-bit frame", (unsigned) this->nbits_);
    this->nbits_ = 0;
    return;
  }

  // The debounce key is built in ROW order, with absent rows represented
  // explicitly. Row order (rather than transmission order) means the edit
  // screen no longer looks like a brand-new screen each time the panel happens
  // to flip which row it sends first; '\x01' marks a row this frame did not
  // carry, so a line-1-only short frame cannot compare equal to a line-2-only
  // one that happens to hold the same text.
  char screen[MAX_LINES * (MAX_COLUMNS + 1)];
  uint16_t p = 0;
  for (uint8_t l = 0; l < MAX_LINES; l++) {
    if (!this->present_[l]) {
      screen[p++] = '\x01';
    } else {
      const uint8_t len = (uint8_t) strlen(this->lines_[l]);
      memcpy(screen + p, this->lines_[l], len);
      p += len;
    }
    screen[p++] = '\n';
  }
  screen[p] = '\0';

  const uint32_t now = millis();
  const bool same = strcmp(screen, this->prev_screen_) == 0;

  // Debounce before believing a change. Anything that does not repeat is a
  // corrupted frame, not the panel.
  if (strcmp(screen, this->pending_screen_) == 0) {
    if (this->pending_count_ < 255)
      this->pending_count_++;
  } else {
    strcpy(this->pending_screen_, screen);
    this->pending_count_ = 1;
  }
  const bool settled = this->pending_count_ >= this->stable_frames_;

  // One-shot line-2 write bypass. The pump draws a standing warning like
  // "Check Water" as a SINGLE full 382/387-bit frame, then immediately reverts
  // to 191-bit line-1-only frames - so a changed line 2 never repeats and the
  // stable_frames_ debounce (which exists to reject single-bit line-1 glitches)
  // discards it forever. Fix: if this frame carries a non-empty line 2 that
  // differs from what is published, accept it now (count=1).
  //
  // !! The original version of this comment claimed "a corrupted frame faking a
  // clean printable line-2 string ... is far less likely than the debounce
  // silently eating every real warning". That is exactly backwards for ONE
  // corruption, and it is the common one: a +1 bit slip on a blank line renders
  // as all '@' (see the block comment above decode_frame_), which is non-empty, and
  // differs from the published "" - so it sailed through this bypass at
  // count=1, skipping stable_frames_ entirely, and painted "@@@@@" onto the
  // Standby screen in Home Assistant. The bypass is a real need, so it stays;
  // the candidate just has to survive the one check that distinguishes a
  // slipped blank from every genuine line-2 string this pump draws.
  //
  // Note the bypass CANNOT be hardened by requiring repeats - "Check Water" is
  // genuinely drawn once and never repeated. That is why it exists.
  // The bypass is also declined on a frame whose row tags did not come out
  // valid and distinct. Such a frame has a known bit error in it and was
  // slotted by the fallback rule, so "line 2 changed" cannot be trusted enough
  // to skip the debounce - and a genuine one-shot warning like "Check Water"
  // arrives on a frame with good tags anyway. Without this, a single flipped
  // tag bit on the Time edit screen publishes a swapped screen immediately.
  bool line2_appeared = false;
  if (this->present_[1] && this->tags_usable_) {
    // Inline trim (the `trimmed` helper is defined further down the file).
    const char *b2 = this->lines_[1];
    while (*b2 == ' ')
      b2++;
    const char *e2 = b2 + strlen(b2);
    while (e2 > b2 && e2[-1] == ' ')
      e2--;
    const std::string l2(b2, (size_t) (e2 - b2));
    // No '@' test is needed here any more: decode_frame_() already turned a
    // slipped cell back into the space it was, so a slipped blank arrives as
    // an empty string and is rejected by the !l2.empty() test below, exactly
    // like the genuine blank it is.
    // A candidate containing a '?' is declined too: that cell is already known
    // to have decoded to a non-character, so the line carries a confirmed bit
    // error and has no business skipping the debounce. Observed live at
    // 21:43:22 as "   ?NOT SET", published for one frame before the next good
    // frame corrected it.
    const bool has_garbled = l2.find('?') != std::string::npos;
    if (!l2.empty() && !has_garbled &&
        (!this->have_published_[1] || l2 != this->published_[1]))
      line2_appeared = true;
  }

  if (!settled && !line2_appeared) {
    this->nbits_ = 0;
    return;
  }

  // Publish before the log throttle below, not after. What Home Assistant sees
  // must not depend on how chatty the log has decided to be.
  this->publish_lines_();

  if (same) {
    this->repeats_++;
    if (!this->log_repeats_ && (now - this->last_log_ms_) < REPRINT_INTERVAL_MS) {
      this->nbits_ = 0;
      return;
    }
  } else if (this->repeats_ > 0) {
    ESP_LOGD(TAG, "(previous screen held for %u frames)", (unsigned) this->repeats_);
    this->repeats_ = 0;
  }

  // Logged in ROW order, so the log reads like the glass. A row this frame did
  // not carry prints as `[--absent--]` rather than silently shifting the rows
  // left - the old `l < nlines_` form made a line-2-only frame look like a
  // line-1 frame in the log, which is exactly the confusion being fixed here.
  std::string joined;
  for (uint8_t l = 0; l < 2; l++) {
    joined += '[';
    joined += this->present_[l] ? this->lines_[l] : "--absent--";
    joined += ']';
  }
  ESP_LOGI(TAG, "%s", joined.c_str());
  this->log_hex_();
  this->last_log_ms_ = now;

  if (!same)
    strcpy(this->prev_screen_, screen);

  this->nbits_ = 0;
}

/// Trim the panel's padding. Published state is centred text on a 16-column
/// panel, so almost every value arrives with leading and trailing spaces.
static std::string trimmed(const char *s) {
  const char *b = s;
  while (*b == ' ')
    b++;
  const char *e = b + strlen(b);
  while (e > b && e[-1] == ' ')
    e--;
  return std::string(b, (size_t) (e - b));
}

void ClockedTap::publish_lines_() {
  text_sensor::TextSensor *sensors[2] = {this->line_1_, this->line_2_};
  for (uint8_t l = 0; l < 2; l++) {
    if (sensors[l] == nullptr)
      continue;

    // Option A (hold on short frame): a line the frame did NOT carry is HELD,
    // not blanked. Verified against real frames: a screen whose row 2 is static
    // and not being refreshed (e.g. Automatic + "Check Water") is sent as a
    // 191-bit SHORT frame with only a line-1 marker, while a row the panel is
    // actively drawing - including a genuine blank (Standby, or the "Always On"
    // blink off-half) - arrives as a full 382-bit frame with a present-but-empty
    // line 2. So: absent => hold last value; present-but-empty => blank via the
    // existing blank_hold timer. This keeps a static warning visible in HA the
    // same way the physical LCD retains it in DDRAM.
    // Presence is now per-ROW, from the panel's own row tag, not "did the
    // frame carry at least l+1 lines". See decode_frame_().
    const bool present = this->present_[l];
    if (!present) {
      this->blank_since_[l] = 0;  // cancel any pending blank; keep current value
      continue;
    }
    // Reaffirmed 2026-11-11 against this project's own captures: Standby's
    // blank line 2 is a real 16-space line inside a full 382-bit frame, while
    // Automatic Mode held a 191-bit short frame for 303 consecutive frames
    // with line 1 still lit. Blanking on the second case reports a line the
    // panel is visibly showing. Standing project rule: always read BOTH lines.
    std::string text = trimmed(this->lines_[l]);

    // Nothing to undo for the +1 slip here either: decode_frame_() recovers a
    // slipped cell to its original space, so a slipped blank has already
    // become an empty string by the time it reaches this point and takes the
    // genuine-blank path below, debounced by blank_hold like any other blank.
    if (!text.empty()) {
      this->blank_since_[l] = 0;
      if (!this->have_published_[l] || text != this->published_[l]) {
        sensors[l]->publish_state(text);
        this->published_[l] = text;
        this->have_published_[l] = true;
        this->save_persist_();
      }
      continue;
    }

    if (this->have_published_[l] && this->published_[l].empty())
      continue;  // already blank, nothing to hold
    if (this->blank_since_[l] == 0)
      this->blank_since_[l] = millis() | 1u;  // 0 means "not pending"
  }
}

void ClockedTap::flush_blanks_() {
  text_sensor::TextSensor *sensors[2] = {this->line_1_, this->line_2_};
  const uint32_t now = millis();
  for (uint8_t l = 0; l < 2; l++) {
    if (sensors[l] == nullptr || this->blank_since_[l] == 0)
      continue;
    if ((now - this->blank_since_[l]) < this->blank_hold_ms_)
      continue;
    sensors[l]->publish_state("");
    this->published_[l].clear();
    this->have_published_[l] = true;
    this->blank_since_[l] = 0;
    this->save_persist_();
  }
}

void ClockedTap::log_hex_() const {
  const uint16_t nbytes = (uint16_t) ((this->nbits_ + 7) / 8);
  std::string hex;
  hex.reserve(nbytes * 3);
  char buf[4];
  for (uint16_t i = 0; i < nbytes; i++) {
    snprintf(buf, sizeof(buf), "%02X ", this->bits_[i]);
    hex += buf;
  }
  ESP_LOGD(TAG, "  %u bits%s | %s", (unsigned) this->nbits_, this->overflowed_ ? " (OVERFLOW)" : "", hex.c_str());
}

void ClockedTap::dump_config() {
  ESP_LOGCONFIG(TAG, "Clocked tap:");
  LOG_PIN("  Clock pin: ", this->clock_pin_);
  LOG_PIN("  Data pin:  ", this->data_pin_);
  ESP_LOGCONFIG(TAG, "  Sampling on:  %s edge", this->on_rising_ ? "rising" : "falling");
  ESP_LOGCONFIG(TAG, "  Bit order:    %s", this->msb_first_ ? "MSB first" : "LSB first");
  ESP_LOGCONFIG(TAG, "  Frame gap:    %uus", (unsigned) this->frame_gap_us_);
  ESP_LOGCONFIG(TAG, "  Word bits:    %u", this->word_bits_);
  ESP_LOGCONFIG(TAG, "  Line marker:  %02X %02X", this->marker_a_, this->marker_b_);
  ESP_LOGCONFIG(TAG, "  Columns:      %u", this->columns_);
  ESP_LOGCONFIG(TAG, "  Stable after: %u frames", this->stable_frames_);
  ESP_LOGCONFIG(TAG, "  Blank hold:   %ums", (unsigned) this->blank_hold_ms_);
  ESP_LOGCONFIG(TAG, "  Log repeats:  %s", YESNO(this->log_repeats_));
  LOG_TEXT_SENSOR("  ", "Line 1", this->line_1_);
  LOG_TEXT_SENSOR("  ", "Line 2", this->line_2_);
  if (this->is_failed())
    ESP_LOGE(TAG, "  SETUP FAILED - not capturing");
}

}  // namespace clocked_tap
}  // namespace esphome
