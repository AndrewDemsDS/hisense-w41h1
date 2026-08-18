import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_HISENSE_AC_ID, HISENSE_AC_CLIENT_SCHEMA

DEPENDENCIES = ["hisense_ac"]

CONF_LINK_TOKEN = "link_token"

CONFIG_SCHEMA = cv.Schema(
    {
        # The device-type / sub-type pair the A/C reports in its DevType reply, which the driver
        # then stamps on outbound frames. It is a static per-model identifier, NOT a session
        # token, and stamping the wrong bytes once killed the link entirely, so it is worth
        # being able to read it back from a deployed unit.
        cv.Optional(CONF_LINK_TOKEN): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:identifier",
        ),
    }
).extend(HISENSE_AC_CLIENT_SCHEMA)


async def to_code(config):
    if CONF_LINK_TOKEN not in config:
        return
    parent = await cg.get_variable(config[CONF_HISENSE_AC_ID])
    var = await text_sensor.new_text_sensor(config[CONF_LINK_TOKEN])
    cg.add(parent.set_link_token_text_sensor(var))
