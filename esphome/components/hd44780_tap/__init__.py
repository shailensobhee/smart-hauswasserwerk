"""Passive tap for a write-only 4-bit HD44780 character LCD bus.

Listens on the six active lines (RS, E, D4-D7) of a host MCU driving an
HD44780-compatible panel, reconstructs the controller's DDRAM, and exposes the
rendered rows as text sensors. Never drives the bus.
"""

import esphome.codegen as cg
from esphome.components import sensor, text_sensor
import esphome.config_validation as cv
from esphome import pins
from esphome.const import (
    CONF_ID,
    CONF_NUMBER,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_HERTZ,
)

CODEOWNERS = ["@shailensobhee"]
# hd44780_core carries the shared controller state machine, which bus_sweep
# also replays. It has no configuration of its own.
AUTO_LOAD = ["text_sensor", "sensor", "hd44780_core"]

hd44780_tap_ns = cg.esphome_ns.namespace("hd44780_tap")
HD44780Tap = hd44780_tap_ns.class_("HD44780Tap", cg.Component)

CONF_E_PIN = "e_pin"
CONF_RS_PIN = "rs_pin"
CONF_D4_PIN = "d4_pin"
CONF_D5_PIN = "d5_pin"
CONF_D6_PIN = "d6_pin"
CONF_D7_PIN = "d7_pin"

CONF_LINE_1 = "line_1"
CONF_LINE_2 = "line_2"
CONF_EDGE_RATE = "edge_rate"
CONF_DROPPED_SAMPLES = "dropped_samples"

CONF_RAW_CAPTURE = "raw_capture"
CONF_IDLE_GAP = "idle_gap"
CONF_COLUMNS = "columns"

# Grouped so a permutation sweep over the unknown wiring is a four-line edit.
DATA_PINS = [CONF_D4_PIN, CONF_D5_PIN, CONF_D6_PIN, CONF_D7_PIN]


def _validate_distinct_pins(config):
    """Two roles landing on one pin would silently decode plausible garbage."""
    seen = {}
    for key in [CONF_E_PIN, CONF_RS_PIN, *DATA_PINS]:
        number = config[key][CONF_NUMBER]
        if number in seen:
            raise cv.Invalid(
                f"'{key}' and '{seen[number]}' are both GPIO{number}; "
                "each tapped HD44780 line needs its own pin"
            )
        seen[number] = key
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(HD44780Tap),
            cv.Required(CONF_E_PIN): pins.internal_gpio_input_pin_schema,
            cv.Required(CONF_RS_PIN): pins.internal_gpio_input_pin_schema,
            cv.Required(CONF_D4_PIN): pins.internal_gpio_input_pin_schema,
            cv.Required(CONF_D5_PIN): pins.internal_gpio_input_pin_schema,
            cv.Required(CONF_D6_PIN): pins.internal_gpio_input_pin_schema,
            cv.Required(CONF_D7_PIN): pins.internal_gpio_input_pin_schema,
            cv.Optional(CONF_COLUMNS, default=16): cv.int_range(min=1, max=40),
            # A gap longer than this forces the next nibble to be read as the
            # high half of a fresh byte. Backs up the RS-change resync, which
            # cannot help during a long run of same-RS writes.
            cv.Optional(
                CONF_IDLE_GAP, default="200us"
            ): cv.positive_time_period_microseconds,
            cv.Optional(CONF_RAW_CAPTURE, default=False): cv.boolean,
            cv.Optional(CONF_LINE_1): text_sensor.text_sensor_schema(),
            cv.Optional(CONF_LINE_2): text_sensor.text_sensor_schema(),
            cv.Optional(CONF_EDGE_RATE): sensor.sensor_schema(
                unit_of_measurement=UNIT_HERTZ,
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:pulse",
            ),
            cv.Optional(CONF_DROPPED_SAMPLES): sensor.sensor_schema(
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:alert-circle-outline",
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_distinct_pins,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    for key, setter in [
        (CONF_E_PIN, "set_e_pin"),
        (CONF_RS_PIN, "set_rs_pin"),
        (CONF_D4_PIN, "set_d4_pin"),
        (CONF_D5_PIN, "set_d5_pin"),
        (CONF_D6_PIN, "set_d6_pin"),
        (CONF_D7_PIN, "set_d7_pin"),
    ]:
        cg.add(getattr(var, setter)(await cg.gpio_pin_expression(config[key])))

    cg.add(var.set_columns(config[CONF_COLUMNS]))
    cg.add(var.set_idle_gap_us(config[CONF_IDLE_GAP]))
    cg.add(var.set_raw_capture(config[CONF_RAW_CAPTURE]))

    if CONF_LINE_1 in config:
        cg.add(var.set_line_1(await text_sensor.new_text_sensor(config[CONF_LINE_1])))
    if CONF_LINE_2 in config:
        cg.add(var.set_line_2(await text_sensor.new_text_sensor(config[CONF_LINE_2])))
    if CONF_EDGE_RATE in config:
        cg.add(var.set_edge_rate(await sensor.new_sensor(config[CONF_EDGE_RATE])))
    if CONF_DROPPED_SAMPLES in config:
        cg.add(
            var.set_dropped_samples(
                await sensor.new_sensor(config[CONF_DROPPED_SAMPLES])
            )
        )
