# Release flavour is the default. The diagnostic console compiles ONLY into the debug flavour
# (CONFIG_HISENSE_DEBUG_BUILD): it listens on :2323 with no authentication and can drive the A/C
# bus, so a shipped image must not carry it. Gating the source here rather than #ifdef-ing the
# whole file also keeps the console's `console` component dependency out of the release image.
set(HISENSE_SRCS
    "app_main.cpp"
    "../../src/sdk-edits/ElectricalPowerMeasurementDelegate.cpp")
set(HISENSE_REQUIRES
    esp_matter hisense_rs485 hisense_hal esp_https_ota esp_http_client esp_netif)

if(CONFIG_HISENSE_DEBUG_BUILD)
    list(APPEND HISENSE_SRCS "diag_console.cpp")
    list(APPEND HISENSE_REQUIRES console)
    message(STATUS "hisense: DEBUG flavour -- diagnostic console INCLUDED (:2323, unauthenticated)")
else()
    message(STATUS "hisense: release flavour -- diagnostic console excluded")
endif()

idf_component_register(
    SRCS ${HISENSE_SRCS}
    INCLUDE_DIRS "." "../../src/sdk-edits"
    REQUIRES ${HISENSE_REQUIRES}
)
