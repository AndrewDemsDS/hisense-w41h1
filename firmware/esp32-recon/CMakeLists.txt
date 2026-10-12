# esp32-recon: independent RS-485 A/C bus diagnostic tool.
# Builds the hardware-validated reference driver through the ESP32 HAL in components/
# (hisense_rs485 -> ../test/reference-driver, hisense_hal -> ESP-IDF UART/GPIO shim).
#
# Build/flash (seconds to a minute):
#   . $IDF_PATH/export.sh && idf.py set-target esp32 && idf.py build
#   idf.py -p <port> flash monitor
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_recon)
