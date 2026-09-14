#!/usr/bin/env bash
#
# dev.sh -- one entry point for the from-source build / flash / test flow on all three targets
# (issue #118). It WRAPS the real scripts (ota-release.sh, esp32-release.sh, esp32-lint.sh,
# firmware/setup.sh, scripts/setup.sh) and never re-implements them: this is the map, those are
# the territory. Every command prints what it is about to run before running it.
#
# Usage:
#   dev.sh walk    <target>                  # guided: doctor -> test -> build -> flash -> next, y/N per step
#   dev.sh doctor  <target>                  # check tools + SDK pins against versions.env (read-only)
#   dev.sh fetch   <target>                  # fetch the pinned SDKs (asks first; several GB)
#   dev.sh test    <target>                  # host QA (run_tests.sh) + the target's lint
#   dev.sh build   <target>                  # build the app (esp32: set-target first if needed)
#   dev.sh erase   <target> --port P         # erase-flash, BRAND-NEW boards only (typed confirmation)
#   dev.sh flash   <target> --port P         # flash + monitor
#   dev.sh monitor <target> --port P         # serial monitor only
#   dev.sh bench   <target> --port P --sim-port S   # busmon/app vs virtual_ac.py over a USB adapter
#   dev.sh next    <target>                  # print the staged bring-up and its safety warnings
#
# Targets: amebaz2 | esp32 | esphome. Board (esp32/esphome): --board c3 (ESP32-C3 SuperMini,
# default) or --board classic (ESP32-D0WDQ6). Env: IDF_PATH / ESP_MATTER_PATH (esp32; defaults
# ~/esp/esp-idf and ~/esp/esp-matter), ESPHOME (esphome command, default `esphome`).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
ESP="$REPO/firmware/esp32-matter"
ESPHOME_DIR="$REPO/firmware/esphome"
TEST="$REPO/firmware/test"
# shellcheck source=versions.env
. "$REPO/versions.env"
# CI's `esphome config` pin (.github/workflows/qa.yaml). Keep the two in step.
ESPHOME_PIN="2026.7.4"

say()  { printf '\033[1;36m[dev]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[dev] WARNING:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[dev] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }
run()  { printf '\033[1;90m  $ %s\033[0m\n' "$*"; "$@"; }
ok()   { printf '  \033[32mok\033[0m    %s\n' "$*"; }
bad()  { printf '  \033[31mMISS\033[0m  %s\n' "$*"; DOCTOR_FAIL=1; }
ask()  { local a; read -r -p "$1 [y/N] " a; [[ "$a" =~ ^[Yy]$ ]]; }

usage() { sed -n '3,24p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

# ---- argument parsing ---------------------------------------------------------------------
cmd="${1:-}"; target="${2:-}"
[ -n "$cmd" ] || usage 1
[ "$cmd" = "-h" ] || [ "$cmd" = "--help" ] || [ "$cmd" = "help" ] && usage 0
case "$target" in amebaz2|esp32|esphome) ;; *) die "target must be amebaz2, esp32 or esphome (got '${target}')";; esac
shift 2
BOARD="c3"; PORT=""; SIM_PORT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --board)    BOARD="${2:?}"; shift 2 ;;
    --port)     PORT="${2:?}"; shift 2 ;;
    --sim-port) SIM_PORT="${2:?}"; shift 2 ;;
    *) die "unknown option: $1" ;;
  esac
done
case "$BOARD" in
  c3)      IDF_TGT="esp32c3"; PINS=(5 6 10);  ESPHOME_BOARD="esp32-c3-devkitm-1" ;;
  classic) IDF_TGT="esp32";   PINS=(19 18 4); ESPHOME_BOARD="esp32dev" ;;
  *) die "--board must be c3 or classic" ;;
esac
need_port() { [ -n "$PORT" ] || die "$cmd needs --port <serial device> (e.g. /dev/ttyACM0)"; }

# ---- environments -------------------------------------------------------------------------
: "${IDF_PATH:=$HOME/esp/esp-idf}"
: "${ESP_MATTER_PATH:=$HOME/esp/esp-matter}"
: "${ESPHOME:=esphome}"

