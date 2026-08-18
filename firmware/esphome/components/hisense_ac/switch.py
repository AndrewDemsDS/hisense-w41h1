import esphome.codegen as cg
from esphome.components import switch
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_CONFIG

from . import CONF_HISENSE_AC_ID, HISENSE_AC_CLIENT_SCHEMA, hisense_ac_ns

DEPENDENCIES = ["hisense_ac"]

HisenseSwitch = hisense_ac_ns.class_("HisenseSwitch", switch.Switch, cg.Component)
SwitchKind = hisense_ac_ns.enum("SwitchKind")

CONF_ECO = "eco"
CONF_TURBO = "turbo"
CONF_QUIET = "quiet"
CONF_DISPLAY = "display"

# Declare only the ones your unit has: the `capabilities` text sensor reports what the A/C
# answers to the ProductType poll, and an absent capability means the switch does nothing.
SWITCHES = {
    CONF_ECO: (SwitchKind.SWITCH_ECO, "mdi:leaf"),
    CONF_TURBO: (SwitchKind.SWITCH_TURBO, "mdi:fan-plus"),
    CONF_QUIET: (SwitchKind.SWITCH_QUIET, "mdi:volume-off"),
    CONF_DISPLAY: (SwitchKind.SWITCH_DISPLAY, "mdi:television-ambient-light"),
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.Optional(key): switch.switch_schema(
            HisenseSwitch,
            icon=icon,
            entity_category=ENTITY_CATEGORY_CONFIG,
        ).extend(cv.COMPONENT_SCHEMA)
        for key, (_, icon) in SWITCHES.items()
    }
).extend(HISENSE_AC_CLIENT_SCHEMA)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_HISENSE_AC_ID])
    for key, (kind, _) in SWITCHES.items():
        if key not in config:
            continue
        var = await switch.new_switch(config[key])
        await cg.register_component(var, config[key])
        cg.add(var.set_parent(parent))
        cg.add(var.set_kind(kind))
