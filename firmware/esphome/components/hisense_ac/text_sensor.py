import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC
from esphome.types import ConfigType

from . import CONF_HISENSE_AC_ID, HISENSE_AC_CLIENT_SCHEMA

DEPENDENCIES = ["hisense_ac"]

CONF_LINK_TOKEN = "link_token"
CONF_FAULT_DETAIL = "fault_detail"
CONF_FAULT_BYTES = "fault_bytes"

CONFIG_SCHEMA = cv.Schema(
    {
        # The device-type / sub-type pair the A/C reports in its DevType reply, which the hub
        # then stamps on outbound frames. It is a static per-model identifier, NOT a session
        # token, and stamping the wrong bytes kills the link entirely, so it is worth
        # being able to read it back from a deployed unit.
        cv.Optional(CONF_LINK_TOKEN): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:identifier",
        ),
        # Which fault the A/C reports: "none", or the set fault bits as a comma list of the
        # fault_* binary sensor names, less the prefix. Other bits show as "reserved byteNN 0xMM",
        # "name? byteNN 0xMM" or "unknown byteNN 0xMM". The text comes from fault_detail_text()
        # in hisense_protocol.cpp.
        cv.Optional(CONF_FAULT_DETAIL): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:alert-circle-outline",
        ),
        # The four raw fault bytes, payload 39, 40, 64 and 66, as "HH HH HH HH".
        cv.Optional(CONF_FAULT_BYTES): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:hexadecimal",
        ),
    }
).extend(HISENSE_AC_CLIENT_SCHEMA)


async def to_code(config: ConfigType) -> None:
    parent = await cg.get_variable(config[CONF_HISENSE_AC_ID])
    if conf := config.get(CONF_LINK_TOKEN):
        var = await text_sensor.new_text_sensor(conf)
        cg.add(parent.set_link_token_text_sensor(var))
    if conf := config.get(CONF_FAULT_DETAIL):
        var = await text_sensor.new_text_sensor(conf)
        cg.add(parent.set_fault_detail_text_sensor(var))
    if conf := config.get(CONF_FAULT_BYTES):
        var = await text_sensor.new_text_sensor(conf)
        cg.add(parent.set_fault_bytes_text_sensor(var))
