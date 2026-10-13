"""Wi-Fi setup mode: the node's hotspot and captive portal on demand.

ESPHome opens its fallback hotspot only when the configured network has been out of reach for
``ap_timeout``. This component opens it while the network is still there: on the A/C remote's
"77" gesture (wire the hub's ``on_setup_request`` to ``wifi_setup_mode.toggle``) or from a button
(``wifi_setup_mode.start``). The panel shows "77" while the mode is on. It ends in a restart: when
a network entered in the portal has connected, on a second gesture, or after ``timeout``. A
network that was entered and did not connect is replaced by the one the node had, also when the
node restarts for another reason or loses power in between.

The decisions are in setup_mode_logic.h, which is tested on a host.
"""

import logging

from esphome import automation
import esphome.codegen as cg
from esphome.components.const import CONF_ENABLED
import esphome.config_validation as cv
from esphome.const import (
    CONF_AP,
    CONF_EAP,
    CONF_FAST_CONNECT,
    CONF_HIDDEN,
    CONF_ID,
    CONF_NETWORKS,
    CONF_PASSWORD,
    CONF_SSID,
    CONF_TIMEOUT,
    CONF_WIFI,
)
from esphome.core import CORE
import esphome.final_validate as fv

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@AndrewDemsDS"]
DEPENDENCIES = ["wifi", "captive_portal", "hisense_ac"]
DOMAIN = "wifi_setup_mode"

wifi_setup_mode_ns = cg.esphome_ns.namespace("wifi_setup_mode")
WiFiSetupMode = wifi_setup_mode_ns.class_("WiFiSetupMode", cg.Component)
ToggleAction = wifi_setup_mode_ns.class_("ToggleAction", automation.Action)
StartAction = wifi_setup_mode_ns.class_("StartAction", automation.Action)
StopAction = wifi_setup_mode_ns.class_("StopAction", automation.Action)
ActiveCondition = wifi_setup_mode_ns.class_("ActiveCondition", automation.Condition)

# The hub, declared by name and not imported from the hisense_ac package: a config that lists
# this component without hisense_ac then fails on DEPENDENCIES with a message that says so, not
# on an import error.
HisenseAC = cg.esphome_ns.namespace("hisense_ac").class_("HisenseAC", cg.Component)

CONF_HISENSE_AC_ID = "hisense_ac_id"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(WiFiSetupMode),
        cv.GenerateID(CONF_HISENSE_AC_ID): cv.use_id(HisenseAC),
        # How long the hotspot stays open before the node restarts onto the network it had. The
        # upper limit is what the 32 bit millisecond clock can compare (setup_mode_logic.h).
        cv.Optional(CONF_TIMEOUT, default="10min"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(
                min=cv.TimePeriod(seconds=30),
                max=cv.TimePeriod(hours=24),
            ),
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


def _on_demand_unavailable(wifi_conf):
    """Why this Wi-Fi configuration cannot open the hotspot on demand, or None when it can.

    The hotspot is forced by restarting Wi-Fi with the station pointed at a network that does
    not exist. That only holds when Wi-Fi scans before it connects.
    """
    networks = wifi_conf.get(CONF_NETWORKS, [])
    if not networks:
        return "no network is configured, so the hotspot is open whenever nothing is connected"
    if wifi_conf.get(CONF_FAST_CONNECT, {}).get(CONF_ENABLED):
        return "with fast_connect the node is back on its network before the hotspot is up"
    if any(network.get(CONF_HIDDEN) for network in networks):
        return (
            "a network marked hidden is connected to without a scan, so the node is back on it "
            "before the hotspot is up"
        )
    if any(CONF_EAP in network for network in networks):
        return "an enterprise login cannot be put back as a name and a password"
    return None


def _final_validate(config):
    wifi_conf = fv.full_config.get()[CONF_WIFI]
    if CONF_AP not in wifi_conf:
        raise cv.Invalid(
            "wifi_setup_mode opens the node's hotspot, and this configuration has none: add "
            "`ap:` under `wifi:`."
        )
    if (reason := _on_demand_unavailable(wifi_conf)) and wifi_conf.get(CONF_NETWORKS):
        _LOGGER.warning(
            "wifi_setup_mode cannot open the hotspot on demand with this Wi-Fi configuration: "
            "%s. The gesture and the button will log that and do nothing. The hotspot still "
            "opens by itself when the network has been out of reach.",
            reason,
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await cg.register_parented(var, config[CONF_HISENSE_AC_ID])
    cg.add(var.set_timeout(config[CONF_TIMEOUT]))

    wifi_conf = CORE.config[CONF_WIFI]
    networks = wifi_conf.get(CONF_NETWORKS, [])
    cg.add(var.set_single_network(len(networks) <= 1))
    if networks:
        # The network to go back to when the node has not been connected in this boot. The
        # strings are in the image already, for the Wi-Fi component.
        first = networks[0]
        cg.add(
            var.set_configured_network(first[CONF_SSID], first.get(CONF_PASSWORD, ""))
        )
    if reason := _on_demand_unavailable(wifi_conf):
        cg.add(var.set_on_demand_unavailable(reason))


_ID_SCHEMA = automation.maybe_simple_id(
    {
        cv.GenerateID(): cv.use_id(WiFiSetupMode),
    }
)


async def _parented_to_code(config, var_id, template_arg, args):
    var = cg.new_Pvariable(var_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


# The gesture: starts setup mode, and ends it when it is on.
automation.register_action(
    "wifi_setup_mode.toggle", ToggleAction, _ID_SCHEMA, synchronous=True
)(_parented_to_code)
# A button: starts setup mode, and does nothing when it is on.
automation.register_action(
    "wifi_setup_mode.start", StartAction, _ID_SCHEMA, synchronous=True
)(_parented_to_code)
automation.register_action(
    "wifi_setup_mode.stop", StopAction, _ID_SCHEMA, synchronous=True
)(_parented_to_code)
# True while setup mode is on ("77" on the panel).
automation.register_condition(
    "wifi_setup_mode.active", ActiveCondition, _ID_SCHEMA
)(_parented_to_code)
