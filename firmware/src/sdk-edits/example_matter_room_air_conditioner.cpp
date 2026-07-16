#include <FreeRTOS.h>
#include <task.h>
#include <platform/platform_stdlib.h>
#include <basic_types.h>
#include <platform_opts.h>
#include <section_config.h>
#include <wifi_constants.h>
#include <wifi/wifi_conf.h>
#include <chip_porting.h>
#include <matter_core.h>
#include <matter_drivers.h>
#include <matter_interaction.h>
#include <serial_api.h>

#if defined(CONFIG_EXAMPLE_MATTER_ROOM_AIR_CONDITIONER) && CONFIG_EXAMPLE_MATTER_ROOM_AIR_CONDITIONER

#if defined(HISENSE_DIAG_SMOKE_TEST)
// --- Phase-A TX SMOKE TEST (build with -DHISENSE_DIAG_SMOKE_TEST only) ---
// Transmit a recognizable pattern on the A/C UART (PA_14) at boot, independent of
// Wi-Fi/Matter, to prove serial_init/serial_putc actually output on that pin. Watch
// the RS-485 A/B bus for "F4 F5 44 49 41 47 F4 FB".
// NOT for production: this owns UART0 (PA_14/PA_13) forever, the same bus the RS-485
// driver drives -- leaving it enabled makes two tasks write one UART with no lock and
// spams the A/C bus every 500 ms. Only enable it to build a dedicated diag image.
static void hisense_diag_task(void *pvParameters)
{
    serial_t dbg;
    serial_init(&dbg, PA_14, PA_13);
    serial_baud(&dbg, 9600);
    serial_format(&dbg, 8, ParityNone, 1);
    static const unsigned char pat[] = {0xF4, 0xF5, 0x44, 0x49, 0x41, 0x47, 0xF4, 0xFB};
    while (1) {
        for (unsigned i = 0; i < sizeof(pat); i++) {
            serial_putc(&dbg, pat[i]);
        }
        vTaskDelay(500);
    }
}
#endif /* HISENSE_DIAG_SMOKE_TEST */

static void example_matter_room_air_conditioner_task(void *pvParameters)
{
    // Bound the Wi-Fi wait. EVERYTHING below (Matter core, the RS-485 "77" recovery
    // handler, both interaction tasks) is gated here, so an infinite wait turns a
    // corrupt/rotated stored Wi-Fi profile into a permanent dead-end with no way in --
    // field-confirmed. After the timeout start Matter anyway: BLE/PASE commissioning is
    // the normal path and does NOT need Wi-Fi already up, so a recovery/commissioning
    // window stays reachable. Healthy boots exit immediately -> no-op for them.
    int wifi_wait = 0;
    while (!(wifi_is_up(RTW_STA_INTERFACE) || wifi_is_up(RTW_AP_INTERFACE)))
    {
        if (++wifi_wait > 240)   // 240 * 500ms = 120s
        {
            ChipLogError(DeviceLayer, "Wi-Fi not up after 120s -- starting Matter anyway for BLE recovery/commissioning");
            break;
        }
        vTaskDelay(500);
    }

    ChipLogProgress(DeviceLayer, "Matter Room Air-Conditioner Example!");

    CHIP_ERROR err = CHIP_NO_ERROR;

    err = matter_core_start();
    if (err != CHIP_NO_ERROR)
    {
        ChipLogProgress(DeviceLayer, "matter_core_start failed!");
    }

    err = matter_driver_room_aircon_init();
    if (err != CHIP_NO_ERROR)
    {
        ChipLogProgress(DeviceLayer, "matter_driver_room_aircon_init failed!");
    }

    err = matter_interaction_start_downlink();
    if (err != CHIP_NO_ERROR)
    {
        ChipLogProgress(DeviceLayer, "matter_interaction_start_downlink failed!\n");
    }

    err = matter_interaction_start_uplink();
    if (err != CHIP_NO_ERROR)
    {
        ChipLogProgress(DeviceLayer, "matter_interaction_start_uplink failed!\n");
    }

    vTaskDelete(NULL);
}

extern "C" void example_matter_room_air_conditioner(void)
{
#if defined(HISENSE_DIAG_SMOKE_TEST)
    // Phase-A only: start the PA_14 test-transmit immediately (before Wi-Fi/Matter).
    // Never enabled in production -- it shares UART0 with the RS-485 driver.
    if (xTaskCreate(hisense_diag_task, ((const char*)"hisense_diag_task"), 512, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS)
    {
        ChipLogProgress(DeviceLayer, "%s xTaskCreate(hisense_diag_task) failed", __FUNCTION__);
    }
#endif /* HISENSE_DIAG_SMOKE_TEST */
    if (xTaskCreate(example_matter_room_air_conditioner_task, ((const char*)"example_matter_room_air_conditioner_task"), 2048, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS)
    {
        ChipLogProgress(DeviceLayer, "%s xTaskCreate(example_matter_room_air_conditioner_task) failed", __FUNCTION__);
    }
}

#endif /* CONFIG_EXAMPLE_MATTER_ROOM_AIR_CONDITIONER */
