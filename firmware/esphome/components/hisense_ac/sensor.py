import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_VOLTAGE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_AMPERE,
    UNIT_CELSIUS,
    UNIT_HERTZ,
    UNIT_VOLT,
    UNIT_WATT,
)

from . import CONF_HISENSE_AC_ID, HISENSE_AC_CLIENT_SCHEMA

DEPENDENCIES = ["hisense_ac"]

CONF_INDOOR_TEMPERATURE = "indoor_temperature"
CONF_OUTDOOR_TEMPERATURE = "outdoor_temperature"
CONF_COIL_TEMPERATURE = "coil_temperature"
CONF_COMPRESSOR_FREQUENCY = "compressor_frequency"
CONF_POWER = "power"
CONF_VOLTAGE = "voltage"
CONF_CURRENT = "current"
CONF_CHECKSUM_ERRORS = "checksum_errors"


def _temperature(icon: str | None = None):
    kwargs = {
        "unit_of_measurement": UNIT_CELSIUS,
        "accuracy_decimals": 0,
        "device_class": DEVICE_CLASS_TEMPERATURE,
        "state_class": STATE_CLASS_MEASUREMENT,
    }
    if icon is not None:
        kwargs["icon"] = icon
    return sensor.sensor_schema(**kwargs)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_INDOOR_TEMPERATURE): _temperature(),
        cv.Optional(CONF_OUTDOOR_TEMPERATURE): _temperature(),
        # Condenser coil. Reverses on a cool -> heat swap, which is how it was told apart
        # from the outdoor sensor.
        cv.Optional(CONF_COIL_TEMPERATURE): _temperature(),
        cv.Optional(CONF_COMPRESSOR_FREQUENCY): sensor.sensor_schema(
            unit_of_measurement=UNIT_HERTZ,
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:sine-wave",
        ),
        # Derived from the bus current proxy (P = 4.15 * raw^2), calibrated against a panel
        # meter. An estimate, not a revenue meter, but good enough for the Energy dashboard
        # once fed through ESPHome's `total_daily_energy`.
        cv.Optional(CONF_POWER): sensor.sensor_schema(
            unit_of_measurement=UNIT_WATT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_POWER,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_VOLTAGE): sensor.sensor_schema(
            unit_of_measurement=UNIT_VOLT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_VOLTAGE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_CURRENT): sensor.sensor_schema(
            unit_of_measurement=UNIT_AMPERE,
            accuracy_decimals=2,
            device_class=DEVICE_CLASS_CURRENT,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        # Bench instrument: a climbing count means framing trouble on the bus.
        cv.Optional(CONF_CHECKSUM_ERRORS): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:alert-circle-outline",
        ),
    }
).extend(HISENSE_AC_CLIENT_SCHEMA)

SENSORS = [
    CONF_INDOOR_TEMPERATURE,
    CONF_OUTDOOR_TEMPERATURE,
    CONF_COIL_TEMPERATURE,
    CONF_COMPRESSOR_FREQUENCY,
    CONF_POWER,
    CONF_VOLTAGE,
    CONF_CURRENT,
    CONF_CHECKSUM_ERRORS,
]


async def to_code(config):
    parent = await cg.get_variable(config[CONF_HISENSE_AC_ID])
    for key in SENSORS:
        if key not in config:
            continue
        var = await sensor.new_sensor(config[key])
        cg.add(getattr(parent, f"set_{key}_sensor")(var))
