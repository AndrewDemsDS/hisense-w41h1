#pragma once
// The driver includes FreeRTOS as <FreeRTOS.h>/<task.h>/<queue.h> (AmebaZ2 layout).
// ESP-IDF ships the same FreeRTOS under freertos/. These three shims redirect so
// the driver's includes resolve unchanged.
#include "freertos/FreeRTOS.h"
