#pragma once
// Minimal mbed-style GPIO surface for the RS-485 DE line, implemented over
// ESP-IDF gpio in hisense_hal.c. Mirrors ../../../test/stubinc/gpio_api.h.
// Enums typedef'd so bare names work in both C and C++.
#include "PinNames.h"

typedef struct gpio_s { PinName pin; } gpio_t;
typedef enum { PIN_INPUT = 0, PIN_OUTPUT = 1 } PinDirection;
typedef enum { PullNone = 0, PullUp = 1, PullDown = 2 } PinMode;

#ifdef __cplusplus
extern "C" {
#endif
void gpio_init(gpio_t *obj, PinName pin);
void gpio_dir(gpio_t *obj, PinDirection dir);
void gpio_mode(gpio_t *obj, PinMode mode);
void gpio_write(gpio_t *obj, int value);
#ifdef __cplusplus
}
#endif
