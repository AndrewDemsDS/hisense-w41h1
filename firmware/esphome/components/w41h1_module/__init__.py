"""The stock AEH-W41H1 Wi-Fi module itself (Realtek RTL8710C, AmebaZ2) under LibreTiny.

Everything the module needs on top of a plain ``rtl87xx`` node, in one place:

* the flash layout: the board's slot and settings addresses are replaced by one of the
  ``layout-*.json`` files next to this one, the image is checked against it at build time and
  against the bootloader's partition table at boot (w41h1_slots.h), and on a mismatch the update
  server is switched off;
* the reboot workaround for LibreTiny issue 396;
* the serial port recovery for the A/C bus (w41h1_module.cpp, with its timing rule in
  w41h1_uart_policy.h);
* two diagnostic entities, ``serial_port_restarts`` and ``flash_layout``.

``layout`` has no default on purpose: an image built for another layout than the one in the
module's flash bricks the unit at its first update.
"""

from pathlib import Path

import esphome.codegen as cg
from esphome.components import sensor, text_sensor, uart
from esphome.components.esphome.ota import ESPHomeOTAComponent
from esphome.components.libretiny import get_libretiny_family
from esphome.components.libretiny.const import FAMILY_RTL8720C
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_UART_ID,
    ENTITY_CATEGORY_DIAGNOSTIC,
    PLATFORM_RTL87XX,
    STATE_CLASS_TOTAL_INCREASING,
)
from esphome.core import CORE
from esphome.helpers import mkdir_p, write_file_if_changed

CODEOWNERS = ["@AndrewDemsDS"]
DEPENDENCIES = ["hisense_ac", "uart", "ota"]
AUTO_LOAD = ["sensor", "text_sensor"]

w41h1_module_ns = cg.esphome_ns.namespace("w41h1_module")
W41H1Module = w41h1_module_ns.class_("W41H1Module", cg.PollingComponent)

# The hub, declared by name and not imported from the hisense_ac package: a config that lists
# this component without hisense_ac then fails on DEPENDENCIES with a message that says so, not
# on an import error.
HisenseAC = cg.esphome_ns.namespace("hisense_ac").class_("HisenseAC", cg.Component)

CONF_LAYOUT = "layout"
CONF_OTA_ID = "ota_id"
CONF_HISENSE_AC_ID = "hisense_ac_id"
CONF_SERIAL_PORT_RESTARTS = "serial_port_restarts"
CONF_FLASH_LAYOUT = "flash_layout"

# One layout-<name>.json each, next to this file. firmware/docs/15-esphome-path.md, "Which flash
# layout a unit has", says how to tell which one a module has.
#   factory  a unit converted over the air from the vendor firmware: the factory partition table
#   sdk      a unit first written with this repo's clip image: the Realtek SDK's partition table
#   native   a module written from scratch with LibreTiny's own partition table and bootloader
LAYOUTS = ("factory", "sdk", "native")


def _only_on_the_stock_module(config):
    if CORE.target_platform != PLATFORM_RTL87XX:
        raise cv.Invalid(
            "w41h1_module is only for the stock AEH-W41H1 module, which is built with the "
            f"rtl87xx platform. This configuration is for {CORE.target_platform}: leave the "
            "component out."
        )
    # The layout check calls the AmebaZ2 SDK's slot helpers, which the other Realtek family
    # (RTL8710B, AmebaZ) does not have.
    if (family := get_libretiny_family()) != FAMILY_RTL8720C:
        raise cv.Invalid(
            "w41h1_module is only for the stock AEH-W41H1 module, an RTL8710C (LibreTiny family "
            f"{FAMILY_RTL8720C}). This board's family is {family}."
        )
    return config


def _listed_once(config):
    # Without this a second entry is refused with ESPHome's "expected a dictionary", which does
    # not say what is wrong.
    if isinstance(config, list):
        raise cv.Invalid(
            "w41h1_module can be listed only once: a module has one flash layout, one A/C bus "
            "and one update server."
        )
    return config


