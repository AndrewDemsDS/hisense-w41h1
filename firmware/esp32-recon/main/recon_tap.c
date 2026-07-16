// esp32-recon TAP mode: passive, listen-only. Drives nothing (DE held LOW), owns
// UART1 directly (the hisense_hal/driver is NOT initialized in tap mode, so there's
// no contention), reassembles F4 F5 .. F4 FB frames in BOTH directions (no buf[2]
// gate) and hands A/C->module status/producttype frames to the pure parsers.
//
// This is the ground-truth check: point it at the stock dongle <-> A/C (or the
// kitchen AmebaZ2 <-> A/C) bus and watch our decode against real traffic.
#include <string.h>

#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "PinNames.h"   // PA_14=19 (TX/DI), PA_13=18 (RX/RO), PA_17=4 (DE) — from hisense_hal
#include "recon.h"

static const char *TAG = "recon-tap";

#define TAP_UART      UART_NUM_1
#define TAP_TX_PIN    PA_14
#define TAP_RX_PIN    PA_13
#define TAP_DE_PIN    PA_17
#define TAP_BAUD      9600

// ---- frame reassembler (both directions; un-stuffs doubled F4) --------------
enum { S_IDLE, S_SF4, S_BODY, S_BODY_F4 };
static uint8_t s_buf[256];
static int     s_len;
static int     s_state = S_IDLE;

static void classify(const uint8_t *f, int len)
{
    if (len < 15) return;                       // too short for a header
    uint8_t dir = f[2];                          // 0x00 module->A/C, 0x01 A/C->module
    uint8_t cls = f[13];

    if (dir == 0x01 && cls == 0x66) {
        HisenseState st;
        HisenseFeatures ft;
        if (hisense_parse_features(f, (size_t)len, &ft) && ft.valid) {
            recon_watch_note_raw("producttype", f, len);
        } else if (hisense_parse_status(f, (size_t)len, &st) && st.valid) {
            recon_note_raw(0, f, len);          // retain for snap/diff
            recon_on_status(&st);               // stats + watch stream + verify
        } else {
            recon_note_chkfail();               // a 0x66 status/feature that didn't validate
            recon_watch_note_raw("bad-0x66", f, len);
        }
        return;
    }
    if (dir == 0x00) { recon_note_cmd_frame(); recon_note_raw(1, f, len); }  // module->A/C
    recon_watch_note_raw(dir == 0x00 ? "cmd" : "link", f, len);
}

static void feed(uint8_t b)
{
    switch (s_state) {
    case S_IDLE:
        if (b == 0xF4) s_state = S_SF4;
        break;
    case S_SF4:
        if (b == 0xF5)      { s_buf[0] = 0xF4; s_buf[1] = 0xF5; s_len = 2; s_state = S_BODY; }
        else if (b == 0xF4) { s_state = S_SF4; }         // re-sync on F4 F4..
        else                { s_state = S_IDLE; }
        break;
    case S_BODY:
        if (b == 0xF4) s_state = S_BODY_F4;              // pending: stuffed F4 or end tag
        else if (s_len < (int)sizeof(s_buf)) s_buf[s_len++] = b;
        else s_state = S_IDLE;                            // overflow -> resync
        break;
    case S_BODY_F4:
        if (b == 0xFB) {                                 // F4 FB = end
            if (s_len + 2 <= (int)sizeof(s_buf)) {
                s_buf[s_len++] = 0xF4; s_buf[s_len++] = 0xFB;
                classify(s_buf, s_len);
            }
            s_state = S_IDLE;
        } else if (b == 0xF4) {                          // F4 F4 = literal F4
            if (s_len < (int)sizeof(s_buf)) s_buf[s_len++] = 0xF4;
            s_state = S_BODY;
        } else if (b == 0xF5) {                          // F4 F5 mid-stream = new frame
            s_buf[0] = 0xF4; s_buf[1] = 0xF5; s_len = 2; s_state = S_BODY;
        } else {                                         // F4 then other = two body bytes
            if (s_len + 2 <= (int)sizeof(s_buf)) { s_buf[s_len++] = 0xF4; s_buf[s_len++] = b; }
            s_state = S_BODY;
        }
        break;
    }
}

static void tap_task(void *arg)
{
    (void)arg;
    uint8_t rx[128];
    ESP_LOGI(TAG, "passive tap up on UART1 (RX=%d), DE=%d held LOW", TAP_RX_PIN, TAP_DE_PIN);
    for (;;) {
        int n = uart_read_bytes(TAP_UART, rx, sizeof(rx), pdMS_TO_TICKS(200));
        for (int i = 0; i < n; i++) feed(rx[i]);
    }
}

void recon_tap_start(void)
{
    // DE (GPIO4) as output, driven LOW: the transceiver stays receive-only. We
    // never transmit in tap mode, so the bus is never energized by us.
    gpio_config_t de = {
        .pin_bit_mask = 1ULL << (uint32_t)TAP_DE_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0, .pull_down_en = 0, .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&de);
    gpio_set_level((gpio_num_t)TAP_DE_PIN, 0);

    uart_config_t cfg = {
        .baud_rate = TAP_BAUD, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(TAP_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(TAP_UART, TAP_TX_PIN, TAP_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(TAP_UART, 1024, 0, 0, NULL, 0));

    xTaskCreate(tap_task, "recon_tap", 4096, NULL, 10, NULL);
}
