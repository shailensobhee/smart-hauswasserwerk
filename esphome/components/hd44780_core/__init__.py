"""Shared HD44780 controller state machine.

Header-only and config-less. Exists purely so that hd44780_tap (one live
decode) and bus_sweep (hundreds of speculative decodes over one captured
buffer) run the same state machine rather than two copies that drift apart.

Both consumers pull it in via AUTO_LOAD; it is never configured directly.
"""

import esphome.config_validation as cv

CODEOWNERS = ["@shailensobhee"]

CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    # Nothing to generate - the decoder is a header-only struct. Declaring the
    # component is what gets its directory copied into the build tree.
    pass
