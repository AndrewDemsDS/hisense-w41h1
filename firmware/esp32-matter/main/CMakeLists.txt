idf_component_register(
    SRCS "app_main.cpp"
         "diag_console.cpp"
         "../../src/sdk-edits/ElectricalPowerMeasurementDelegate.cpp"
    INCLUDE_DIRS "." "../../src/sdk-edits"
    REQUIRES esp_matter hisense_rs485 hisense_hal esp_https_ota esp_http_client
             console esp_netif
)
