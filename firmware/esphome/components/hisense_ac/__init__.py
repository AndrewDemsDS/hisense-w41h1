"""Hisense A/C over RS-485, wrapping this repo's hardware-validated driver.

The C++ under this directory is glue only. The protocol lives in firmware/src/rs485-driver
and is reused unchanged (symlinked in, so there is exactly one copy in the repo), the same
way the esp-matter build reuses it.

Deliberately NOT built on ESPHome's `uart:` component: the driver's HAL opens the port itself
and owns the DE (transmit-enable) timing, which is the part that took a multi-day debug to get
right on real hardware. Handing the port to ESPHome would mean re-validating that against a
mainboard. Pins therefore belong to this component, and there is no `uart:` block in the YAML.
"""

from pathlib import Path

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components.esp32 import add_idf_component
from esphome.const import CONF_ID, CONF_RX_PIN, CONF_TX_PIN

CODEOWNERS = ["@AndrewDemsDS"]
DEPENDENCIES = ["esp32"]

# The driver and its HAL are already proper ESP-IDF components under firmware/esp32-matter/,
# with the include dirs and the bus-task stack size the driver needs. Registering them as local
# IDF dependencies reuses them in place, so there is exactly one copy of every shared file in
# the repo and no sync step to forget. (Copying or symlinking them into this directory also
# works for the sources, but not for the headers: ESPHome drops -I flags on the ESP-IDF
# framework, forwarding only -D and -W, so the driver's <platform_stdlib.h> would not resolve.)
_COMPONENT_DIR = Path(__file__).resolve().parent  # firmware/esphome/components/hisense_ac
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

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(HisenseAC),
        cv.Required(CONF_TX_PIN): pins.internal_gpio_output_pin_number,
        cv.Required(CONF_RX_PIN): pins.internal_gpio_input_pin_number,
        cv.Required(CONF_DE_PIN): pins.internal_gpio_output_pin_number,
    }
).extend(cv.COMPONENT_SCHEMA)

# Every platform in this package hangs off the one hub instance.
HISENSE_AC_CLIENT_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_HISENSE_AC_ID): cv.use_id(HisenseAC),
    }
)


async def to_code(config):
    for name, path in _IDF_COMPONENTS.items():
        add_idf_component(name=name, path=str(path))

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # The HAL names its pins with the AmebaZ2 symbols the driver was written against
    # (PA_14 TX / PA_13 RX / PA_17 DE). PinNames.h guards each define with #ifndef, so the
    # YAML wins here while the esp-matter build keeps its per-target defaults.
    cg.add_build_flag(f"-DPA_14={config[CONF_TX_PIN]}")
    cg.add_build_flag(f"-DPA_13={config[CONF_RX_PIN]}")
    cg.add_build_flag(f"-DPA_17={config[CONF_DE_PIN]}")

    # xTaskCreate's stack depth is BYTES on ESP-IDF but WORDS on AmebaZ2, so the driver's
    # default of 1024 is 4 KB there and only 1 KB here, which overflowed and double-faulted.
    # The hisense_rs485 component already sets this for its own sources; it is repeated here
    # because the value is part of the driver's ABI expectations, not a private detail.
    cg.add_build_flag("-DHISENSE_BUS_TASK_STACK=4096")
