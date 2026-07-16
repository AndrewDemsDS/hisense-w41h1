#!/usr/bin/env bash
# sync-files.sh -- SINGLE SOURCE OF TRUTH for the set of repo source files copied into the
# room_air_conditioner SDK example dir. Sourced by BOTH firmware/scripts/ota-release.sh
# (sync_mirror) and scripts/setup.sh (copy loop) so the two can never drift out of lockstep.
# (matter_aircon_map.h once went missing from ota-release -> Matter<->Hisense mapping edits
# never reached a rebuild. Defining the list once makes that class of bug impossible.)
#
# Paths are RELATIVE to the repo root; each consumer prefixes its own root var ($REPO / $HERE).
#   REQUIRED -- tracked source that must exist; ota-release copies these unconditionally so a
#               missing one hard-fails the release build (a stale build would otherwise ship).
#   OPTIONAL -- copied only if present (inlined/absent delegate + the power estimate header).
#   SYNC_FILES -- the full set (required + optional): used by the setup copy loop + verification.

SYNC_FILES_REQUIRED=(
  firmware/src/rs485-driver/hisense_rs485.h
  firmware/src/rs485-driver/hisense_rs485.cpp
  firmware/src/rs485-driver/matter_aircon_map.h
  firmware/src/sdk-edits/matter_drivers.cpp
  firmware/src/sdk-edits/ElectricalPowerMeasurementDelegate.h
  firmware/src/sdk-edits/ElectricalPowerMeasurementDelegate.cpp
  firmware/src/sdk-edits/room-air-conditioner-app.zap
)
SYNC_FILES_OPTIONAL=(
  firmware/src/rs485-driver/power_estimate.h
)
SYNC_FILES=( "${SYNC_FILES_REQUIRED[@]}" "${SYNC_FILES_OPTIONAL[@]}" )
