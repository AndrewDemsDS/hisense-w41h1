"""Per-bit fault and capability entities.

The Matter builds pack these into the `Faults1` / `Features1` bitmaps and hand the decoding
to a companion HACS integration, because Home Assistant cannot render a manufacturer cluster
without changes to two upstream projects. Here each bit is simply its own binary sensor.

The bit indices below mirror the HISENSE_FAULT1_* / HISENSE_FEAT1_* macros in hisense_rs485.h.
firmware/test/test_diag_contract.py asserts that agreement, so a renumbered bit fails the host
QA gate instead of silently mislabelling a fault.
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
CAPABILITY_BITS = {
    "capability_cool_heat": (0, "Capability heat pump"),
    "capability_ai": (1, "Capability AI mode"),
    "capability_infinite_fan": (2, "Capability infinite fan"),
    "capability_power_save": (3, "Capability eco"),
    "capability_fan_mute": (4, "Capability quiet"),
    "capability_swing_dir_8": (5, "Capability 8-position louvre"),
    "capability_swing_follow": (6, "Capability swing follow"),
    "capability_humidity": (7, "Capability humidity"),
    "capability_heat_8c": (8, "Capability 8C frost guard"),
    "capability_purify": (9, "Capability purify"),
    "capability_q_display": (10, "Capability display control"),
    "capability_enable_8heat": (11, "Capability enable 8C heat"),
    "capability_trans_102_64": (12, "Capability trans 102/64"),
}


def _diagnostic(device_class: str | None = None):
    kwargs = {"entity_category": ENTITY_CATEGORY_DIAGNOSTIC}
    if device_class is not None:
        kwargs["device_class"] = device_class
    return binary_sensor.binary_sensor_schema(**kwargs)


CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.Optional(CONF_AUX_HEAT): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_HEAT,
            ),
            # The #56 link-health signal, which the Matter builds could only express by nulling
            # liveness attributes and letting entities go unavailable.
            cv.Optional(CONF_BUS_LINK): _diagnostic(DEVICE_CLASS_CONNECTIVITY),
            cv.Optional(CONF_PROBLEM): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_PROBLEM,
            ),
        }
    )
    .extend(
        {
            cv.Optional(key): _diagnostic(DEVICE_CLASS_PROBLEM)
            for key in FAULT_BITS
        }
    )
    .extend({cv.Optional(key): _diagnostic() for key in CAPABILITY_BITS})
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

    for key, (bit, _) in CAPABILITY_BITS.items():
        if key in config:
            var = await binary_sensor.new_binary_sensor(config[key])
            cg.add(parent.add_capability_binary_sensor(bit, var))
