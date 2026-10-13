"""Hisense A/C over RS-485 (the AEH-W41H1 module's indoor-unit bus).

ESPHome's uart component carries the bytes and hisense_bus.* schedules the transactions in
loop(). The component carries its own codec (hisense_protocol.*, hisense_map.h).

``de_pin`` drives the transceiver's DE line in software with the hardware-validated timing (5 ms
settle, wire time plus 25 ms drain). Leave it out only when the UART's ``flow_control_pin``
owns DE instead.
"""

import logging

from esphome import automation, pins
import esphome.codegen as cg
from esphome.components import uart
import esphome.config_validation as cv
from esphome.const import (
    CONF_FLOW_CONTROL_PIN,
    CONF_ID,
    CONF_INPUT,
    CONF_MODE,
    CONF_OUTPUT,
    CONF_STATE,
    CONF_UART_ID,
)
import esphome.final_validate as fv

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@AndrewDemsDS"]
DEPENDENCIES = ["uart"]
DOMAIN = "hisense_ac"

hisense_ac_ns = cg.esphome_ns.namespace("hisense_ac")
HisenseAC = hisense_ac_ns.class_("HisenseAC", cg.Component)
StatusListener = hisense_ac_ns.class_("StatusListener")
SetSetupIndicatorAction = hisense_ac_ns.class_(
    "SetSetupIndicatorAction", automation.Action
)

CONF_HISENSE_AC_ID = "hisense_ac_id"
CONF_DE_PIN = "de_pin"
CONF_ON_SETUP_REQUEST = "on_setup_request"


def _listed_once(config):
    # Without this a second entry is refused with ESPHome's "expected a dictionary", which does
    # not say what is wrong.
    if isinstance(config, list):
        raise cv.Invalid(
            "hisense_ac can be listed only once: the component drives one A/C bus. A board "
            "wired to two indoor units is not supported."
        )
    return config


def _de_pin_is_an_output(pin):
    # gpio_output_pin_schema only makes output the default mode: `mode: INPUT` gets through it,
    # and the hub would then write to an input and never transmit.
    mode = pin.get(CONF_MODE, {})
    if mode.get(CONF_INPUT) or not mode.get(CONF_OUTPUT):
        raise cv.Invalid(
            "de_pin drives the transceiver's DE line, so its mode has to be an output "
            "(leave `mode:` out, or set `output: true` and no `input`)."
        )
    return pin


CONFIG_SCHEMA = cv.All(
    _listed_once,
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(HisenseAC),
            cv.Optional(CONF_DE_PIN): cv.All(
                pins.gpio_output_pin_schema, _de_pin_is_an_output
            ),
            # The remote's "77" gesture (horizontal airflow six times). `x` is the request
            # bits of the unit's 0x1E reply.
            cv.Optional(CONF_ON_SETUP_REQUEST): automation.validate_automation({}),
        }
    )
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA),
)


def _one_owner_for_de(config):
    """DE has exactly one driver: the hub's de_pin, the UART's flow_control_pin, or neither."""

    def check(uart_config):
        uart_drives_de = CONF_FLOW_CONTROL_PIN in uart_config
        if CONF_DE_PIN in config and uart_drives_de:
            raise cv.Invalid(
                "The transceiver's DE line is driven twice: by hisense_ac (de_pin) and by the "
                "uart (flow_control_pin). Keep one. de_pin has the timing that was validated "
                "on hardware (5 ms settle, 25 ms hold)."
            )
        if CONF_DE_PIN not in config and not uart_drives_de:
            _LOGGER.warning(
                "hisense_ac has no de_pin and its uart has no flow_control_pin: nothing "
                "drives the transceiver's DE line. That is right only for a transceiver that "
                "switches direction by itself. With any other the node never transmits and "
                "the bus link stays down."
            )
        return uart_config

    cv.Schema(
        {cv.Required(CONF_UART_ID): fv.id_declaration_match_schema(check)},
        extra=cv.ALLOW_EXTRA,
    )(config)
    return config


# The A/C link is 9600 8N1, firmware-confirmed.
_UART_SETTINGS = uart.final_validate_device_schema(
    "hisense_ac",
    baud_rate=9600,
    require_tx=True,
    require_rx=True,
    data_bits=8,
    parity="NONE",
    stop_bits=1,
)


def _final_validate(config):
    # Each check looks the uart up through uart_id, so each gets the config as it was written
    # (a schema returns the uart's own config in place of the id).
    _UART_SETTINGS(config)
    _one_owner_for_de(config)
    return config


FINAL_VALIDATE_SCHEMA = _final_validate

# Every platform in this package hangs off the one hub instance.
HISENSE_AC_CLIENT_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_HISENSE_AC_ID): cv.use_id(HisenseAC),
    }
)


_CALLBACK_AUTOMATIONS = (
    automation.CallbackAutomation(
        CONF_ON_SETUP_REQUEST,
        "add_on_setup_request_callback",
        [(cg.uint8, "x")],
    ),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    if CONF_DE_PIN in config:
        cg.add(var.set_de_pin(await cg.gpio_pin_expression(config[CONF_DE_PIN])))
    await automation.build_callback_automations(var, config, _CALLBACK_AUTOMATIONS)


_SET_SETUP_INDICATOR_SCHEMA = cv.maybe_simple_value(
    {
        cv.GenerateID(): cv.use_id(HisenseAC),
        cv.Required(CONF_STATE): cv.templatable(cv.boolean),
    },
    key=CONF_STATE,
)


def _setup_indicator_schema(value):
    # maybe_simple_value takes a mapping without the key as the value itself, so `{id: ac_bus}`
    # was refused with "cannot convert {'id': 'ac_bus'} to a boolean".
    if isinstance(value, dict) and CONF_STATE not in value:
        raise cv.Invalid(
            "'state' is a required option for hisense_ac.set_setup_indicator: true shows "
            '"77" on the panel, false takes it off.'
        )
    return _SET_SETUP_INDICATOR_SCHEMA(value)


@automation.register_action(
    "hisense_ac.set_setup_indicator",
    SetSetupIndicatorAction,
    _setup_indicator_schema,
    synchronous=True,
)
async def hisense_ac_set_setup_indicator_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_state(await cg.templatable(config[CONF_STATE], args, cg.bool_)))
    return var
