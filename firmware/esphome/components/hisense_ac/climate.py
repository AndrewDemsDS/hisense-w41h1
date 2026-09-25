import esphome.codegen as cg
from esphome.components import climate
import esphome.config_validation as cv
from esphome.const import CONF_MAX_TEMPERATURE, CONF_MIN_TEMPERATURE, CONF_SUPPORTS_HEAT

from . import CONF_HISENSE_AC_ID, HISENSE_AC_CLIENT_SCHEMA, hisense_ac_ns

DEPENDENCIES = ["hisense_ac"]

HisenseClimate = hisense_ac_ns.class_("HisenseClimate", climate.Climate, cg.Component)

CONF_SUPPORTS_HORIZONTAL_SWING = "supports_horizontal_swing"
CONF_SUPPORTS_ECO = "supports_eco"
CONF_SUPPORTS_QUIET = "supports_quiet"
CONF_SUPPORTS_TURBO = "supports_turbo"
CONF_SUPPORTS_SLEEP = "supports_sleep"

# Bits of ESPHOME_SUPPORT_* in esphome_aircon_map.h.
PRESET_SUPPORT_BITS = {
    CONF_SUPPORTS_ECO: 0x01,
    CONF_SUPPORTS_QUIET: 0x02,
    CONF_SUPPORTS_TURBO: 0x04,
    CONF_SUPPORTS_SLEEP: 0x08,
}

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
            # Special modes offered as climate presets (eco, quiet, turbo, eco_quiet, sleep_*,
            # eco_sleep_*), named exactly as the hisense-unified-ac wrapper names them for the
            # Matter builds. Set false for a mode your unit lacks and every preset needing it
            # disappears; the capability_* binary sensors report what the A/C answers.
            **{
                cv.Optional(key, default=True): cv.boolean
                for key in PRESET_SUPPORT_BITS
            },
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
    support = 0
    for key, bit in PRESET_SUPPORT_BITS.items():
        if config[key]:
            support |= bit
    cg.add(var.set_preset_support(support))
    cg.add(var.set_visual_min(config[CONF_MIN_TEMPERATURE]))
    cg.add(var.set_visual_max(config[CONF_MAX_TEMPERATURE]))