# Source IDF (+ esp-matter unless $1=idf-only) into THIS shell. export.sh is noisy; keep the tail.
esp_env() {
  command -v idf.py >/dev/null 2>&1 && [ "${1:-}" = "idf-only" ] && return 0
  [ -f "$IDF_PATH/export.sh" ] || die "no ESP-IDF at $IDF_PATH (set IDF_PATH, or: dev.sh fetch esp32)"
  say "sourcing ESP-IDF ($IDF_PATH)"
  # shellcheck disable=SC1091
  . "$IDF_PATH/export.sh" >/dev/null
  [ "${1:-}" = "idf-only" ] && return 0
  [ -f "$ESP_MATTER_PATH/export.sh" ] || die "no esp-matter at $ESP_MATTER_PATH (set ESP_MATTER_PATH, or: dev.sh fetch esp32)"
  say "sourcing esp-matter ($ESP_MATTER_PATH)"
  # shellcheck disable=SC1091
  . "$ESP_MATTER_PATH/export.sh" >/dev/null
}

# idf.py set-target wipes sdkconfig + build/, so only run it when the target actually changes.
ensure_target() {  # $1 = project dir
  local cur; cur=$(sed -n 's/^CONFIG_IDF_TARGET="\(.*\)"/\1/p' "$1/sdkconfig" 2>/dev/null | head -1)
  if [ "$cur" != "$IDF_TGT" ]; then
    [ -n "$cur" ] && warn "$1 is configured for $cur; switching to $IDF_TGT wipes its sdkconfig and build/"
    (cd "$1" && run idf.py set-target "$IDF_TGT")
  fi
}

# ESPHome board/pins come from --board via -s substitutions; w41h1.yaml keeps the classic defaults.
esphome_subs() {
  printf '%s\n' -s board "$ESPHOME_BOARD" -s tx_pin "${PINS[0]}" -s rx_pin "${PINS[1]}" -s de_pin "${PINS[2]}"
}
esphome_run() {  # esphome <subcmd> w41h1.yaml [args...] with the board substitutions applied
  local sub="$1"; shift
  local subs; mapfile -t subs < <(esphome_subs)
  command -v "$ESPHOME" >/dev/null 2>&1 || die "'$ESPHOME' not found (dev.sh fetch esphome, or set ESPHOME=)"
  [ -f "$ESPHOME_DIR/secrets.yaml" ] || die "no $ESPHOME_DIR/secrets.yaml -- cp secrets.yaml.example secrets.yaml and fill it in"
  (cd "$ESPHOME_DIR" && run "$ESPHOME" "${subs[@]}" "$sub" w41h1.yaml "$@")
}

# ---- doctor -------------------------------------------------------------------------------
DOCTOR_FAIL=0
git_head_is() {  # $1=dir $2=expected sha-or-tag $3=label
  local dir="$1" want="$2" label="$3" have
  if [ ! -d "$dir/.git" ] && [ ! -f "$dir/.git" ]; then bad "$label: not found at $dir"; return; fi
  have=$(git -C "$dir" rev-parse HEAD 2>/dev/null)
  if [ "$have" = "$(git -C "$dir" rev-parse "${want}^{commit}" 2>/dev/null)" ]; then
    ok "$label @ $want"
  else
    bad "$label is at ${have:0:12}, versions.env pins $want"
  fi
}
have_tool() { if command -v "$1" >/dev/null 2>&1; then ok "$1"; else bad "$1 not on PATH${2:+ ($2)}"; fi; }

