# ESP-IDF component that builds the hardware-validated reference RS-485 driver from
# ../../../test/reference-driver UNCHANGED. hisense_hal provides the serial/gpio +
# FreeRTOS-shim headers the driver's #includes resolve against.
set(RS485_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../test/reference-driver")

idf_component_register(
    SRCS "${RS485_DIR}/hisense_rs485.cpp"
    INCLUDE_DIRS "${RS485_DIR}"
    REQUIRES hisense_hal esp_system
)

# ESP-IDF xTaskCreate stack depth is in BYTES (not words like AmebaZ2). Give the bus
# task 4KB so its status callback (printf) doesn't overflow -> double-exception crash.
target_compile_definitions(${COMPONENT_LIB} PRIVATE HISENSE_BUS_TASK_STACK=4096)
