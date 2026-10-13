import esphome.codegen as cg
from esphome.components import climate
import esphome.config_validation as cv
from esphome.const import (
    CONF_MAX_TEMPERATURE,
    CONF_MIN_TEMPERATURE,
    CONF_PLATFORM,
    CONF_SUPPORTS_HEAT,
)
import esphome.final_validate as fv

from . import CONF_HISENSE_AC_ID, HISENSE_AC_CLIENT_SCHEMA, hisense_ac_ns

DEPENDENCIES = ["hisense_ac"]

HisenseClimate = hisense_ac_ns.class_("HisenseClimate", climate.Climate, cg.Component)

CONF_SUPPORTS_HORIZONTAL_SWING = "supports_horizontal_swing"
CONF_SUPPORTS_ECO = "supports_eco"
CONF_SUPPORTS_QUIET = "supports_quiet"
CONF_SUPPORTS_TURBO = "supports_turbo"
CONF_SUPPORTS_SLEEP = "supports_sleep"

# Option -> the SUPPORT_* constant in hisense_special.h that the generated code names. The
# number lives in the C++ header only.
PRESET_SUPPORT_BITS = {
    CONF_SUPPORTS_ECO: "SUPPORT_ECO",
    CONF_SUPPORTS_QUIET: "SUPPORT_QUIET",
    CONF_SUPPORTS_TURBO: "SUPPORT_TURBO",
    CONF_SUPPORTS_SLEEP: "SUPPORT_SLEEP",
}

# What the bus carries: SETPOINT_MIN_C, SETPOINT_MAX_C and SETPOINT_MAX_F in hisense_protocol.h
# (the host test test_esphome_tables.py holds these equal to the header).
SETPOINT_MIN_C = 16
SETPOINT_MAX_C = 32
SETPOINT_MAX_F = 90


def _setpoint_range(config):
    low = config[CONF_MIN_TEMPERATURE]
    high = config[CONF_MAX_TEMPERATURE]
    if low > high:
        raise cv.Invalid(
            f"min_temperature ({low:g} °C) is above max_temperature ({high:g} °C)."
        )
    # A range written in Fahrenheit arrives here in Celsius: 90 °F is 32.2 °C.
    highest = max(SETPOINT_MAX_C, (SETPOINT_MAX_F - 32) * 5 / 9)
    if low < SETPOINT_MIN_C or high > highest + 0.01:
        raise cv.Invalid(
            f"The unit takes a setpoint from {SETPOINT_MIN_C} to {SETPOINT_MAX_C} °C "
            f"(61 to {SETPOINT_MAX_F} °F). A range of {low:g} to {high:g} °C would offer "
            "values in Home Assistant that are sent as the nearest one the unit takes."
        )
    return config


CONFIG_SCHEMA = cv.All(
    climate.climate_schema(HisenseClimate)
    .extend(
        {
            # Capability gating is a YAML decision here rather than runtime firmware logic:
            # you just do not declare what your unit lacks.
            cv.Optional(CONF_SUPPORTS_HEAT, default=True): cv.boolean,
            cv.Optional(CONF_SUPPORTS_HORIZONTAL_SWING, default=False): cv.boolean,
            # Special modes offered as climate presets (eco, quiet, turbo, eco_quiet, sleep_*,
            # eco_sleep_*). Set false for a mode your unit lacks and every preset needing it
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
    .extend(HISENSE_AC_CLIENT_SCHEMA),
    _setpoint_range,
)


def _one_climate_per_hub(config):
    # The hub hands each status frame to one climate entity. A second one would never update.
    hub = config[CONF_HISENSE_AC_ID]
    on_this_hub = [
        entry
        for entry in fv.full_config.get().get("climate", [])
        if entry.get(CONF_PLATFORM) == "hisense_ac"
        and entry.get(CONF_HISENSE_AC_ID) == hub
    ]
    if len(on_this_hub) > 1:
        raise cv.Invalid(
            f"Only one hisense_ac climate entity per hub: {len(on_this_hub)} are declared "
            f"for '{hub}'. The A/C is one climate device. Remove the others."
        )
    return config


FINAL_VALIDATE_SCHEMA = _one_climate_per_hub


async def to_code(config):
    var = await climate.new_climate(config)
    await cg.register_component(var, config)

    await cg.register_parented(var, config[CONF_HISENSE_AC_ID])
    cg.add(var.set_supports_heat(config[CONF_SUPPORTS_HEAT]))
    cg.add(var.set_supports_horizontal_swing(config[CONF_SUPPORTS_HORIZONTAL_SWING]))
    support = [
        f"{hisense_ac_ns}::{name}"
        for key, name in PRESET_SUPPORT_BITS.items()
        if config[key]
    ]
    cg.add(var.set_preset_support(cg.RawExpression(" | ".join(support) or "0")))
    cg.add(var.set_visual_min(config[CONF_MIN_TEMPERATURE]))
    cg.add(var.set_visual_max(config[CONF_MAX_TEMPERATURE]))
