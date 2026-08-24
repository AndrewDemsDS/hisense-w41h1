import esphome.codegen as cg
from esphome.components import select
import esphome.config_validation as cv

from . import CONF_HISENSE_AC_ID, HISENSE_AC_CLIENT_SCHEMA, hisense_ac_ns

DEPENDENCIES = ["hisense_ac"]

HisenseSleepSelect = hisense_ac_ns.class_("HisenseSleepSelect", select.Select, cg.Component)

CONF_SLEEP_PROFILE = "sleep_profile"

# Order is the wire order: the index IS the profile number the command frame carries.
SLEEP_PROFILES = ["Off", "General", "Old", "Young", "Kids"]

CONFIG_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_SLEEP_PROFILE): select.select_schema(
            HisenseSleepSelect,
            icon="mdi:sleep",
        ).extend(cv.COMPONENT_SCHEMA),
    }
).extend(HISENSE_AC_CLIENT_SCHEMA)


async def to_code(config):
    if CONF_SLEEP_PROFILE not in config:
        return
    conf = config[CONF_SLEEP_PROFILE]
    var = await select.new_select(conf, options=SLEEP_PROFILES)
    await cg.register_component(var, conf)
    parent = await cg.get_variable(config[CONF_HISENSE_AC_ID])
    cg.add(var.set_parent(parent))
