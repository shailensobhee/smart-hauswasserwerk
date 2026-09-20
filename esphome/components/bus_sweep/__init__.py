"""Identify an unknown group of digital lines by brute force.

Captures every edge on up to ten pins into one buffer, then replays that
single buffer under every hypothesis it knows: all ordered HD44780 pin
assignments, async serial on each line at each standard baud, and a clocked
stream for each clock/data/polarity/bit-order combination. Each interpretation
is scored, and the ranked table plus a verdict goes to the log.

Only the lines that carried edges are permuted. That is what lets a whole
header be tapped at once instead of a hand-picked subset: rails and strapped
pins are identified by the capture and then dropped from the search.

Capturing once and replaying many times is the point. Every hypothesis is
scored against identical data, so nothing that separates them can be an
artefact of one run being busier than another.

Purely passive - every pin is an input and none is ever driven.
"""

import logging

import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome import pins
from esphome.const import CONF_ID, CONF_PIN

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@shailensobhee"]
AUTO_LOAD = ["text_sensor", "hd44780_core"]

bus_sweep_ns = cg.esphome_ns.namespace("bus_sweep")
BusSweep = bus_sweep_ns.class_("BusSweep", cg.Component)

CONF_PINS = "pins"
CONF_LABEL = "label"
CONF_CAPACITY = "capacity"
CONF_SETTLE_DELAY = "settle_delay"
CONF_CAPTURE_TIMEOUT = "capture_timeout"
CONF_IDLE_GAP = "idle_gap"
CONF_COLUMNS = "columns"
CONF_VERDICT = "verdict"

# Bounded by the single-register capture: every pin must live in GPIO_IN_REG.
# Ten covers a whole HD44780 header, power rails and all. Keep in step with
# MAX_SWEEP_PINS in bus_sweep.h.
MAX_SWEEP_PINS = 10

PIN_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_PIN): pins.internal_gpio_input_pin_schema,
        cv.Optional(CONF_LABEL): cv.string,
    }
)


def _validate_distinct_pins(config):
    seen = {}
    for index, entry in enumerate(config[CONF_PINS]):
        number = entry[CONF_PIN]["number"]
        if number in seen:
            raise cv.Invalid(
                f"sweep pin #{index} and #{seen[number]} are both GPIO{number}"
            )
        seen[number] = index
    return config


def _note_trial_count(config):
    """Report the worst-case HD44780 permutation count, but do not refuse it.

    The space is P(n-2,4)*n*(n-1): 720 for six lines, 20 160 for eight,
    151 200 for ten. That growth used to be a reason to reject wide taps, but
    the count here is only an upper bound - at runtime the sweep enumerates
    over the lines that actually carried edges, and a whole-header tap is
    mostly rails and strapped pins that never move. A ten-pin LCD header with
    six live signals costs the same 720 trials as taping only those six, with
    none of the guesswork about which six to tap. Refusing the config would
    force exactly the guess the sweep exists to remove.
    """
    n = len(config[CONF_PINS])
    if n >= 6:
        trials = n * (n - 1) * (n - 2) * (n - 3) * (n - 4) * (n - 5)
        if trials > 10000:
            _LOGGER.info(
                "%d pins is up to %d HD44780 permutations if every line turns "
                "out to be active. Static lines are pruned after capture, so "
                "the real count is usually far lower - the log prints it.",
                n,
                trials,
            )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(BusSweep),
            cv.Required(CONF_PINS): cv.All(
                cv.ensure_list(PIN_SCHEMA),
                cv.Length(min=2, max=MAX_SWEEP_PINS),
            ),
            # 8 bytes per sample, allocated once at setup.
            cv.Optional(CONF_CAPACITY, default=4096): cv.int_range(min=256, max=16384),
            # Long enough for WiFi and the API to settle, so the capture is not
            # competing with association for CPU.
            cv.Optional(
                CONF_SETTLE_DELAY, default="10s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_CAPTURE_TIMEOUT, default="60s"
            ): cv.positive_time_period_milliseconds,
            # Byte-boundary signal for the HD44780 hypothesis. Too small a value
            # corrupts every byte, so the default is deliberately generous.
            cv.Optional(
                CONF_IDLE_GAP, default="200us"
            ): cv.positive_time_period_microseconds,
            cv.Optional(CONF_COLUMNS, default=16): cv.int_range(min=1, max=40),
            cv.Optional(CONF_VERDICT): text_sensor.text_sensor_schema(),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_distinct_pins,
    _note_trial_count,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    for index, entry in enumerate(config[CONF_PINS]):
        pin = await cg.gpio_pin_expression(entry[CONF_PIN])
        label = entry.get(CONF_LABEL, f"P{index}")
        cg.add(var.add_sweep_pin(pin, label))

    cg.add(var.set_capacity(config[CONF_CAPACITY]))
    cg.add(var.set_settle_delay(config[CONF_SETTLE_DELAY]))
    cg.add(var.set_capture_timeout(config[CONF_CAPTURE_TIMEOUT]))
    cg.add(var.set_idle_gap_us(config[CONF_IDLE_GAP]))
    cg.add(var.set_columns(config[CONF_COLUMNS]))

    if CONF_VERDICT in config:
        cg.add(var.set_verdict(await text_sensor.new_text_sensor(config[CONF_VERDICT])))
