import esphome.codegen as cg
from esphome.components import climate
import esphome.config_validation as cv
from esphome.const import CONF_MAX_TEMPERATURE, CONF_MIN_TEMPERATURE

from . import CONF_HISENSE_AC_ID, HISENSE_AC_CLIENT_SCHEMA, hisense_ac_ns

DEPENDENCIES = ["hisense_ac"]

HisenseClimate = hisense_ac_ns.class_("HisenseClimate", climate.Climate, cg.Component)

CONF_SUPPORTS_HEAT = "supports_heat"
CONF_SUPPORTS_HORIZONTAL_SWING = "supports_horizontal_swing"

CONFIG_SCHEMA = (
    climate.climate_schema(HisenseClimate)
    .extend(
        {
            # Capability gating is a YAML decision here rather than runtime firmware logic:
            # a Matter node's endpoint list is fixed once commissioned, so that firmware has
            # to hide unsupported controls itself. In ESPHome you just do not declare them.
            # `capabilities` on the text_sensor platform reports what your unit answers.
            cv.Optional(CONF_SUPPORTS_HEAT, default=True): cv.boolean,
            cv.Optional(CONF_SUPPORTS_HORIZONTAL_SWING, default=False): cv.boolean,
            cv.Optional(CONF_MIN_TEMPERATURE, default=16): cv.temperature,
            cv.Optional(CONF_MAX_TEMPERATURE, default=32): cv.temperature,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(HISENSE_AC_CLIENT_SCHEMA)
)


async def to_code(config):
    var = await climate.new_climate(config)
    await cg.register_component(var, config)

    parent = await cg.get_variable(config[CONF_HISENSE_AC_ID])
    cg.add(var.set_parent(parent))
    cg.add(var.set_supports_heat(config[CONF_SUPPORTS_HEAT]))
    cg.add(var.set_supports_horizontal_swing(config[CONF_SUPPORTS_HORIZONTAL_SWING]))
    cg.add(var.set_visual_min(config[CONF_MIN_TEMPERATURE]))
    cg.add(var.set_visual_max(config[CONF_MAX_TEMPERATURE]))
