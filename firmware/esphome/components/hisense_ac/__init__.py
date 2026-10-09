"""Hisense A/C over RS-485 (the AEH-W41H1 module's indoor-unit bus).

ESPHome's uart component carries the bytes and hisense_bus.* schedules the transactions in
loop(). The component carries its own codec (hisense_protocol.*, hisense_map.h).

``de_pin`` drives the transceiver's DE line in software with the hardware-validated timing (5 ms
settle, wire time plus 25 ms drain). Leave it out only when the UART's ``flow_control_pin``
owns DE instead.
"""

from esphome import pins
import esphome.codegen as cg
from esphome.components import uart
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@AndrewDemsDS"]
DEPENDENCIES = ["uart"]

hisense_ac_ns = cg.esphome_ns.namespace("hisense_ac")
HisenseAC = hisense_ac_ns.class_("HisenseAC", cg.Component)
StatusListener = hisense_ac_ns.class_("StatusListener")

CONF_HISENSE_AC_ID = "hisense_ac_id"
CONF_DE_PIN = "de_pin"

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(HisenseAC),
            cv.Optional(CONF_DE_PIN): pins.gpio_output_pin_schema,
        }
    )
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)

# The A/C link is 9600 8N1, firmware-confirmed.
FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "hisense_ac",
    baud_rate=9600,
    require_tx=True,
    require_rx=True,
    data_bits=8,
    parity="NONE",
    stop_bits=1,
)

# Every platform in this package hangs off the one hub instance.
HISENSE_AC_CLIENT_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_HISENSE_AC_ID): cv.use_id(HisenseAC),
    }
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    if CONF_DE_PIN in config:
        cg.add(var.set_de_pin(await cg.gpio_pin_expression(config[CONF_DE_PIN])))
