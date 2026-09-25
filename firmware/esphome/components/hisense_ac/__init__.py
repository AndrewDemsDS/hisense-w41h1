"""Hisense A/C over RS-485 (the AEH-W41H1 module's indoor-unit bus).

Two transports:

- uart (``uart_id``, optional ``de_pin``): ESPHome's uart component carries the bytes and
  hisense_bus.* schedules the transactions in loop(). The component carries its own codec
  (hisense_protocol.*, hisense_map.h), held byte-for-byte equal to the shared driver in
  firmware/src/rs485-driver by firmware/test/test_esphome_codec_parity.cpp.
- legacy (``tx_pin`` / ``rx_pin`` / ``de_pin`` as pin numbers): the shared driver's own FreeRTOS
  bus task over its ESP-IDF HAL, as this component ran before the uart transport. Kept as a
  fallback while the uart path proves itself on hardware.

``de_pin`` drives the transceiver's DE line in software with the hardware-validated timing (5 ms
settle, wire time plus 25 ms drain). Leave it out only when the UART's ``flow_control_pin``
owns DE instead.
"""

from pathlib import Path

from esphome import pins
import esphome.codegen as cg
from esphome.components import uart
from esphome.components.esp32 import add_idf_component
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_RX_PIN, CONF_TX_PIN, CONF_UART_ID

CODEOWNERS = ["@AndrewDemsDS"]
DEPENDENCIES = ["esp32"]

# LEGACY only. The driver and its HAL are proper ESP-IDF components under firmware/esp32-matter/,
# registered as local IDF dependencies so they are reused in place. (ESPHome drops -I flags on
# the ESP-IDF framework, forwarding only -D and -W, so the driver's <platform_stdlib.h> would not
# resolve from a copy or a symlink.)
# firmware/esphome/components/hisense_ac
_COMPONENT_DIR = Path(__file__).resolve().parent
_FIRMWARE_ROOT = _COMPONENT_DIR.parents[2]  # firmware/
_IDF_COMPONENTS = {
    "hisense_hal": _FIRMWARE_ROOT / "esp32-matter" / "components" / "hisense_hal",
    "hisense_rs485": _FIRMWARE_ROOT / "esp32-matter" / "components" / "hisense_rs485",
}

hisense_ac_ns = cg.esphome_ns.namespace("hisense_ac")
HisenseAC = hisense_ac_ns.class_("HisenseAC", cg.Component)
StatusListener = hisense_ac_ns.class_("StatusListener")

CONF_HISENSE_AC_ID = "hisense_ac_id"
CONF_DE_PIN = "de_pin"

_UART_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(HisenseAC),
            cv.Optional(CONF_DE_PIN): pins.gpio_output_pin_schema,
        }
    )
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)

_LEGACY_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(HisenseAC),
        cv.Required(CONF_TX_PIN): pins.internal_gpio_output_pin_number,
        cv.Required(CONF_RX_PIN): pins.internal_gpio_input_pin_number,
        cv.Required(CONF_DE_PIN): pins.internal_gpio_output_pin_number,
    }
).extend(cv.COMPONENT_SCHEMA)


def _is_legacy(config):
    return CONF_TX_PIN in config or CONF_RX_PIN in config


def _validate(config):
    if _is_legacy(config):
        if CONF_UART_ID in config:
            raise cv.Invalid("use either uart_id or tx_pin/rx_pin, not both")
        return _LEGACY_SCHEMA(config)
    return _UART_SCHEMA(config)


CONFIG_SCHEMA = _validate


def _final_validate(config):
    if _is_legacy(config):
        return config
    # The A/C link is 9600 8N1, firmware-confirmed (UART_PINS.md).
    return uart.final_validate_device_schema(
        "hisense_ac",
        baud_rate=9600,
        require_tx=True,
        require_rx=True,
        data_bits=8,
        parity="NONE",
        stop_bits=1,
    )(config)


FINAL_VALIDATE_SCHEMA = _final_validate

# Every platform in this package hangs off the one hub instance.
HISENSE_AC_CLIENT_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_HISENSE_AC_ID): cv.use_id(HisenseAC),
    }
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if not _is_legacy(config):
        await uart.register_uart_device(var, config)
        if CONF_DE_PIN in config:
            cg.add(var.set_de_pin(await cg.gpio_pin_expression(config[CONF_DE_PIN])))
        return

    # LEGACY transport.
    cg.add_define("USE_HISENSE_AC_LEGACY_DRIVER")
    for name, path in _IDF_COMPONENTS.items():
        add_idf_component(name=name, path=str(path))
    # The HAL names its pins with the AmebaZ2 symbols the driver was written against
    # (PA_14 TX / PA_13 RX / PA_17 DE). PinNames.h guards each define with #ifndef, so the
    # YAML wins here while the esp-matter build keeps its per-target defaults.
    cg.add_build_flag(f"-DPA_14={config[CONF_TX_PIN]}")
    cg.add_build_flag(f"-DPA_13={config[CONF_RX_PIN]}")
    cg.add_build_flag(f"-DPA_17={config[CONF_DE_PIN]}")
    # xTaskCreate's stack depth is BYTES on ESP-IDF but WORDS on AmebaZ2, so the driver's
    # default of 1024 is 4 KB there and only 1 KB here, which overflowed and double-faulted.
    cg.add_build_flag("-DHISENSE_BUS_TASK_STACK=4096")
