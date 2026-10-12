# ESP-IDF component: mbed-style serial/gpio HAL backing the reused Hisense driver.
idf_component_register(
    SRCS "hisense_hal.c"
    INCLUDE_DIRS "include"
    REQUIRES driver
)
