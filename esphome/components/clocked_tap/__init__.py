"""Continuous passive tap on a two-wire clocked serial display bus.

Where bus_sweep answers "what is on these lines?" in one 0.4 s shot, this
answers "what does the panel say *right now*" and keeps answering it. That is
the difference that matters for watching a button press: the sweep's buffer
fills faster than a human can reach the front panel, so a press essentially
never lands inside its window.

Samples the data line on one configured edge of the clock line, frames on the
idle gap between refreshes, and decodes each frame into display lines. Never
drives either line.

The decode is marker-driven rather than offset-driven. Each line is introduced
by a two-word marker, and the scan looks for it at every BIT position, because
on the measured panel the two lines within a single frame are two bits out of
phase with each other - no fixed byte alignment renders both. Resyncing on
content also contains damage: a slipped clock edge costs the line it lands in
rather than everything after it.

Words are `word_bits` long and the payload is always their last eight bits, so
a 9-bit stream (one flag bit then a byte) and a plain 8-bit stream go through
the same path.
"""

import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome import pins
from esphome.const import CONF_ID, CONF_NUMBER

CODEOWNERS = ["@shailensobhee"]
AUTO_LOAD = ["text_sensor"]

clocked_tap_ns = cg.esphome_ns.namespace("clocked_tap")
ClockedTap = clocked_tap_ns.class_("ClockedTap", cg.Component)

CONF_CLOCK_PIN = "clock_pin"
CONF_DATA_PIN = "data_pin"
CONF_EDGE = "edge"
CONF_BIT_ORDER = "bit_order"
CONF_FRAME_GAP = "frame_gap"
CONF_WORD_BITS = "word_bits"
CONF_LINE_MARKER = "line_marker"
CONF_COLUMNS = "columns"
CONF_STABLE_FRAMES = "stable_frames"
CONF_BLANK_HOLD = "blank_hold"
CONF_LOG_REPEATS = "log_repeats"
CONF_LINE_1 = "line_1"
CONF_LINE_2 = "line_2"

EDGES = {"rising": True, "falling": False}
BIT_ORDERS = {"msb_first": True, "lsb_first": False}


def _validate(config):
    """Clock and data on one pin decodes into a constant, not an error."""
    if config[CONF_CLOCK_PIN][CONF_NUMBER] == config[CONF_DATA_PIN][CONF_NUMBER]:
        raise cv.Invalid("'clock_pin' and 'data_pin' must be different pins")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ClockedTap),
            cv.Required(CONF_CLOCK_PIN): pins.internal_gpio_input_pin_schema,
            cv.Required(CONF_DATA_PIN): pins.internal_gpio_input_pin_schema,
            cv.Optional(CONF_EDGE, default="rising"): cv.enum(EDGES, lower=True),
            cv.Optional(CONF_BIT_ORDER, default="msb_first"): cv.enum(
                BIT_ORDERS, lower=True
            ),
            # Anything longer than this between clock edges ends the frame. The
            # measured bus leaves a three-decade hole to sit in - intra-frame
            # gaps top out near 64us, frames are ~90ms apart - so the default is
            # not a delicate setting. It only needs revisiting on a bus whose
            # byte spacing approaches its frame spacing.
            cv.Optional(
                CONF_FRAME_GAP, default="1ms"
            ): cv.positive_time_period_microseconds,
            # 9 on the measured panel: one flag bit, then the byte. Set 8 for a
            # stream with no per-word flag.
            cv.Optional(CONF_WORD_BITS, default=9): cv.int_range(min=8, max=16),
            # The two payload bytes that introduce a display line. Found by
            # inspecting a captured frame, not guessable - if a new panel
            # decodes nothing, dump the hex and look for what repeats once per
            # line.
            cv.Optional(CONF_LINE_MARKER, default=[0x7C, 0x40]): cv.All(
                cv.ensure_list(cv.hex_uint8_t), cv.Length(min=2, max=2)
            ),
            cv.Optional(CONF_COLUMNS, default=16): cv.int_range(min=1, max=40),
            # How many consecutive frames must decode identically before the
            # screen is believed. A flipped bit renders as a one-cell
            # difference that looks exactly like a real change; at 10 frames/s
            # two frames costs 100 ms and removes every single-frame glitch.
            # Set 1 to see raw per-frame corruption.
            cv.Optional(CONF_STABLE_FRAMES, default=2): cv.int_range(min=1, max=20),
            # How long a line must STAY blank before the blank is published.
            # Only the published text sensors are affected; the log always
            # prints what the panel actually sent.
            #
            # This exists because of a real 1 Hz blink: the Always On screen
            # alternates its second line between a warning and blank, 500 ms
            # each, forever. stable_frames cannot help - the blink is stable,
            # it is the panel's genuine state - so without a hold, HA's
            # recorder takes two state changes a second for as long as the
            # pump sits in that mode. 1500 ms clears the 500 ms half-period
            # with room to spare while still letting a real blank through
            # after a delay a human would not notice.
            cv.Optional(
                CONF_BLANK_HOLD, default="1500ms"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_LOG_REPEATS, default=False): cv.boolean,
            cv.Optional(CONF_LINE_1): text_sensor.text_sensor_schema(),
            cv.Optional(CONF_LINE_2): text_sensor.text_sensor_schema(),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_clock_pin(await cg.gpio_pin_expression(config[CONF_CLOCK_PIN])))
    cg.add(var.set_data_pin(await cg.gpio_pin_expression(config[CONF_DATA_PIN])))
    cg.add(var.set_on_rising(config[CONF_EDGE]))
    cg.add(var.set_msb_first(config[CONF_BIT_ORDER]))
    cg.add(var.set_frame_gap_us(config[CONF_FRAME_GAP]))
    cg.add(var.set_word_bits(config[CONF_WORD_BITS]))
    marker = config[CONF_LINE_MARKER]
    cg.add(var.set_marker(marker[0], marker[1]))
    cg.add(var.set_columns(config[CONF_COLUMNS]))
    cg.add(var.set_stable_frames(config[CONF_STABLE_FRAMES]))
    cg.add(var.set_blank_hold_ms(config[CONF_BLANK_HOLD]))
    cg.add(var.set_log_repeats(config[CONF_LOG_REPEATS]))

    if CONF_LINE_1 in config:
        cg.add(var.set_line_1(await text_sensor.new_text_sensor(config[CONF_LINE_1])))
    if CONF_LINE_2 in config:
        cg.add(var.set_line_2(await text_sensor.new_text_sensor(config[CONF_LINE_2])))