doctor() {
  say "doctor: $target"
  have_tool git; have_tool python3; have_tool g++ "needed by run_tests.sh"
  case "$target" in
    amebaz2)
      local sdk; sdk="$(readlink -f "$REPO/sdk" 2>/dev/null || true)"
      if [ -n "$sdk" ] && [ -d "$sdk" ]; then
        ok "sdk symlink -> $sdk"
        git_head_is "$sdk/ameba-rtos-z2" "$AMEBA_Z2_PIN" "ameba-rtos-z2"
        git_head_is "$sdk/connectedhomeip" "$CHIP_PIN" "connectedhomeip"
      else
        bad "no ./sdk symlink (dev.sh fetch amebaz2)"
      fi
      if [ -f "$HERE/ota-release.env" ]; then ok "ota-release.env"; else bad "ota-release.env (cp ota-release.env.example)"; fi
      ;;
    esp32)
      git_head_is "$IDF_PATH" "$IDF_PIN" "ESP-IDF"
      git_head_is "$ESP_MATTER_PATH" "$ESP_MATTER_PIN" "esp-matter"
      ;;
    esphome)
      if command -v "$ESPHOME" >/dev/null 2>&1; then
        local v; v=$("$ESPHOME" version 2>/dev/null | awk '{print $NF}')
        if [ "$v" = "$ESPHOME_PIN" ]; then ok "esphome $v"; else bad "esphome is $v, CI pins $ESPHOME_PIN"; fi
      else
        bad "esphome not on PATH (dev.sh fetch esphome)"
      fi
      if [ -f "$ESPHOME_DIR/secrets.yaml" ]; then ok "secrets.yaml"; else bad "secrets.yaml (cp secrets.yaml.example)"; fi
      ;;
  esac
  if [ "$DOCTOR_FAIL" = 0 ]; then say "doctor: all good"; else warn "doctor found gaps (above)"; return 1; fi
}

# ---- fetch --------------------------------------------------------------------------------
fetch() {
  case "$target" in
    amebaz2)
      say "AmebaZ2: firmware/setup.sh fetches + pins the Realtek SDK and connectedhomeip (~15 GB,"
      say "needs sudo for host packages), then scripts/setup.sh applies the patches and overlays."
      local root="${SDK_ROOT:-$HOME/ameba-dev}"
      ask "fetch into $root?" || return 0
      run bash "$REPO/firmware/setup.sh" "$root"
      [ -e "$REPO/sdk" ] || run ln -s "$root" "$REPO/sdk"
      run bash "$REPO/scripts/setup.sh"
      ;;
    esp32)
      # The esp-matter documented procedure (docs/en/developing.rst), pinned to versions.env.
      say "ESP32: ESP-IDF $IDF_PIN -> $IDF_PATH, esp-matter ${ESP_MATTER_PIN:0:12} -> $ESP_MATTER_PATH (several GB)"
      ask "fetch?" || return 0
      if [ ! -d "$IDF_PATH" ]; then
        run git clone -b "$IDF_PIN" --recursive https://github.com/espressif/esp-idf.git "$IDF_PATH"
      else
        say "$IDF_PATH exists; checking out $IDF_PIN"
        run git -C "$IDF_PATH" fetch --tags origin
        run git -C "$IDF_PATH" checkout "$IDF_PIN"
        run git -C "$IDF_PATH" submodule update --init --recursive
      fi
      (cd "$IDF_PATH" && run ./install.sh esp32,esp32c3)
      esp_env idf-only
      if [ ! -d "$ESP_MATTER_PATH" ]; then
        run git init -q "$ESP_MATTER_PATH"
        run git -C "$ESP_MATTER_PATH" remote add origin https://github.com/espressif/esp-matter.git
      fi
      run git -C "$ESP_MATTER_PATH" fetch --depth 1 origin "$ESP_MATTER_PIN"
      run git -C "$ESP_MATTER_PATH" checkout -q FETCH_HEAD
      run git -C "$ESP_MATTER_PATH" submodule update --init --depth 1
      (cd "$ESP_MATTER_PATH/connectedhomeip/connectedhomeip" \
        && run ./scripts/checkout_submodules.py --platform esp32 linux --shallow)
      (cd "$ESP_MATTER_PATH" && run ./install.sh)
      ;;
    esphome)
      command -v pipx >/dev/null 2>&1 || die "install pipx first (or: pip install esphome==$ESPHOME_PIN in a venv)"
      ask "pipx install esphome==$ESPHOME_PIN?" || return 0
      run pipx install --force "esphome==$ESPHOME_PIN"
      [ -f "$ESPHOME_DIR/secrets.yaml" ] || run cp "$ESPHOME_DIR/secrets.yaml.example" "$ESPHOME_DIR/secrets.yaml"
      say "now fill in $ESPHOME_DIR/secrets.yaml (it is gitignored)"
      ;;
  esac
}

