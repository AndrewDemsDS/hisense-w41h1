// esp32-recon — independent RS-485 A/C bus diagnostic tool (no Matter).
//
// Boot flow: NVS -> netif/event loop -> core state -> select capture mode
// (tap|master, from NVS, default tap = passive/safe) -> esp_console + commands ->
// UART0 REPL (USB-serial fallback) -> WiFi (if creds in NVS) + mDNS + TCP :2323
// remote console. Reach it remotely with:  nc esp32-recon.local 2323
//
// Wiring (from hisense_hal/PinNames.h): UART1 TX=GPIO19->DI, RX=GPIO18<-RO,
// DE=GPIO4->DE+RE(tied); transceiver A/B -> A/C bus; COMMON GROUND is mandatory.
#include <stdio.h>

#include "esp_log.h"
#include "esp_console.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "recon.h"

static const char *TAG = "recon";

// Driver status callback (master mode). Just funnels into the shared core.
static void on_status(const HisenseState *st) { recon_on_status(st); }

void app_main(void)
{
    // NVS: capture mode + WiFi creds live here.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    recon_core_init();

    recon_mode_t mode = recon_mode_get();
    ESP_LOGI(TAG, "=== esp32-recon (ESP32-D0WDQ6) — RS-485 A/C diagnostic ===");
    ESP_LOGI(TAG, "capture mode = %s  (UART1 TX=19 RX=18 DE=4, 9600 8N1)", recon_mode_str(mode));

    if (mode == RECON_MODE_MASTER) {
        ESP_LOGI(TAG, "MASTER: driving link + ~1Hz poll; commands enabled");
        if (hisense_init(on_status) != pdPASS) {
            ESP_LOGE(TAG, "hisense_init FAILED — staying up so the console still works");
        }
    } else {
        ESP_LOGI(TAG, "TAP: passive listen-only (DE held low); decoding both directions");
        recon_tap_start();
    }

    // Shared console command set, used by both the UART REPL and the TCP server.
    esp_console_config_t cc = {
        .max_cmdline_length = 256,
        .max_cmdline_args   = 12,
    };
    ESP_ERROR_CHECK(esp_console_init(&cc));
    esp_console_register_help_command();
    recon_register_commands();

    // USB-serial (UART0) REPL — always available, even if WiFi is down. This is
    // where you first provision WiFi:  wifi <ssid> <pass>
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "esp32-recon>";
    rc.max_cmdline_length = 256;
    esp_console_dev_uart_config_t uc = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    if (esp_console_new_repl_uart(&uc, &rc, &repl) == ESP_OK) {
        esp_console_start_repl(repl);
    } else {
        ESP_LOGW(TAG, "UART REPL init failed (continuing; TCP console may still work)");
    }

    // WiFi (if provisioned) + mDNS + TCP :2323 remote console.
    recon_net_start();

    ESP_LOGI(TAG, "ready. UART console up; remote: nc esp32-recon.local 2323");
}
