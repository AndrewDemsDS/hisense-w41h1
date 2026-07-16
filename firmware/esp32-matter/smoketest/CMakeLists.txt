# Minimal esp-idf project: runs the codec golden vectors on-target through the reused
# driver + ESP32 HAL. No esp-matter, no transceiver needed.
#   . $IDF_PATH/export.sh && idf.py set-target esp32 && idf.py build
cmake_minimum_required(VERSION 3.16)
# Reuse the ported components (hisense_hal + hisense_rs485 -> ../../src/rs485-driver).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hisense_codec_smoketest)