# ---- test ---------------------------------------------------------------------------------
test_target() {
  run bash "$TEST/run_tests.sh"
  case "$target" in
    amebaz2) run bash "$HERE/ota-release.sh" lint ;;
    esp32)   run bash "$HERE/esp32-lint.sh" ;;
    esphome) esphome_run config ;;
  esac
}

# ---- build / flash / monitor --------------------------------------------------------------
build() {
  case "$target" in
    amebaz2) run bash "$HERE/ota-release.sh" build ;;   # full clean, FWHS serial, verify: see docs/10
    esp32)
      esp_env; ensure_target "$ESP"
      (cd "$ESP" && run idf.py build)
      say "dev build only. A shippable OTA goes through esp32-release.sh (delta base archive, #82)."
      ;;
    esphome) esphome_run compile ;;
  esac
}

erase() {
  need_port
  case "$target" in
    amebaz2) die "AmebaZ2 has no erase step here: the clip flasher writes regions (see Installing-Custom-Firmware)" ;;
  esac
  warn "erase-flash is for a BRAND-NEW board only: it wipes NVS, which on a working node holds the"
  warn "Matter fabric / Wi-Fi config. A factory board needs it (stale vendor NVS breaks commissioning)."
  local a; read -r -p "type ERASE to erase $PORT: " a
  [ "$a" = "ERASE" ] || die "not confirmed; nothing erased"
  case "$target" in
    esp32)   esp_env idf-only; (cd "$ESP" && run idf.py -p "$PORT" erase-flash) ;;
    esphome)
      # esptool ships as an esphome dependency; use the interpreter esphome itself runs under.
      local py; py="$(dirname "$(readlink -f "$(command -v "$ESPHOME")")")/python"
      [ -x "$py" ] || py=python3
      run "$py" -m esptool --port "$PORT" erase_flash ;;
  esac
}

flash() {
  case "$target" in
    amebaz2)
      say "AmebaZ2 first install is a SOIC-8 clip write; a commissioned node takes OTA instead:"
      say "  clip: python3 firmware/flasher/ch341flash.py firmware/built-images/flash_rac-integrated-v<ver>.bin"
      say "  OTA:  firmware/scripts/ota-release.sh package && ota-release.sh stage && ota-release.sh flash"
      say "Read docs/guide/Installing-Custom-Firmware.md first: dump the chip before every clip write."
      ;;
    esp32)   need_port; esp_env; ensure_target "$ESP"; (cd "$ESP" && run idf.py -p "$PORT" flash monitor) ;;
    esphome) need_port; esphome_run run --device "$PORT" ;;
  esac
}

monitor() {
  need_port
  case "$target" in
    amebaz2) die "AmebaZ2 has no USB console; use the debug build's :2323 console or the UART pads" ;;
    esp32)   esp_env idf-only; (cd "$ESP" && run idf.py -p "$PORT" monitor) ;;
    esphome) esphome_run logs --device "$PORT" ;;
  esac
}

# ---- bench: firmware vs virtual_ac.py, no A/C ---------------------------------------------
bench() {
  [ "$target" != amebaz2 ] || die "bench is for esp32/esphome; for AmebaZ2 run virtual_ac.py --port on the DI/RO tap"
  need_port; [ -n "$SIM_PORT" ] || die "bench needs --sim-port <USB-TTL or USB-RS485 adapter>"
  python3 -c 'import serial' 2>/dev/null || die "virtual_ac.py needs pyserial (pip install pyserial)"
  say "bench wiring (no A/C, no mains):"
  say "  USB-TTL:    board TX GPIO${PINS[0]} -> adapter RX, board RX GPIO${PINS[1]} <- adapter TX, GND-GND (3.3 V adapter)"
  say "  USB-RS485:  transceiver A-A, B-B, GND-GND (board DI/RO/DE wired as for the A/C)"
  ask "wired like that?" || return 0
  local proj=""
  if [ "$target" = esp32 ]; then
    proj="$ESP/smoketest"
    esp_env idf-only; ensure_target "$proj"
    (cd "$proj" && run idf.py -p "$PORT" build flash)
  else
    esphome_run run --no-logs --device "$PORT"   # compile + upload; logs follow below
  fi
  say "starting virtual_ac.py on $SIM_PORT (Ctrl-C stops both)"
  python3 "$TEST/virtual_ac.py" --port "$SIM_PORT" &
  local sim=$!
  trap 'kill "$sim" 2>/dev/null || true' EXIT INT TERM
  say "PASS looks like: sim prints '[0x0A] handshake poll -> echoed slave reply', then busmon logs"
  say "'A/C #N: power=1 mode=... set=24C indoor=25C' about once a second with RX climbing."
  if [ "$target" = esp32 ]; then
    (cd "$proj" && run idf.py -p "$PORT" monitor)
  else
    esphome_run logs --device "$PORT"
  fi
}

