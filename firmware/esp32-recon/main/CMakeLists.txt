idf_component_register(
    SRCS "app_main.c"
         "recon_console.c"
         "recon_net.c"
         "recon_tap.c"
         "recon_selftest.c"
    INCLUDE_DIRS "."
    REQUIRES hisense_rs485 hisense_hal console
             esp_wifi esp_netif esp_event nvs_flash driver
)
