#!/usr/bin/env python3
"""The configuration schema of hisense_ac, w41h1_module and the packages, through `esphome config`.

Three things are checked, none of which needs a toolchain, hardware or the network:

1. Valid configurations, written here as a matrix (every optional entity on and off, the hub with
   no platform, each platform alone, the climate options at their extremes, two hubs on two
   UARTs, every flash layout, the status LED and the status pixel alone and together). Each has
   to validate with no warning.
2. Invalid configurations, one file each under firmware/esphome/tests/invalid/. Each has to be
   rejected, with the message its `# expect:` line gives (a regular expression). A file that
   ESPHome accepts although it should not carries `# accepted:` and the reason. It is run all the
   same and has to validate, so the day it is rejected the marker has to go.
3. The board files: every one of them gives the same entities, with the same names and ids.

    python3 firmware/test/test_esphome_config_matrix.py            the quick set, a few seconds
    python3 firmware/test/test_esphome_config_matrix.py --full     adds one run per single entity
    python3 firmware/test/test_esphome_config_matrix.py --show hub-no-uart    print what esphome says
    python3 firmware/test/test_esphome_config_matrix.py --codegen  the generated C++ as well (below)

--codegen is not part of the default run: `esphome compile --only-generate` writes main.cpp without
a toolchain, but it fetches the libraries a configuration names (noise-c for the API encryption)
from the network. It checks that an entity left out of the YAML leaves nothing in main.cpp and no
USE_ define behind, and that every board file registers the same entities with the same object-id
hashes.

ESPHOME names the esphome command (default: the one on PATH). Without one the test is skipped
and says so, unless --require is given. Everything is run on copies in a temporary directory with
the placeholder secrets, so nothing is written into the checkout.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
ESPHOME_DIR = HERE.parent / "esphome"
INVALID_DIR = ESPHOME_DIR / "tests" / "invalid"

ANSI = re.compile(r"\x1b\[[0-9;]*m")

# ---- Building blocks of the generated configurations ---------------------------------------------
# Generated files live in the sandbox beside the board files, components/ and packages/.

PLATFORMS = {
    # name: (platform block, tx, rx, de, a spare output pin)
    "esp32c3": (
        "esp32:\n  board: esp32-c3-devkitm-1\n  framework:\n    type: esp-idf\n",
        "GPIO5",
        "GPIO6",
        "GPIO10",
        "GPIO7",
    ),
    "esp32": (
        "esp32:\n  board: esp32dev\n  framework:\n    type: esp-idf\n",
        "GPIO17",
        "GPIO16",
        "GPIO4",
        "GPIO5",
    ),
    "esp8266": ("esp8266:\n  board: d1_mini\n", "GPIO15", "GPIO13", "GPIO12", "GPIO14"),
    "rp2040": ("rp2:\n  board: rpipicow\n", "GPIO0", "GPIO1", "GPIO2", "GPIO3"),
    "rtl87xx": ("rtl87xx:\n  board: cr3l\n", "PA14", "PA13", "PA17", "PA18"),
}

SWITCHES = {
    "eco": "Eco",
    "turbo": "Turbo",
    "quiet": "Quiet",
    "display": "Panel display",
    "beeper": "Beeper",
}
SENSORS = {
    "indoor_temperature": "Indoor temperature",
    "outdoor_temperature": "Outdoor temperature",
    "coil_temperature": "Coil temperature",
    "compressor_frequency": "Compressor frequency",
    "power": "Power",
    "voltage": "Voltage",
    "current": "Current",
    "checksum_errors": "Bus checksum errors",
    "reply_timeouts": "Bus reply timeouts",
    "unanswered_commands": "Unanswered commands",
    "command_retries": "Command retries",
    "failed_commands": "Failed commands",
    "link_losses": "Bus link losses",
}
FAULTS = [
    "fault_indoor_temp",
    "fault_indoor_coil_temp",
    "fault_indoor_humidity",
    "fault_water_full",
    "fault_indoor_fan_motor",
    "fault_grille",
    "fault_indoor_vzero",
    "fault_indoor_comms",
    "fault_indoor_display",
    "fault_indoor_keys",
    "fault_indoor_wifi",
    "fault_indoor_ele",
    "fault_indoor_eeprom",
    "fault_outdoor_eeprom",
    "fault_outdoor_coil_temp",
    "fault_outdoor_gas_temp",
    "fault_outdoor_temp",
    "fault_over_temp",
]
CAPABILITIES = [
    "capability_cool_heat",
    "capability_ai",
    "capability_infinite_fan",
    "capability_power_save",
    "capability_fan_mute",
    "capability_swing_dir_8",
    "capability_swing_follow",
    "capability_humidity",
    "capability_heat_8c",
    "capability_purify",
    "capability_q_display",
    "capability_enable_8heat",
    "capability_trans_102_64",
]
BINARY_SENSORS = {
    "aux_heat": "Aux heat relay",
    "bus_link": "AC bus link",
    "problem": "Fault",
    **{key: key.replace("_", " ").capitalize() for key in FAULTS + CAPABILITIES},
}
CLIMATE_FLAGS = [
    "supports_heat",
    "supports_horizontal_swing",
    "supports_eco",
    "supports_quiet",
    "supports_turbo",
    "supports_sleep",
]


def head(platform: str, name: str, components: str = "hisense_ac") -> str:
    block = PLATFORMS[platform][0]
    return (
        f"esphome:\n  name: {name}\n\n{block}\nlogger:\n\n"
        "external_components:\n  - source:\n      type: local\n      path: components\n"
        f"    components: [{components}]\n"
    )


def uart(platform: str, uart_id: str = "uart_bus") -> str:
    _, tx, rx, _, _ = PLATFORMS[platform]
    return f"  - id: {uart_id}\n    tx_pin: {tx}\n    rx_pin: {rx}\n    baud_rate: 9600\n"


def hub(platform: str, hub_id: str = "ac_bus", uart_id: str = "uart_bus", de: str | None = "") -> str:
    out = f"  id: {hub_id}\n  uart_id: {uart_id}\n"
    if de is not None:
        out += f"  de_pin: {de or PLATFORMS[platform][3]}\n"
    return out


def entities(domain: str, keys: dict[str, str] | list[str], hub_id: str = "ac_bus", prefix: str = "") -> str:
    if isinstance(keys, list):
        keys = {key: BINARY_SENSORS.get(key) or SENSORS.get(key) or SWITCHES.get(key) or key for key in keys}
    out = f"{domain}:\n  - platform: hisense_ac\n    hisense_ac_id: {hub_id}\n"
    for key, name in keys.items():
        out += f"    {key}:\n      name: {prefix}{name}\n"
    return out


def climate(hub_id: str = "ac_bus", name: str = "AC", **options) -> str:
    out = f"climate:\n  - platform: hisense_ac\n    hisense_ac_id: {hub_id}\n    name: {name}\n"
    for key, value in options.items():
        out += f"    {key}: {str(value).lower() if isinstance(value, bool) else value}\n"
    return out


def base(platform: str, name: str, de: str | None = "") -> str:
    return head(platform, name) + "\nuart:\n" + uart(platform) + "\nhisense_ac:\n" + hub(platform, de=de) + "\n"


def everything(platform: str, name: str) -> str:
    return (
        base(platform, name)
        + climate(**{flag: True for flag in CLIMATE_FLAGS}, min_temperature=16, max_temperature=32)
        + entities("switch", SWITCHES)
        + entities("select", {"sleep_profile": "Sleep profile"})
        + entities("sensor", SENSORS)
        + entities("binary_sensor", BINARY_SENSORS)
        + entities("text_sensor", {"link_token": "AC device type"})
    )


NETWORK = (
    "wifi:\n  ssid: example-ssid\n  password: example-password\n\nota:\n  - platform: esphome\n    id: ota_server\n\n"
)


def module(layout: str, extras: bool) -> str:
    out = (
        head("rtl87xx", f"module-{layout}", "hisense_ac, w41h1_module")
        + "\n"
        + NETWORK
        + "uart:\n"
        + uart("rtl87xx")
        + "\nhisense_ac:\n"
        + hub("rtl87xx")
        + f"\nw41h1_module:\n  layout: {layout}\n"
    )
    if extras:
        out += (
            "  hisense_ac_id: ac_bus\n  uart_id: uart_bus\n  ota_id: ota_server\n  update_interval: 30s\n"
            "  serial_port_restarts:\n    name: Serial port restarts\n  flash_layout:\n    name: Flash layout\n"
        )
    return out


def packaged(platform: str, name: str, packages: list[str], substitutions: dict[str, str], tail: str = "") -> str:
    """A node built from the packages, as the board files are."""
    _, tx, rx, de, _ = PLATFORMS[platform]
    subs = {"name": name, "friendly_name": "Matrix node", "tx_pin": tx, "rx_pin": rx, "de_pin": de, **substitutions}
    out = "substitutions:\n" + "".join(f'  {key}: "{value}"\n' for key, value in subs.items())
    out += "\npackages:\n" + "".join(f"  {p.replace('-', '_')}: !include packages/{p}.yaml\n" for p in packages)
    return out + "\n" + PLATFORMS[platform][0] + tail


@dataclass
class Case:
    name: str
    text: str
    full_only: bool = False


def valid_cases() -> list[Case]:
    cases: list[Case] = []

    def add(name: str, text: str, full_only: bool = False) -> None:
        cases.append(Case(name, text, full_only))

    c3 = "esp32c3"
    # The hub by itself, with and without a DE pin, and with every form of its one action.
    add("hub-only", base(c3, "hub-only"))
    # No de_pin, with the UART peripheral driving DE (flow_control_pin). Neither of the two is
    # tests/invalid/hub-no-de-pin-no-flow-control.yaml.
    add(
        "hub-de-on-flow-control-pin",
        head(c3, "hub-flow") + "\nuart:\n" + uart(c3) + "    flow_control_pin: GPIO10\n"
        "\nhisense_ac:\n" + hub(c3, de=None),
    )
    add(
        "hub-inverted-de-pin",
        head(c3, "hub-inv") + "\nuart:\n" + uart(c3) + "\nhisense_ac:\n  id: ac_bus\n  uart_id: uart_bus\n"
        "  de_pin:\n    number: GPIO10\n    inverted: true\n",
    )
    add(
        "hub-setup-request-actions",
        base(c3, "hub-actions").rstrip("\n") + "\n  on_setup_request:\n    - hisense_ac.set_setup_indicator: true\n"
        "    - hisense_ac.set_setup_indicator:\n        id: ac_bus\n        state: !lambda return (x & 0x20) == 0;\n"
        "    - delay: 30s\n    - hisense_ac.set_setup_indicator: false\n",
    )
    # No uart_id and no hisense_ac_id anywhere: with one of each ESPHome finds them.
    add(
        "ids-left-to-esphome",
        head(c3, "auto-ids")
        + "\nuart:\n  tx_pin: GPIO5\n  rx_pin: GPIO6\n  baud_rate: 9600\n\nhisense_ac:\n  de_pin: GPIO10\n\n"
        "climate:\n  - platform: hisense_ac\n    name: AC\n\n"
        "switch:\n  - platform: hisense_ac\n    eco:\n      name: Eco\n",
    )
    # Every optional entity on, on every platform the board files use.
    for platform in PLATFORMS:
        add(f"everything-{platform}", everything(platform, f"all-{platform}"), full_only=platform != c3)
    # Each platform alone on the hub, with all of its entities, and with none of them.
    alone = {
        "climate": climate(),
        "switch": entities("switch", SWITCHES),
        "select": entities("select", {"sleep_profile": "Sleep profile"}),
        "sensor": entities("sensor", SENSORS),
        "binary_sensor": entities("binary_sensor", BINARY_SENSORS),
        "text_sensor": entities("text_sensor", {"link_token": "AC device type"}),
    }
    for domain, text in alone.items():
        add(f"only-{domain}", base(c3, f"only-{domain.replace('_', '-')}") + text)
        if domain != "climate":
            add(f"empty-{domain}", base(c3, f"empty-{domain.replace('_', '-')}") + entities(domain, {}))
    # Everything except the climate entity: a node of sensors and switches.
    add(
        "no-climate",
        base(c3, "no-climate") + "".join(text for domain, text in alone.items() if domain != "climate"),
    )
    # The climate options at their extremes.
    add("climate-all-off", base(c3, "climate-off") + climate(**{flag: False for flag in CLIMATE_FLAGS}))
    add("climate-all-on", base(c3, "climate-on") + climate(**{flag: True for flag in CLIMATE_FLAGS}))
    add("climate-full-range", base(c3, "climate-range") + climate(min_temperature=16, max_temperature=32))
    add("climate-one-degree", base(c3, "climate-point") + climate(min_temperature=24, max_temperature=24))
    add("climate-fahrenheit", base(c3, "climate-f") + climate(min_temperature="61°F", max_temperature="90°F"))
    for flag in CLIMATE_FLAGS:
        others = {other: other != flag for other in CLIMATE_FLAGS}
        add(f"climate-without-{flag}", base(c3, "climate-flag") + climate(**others), full_only=True)
        add(
            f"climate-only-{flag}",
            base(c3, "climate-flag") + climate(**{o: not v for o, v in others.items()}),
            full_only=True,
        )
    # The stock module: every layout, with the least and with every option.
    for layout in ("factory", "sdk", "native"):
        add(f"module-{layout}", module(layout, extras=True), full_only=layout != "factory")
        add(f"module-{layout}-minimal", module(layout, extras=False), full_only=layout != "sdk")
    add("module-layout-uppercase", module("factory", extras=False).replace("layout: factory", "layout: FACTORY"))
    # The packages, as a board file takes them.
    common = ["node", "ota", "hisense-ac"]
    add("packages-plain", packaged(c3, "pkg-plain", common, {}))
    add(
        "packages-status-led",
        packaged(c3, "pkg-led", common + ["status-led"], {"status_led_pin": "GPIO7"}),
    )
    add(
        "packages-status-led-inverted",
        packaged(
            c3, "pkg-led-inv", common + ["status-led"], {"status_led_pin": "GPIO7", "status_led_inverted": "true"}
        ),
    )
    add(
        "packages-status-pixel",
        packaged(c3, "pkg-pixel", common + ["status-pixel"], {"status_pixel_pin": "GPIO7"}),
    )
    add(
        "packages-status-pixel-rgb-order",
        packaged(
            c3, "pkg-pixel-rgb", common + ["status-pixel"], {"status_pixel_pin": "GPIO7", "status_pixel_order": "RGB"}
        ),
    )
    # A board with both kinds of LED, on two pins.
    add(
        "packages-status-led-and-pixel",
        packaged(
            c3,
            "pkg-both",
            common + ["status-led", "status-pixel"],
            {"status_led_pin": "GPIO7", "status_pixel_pin": "GPIO4"},
        ),
    )
    add(
        "packages-status-led-esp8266",
        packaged(
            "esp8266",
            "pkg-led-8266",
            common + ["status-led"],
            {"status_led_pin": "GPIO2", "status_led_inverted": "true"},
        ),
        full_only=True,
    )
    add(
        "packages-status-pixel-esp32",
        packaged("esp32", "pkg-pixel-esp32", common + ["status-pixel"], {"status_pixel_pin": "GPIO13"}),
        full_only=True,
    )
    add(
        "packages-setup-timeout-and-power-save",
        packaged(c3, "pkg-subs", common, {"wifi_setup_timeout": "90s", "wifi_power_save": "light"}),
    )
    for layout in ("factory", "sdk", "native"):
        add(
            f"packages-amebaz2-{layout}",
            packaged("rtl87xx", f"pkg-{layout}", ["node", "hisense-ac", "amebaz2"], {"layout": layout}).replace(
                PLATFORMS["rtl87xx"][0], ""
            ),
            full_only=layout != "native",
        )
    # One run per single entity: a platform holding just that one.
    for domain, keys in (("switch", SWITCHES), ("sensor", SENSORS), ("binary_sensor", BINARY_SENSORS)):
        for key in keys:
            add(f"single-{domain}-{key}", base(c3, "single") + entities(domain, [key]), full_only=True)
    return cases


# ---- Running esphome -----------------------------------------------------------------------------
@dataclass
class Result:
    name: str
    path: Path
    returncode: int
    output: str
    seconds: float
    problems: list[str] = field(default_factory=list)


class Runner:
    def __init__(self, esphome: str, sandbox: Path, jobs: int):
        self.esphome = esphome
        self.sandbox = sandbox
        self.jobs = jobs
        # esphome can call git (for a git source). None of these configurations has one, but a
        # test run from a commit hook inherits GIT_DIR and friends, and they must not leak.
        self.env = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
        self.env["PYTHONDONTWRITEBYTECODE"] = "1"  # components/ is a link into the checkout
        self.env["NO_COLOR"] = "1"

    def run(
        self, name: str, path: Path, command: tuple[str, ...] = ("config",), before: tuple[str, ...] = ()
    ) -> Result:
        start = time.monotonic()
        proc = subprocess.run(
            [self.esphome, *before, *command, path.name],
            cwd=path.parent,
            env=self.env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            timeout=600,
            check=False,
        )
        return Result(name, path, proc.returncode, ANSI.sub("", proc.stdout), time.monotonic() - start)

    def run_all(self, items: list[tuple[str, Path]]) -> list[Result]:
        with ThreadPoolExecutor(max_workers=self.jobs) as pool:
            return list(pool.map(lambda item: self.run(*item), items))


def make_sandbox(root: Path) -> Path:
    """<root>/esphome with the board files, links to components/ and packages/, and the placeholder secrets."""
    sandbox = root / "esphome"
    sandbox.mkdir()
    (sandbox / "components").symlink_to(ESPHOME_DIR / "components", target_is_directory=True)
    (sandbox / "packages").symlink_to(ESPHOME_DIR / "packages", target_is_directory=True)
    for board in sorted(ESPHOME_DIR.glob("w41h1*.yaml")):
        shutil.copy(board, sandbox / board.name)
    secrets = ESPHOME_DIR / "secrets.yaml.example"
    for directory in (sandbox, sandbox / "tests" / "invalid"):
        directory.mkdir(parents=True, exist_ok=True)
        shutil.copy(secrets, directory / "secrets.yaml")
    for config in sorted(INVALID_DIR.glob("*.yaml")):
        shutil.copy(config, sandbox / "tests" / "invalid" / config.name)
    return sandbox


def errors_of(output: str) -> str:
    """What esphome printed, without the dump of the configuration that follows a failure."""
    lines = output.splitlines()
    kept = [line for line in lines if not line.startswith(("INFO ESPHome", "INFO Reading configuration"))]
    return "\n".join(kept)


def warnings_of(output: str) -> list[str]:
    return [line for line in output.splitlines() if line.startswith("WARNING") or "DeprecationWarning" in line]


# ---- The three checks ----------------------------------------------------------------------------
def check_valid(runner: Runner, full: bool) -> tuple[int, list[str]]:
    cases = [case for case in valid_cases() if full or not case.full_only]
    items = []
    for case in cases:
        path = runner.sandbox / f"gen-{case.name}.yaml"
        path.write_text(case.text, encoding="utf-8")
        items.append((case.name, path))
    failures = []
    for result in runner.run_all(items):
        if result.returncode != 0:
            failures.append(f"valid/{result.name}: rejected\n{indent(errors_of(result.output))}")
        elif "Configuration is valid" not in result.output:
            failures.append(f"valid/{result.name}: no verdict\n{indent(result.output[-2000:])}")
        elif warnings_of(result.output):
            failures.append(
                f"valid/{result.name}: accepted with a warning\n{indent(chr(10).join(warnings_of(result.output)))}"
            )
    return len(cases), failures


def read_header(path: Path) -> dict[str, list[str]]:
    header: dict[str, list[str]] = {"expect": [], "accepted": [], "warns": []}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.startswith("#"):
            break
        match = re.match(r"#\s*(expect|accepted|warns):\s*(.+)", line)
        if match:
            header[match.group(1)].append(match.group(2).strip())
    return header


def check_invalid(runner: Runner) -> tuple[int, int, list[str]]:
    configs = sorted((runner.sandbox / "tests" / "invalid").glob("*.yaml"))
    configs = [config for config in configs if config.name != "secrets.yaml"]
    failures = []
    known = 0
    if not configs:
        failures.append(f"no configuration found under {INVALID_DIR}")
    for result in runner.run_all([(config.stem, config) for config in configs]):
        header = read_header(result.path)
        text = errors_of(result.output)
        label = f"invalid/{result.name}"
        if header["accepted"]:
            # Known: ESPHome takes it although it should not. Kept visible, and pinned.
            known += 1
            print(f"  known {label}: accepted. {header['accepted'][0]}")
            if result.returncode != 0:
                failures.append(
                    f"{label}: now rejected, remove its `# accepted:` line and add `# expect:`\n{indent(text)}"
                )
            for pattern in header["warns"]:
                if not re.search(pattern, text):
                    failures.append(f"{label}: the warning /{pattern}/ is gone\n{indent(text)}")
            continue
        if not header["expect"]:
            failures.append(f"{label}: no `# expect:` line")
            continue
        if result.returncode == 0:
            failures.append(f"{label}: accepted, it has to be rejected")
            continue
        for pattern in header["expect"]:
            if not re.search(pattern, text):
                failures.append(f"{label}: rejected, but not with /{pattern}/\n{indent(text[:3000])}")
    return len(configs), known, failures


ENTITY_DOMAINS = ("binary_sensor", "button", "climate", "select", "sensor", "switch", "text_sensor")
# The stock module's own two diagnostics sit under its component key, not under sensor: or text_sensor:.
MODULE_KEY = "w41h1_module"
MODULE_ENTITIES = ["Flash layout", "Serial port restarts"]


def entities_of(dump: str) -> dict[str, list[tuple[str, str]]]:
    """Per entity domain, the (name, id) pairs in the configuration esphome prints once it is valid.

    The dump is YAML with ESPHome's own tags, read here by indentation: a top-level `domain:` key,
    then every mapping under it that has a `name:`. The object id Home Assistant sees is made from
    the name, so equal names are equal object ids. The ids are ESPHome's (given or generated) and
    are compared as well: a lambda in a package reaches an entity through them.
    """
    found: dict[str, list[tuple[str, str]]] = {domain: [] for domain in (*ENTITY_DOMAINS, MODULE_KEY)}
    domain = None
    pending: dict[int, dict[str, str]] = {}

    def flush(min_indent: int) -> None:
        for level in [lvl for lvl in pending if lvl >= min_indent]:
            entry = pending.pop(level)
            if domain in found and "name" in entry:
                found[domain].append((entry["name"], entry.get("id", "")))

    for line in dump.splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        depth = len(line) - len(line.lstrip())
        top = re.match(r"^([a-z_0-9]+):", line)
        if top:
            flush(0)
            domain = top.group(1)
            continue
        if domain not in found:
            continue
        stripped = line.strip()
        if stripped.startswith("- "):
            flush(depth)
            stripped = stripped[2:]
            depth += 2
        else:
            flush(depth + 1)
        match = re.match(r"^(name|id):\s*(.*)$", stripped)
        if match:
            pending.setdefault(depth, {})[match.group(1)] = match.group(2).strip().strip("'\"")
    flush(0)
    for entries in found.values():
        entries.sort()
    return found


def check_board_files(runner: Runner) -> tuple[int, int, list[str]]:
    boards = sorted(runner.sandbox.glob("w41h1*.yaml"))
    failures = []
    per_board = {}
    for result in runner.run_all([(board.name, board) for board in boards]):
        if result.returncode != 0:
            failures.append(f"board/{result.name}: rejected\n{indent(errors_of(result.output))}")
            continue
        if warnings_of(result.output):
            failures.append(
                f"board/{result.name}: accepted with a warning\n{indent(chr(10).join(warnings_of(result.output)))}"
            )
        per_board[result.name] = entities_of(result.output)
    if not per_board:
        return len(boards), 0, failures + ["no board file validated"]
    reference_name = "w41h1-esp32c3.yaml" if "w41h1-esp32c3.yaml" in per_board else sorted(per_board)[0]
    reference = per_board[reference_name]
    total = sum(len(reference[domain]) for domain in ENTITY_DOMAINS)
    # 37 today. A reader that finds far fewer is not reading the dump any more.
    if total < 30 or len(reference["climate"]) != 1 or len(reference["button"]) != 1:
        failures.append(f"board/{reference_name}: {total} entities read from its configuration, the reader is broken")
    for name, got in per_board.items():
        for domain in ENTITY_DOMAINS:
            want = set(reference[domain])
            have = set(got[domain])
            for entry in sorted(want - have):
                failures.append(f"board/{name}: {domain} {entry!r} of {reference_name} is missing")
            for entry in sorted(have - want):
                failures.append(f"board/{name}: {domain} {entry!r} is not in {reference_name}")
        # The stock module adds its own two diagnostics, and only the stock module does.
        names = [entry[0] for entry in got[MODULE_KEY]]
        if names != (MODULE_ENTITIES if "amebaz2" in name else []):
            failures.append(f"board/{name}: the module's own diagnostics are {names}")
    return len(boards), total, failures


# ---- The generated code (--codegen) --------------------------------------------------------------
# App.register_switch(id, "Eco", 747835640, 67371008): the domain, the name and the hash of the
# object id are what Home Assistant knows the entity by. The fourth number packs indices into
# string tables and is the chip's own business.
REGISTER = re.compile(r'App\.register_([a-z_]+)\(\w+, "((?:[^"\\]|\\.)*)", (\d+)(?:UL)?, ')
HUB_ONLY_ABSENT = (
    "HisenseClimate",
    "HisenseSwitch",
    "HisenseSleepSelect",
    "_sensor(",
    "set_link_token",
)
# Registered, but `internal: true` (packages/status-pixel.yaml): Home Assistant never sees it.
INTERNAL_ENTITIES = {("light", "status_pixel")}
ENTITY_DEFINES = ("USE_CLIMATE", "USE_SWITCH", "USE_SELECT", "USE_SENSOR", "USE_BINARY_SENSOR", "USE_TEXT_SENSOR")


def generate(runner: Runner, node: str, path: Path) -> tuple[str, str, str]:
    """main.cpp and defines.h of one configuration, and what went wrong if anything did."""
    result = runner.run(node, path, ("compile", "--only-generate"), ("-s", "name", node))
    build = path.parent / ".esphome" / "build" / node / "src"
    if result.returncode != 0 or not (build / "main.cpp").exists():
        return "", "", f"codegen/{node}: no code generated\n{indent(result.output[-1500:])}"
    main_cpp = (build / "main.cpp").read_text(encoding="utf-8", errors="replace")
    # The configuration is repeated in main.cpp as comments. Only the code counts.
    code = "\n".join(line for line in main_cpp.splitlines() if not line.lstrip().startswith("//"))
    defines = (build / "esphome" / "core" / "defines.h").read_text(encoding="utf-8", errors="replace")
    return code, defines, ""


def check_codegen(runner: Runner) -> tuple[int, list[str]]:
    failures: list[str] = []
    c3 = "esp32c3"
    generated = {
        "cg-hub-only": base(c3, "cg-hub-only"),
        "cg-everything": everything(c3, "cg-everything"),
        # One entity fewer than everything, per platform that takes several.
        "cg-less-one": base(c3, "cg-less-one")
        + entities("switch", {k: v for k, v in SWITCHES.items() if k != "beeper"})
        + entities("sensor", {k: v for k, v in SENSORS.items() if k != "power"})
        + entities("binary_sensor", {k: v for k, v in BINARY_SENSORS.items() if k != "fault_grille"}),
    }
    code: dict[str, str] = {}
    defines: dict[str, str] = {}
    for node, text in generated.items():
        path = runner.sandbox / f"{node}.yaml"
        path.write_text(text, encoding="utf-8")
        code[node], defines[node], problem = generate(runner, node, path)
        if problem:
            failures.append(problem)
    if failures:
        return len(generated), failures

    # The hub alone: no entity class, no setter, no entity domain compiled in.
    for token in HUB_ONLY_ABSENT:
        if token in code["cg-hub-only"]:
            failures.append(f"codegen/hub-only: main.cpp mentions {token}")
    for define in ENTITY_DEFINES:
        if re.search(rf"^#define {define}\b", defines["cg-hub-only"], re.M):
            failures.append(f"codegen/hub-only: defines.h has {define}")
        if not re.search(rf"^#define {define}\b", defines["cg-everything"], re.M):
            failures.append(f"codegen/everything: defines.h lacks {define}")

    # Everything: each entity is constructed and handed to the hub exactly once.
    def count(node: str, pattern: str) -> int:
        return len(re.findall(pattern, code[node]))

    wanted = {
        r"new\(\w+\) hisense_ac::HisenseSwitch\(\)": (len(SWITCHES), len(SWITCHES) - 1),
        r"->set_kind\(hisense_ac::SWITCH_": (len(SWITCHES), len(SWITCHES) - 1),
        r"ac_bus->set_\w+_sensor\(": (
            len(SENSORS) + 4,
            len(SENSORS) + 3 - 1,
        ),  # aux_heat, bus_link, problem, link_token
        r"ac_bus->add_fault_binary_sensor\(": (len(FAULTS), len(FAULTS) - 1),
        r"ac_bus->add_capability_binary_sensor\(": (len(CAPABILITIES), len(CAPABILITIES)),
        r"App\.register_sensor\(": (len(SENSORS), len(SENSORS) - 1),
        r"App\.register_binary_sensor\(": (len(BINARY_SENSORS), len(BINARY_SENSORS) - 1),
        r"App\.register_switch\(": (len(SWITCHES), len(SWITCHES) - 1),
    }
    for pattern, (everything_n, less_one_n) in wanted.items():
        if count("cg-everything", pattern) != everything_n:
            failures.append(
                f"codegen/everything: /{pattern}/ {count('cg-everything', pattern)} times, expected {everything_n}"
            )
        if count("cg-less-one", pattern) != less_one_n:
            failures.append(
                f"codegen/less-one: /{pattern}/ {count('cg-less-one', pattern)} times, expected {less_one_n}"
            )
    for token in ("SWITCH_BEEPER", "set_power_sensor", '"Beeper"', '"Power"', '"Fault grille"'):
        if token not in code["cg-everything"]:
            failures.append(f"codegen/everything: main.cpp lacks {token}")
        if token in code["cg-less-one"]:
            failures.append(f"codegen/less-one: {token} was left out of the YAML and is still in main.cpp")

    # The board files: the same entities, by domain, name and object-id hash.
    boards = sorted(runner.sandbox.glob("w41h1*.yaml"))
    registered = {}
    for board in boards:
        board_code, _, problem = generate(runner, "cg-" + board.stem.replace(".", "-"), board)
        if problem:
            failures.append(problem)
            continue
        registered[board.name] = {
            entry for entry in REGISTER.findall(board_code) if (entry[0], entry[1]) not in INTERNAL_ENTITIES
        }
    if registered:
        reference_name = "w41h1-esp32c3.yaml" if "w41h1-esp32c3.yaml" in registered else sorted(registered)[0]
        reference = registered[reference_name]
        if len(reference) < 30:
            failures.append(f"codegen/{reference_name}: {len(reference)} registrations read, the reader is broken")
        for name, got in registered.items():
            module = {entry for entry in got if entry[1] in MODULE_ENTITIES}
            for entry in sorted(reference - got):
                failures.append(f"codegen/{name}: {entry!r} of {reference_name} is missing")
            for entry in sorted(got - module - reference):
                failures.append(f"codegen/{name}: {entry!r} is not in {reference_name}")
            if len(module) != (len(MODULE_ENTITIES) if "amebaz2" in name else 0):
                failures.append(f"codegen/{name}: the module's own diagnostics are {sorted(module)}")
    return len(generated) + len(boards), failures


def indent(text: str) -> str:
    return "\n".join("      " + line for line in text.splitlines())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--full", action="store_true", help="the whole matrix, not the quick set")
    parser.add_argument("--require", action="store_true", help="fail when esphome is not installed")
    parser.add_argument("--codegen", action="store_true", help="also check the generated C++ (fetches libraries)")
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 2))
    parser.add_argument("--show", metavar="NAME", help="print what esphome says about one invalid file or valid case")
    parser.add_argument("--list", action="store_true", help="list the cases and stop")
    args = parser.parse_args()

    print("\n== ESPHome configuration matrix (esphome config: valid, invalid, board files) ==")
    if args.list:
        for case in valid_cases():
            print(f"valid/{case.name}{'  (--full)' if case.full_only else ''}")
        for config in sorted(INVALID_DIR.glob("*.yaml")):
            print(f"invalid/{config.stem}")
        return 0
    esphome = os.environ.get("ESPHOME") or shutil.which("esphome")
    if not esphome:
        print("esphome is not installed: SKIPPED (pipx install esphome, or set ESPHOME)")
        return 1 if args.require else 0

    start = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="w41h1-config-matrix-") as tmp:
        runner = Runner(esphome, make_sandbox(Path(tmp)), max(1, args.jobs))
        if args.show:
            path = runner.sandbox / "tests" / "invalid" / f"{args.show}.yaml"
            if not path.exists():
                case = next((c for c in valid_cases() if c.name == args.show), None)
                if case is None:
                    print(f"no case named {args.show}")
                    return 2
                path = runner.sandbox / f"gen-{case.name}.yaml"
                path.write_text(case.text, encoding="utf-8")
            result = runner.run(args.show, path)
            print(path.read_text(encoding="utf-8"))
            print(f"-- esphome config: exit {result.returncode}")
            print(result.output)
            return 0
        version = subprocess.run([esphome, "version"], env=runner.env, capture_output=True, text=True, check=False)
        print(version.stdout.strip() or version.stderr.strip())

        valid_count, failures = check_valid(runner, args.full)
        invalid_count, known, invalid_failures = check_invalid(runner)
        board_count, entity_count, board_failures = check_board_files(runner)
        failures += invalid_failures + board_failures
        codegen_count = 0
        if args.codegen:
            codegen_count, codegen_failures = check_codegen(runner)
            failures += codegen_failures

    for failure in failures:
        print(f"  FAIL {failure}")
    print(
        f"\n{valid_count} valid, {invalid_count} invalid ({known} of them known to be accepted), "
        f"{board_count} board files with the same {entity_count} entities, "
        f"{codegen_count} generated ({'--codegen' if args.codegen else 'not run, --codegen'}), {len(failures)} failed, "
        f"{time.monotonic() - start:.1f} s"
    )
    if not failures:
        print("== ESPHome configuration matrix OK ==")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
