#pragma once
// Minimal mbed-style serial surface the Hisense driver uses, implemented over
// ESP-IDF UART in hisense_hal.c. Mirrors ../../../test/stubinc/serial_api.h.
// Enums are typedef'd so the bare names work in both C (hisense_hal.c) and C++
// (hisense_rs485.cpp).
#include <stdint.h>
#include "PinNames.h"

typedef struct serial_s { PinName tx; PinName rx; } serial_t;
typedef enum { ParityNone = 0, ParityOdd = 1, ParityEven = 2 } SerialParity;
typedef enum { RxIrq = 0, TxIrq = 1 } SerialIrq;
typedef void (*uart_irq_handler)(uint32_t id, SerialIrq event);

#ifdef __cplusplus
extern "C" {
#endif
void serial_init(serial_t *obj, PinName tx, PinName rx);
void serial_free(serial_t *obj);
void serial_baud(serial_t *obj, int baud);
void serial_format(serial_t *obj, int data_bits, SerialParity parity, int stop_bits);
void serial_irq_handler(serial_t *obj, uart_irq_handler handler, uint32_t id);
void serial_irq_set(serial_t *obj, SerialIrq irq, uint32_t enable);
int  serial_getc(serial_t *obj);
void serial_putc(serial_t *obj, int c);
int  serial_readable(serial_t *obj);
#ifdef __cplusplus
}
#endif