# ---- next: the staged bring-up ------------------------------------------------------------
next_steps() {
  cat <<EOF
Staged bring-up for $target (never leave the A/C in an unknown state):

  1. Bench, no A/C     dev.sh test $target; then dev.sh bench $target --port P --sim-port S
  2. Real bus, USB     module out, tap A/B ONLY, watch decoded status (read), then one control (write)
  3. Integration       power from the connector's 5 V, no laptop attached, close it up

Safety, before stage 2:
  ! Ground loop: while USB-powered connect ONLY A/B. Joining the mains-earthed A/C GND to a
    laptop-earthed board browns it out (RTCWDT resets, flash-read errors). GND/5V join at stage 3.
  ! 3.3 V transceiver only (MAX3485 / SP3485 / SN65HVD75). A 5 V MAX485 module's RO kills the RX pin.
EOF
  case "$target" in
    esp32|esphome) cat <<EOF
  ! ESP32-C3: fit a ~10k pulldown on DE (GPIO10). DE floats until gpio_init() and a high DE parks
    a second driver on the A/C bus. Never put the UART on GPIO18/19 (the C3's only USB).
  ! Classic ESP32: never GPIO16/17 on WROVER/D0WDQ6 (PSRAM-bonded, dead as I/O).
  ! Brand-new board: dev.sh erase $target --port P before the first flash (stale vendor NVS).
Full detail: firmware/esp32-matter/README.md, docs/guide/Build-Flash-Test.md
EOF
      ;;
    amebaz2) cat <<EOF
  ! Dump the whole chip (firmware/flasher/ch341dump.py) before every clip write; never flashrom.
  ! OTA: FWHS serial and version must bump (ota-release.sh build does it) or the update reverts.
Full detail: firmware/docs/10-firmware-ota-procedure.md, docs/guide/Installing-Custom-Firmware.md
EOF
      ;;
  esac
}

# ---- walk: the guided sequence ------------------------------------------------------------
walk() {
  say "guided flow for $target (board: $BOARD). Each step asks first; N skips it."
  if ask "1/5 doctor: check tools and SDK pins?"; then
    if ! doctor && ask "gaps found. run fetch?"; then fetch; fi
  fi
  if ask "2/5 host QA + lint?"; then test_target; fi
  if ask "3/5 build?"; then build; fi
  if [ "$target" != amebaz2 ]; then
    if [ -z "$PORT" ]; then read -r -p "serial port for flashing (blank to skip flash): " PORT; fi
    if [ -n "$PORT" ]; then
      if ask "   brand-new board that needs erase-flash first?"; then erase; fi
      if ask "4/5 flash + monitor $PORT?"; then flash; fi
    fi
  else
    if ask "4/5 show the AmebaZ2 flash options?"; then flash; fi
  fi
  say "5/5 next steps"
  next_steps
}

case "$cmd" in
  walk)    walk ;;
  doctor)  doctor ;;
  fetch)   fetch ;;
  test)    test_target ;;
  build)   build ;;
  erase)   erase ;;
  flash)   flash ;;
  monitor) monitor ;;
  bench)   bench ;;
  next)    next_steps ;;
  *)       usage 1 ;;
esac
