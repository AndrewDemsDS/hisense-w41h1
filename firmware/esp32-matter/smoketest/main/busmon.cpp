// RS-485 bus monitor: runs the REAL Hisense driver (hisense_init -> UART + DE +
// bus task) against the actual A/C and prints each decoded status frame. This is
// the full-path on-target test: HAL UART TX/RX + DE half-duplex + frame reassembly
// + status parse, end-to-end against real hardware. No esp-matter yet.
//
// Wiring (PinNames.h): ESP32 TX=GPIO19->DI, RX=GPIO18<-RO, DE=GPIO4->DE+RE(tied);
// transceiver A/B -> A/C bus; COMMON GROUND between ESP32 and the A/C bus.
#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hisense_rs485.h"

static const char *TAG = "busmon";
static volatile int s_frames = 0;
// HAL diagnostic byte counters (defined in hisense_hal.c).
extern "C" {
extern volatile uint32_t g_hal_tx_bytes; extern volatile uint32_t g_hal_rx_bytes;
extern volatile uint32_t g_hal_evt_total; extern volatile uint32_t g_hal_evt_data;
extern volatile uint8_t  g_hal_rx_task_up;
}

static void on_status(const HisenseState *st)
{
    if (!st || !st->valid) return;
    s_frames++;
    ESP_LOGI(TAG, "A/C #%d: power=%d mode=%d set=%dC indoor=%dC outdoor=%dC "
                  "fan=0x%02x comp=%dHz eco=%d turbo=%d mute=%d sleep=%d vswing=%d",
             s_frames, st->power_on, st->mode, st->setpoint_c, st->indoor_temp_c,
             st->outdoor_temp_c, st->fan_raw, st->compressor_freq,
             st->eco_on, st->turbo_on, st->mute_on, st->sleep_on, st->vswing_on);
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== Hisense RS-485 bus monitor (ESP32-D0WDQ6, TX=19 RX=18 DE=4) ===");
    ESP_LOGI(TAG, "starting driver -> DevType handshake + ~1Hz poll of the A/C...");
    if (hisense_init(on_status) != pdPASS) {
        ESP_LOGE(TAG, "hisense_init FAILED");
        return;
    }
    int t = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        t += 3;
        ESP_LOGW(TAG, "t=%ds: frames=%d TX=%u RX=%u | rx_task_up=%u uart_evts=%u data_evts=%u",
                 t, s_frames, (unsigned)g_hal_tx_bytes, (unsigned)g_hal_rx_bytes,
                 (unsigned)g_hal_rx_task_up, (unsigned)g_hal_evt_total, (unsigned)g_hal_evt_data);
    }
}
