#!/usr/bin/env bash
# Toolchain + SDK setup for building custom W41H1 (RTL8710C / AmebaZ2) Matter firmware.
#
# The build is DRIVEN FROM THE AmebaZ2 BASE SDK (ameba-rtos-z2). ameba-rtos-matter is a
# COMPONENT that plugs into it, and connectedhomeip provides the Matter SDK / build env.
# Layout (per upstream docs/amebaz2_general_build.md + ameba_matter_integration.md):
#
#   <root>/                                    default: ~/ameba-dev (the repo's sdk symlink)
#   ├── ameba-rtos-z2/                         base SDK — build runs here
#   │   ├── component/common/application/matter/   ← ameba-rtos-matter @ release/v1.4.2
#   │   └── third_party/connectedhomeip        → ../../connectedhomeip (made by matter_setup.sh)
#   └── connectedhomeip/                       project-chip @ v1.4.2-branch (recursive submodules)
#
# Heavy: connectedhomeip + submodules are several GB. >=16 GB RAM, ~30 GB free, Linux x86_64.
set -euo pipefail
ROOT="${1:-$HOME/ameba-dev}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Pinned upstream commits — single source of truth (see UPSTREAM.md / NOTICE.md).
# shellcheck source=/dev/null
[ -f "$REPO_ROOT/versions.env" ] && source "$REPO_ROOT/versions.env"

# Check out the exact pinned commit the patches target. Shallow/branch-tip clones
# don't contain an older SHA, so fall back to a by-SHA fetch (GitHub allows it).
checkout_pin() {  # $1=dir  $2=pin (empty = skip)
  local dir="$1" pin="${2:-}"; [ -z "$pin" ] && return 0
  if git -C "$dir" cat-file -e "${pin}^{commit}" 2>/dev/null; then
    git -C "$dir" checkout -q "$pin"
  elif git -C "$dir" fetch --depth 1 origin "$pin" 2>/dev/null; then
    git -C "$dir" checkout -q FETCH_HEAD
  else
    echo "  [!] could not pin $dir to $pin (branch-tip clone) — patches may need --3way. See UPSTREAM.md."
  fi
}

echo "== host packages =="
if [ -n "${SKIP_PKGS:-}" ]; then
  echo "  SKIP_PKGS set — assuming host packages are already installed (CI / provisioned box)"
elif command -v pacman >/dev/null 2>&1; then
  sudo pacman -S --needed --noconfirm base-devel git python ninja unzip pkgconf openssl \
       dbus glib2 avahi cairo gobject-introspection || echo "  (pacman step skipped — install the above manually)"
elif command -v apt-get >/dev/null 2>&1; then
  sudo apt-get install -y git gcc g++ pkg-config libssl-dev libdbus-1-dev libglib2.0-dev \
       libavahi-client-dev ninja-build python3 python3-venv python3-dev unzip \
       libgirepository1.0-dev libcairo2-dev || echo "  (apt step skipped — install the above manually)"
else
  echo "  unknown distro — install: git gcc g++ pkg-config openssl dbus glib avahi ninja python3 cairo gobject-introspection"
fi

mkdir -p "$ROOT"; cd "$ROOT"

echo "== 1/4 clone ameba-rtos-z2 (base SDK) @ ${AMEBA_Z2_PIN:-tip} =="
# NOT shallow: scripts/setup.sh applies patches/ameba-rtos-z2.patch with `git apply --3way`, which
# needs the base blobs. matter_setup.sh (step 4/4) rewrites tracked files (e.g. the example
# Makefile) first, so on a --depth 1 clone git can't reconstruct the 3-way base ("repository lacks
# the necessary blob to perform 3-way merge") and hunks get rejected. Full history keeps it robust.
[ -d ameba-rtos-z2 ] || git clone \
    "${AMEBA_Z2_REPO:-https://github.com/Ameba-AIoT/ameba-rtos-z2.git}" ameba-rtos-z2
checkout_pin "$ROOT/ameba-rtos-z2" "${AMEBA_Z2_PIN:-}"

echo "== 2/4 clone connectedhomeip (Matter ${CHIP_BRANCH:-v1.4.2-branch}) @ ${CHIP_PIN:-tip} + submodules =="
[ -d connectedhomeip ] || git clone --branch "${CHIP_BRANCH:-v1.4.2-branch}" --depth 1 \
    "${CHIP_REPO:-https://github.com/project-chip/connectedhomeip.git}" connectedhomeip
checkout_pin "$ROOT/connectedhomeip" "${CHIP_PIN:-}"
( cd connectedhomeip && git submodule update --init --recursive --depth 1 )

echo "== 3/4 place ameba-rtos-matter into the z2 component slot (${AMEBA_MATTER_BRANCH:-release/v1.4.2}) =="
MATTER_DIR="$ROOT/ameba-rtos-z2/component/common/application/matter"
if [ ! -d "$MATTER_DIR/.git" ]; then
  mkdir -p "$(dirname "$MATTER_DIR")"
  git clone --branch "${AMEBA_MATTER_BRANCH:-release/v1.4.2}" --depth 1 \
      "${AMEBA_MATTER_REPO:-https://github.com/Ameba-AIoT/ameba-rtos-matter.git}" "$MATTER_DIR"
fi

echo "== 4/4 wire it up: third_party symlink + version selection + ENABLE_MATTER =="
( cd "$ROOT/ameba-rtos-z2" && chmod u+x matter_setup.sh && ./matter_setup.sh amebaz2 )

echo "== bootstrap the connectedhomeip build env (pigweed, gn, ninja, zap, venv) =="
# bootstrap.sh (pigweed) references unbound vars — disable nounset around it.
( cd "$ROOT/connectedhomeip" && set +u && source scripts/bootstrap.sh )

echo
echo "DONE. To build the Room A/C firmware for this device:"
echo "  source $ROOT/connectedhomeip/scripts/activate.sh          # per-shell env"
echo "  cd $ROOT/ameba-rtos-z2/project/realtek_amebaz2_v0_example/GCC-RELEASE"
echo "  make room_air_conditioner_port          # CHIP lib + Room A/C app (our sdk-edits go in first)"
echo "  make is_matter            # final flash_is.bin image"
echo "  # output: .../GCC-RELEASE/application_is/Debug/bin/flash_is.bin"
echo "  see docs/10-firmware-ota-procedure.md"
