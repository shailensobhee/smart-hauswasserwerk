"""Identify what an unknown group of digital lines actually is.

Attaches an any-edge ISR to every configured pin and reports per-pin edge
rates, duty cycle, pulse-width extremes and a cross-pin coincidence matrix.
That is enough to tell a clock from a strobe from a data line from a static
rail, and to spot async serial and estimate its baud rate.

Purely passive - every pin is an input and none is ever driven.
"""

import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome import pins
from esphome.const import CONF_ID, CONF_PIN

CODEOWNERS = ["@shailensobhee"]
AUTO_LOAD = ["text_sensor"]

bus_probe_ns = cg.esphome_ns.namespace("bus_probe")
BusProbe = bus_probe_ns.class_("BusProbe", cg.Component)

CONF_PINS = "pins"
CONF_LABEL = "label"
CONF_REPORT_INTERVAL = "report_interval"
CONF_RAW_DUMP = "raw_dump"
CONF_SUMMARY = "summary"

# Bounded by the single-register capture: every pin must live in GPIO_IN_REG.
MAX_PROBE_PINS = 8

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
                f"probe pin #{index} and #{seen[number]} are both GPIO{number}"
            )
        seen[number] = index
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(BusProbe),
            cv.Required(CONF_PINS): cv.All(
                cv.ensure_list(PIN_SCHEMA),
                cv.Length(min=1, max=MAX_PROBE_PINS),
            ),
            cv.Optional(
                CONF_REPORT_INTERVAL, default="10s"
            ): cv.positive_time_period_milliseconds,
            # Logs every single transition. Decisive for reconstructing a
            # waveform, far too chatty to leave on.
            cv.Optional(CONF_RAW_DUMP, default=False): cv.boolean,
            cv.Optional(CONF_SUMMARY): text_sensor.text_sensor_schema(),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_distinct_pins,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    for index, entry in enumerate(config[CONF_PINS]):
        pin = await cg.gpio_pin_expression(entry[CONF_PIN])
        label = entry.get(CONF_LABEL, f"P{index}")
        cg.add(var.add_probe_pin(pin, label))

    cg.add(var.set_report_interval(config[CONF_REPORT_INTERVAL]))
    cg.add(var.set_raw_dump(config[CONF_RAW_DUMP]))

    if CONF_SUMMARY in config:
        cg.add(var.set_summary(await text_sensor.new_text_sensor(config[CONF_SUMMARY])))
