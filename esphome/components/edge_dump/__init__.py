"""Dump the raw edge timing of one line, for off-device clock recovery.

Captures every transition on a single pin into a buffer, frames on an idle
gap, and prints each frame as level:duration run lengths. Nothing is decoded
here on purpose - the recovery algorithm is the part that needs iterating, and
iterating on the device costs a reflash per attempt.

Purely passive: the pin is an input and is never driven.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.const import CONF_ID

CODEOWNERS = ["@shailensobhee"]

edge_dump_ns = cg.esphome_ns.namespace("edge_dump")
EdgeDump = edge_dump_ns.class_("EdgeDump", cg.Component)

CONF_FRAME_GAP = "frame_gap"
CONF_MIN_EDGES = "min_edges"
CONF_MAX_FRAMES = "max_frames"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(EdgeDump),
        cv.Required(CONF_PIN := "pin"): pins.internal_gpio_input_pin_schema,
        # Intra-frame gaps on the measured bus top out near 256 us and frames
        # are ~90 ms apart, so there are two decades of daylight to sit in.
        cv.Optional(
            CONF_FRAME_GAP, default="1ms"
        ): cv.positive_time_period_microseconds,
        # Below this an "idle gap" is just a quiet patch inside a frame.
        cv.Optional(CONF_MIN_EDGES, default=40): cv.int_range(min=2, max=2000),
        # A handful of frames is enough to cross-check a recovery against
        # itself. The high ceiling is for runs that have to span an ACTION -
        # walking the MODE cycle takes tens of seconds at ~10 frames/s, and the
        # whole point of such a run is to catch the screen changing.
        cv.Optional(CONF_MAX_FRAMES, default=4): cv.int_range(min=1, max=2000),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    pin = await cg.gpio_pin_expression(config["pin"])
    cg.add(var.set_pin(pin))
    cg.add(var.set_frame_gap_us(config[CONF_FRAME_GAP]))
    cg.add(var.set_min_edges(config[CONF_MIN_EDGES]))
    cg.add(var.set_max_frames(config[CONF_MAX_FRAMES]))
