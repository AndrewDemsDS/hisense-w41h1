// Host test stubs for the Ameba mbed HAL + FreeRTOS, so hisense_rs485.cpp can
// be compiled and its pure functions (build_command / build_power_frame /
// build_status_request / parse_status / checksum / stuffing) unit-tested on a
// PC. The RX/TX task machinery is NOT exercised here (that's FreeRTOS/IRQ glue,
// covered by the virtual_ac.py integration path); these stubs just satisfy the
// linker so the codec functions can run.
#pragma once
#include <cstdint>
#include <cstring>
#include <cstddef>

typedef struct serial_s { int x; } serial_t;
typedef int PinName;
#define PA_14 14
#define PA_13 13
enum SerialParity { ParityNone = 0 };
enum SerialIrq { RxIrq, TxIrq };
typedef void (*uart_irq_handler)(uint32_t id, SerialIrq event);
// GPIO types for the half-duplex DE line (PA_17)
#define PA_17 17
typedef struct gpio_s { int x; } gpio_t;
enum PinDirection { PIN_INPUT = 0, PIN_OUTPUT };
enum PinMode { PullNone = 0, PullUp = 1, PullDown = 2 };
extern "C" {
inline void serial_init(serial_t*, PinName, PinName) {}
inline void serial_free(serial_t*) {}
inline void serial_baud(serial_t*, int) {}
inline void serial_format(serial_t*, int, SerialParity, int) {}
inline void serial_irq_handler(serial_t*, uart_irq_handler, uint32_t) {}
inline void serial_irq_set(serial_t*, SerialIrq, uint32_t) {}
inline int  serial_getc(serial_t*) { return 0; }
inline void serial_putc(serial_t*, int) {}
inline int  serial_readable(serial_t*) { return 0; }
inline void gpio_init(gpio_t*, PinName) {}
inline void gpio_dir(gpio_t*, PinDirection) {}
inline void gpio_mode(gpio_t*, PinMode) {}
inline void gpio_write(gpio_t*, int) {}
}
typedef long BaseType_t; typedef unsigned long TickType_t;
typedef void* TaskHandle_t; typedef void* QueueHandle_t;
#define pdPASS 1
#define pdFAIL 0
#define pdTRUE 1
#define tskIDLE_PRIORITY 0
#define pdMS_TO_TICKS(x) ((TickType_t)(x))
inline void vTaskDelay(TickType_t) {}
inline TickType_t xTaskGetTickCount() { return 0; }
inline BaseType_t xTaskCreate(void(*)(void*), const char*, unsigned short, void*, unsigned, TaskHandle_t*) { return pdPASS; }
inline void vTaskDelete(TaskHandle_t) {}
inline QueueHandle_t xQueueCreate(unsigned long, unsigned long) { return (QueueHandle_t)1; }
inline void vQueueDelete(QueueHandle_t) {}
inline BaseType_t xQueueSend(QueueHandle_t, const void*, TickType_t) { return pdTRUE; }
inline BaseType_t xQueueReceive(QueueHandle_t, void*, TickType_t) { return pdTRUE; }
