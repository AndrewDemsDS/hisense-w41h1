// ESP-IDF implementation of the mbed-style serial/gpio HAL that
// hisense_rs485.cpp expects. Mirrors ../../test/hal_stub.h (the host-test HAL
// surface) but backed by real ESP-IDF UART + GPIO, so hisense_rs485.{h,cpp} are
// reused UNCHANGED. FreeRTOS (tasks/queues) is native on ESP-IDF; only UART + the
// DE GPIO are shimmed here.
//
// RX model: the driver installs an RxIrq handler (serial_irq_handler) and expects
// it to be invoked when bytes arrive, then drains via serial_readable()/serial_getc().
// AmebaZ2 does this from a true ISR. On ESP-IDF we replicate the *contract* with a
// "soft IRQ": a task blocks on the UART event queue and calls the handler, which
// drains the ESP-IDF UART RX buffer via serial_getc(). Same producer/consumer shape
// the driver's volatile ring buffer was designed for.
#include "serial_api.h"
#include "gpio_api.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#ifndef HAL_UART_NUM
#define HAL_UART_NUM      UART_NUM_1
#endif
#define HAL_UART_RX_BUF   512
#define HAL_UART_EVT_QLEN 20

static uart_irq_handler s_handler    = 0;
static uint32_t         s_handler_id = 0;
static QueueHandle_t    s_evt_q      = 0;
static TaskHandle_t     s_rx_task    = 0;

// Diagnostic counters (read by the bus monitor to see what's on the wire).
volatile uint32_t g_hal_tx_bytes  = 0;
volatile uint32_t g_hal_rx_bytes  = 0;
volatile uint32_t g_hal_evt_total = 0;   // any UART event dequeued
volatile uint32_t g_hal_evt_data  = 0;   // UART_DATA events
volatile uint8_t  g_hal_rx_task_up = 0;  // RX task reached its loop

// The driver's RxIrq handler drains via serial_readable()/serial_getc(); we just
// hand it control when the ESP-IDF UART reports data (or FIFO overflow).
static void hal_uart_rx_task(void *arg)
{
    g_hal_rx_task_up = 1;
    uart_event_t ev;
    for (;;) {
        if (xQueueReceive(s_evt_q, &ev, portMAX_DELAY)) {
            g_hal_evt_total++;
            switch (ev.type) {
            case UART_DATA:
                g_hal_evt_data++;
                if (s_handler) s_handler(s_handler_id, RxIrq);
                break;
            case UART_FIFO_OVF:
            case UART_BUFFER_FULL:
                uart_flush_input(HAL_UART_NUM);
                xQueueReset(s_evt_q);
                break;
            default:
                break;
            }
        }
    }
}

// ---------- serial ----------
void serial_init(serial_t *obj, PinName tx, PinName rx)
{
    obj->tx = tx;
    obj->rx = rx;
    // Config is finalized in serial_baud()/serial_format(); driver calls those next.
}

void serial_baud(serial_t *obj, int baud)
{
    uart_config_t cfg = {
        .baud_rate  = baud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(HAL_UART_NUM, &cfg);
    uart_set_pin(HAL_UART_NUM, obj->tx, obj->rx,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (!s_evt_q) {
        uart_driver_install(HAL_UART_NUM, HAL_UART_RX_BUF, 0,
                            HAL_UART_EVT_QLEN, &s_evt_q, 0);
    }
#ifdef HISENSE_HAL_UART_INTERNAL_LOOPBACK
    // DIAGNOSTIC ONLY: internally wire this UART's TX to its own RX inside the
    // chip, so a transmit must show up as a receive if the RX path is healthy.
    uart_set_loop_back(HAL_UART_NUM, true);
#endif
}

void serial_format(serial_t *obj, int data_bits, SerialParity parity, int stop_bits)
{
    (void)obj;
    uart_set_word_length(HAL_UART_NUM,
        data_bits == 8 ? UART_DATA_8_BITS : UART_DATA_7_BITS);
    uart_set_parity(HAL_UART_NUM,
        parity == ParityNone ? UART_PARITY_DISABLE : UART_PARITY_EVEN);
    uart_set_stop_bits(HAL_UART_NUM,
        stop_bits == 1 ? UART_STOP_BITS_1 : UART_STOP_BITS_2);
}

void serial_irq_handler(serial_t *obj, uart_irq_handler handler, uint32_t id)
{
    (void)obj;
    s_handler    = handler;
    s_handler_id = id;
}

void serial_irq_set(serial_t *obj, SerialIrq irq, uint32_t enable)
{
    (void)obj;
    if (irq == RxIrq && enable && !s_rx_task) {
        // Priority above the driver's bus task (tskIDLE_PRIORITY+2) so RX isn't starved.
        xTaskCreate(hal_uart_rx_task, "hal_uart_rx", 3072, 0, 12, &s_rx_task);
    }
}

int serial_readable(serial_t *obj)
{
    (void)obj;
    size_t n = 0;
    uart_get_buffered_data_len(HAL_UART_NUM, &n);
    return n > 0;
}

int serial_getc(serial_t *obj)
{
    (void)obj;
    uint8_t b = 0;
    // Only called after serial_readable()==true, so data is present; 0 timeout.
    uart_read_bytes(HAL_UART_NUM, &b, 1, 0);
    g_hal_rx_bytes++;
    return b;
}

void serial_putc(serial_t *obj, int c)
{
    (void)obj;
    uint8_t b = (uint8_t)c;
    uart_write_bytes(HAL_UART_NUM, &b, 1);
    g_hal_tx_bytes++;
    // Block until the byte has actually left the wire. uart_write_bytes() only
    // queues to the TX FIFO; hisense_tx_raw() drops the DE line right after its
    // putc loop, so without this the tail of the frame would be cut off while DE
    // is already low (RS-485 half-duplex). At 9600 baud this is ~1ms/byte.
    uart_wait_tx_done(HAL_UART_NUM, pdMS_TO_TICKS(20));
}

void serial_free(serial_t *obj)
{
    (void)obj;
    if (s_rx_task) { vTaskDelete(s_rx_task); s_rx_task = 0; }
    if (s_evt_q)   { uart_driver_delete(HAL_UART_NUM); s_evt_q = 0; }
}

// ---------- gpio (RS-485 DE line) ----------
void gpio_init(gpio_t *obj, PinName pin)
{
    obj->pin = pin;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << (uint32_t)pin,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

void gpio_dir(gpio_t *obj, PinDirection dir)
{
    gpio_set_direction((gpio_num_t)obj->pin,
        dir == PIN_OUTPUT ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT);
}

void gpio_mode(gpio_t *obj, PinMode mode) { (void)obj; (void)mode; }

void gpio_write(gpio_t *obj, int value)
{
    gpio_set_level((gpio_num_t)obj->pin, value ? 1 : 0);
}
