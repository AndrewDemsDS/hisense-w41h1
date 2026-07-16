# esp32-recon: independent RS-485 A/C bus diagnostic tool (no Matter).
# Reuses the hardware-validated driver + ESP32 HAL from esp32-matter/components
# (hisense_rs485 -> ../src/rs485-driver, hisense_hal -> ESP-IDF UART/GPIO shim).
#
# Build/flash (fast, seconds-to-a-minute, NOT the 6-min Matter build):
#   . $IDF_PATH/export.sh && idf.py set-target esp32 && idf.py build
#   idf.py -p <port> flash monitor
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../esp32-matter/components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_recon)
