# Project CMake. Source both env scripts first:
#   . ~/esp/esp-idf-v5.5.4/export.sh && . $ESP_MATTER_PATH/export.sh
#   idf.py set-target esp32 && idf.py build flash monitor
cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "1.0.5")

# Unified versioning (issue #77): the Matter softwareVersion INT is DERIVED from PROJECT_VER --
# MAJOR*10000+MINOR*100+PATCH -> a readable, strictly-monotonic uint32. This keeps the human
# semver in PROJECT_VER + git tags (esp32-vX.Y.Z) while the int keeps climbing (the OTA provider
# only serves a strictly-greater int; 1.0.3 -> 10003 > the deployed sw3). Mirrors ota-release.sh's
# semver_to_int for the AmebaZ2 path so both firmware paths share one policy.
string(REPLACE "." ";" _ver_parts "${PROJECT_VER}")
list(GET _ver_parts 0 _ver_major)
list(GET _ver_parts 1 _ver_minor)
list(GET _ver_parts 2 _ver_patch)
math(EXPR PROJECT_VER_INT "${_ver_major}*10000 + ${_ver_minor}*100 + ${_ver_patch}")

if(NOT DEFINED ENV{ESP_MATTER_PATH})
    message(FATAL_ERROR "ESP_MATTER_PATH not set -- run: . <esp-matter>/export.sh")
endif()

# esp_matter/CMakeLists.txt references ${MATTER_SDK_PATH} (the connectedhomeip tree) for
# its zzz_generated + src SRC_DIRS; the project CMake must define it or those paths
# collapse to "/zzz_generated/..." and configure fails. Mirrors esp-matter's own examples.
set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)

# esp-matter + connectedhomeip components, plus our local components/ (hisense_hal,
# hisense_rs485 -> reuses ../src/rs485-driver unchanged). We skip esp-matter's device_hal
# (led/button demo drivers) -- app_main.cpp drives the A/C endpoint directly, no board HAL.
set(EXTRA_COMPONENT_DIRS
    "${MATTER_SDK_PATH}/config/esp32/components"
    "${ESP_MATTER_PATH}/components"
    "${CMAKE_CURRENT_LIST_DIR}/components"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hisense_ac_matter)

# CHIP requires C++17 + its config define; without these the esp_matter/CHIP sources
# fail to compile. Copied from esp-matter's example project CMake.
idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)

# Force the Matter softwareVersion int from PROJECT_VER (issue #77). Wins over sdkconfig's
# CONFIG_DEVICE_SOFTWARE_VERSION_NUMBER because CHIPDeviceConfig.h guards the macro with #ifndef,
# so the OTA int can never drift from the semver. (The string stays in sdkconfig.defaults, kept
# equal to PROJECT_VER.)
idf_build_set_property(COMPILE_DEFINITIONS "CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION=${PROJECT_VER_INT}" APPEND)
message(STATUS "hisense: Matter softwareVersion ${PROJECT_VER_INT} (\"${PROJECT_VER}\") [issue #77]")
