"""Per-bit fault and capability entities.

Each fault bit and each capability bit the A/C reports is its own binary sensor.

The tables below name the FAULT1_* / FEAT1_* constants in hisense_protocol.h, and the generated
code passes the constant, not a number: the bit indices are written once, in the C++ header. A
name that does not exist there fails the build, and the host test test_esphome_tables.py says so
before a build is needed.
"""

import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_DISABLED_BY_DEFAULT,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_HEAT,
    DEVICE_CLASS_PROBLEM,
    ENTITY_CATEGORY_DIAGNOSTIC,
)
from esphome.types import ConfigType

from . import CONF_HISENSE_AC_ID, HISENSE_AC_CLIENT_SCHEMA, hisense_ac_ns

DEPENDENCIES = ["hisense_ac"]

CONF_AUX_HEAT = "aux_heat"
CONF_BUS_LINK = "bus_link"
CONF_PROBLEM = "problem"

# option -> (FAULT1_* constant, what the fault is)
FAULT_BITS = {
    "fault_indoor_temp": ("FAULT1_IN_TEMP", "Fault indoor temp sensor"),
    "fault_indoor_coil_temp": ("FAULT1_IN_COIL_TEMP", "Fault indoor coil sensor"),
    "fault_indoor_humidity": ("FAULT1_IN_HUMIDITY", "Fault indoor humidity sensor"),
    "fault_water_full": ("FAULT1_WATER_FULL", "Fault condensate tray full"),
    "fault_indoor_fan_motor": ("FAULT1_IN_FAN_MOTOR", "Fault indoor fan motor"),
    "fault_grille": ("FAULT1_GRILLE", "Fault grille"),
    "fault_indoor_vzero": ("FAULT1_IN_VZERO", "Fault zero-cross detect"),
    "fault_indoor_comms": ("FAULT1_IN_COM", "Fault indoor to outdoor comms"),
    "fault_indoor_display": ("FAULT1_IN_DISPLAY", "Fault indoor display"),
    "fault_indoor_keys": ("FAULT1_IN_KEYS", "Fault indoor keypad"),
    "fault_indoor_wifi": ("FAULT1_IN_WIFI", "Fault indoor wifi module"),
    "fault_indoor_ele": ("FAULT1_IN_ELE", "Fault indoor electrical"),
    "fault_indoor_eeprom": ("FAULT1_IN_EEPROM", "Fault indoor EEPROM"),
    "fault_outdoor_eeprom": ("FAULT1_OUT_EEPROM", "Fault outdoor EEPROM"),
    "fault_outdoor_coil_temp": ("FAULT1_OUT_COIL_TEMP", "Fault outdoor coil sensor"),
    "fault_outdoor_gas_temp": ("FAULT1_OUT_GAS_TEMP", "Fault outdoor gas sensor"),
    "fault_outdoor_temp": ("FAULT1_OUT_TEMP", "Fault outdoor temp sensor"),
    "fault_over_temp": ("FAULT1_OVER_TEMP", "Fault over temperature"),
}

# The two-bit fields (power_display, demand_resp) are not booleans and are left out.
# Each carries its own icon: a capability has no device class, so without one every node shows
# the generic binary-sensor icon. Eco, quiet and display reuse the icon of the switch they gate.
CAPABILITY_BITS = {
    "capability_cool_heat": (
        "FEAT1_COOL_HEAT",
        "Capability heat pump",
        "mdi:heat-pump",
    ),
    "capability_ai": ("FEAT1_AI", "Capability AI mode", "mdi:brain"),
    "capability_infinite_fan": (
        "FEAT1_INFINITE_FAN",
        "Capability infinite fan",
        "mdi:fan",
    ),
    "capability_power_save": ("FEAT1_POWER_SAVE", "Capability eco", "mdi:leaf"),
    "capability_fan_mute": ("FEAT1_FAN_MUTE", "Capability quiet", "mdi:volume-off"),
    "capability_swing_dir_8": (
        "FEAT1_SWING_DIR_8",
        "Capability 8-position louvre",
        "mdi:arrow-up-down",
    ),
    "capability_swing_follow": (
        "FEAT1_SWING_FOLLOW",
        "Capability swing follow",
        "mdi:arrow-oscillating",
    ),
    "capability_humidity": (
        "FEAT1_HUMIDITY",
        "Capability humidity",
        "mdi:water-percent",
    ),
    "capability_heat_8c": (
        "FEAT1_HEAT_8C",
        "Capability 8C frost guard",
        "mdi:snowflake-thermometer",
    ),
    "capability_purify": ("FEAT1_PURIFY", "Capability purify", "mdi:air-purifier"),
    "capability_q_display": (
        "FEAT1_Q_DISPLAY",
        "Capability display control",
        "mdi:television-ambient-light",
    ),
    "capability_enable_8heat": (
        "FEAT1_ENABLE_8HEAT",
        "Capability enable 8C heat",
        "mdi:snowflake-thermometer",
    ),
    "capability_trans_102_64": (
        "FEAT1_TRANS_102_64",
        "Capability trans 102-64",
        "mdi:swap-horizontal",
    ),
}


def _diagnostic(
    device_class: str | None = None,
    icon: str | None = None,
    disabled_by_default: bool = False,
) -> cv.Schema:
    kwargs = {"entity_category": ENTITY_CATEGORY_DIAGNOSTIC}
    if device_class is not None:
        kwargs["device_class"] = device_class
    if icon is not None:
        kwargs["icon"] = icon
    schema = binary_sensor.binary_sensor_schema(**kwargs)
    if disabled_by_default:
        # The schema default only: `disabled_by_default: false` on the entity turns it round.
        schema = schema.extend(
            {cv.Optional(CONF_DISABLED_BY_DEFAULT, default=True): cv.boolean}
        )
    return schema


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
    # The per-bit faults are disabled by default in Home Assistant: the summary `problem`
    # sensor says that there is a fault, and these say which, for whoever goes looking. Home
    # Assistant reads the flag when it first registers an entity, so one it already knows
    # stays as it is.
    .extend(
        {
            cv.Optional(key): _diagnostic(
                DEVICE_CLASS_PROBLEM, disabled_by_default=True
            )
            for key in FAULT_BITS
        }
    )
    .extend(
        {
            cv.Optional(key): _diagnostic(icon=icon)
            for key, (_, _, icon) in CAPABILITY_BITS.items()
        }
    )
    .extend(HISENSE_AC_CLIENT_SCHEMA)
)


async def to_code(config: ConfigType) -> None:
    parent = await cg.get_variable(config[CONF_HISENSE_AC_ID])

    for key in (CONF_AUX_HEAT, CONF_BUS_LINK, CONF_PROBLEM):
        if conf := config.get(key):
            var = await binary_sensor.new_binary_sensor(conf)
            cg.add(getattr(parent, f"set_{key}_binary_sensor")(var))

    for key, (constant, _) in FAULT_BITS.items():
        if conf := config.get(key):
            var = await binary_sensor.new_binary_sensor(conf)
            bit = cg.RawExpression(f"{hisense_ac_ns}::{constant}")
            cg.add(parent.add_fault_binary_sensor(bit, var))

    for key, (constant, _, _) in CAPABILITY_BITS.items():
        if conf := config.get(key):
            var = await binary_sensor.new_binary_sensor(conf)
            bit = cg.RawExpression(f"{hisense_ac_ns}::{constant}")
            cg.add(parent.add_capability_binary_sensor(bit, var))