CONFIG_SCHEMA = cv.All(
    _listed_once,
    _only_on_the_stock_module,
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(W41H1Module),
            cv.Required(CONF_LAYOUT): cv.one_of(*LAYOUTS, lower=True),
            cv.GenerateID(CONF_HISENSE_AC_ID): cv.use_id(HisenseAC),
            # The port the hub is on: the one that is closed and reopened when the link is lost.
            cv.GenerateID(CONF_UART_ID): cv.use_id(uart.LibreTinyUARTComponent),
            # ESPHome's own update server, the one switched off on a layout mismatch.
            cv.GenerateID(CONF_OTA_ID): cv.use_id(ESPHomeOTAComponent),
            # How often the serial port recovery has fired since boot. On a healthy bus it stays
            # at 0. It also counts while the unit does not answer at all: off at the mains with
            # the node powered some other way, or a wiring or transceiver fault, which no
            # restart cures.
            cv.Optional(CONF_SERIAL_PORT_RESTARTS): sensor.sensor_schema(
                icon="mdi:restart-alert",
                accuracy_decimals=0,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            # The running slot, the address the bootloader gives for the next update, and whether
            # that agrees with the layout this image was built for. "MISMATCH" means: do not
            # update this unit.
            cv.Optional(CONF_FLASH_LAYOUT): text_sensor.text_sensor_schema(
                icon="mdi:memory",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
        }
    ).extend(cv.polling_component_schema("60s")),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await cg.register_parented(var, config[CONF_HISENSE_AC_ID])
    cg.add(var.set_uart(await cg.get_variable(config[CONF_UART_ID])))
    cg.add(var.set_ota(await cg.get_variable(config[CONF_OTA_ID])))
    if restarts_config := config.get(CONF_SERIAL_PORT_RESTARTS):
        cg.add(
            var.set_serial_port_restarts_sensor(
                await sensor.new_sensor(restarts_config)
            )
        )
    if layout_config := config.get(CONF_FLASH_LAYOUT):
        cg.add(
            var.set_flash_layout_text_sensor(
                await text_sensor.new_text_sensor(layout_config)
            )
        )

    # The flash layout. LibreTiny merges `custom_board` over the board definition, which replaces
    # the board's slot and settings addresses. PlatformIO reads the path relative to the build
    # directory, so the file is copied there from this directory. That also holds when the
    # component comes from a git source, where nothing outside this directory exists.
    layout = config[CONF_LAYOUT]
    board_file = Path("w41h1_module") / f"layout-{layout}.json"
    target = Path(CORE.relative_build_path(board_file))
    mkdir_p(target.parent)
    write_file_if_changed(
        target, (Path(__file__).parent / board_file.name).read_text(encoding="utf-8")
    )
    cg.add_platformio_option("custom_board", board_file.as_posix())
    # w41h1_slots.h fails the build unless the image's offsets are those of this layout.
    cg.add_build_flag(f"-DW41H1_LAYOUT_{layout}=1")

    # LibreTiny 1.13.0 reboots this family with a CPU-only reset, which can leave the chip dark
    # until power has been off for half a minute: after a restart, and after every OTA
    # (libretiny-eu/libretiny issue 396, fix proposed in pull request 397, not released). These
    # two flags point lt_reboot() at the SDK's watchdog system reset instead, the workaround
    # confirmed in that issue. They stay harmless once the fix ships. With them in the image, two
    # updates and one mains power cycle came back without a hang (one module, 2026-10-10).
    cg.add_build_flag("-Wl,--wrap=lt_reboot")
    cg.add_build_flag("-Wl,--defsym=__wrap_lt_reboot=sys_reset")

    # LibreTiny 1.13.0 moves the bootloader's bit flag at flash 0x1004 on every update, also on
    # a bootloader that picks the slot by serial number, as this module's does. Every 33rd update
    # it erases the 4 KB sector at 0x1000 and writes back 128 bytes: on this module that keeps all
    # of its data (8 bytes, the same in every dump on record), but a power cut in between loses
    # the flash calibration the boot code reads. w41h1_module.cpp leaves the flag alone unless
    # the bootloader uses it.
    cg.add_build_flag("-Wl,--wrap=lt_ota_dual_switch_flag")
