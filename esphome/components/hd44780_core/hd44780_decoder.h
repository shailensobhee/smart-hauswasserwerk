#pragma once

// The HD44780 controller state machine, with no ESPHome dependencies.
//
// Extracted from hd44780_tap so that bus_sweep can run it hundreds of times
// over one captured buffer - once per candidate pin assignment - without
// duplicating the logic. The parts that are easy to get wrong (the CGRAM
// diversion, display shift, the non-contiguous DDRAM hole) are subtle enough
// that two copies would certainly drift.
//
// Deliberately free of allocation and of any ESPHome type: a sweep trial resets
// and replays this thousands of times, so reset() must be cheap and total.

#include <cstdint>
#include <string>

namespace esphome {
namespace hd44780_core {

// DDRAM is not contiguous: 0x00-0x27 backs line 1, 0x40-0x67 backs line 2, and
// the auto-increment runs 0x27 -> 0x40 -> ... -> 0x67 -> 0x00.
static const uint8_t DDRAM_SIZE = 0x68;
static const uint8_t LINE_1_BASE = 0x00;
static const uint8_t LINE_2_BASE = 0x40;
static const uint8_t LINE_LEN = 40;

/// Counters a scorer can use to judge whether a byte stream is really HD44780
/// traffic. Free to maintain, and useful as diagnostics in the live tap too.
struct DecodeStats {
  uint32_t commands{0};
  uint32_t data_bytes{0};
  uint32_t printable{0};

  /// Set DDRAM Address is the single strongest discriminator. Real traffic
  /// targets 0x00-0x27 and 0x40-0x67; a wrong pin assignment scatters the
  /// address uniformly across 0x00-0x7F, so roughly a quarter land in the hole.
  uint32_t ddram_addr_cmds{0};
  uint32_t ddram_addr_valid{0};

  uint32_t clear_display{0};
  uint32_t function_set{0};
  bool saw_2line_function_set{false};  ///< 0x28: 4-bit, 2 lines, 5x8 font
};

struct Hd44780Decoder {
  uint8_t ddram[DDRAM_SIZE]{};
  bool written[DDRAM_SIZE]{};
  uint8_t address{0};
  bool cgram_mode{false};  ///< Set CGRAM Address diverts RS=1 writes off-screen
  bool increment{true};    ///< Entry-mode direction
  uint8_t shift{0};        ///< Display shift offset, modulo LINE_LEN
  bool dirty{false};

  DecodeStats stats;

  void reset() {
    for (uint16_t i = 0; i < DDRAM_SIZE; i++) {
      this->ddram[i] = 0;
      this->written[i] = false;
    }
    this->address = 0;
    this->cgram_mode = false;
    this->increment = true;
    this->shift = 0;
    this->dirty = false;
    this->stats = DecodeStats{};
  }

  void feed_byte(bool rs, uint8_t value) {
    if (rs) {
      this->handle_data(value);
    } else {
      this->handle_command(value);
    }
  }

  void handle_command(uint8_t cmd) {
    this->stats.commands++;

    // Tested from the most significant set bit down - that is how the HD44780
    // decodes its instruction set.
    if (cmd & 0x80) {  // Set DDRAM Address
      this->address = cmd & 0x7F;
      this->cgram_mode = false;
      this->stats.ddram_addr_cmds++;
      if (this->address <= 0x27 || (this->address >= LINE_2_BASE && this->address <= 0x67))
        this->stats.ddram_addr_valid++;
      return;
    }
    if (cmd & 0x40) {
      // Set CGRAM Address. The address counter is shared, so every following
      // RS=1 write goes to the character generator, not to the screen. Treating
      // those as display data would smear custom-glyph bytes across a row.
      this->cgram_mode = true;
      return;
    }
    if (cmd & 0x20) {  // Function Set - bus width / lines / font
      this->stats.function_set++;
      if (cmd == 0x28)
        this->stats.saw_2line_function_set = true;
      return;
    }

    if (cmd & 0x10) {  // Cursor or Display Shift
      const bool display = cmd & 0x08;
      const bool right = cmd & 0x04;
      if (display) {
        // The window moves, so a DDRAM address no longer maps to a fixed column.
        this->shift = (this->shift + (right ? LINE_LEN - 1 : 1)) % LINE_LEN;
        this->dirty = true;
      } else {
        const bool saved = this->increment;
        this->increment = right;
        this->advance_address();
        this->increment = saved;
      }
      return;
    }
    if (cmd & 0x08)  // Display / cursor / blink on-off
      return;
    if (cmd & 0x04) {  // Entry Mode Set
      this->increment = cmd & 0x02;
      return;
    }
    if (cmd & 0x02) {  // Return Home
      this->address = 0;
      this->shift = 0;
      this->cgram_mode = false;
      this->dirty = true;
      return;
    }
    if (cmd & 0x01)  // Clear Display
      this->clear_display();
    // 0x00 is not an instruction; it also appears during 4-bit initialisation.
  }

  void handle_data(uint8_t value) {
    this->stats.data_bytes++;
    if (value >= 0x20 && value <= 0x7E)
      this->stats.printable++;

    if (this->cgram_mode)
      return;  // Character generator write - never reaches the screen as text

    if (this->address < DDRAM_SIZE) {
      this->ddram[this->address] = value;
      this->written[this->address] = true;
      this->dirty = true;
    }
    this->advance_address();
  }

  void advance_address() {
    // The counter skips the hole between 0x27 and 0x40.
    if (this->increment) {
      if (this->address == 0x27) {
        this->address = LINE_2_BASE;
      } else if (this->address == 0x67) {
        this->address = LINE_1_BASE;
      } else {
        this->address++;
      }
    } else {
      if (this->address == LINE_2_BASE) {
        this->address = 0x27;
      } else if (this->address == LINE_1_BASE) {
        this->address = 0x67;
      } else {
        this->address--;
      }
    }
  }

  void clear_display() {
    for (uint16_t i = 0; i < DDRAM_SIZE; i++) {
      this->ddram[i] = ' ';
      this->written[i] = true;
    }
    this->address = 0;
    this->shift = 0;
    this->increment = true;  // Clear Display forces I/D = 1
    this->cgram_mode = false;
    this->dirty = true;
    this->stats.clear_display++;
  }

  /// A cell that has never been written is not a space - it is unknown. The
  /// pump's firmware may only refresh the fields that change, so a sniffer that
  /// starts mid-run never sees the static parts. Unknown cells render as '~' so
  /// a partial view is never mistaken for a blank display; a row with nothing
  /// observed at all returns "" rather than a string of spaces.
  std::string render_line(uint8_t base, uint8_t columns) const {
    bool any = false;
    std::string out;
    out.reserve(columns);
    for (uint8_t i = 0; i < columns; i++) {
      const uint8_t idx = base + ((i + this->shift) % LINE_LEN);
      if (idx >= DDRAM_SIZE)
        continue;
      if (!this->written[idx]) {
        out.push_back('~');
        continue;
      }
      any = true;
      const uint8_t c = this->ddram[idx];
      out.push_back((c >= 0x20 && c <= 0x7E) ? (char) c : '.');
    }
    if (!any)
      return "";
    while (!out.empty() && out.back() == ' ')
      out.pop_back();
    return out;
  }
};

}  // namespace hd44780_core
}  // namespace esphome
