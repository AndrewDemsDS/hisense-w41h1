"""Per-bit fault and capability entities.

Each fault bit and each capability bit the A/C reports is its own binary sensor.

The bit indices below mirror the FAULT1_* / FEAT1_* constants in hisense_protocol.h.
"""

import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import (
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_HEAT,
    DEVICE_CLASS_PROBLEM,
    ENTITY_CATEGORY_DIAGNOSTIC,
)

from . import CONF_HISENSE_AC_ID, HISENSE_AC_CLIENT_SCHEMA

DEPENDENCIES = ["hisense_ac"]

CONF_AUX_HEAT = "aux_heat"
CONF_BUS_LINK = "bus_link"
CONF_PROBLEM = "problem"

# name -> (bit index, default entity name)
FAULT_BITS = {
    "fault_indoor_temp": (0, "Fault indoor temp sensor"),
    "fault_indoor_coil_temp": (1, "Fault indoor coil sensor"),
    "fault_indoor_humidity": (2, "Fault indoor humidity sensor"),
    "fault_water_full": (3, "Fault condensate tray full"),
    "fault_indoor_fan_motor": (4, "Fault indoor fan motor"),
    "fault_grille": (5, "Fault grille"),
    "fault_indoor_vzero": (6, "Fault zero-cross detect"),
    "fault_indoor_comms": (7, "Fault indoor to outdoor comms"),
    "fault_indoor_display": (8, "Fault indoor display"),
    "fault_indoor_keys": (9, "Fault indoor keypad"),
    "fault_indoor_wifi": (10, "Fault indoor wifi module"),
    "fault_indoor_ele": (11, "Fault indoor electrical"),
    "fault_indoor_eeprom": (12, "Fault indoor EEPROM"),
    "fault_outdoor_eeprom": (13, "Fault outdoor EEPROM"),
    "fault_outdoor_coil_temp": (14, "Fault outdoor coil sensor"),
    "fault_outdoor_gas_temp": (15, "Fault outdoor gas sensor"),
    "fault_outdoor_temp": (16, "Fault outdoor temp sensor"),
    "fault_over_temp": (17, "Fault over temperature"),
}

# The two-bit fields (power_display, demand_resp) are not booleans and are left out.
# Each carries its own icon: a capability has no device class, so without one every node shows
# the generic binary-sensor icon. Eco, quiet and display reuse the icon of the switch they gate.
CAPABILITY_BITS = {
    "capability_cool_heat": (0, "Capability heat pump", "mdi:heat-pump"),
    "capability_ai": (1, "Capability AI mode", "mdi:brain"),
    "capability_infinite_fan": (2, "Capability infinite fan", "mdi:fan"),
    "capability_power_save": (3, "Capability eco", "mdi:leaf"),
    "capability_fan_mute": (4, "Capability quiet", "mdi:volume-off"),
    "capability_swing_dir_8": (5, "Capability 8-position louvre", "mdi:arrow-up-down"),
    "capability_swing_follow": (6, "Capability swing follow", "mdi:arrow-oscillating"),
    "capability_humidity": (7, "Capability humidity", "mdi:water-percent"),
    "capability_heat_8c": (8, "Capability 8C frost guard", "mdi:snowflake-thermometer"),
    "capability_purify": (9, "Capability purify", "mdi:air-purifier"),
    "capability_q_display": (
        10,
        "Capability display control",
        "mdi:television-ambient-light",
    ),
    "capability_enable_8heat": (
        11,
        "Capability enable 8C heat",
        "mdi:snowflake-thermometer",
    ),
    "capability_trans_102_64": (12, "Capability trans 102-64", "mdi:swap-horizontal"),
}


def _diagnostic(device_class: str | None = None, icon: str | None = None):
    kwargs = {"entity_category": ENTITY_CATEGORY_DIAGNOSTIC}
    if device_class is not None:
        kwargs["device_class"] = device_class
    if icon is not None:
        kwargs["icon"] = icon
    return binary_sensor.binary_sensor_schema(**kwargs)


CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.Optional(CONF_AUX_HEAT): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_HEAT,
            ),
            # Link health: off while the A/C is not answering status polls.
            cv.Optional(CONF_BUS_LINK): _diagnostic(DEVICE_CLASS_CONNECTIVITY),
            cv.Optional(CONF_PROBLEM): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_PROBLEM,
            ),
        }
    )
    .extend({cv.Optional(key): _diagnostic(DEVICE_CLASS_PROBLEM) for key in FAULT_BITS})
    .extend(
        {
            cv.Optional(key): _diagnostic(icon=icon)
            for key, (_, _, icon) in CAPABILITY_BITS.items()
        }
    )
    .extend(HISENSE_AC_CLIENT_SCHEMA)
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_HISENSE_AC_ID])

    for key in (CONF_AUX_HEAT, CONF_BUS_LINK, CONF_PROBLEM):
        if key in config:
            var = await binary_sensor.new_binary_sensor(config[key])
            cg.add(getattr(parent, f"set_{key}_binary_sensor")(var))

    for key, (bit, _) in FAULT_BITS.items():
        if key in config:
            var = await binary_sensor.new_binary_sensor(config[key])
            cg.add(parent.add_fault_binary_sensor(bit, var))

    for key, (bit, _, _) in CAPABILITY_BITS.items():
        if key in config:
            var = await binary_sensor.new_binary_sensor(config[key])
            cg.add(parent.add_capability_binary_sensor(bit, var))
