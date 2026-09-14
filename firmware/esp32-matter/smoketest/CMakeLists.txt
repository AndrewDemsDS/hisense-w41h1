# Minimal esp-idf project: busmon, a live RS-485 bus monitor. It runs the reused driver + ESP32
# HAL (handshake, ~1 Hz poll, frame reassembly, status parse) and logs every decoded status frame.
# No esp-matter. It needs something answering on the bus: the real A/C (A/B tap) or
# firmware/test/virtual_ac.py on a USB adapter. The codec golden vectors run on the host instead
# (firmware/test/run_tests.sh).
#   . $IDF_PATH/export.sh && idf.py set-target esp32c3 && idf.py build flash monitor
#   or: firmware/scripts/dev.sh bench esp32 --port <board> --sim-port <adapter>
cmake_minimum_required(VERSION 3.16)
# Reuse the ported components (hisense_hal + hisense_rs485 -> ../../src/rs485-driver).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hisense_codec_smoketest)
