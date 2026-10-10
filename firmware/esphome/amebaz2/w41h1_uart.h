// Serial port recovery for the A/C bus on the stock AEH-W41H1 module under LibreTiny (UART0).
//
// What was seen, on one module in an A/C, 2026-10-10: twice the receive side of the bus port
// stopped and stayed stopped until the unit was power cycled. Once within the first minute of the
// first boot, once right after the first command sent to a unit that had been idle for five
// minutes. Transmit kept working both times: the unit obeyed commands while the node reported
// the link as lost.
//
// What is known:
//   - Closing and reopening the port brings the receiver back. With the receive interrupt
//     switched off on purpose (UART0->ier_b.erbi = 0), the link was reported lost after 5 silent
//     polls and was back 0.6 s after the restart.
//   - LibreTiny 1.13.0 enables only the receive-data interrupt on this port and services no
//     line-status condition (cores/realtek-ambz2/arduino/libraries/Serial/Serial.cpp).
//
// What is not known: the cause. 115 forced preference writes to flash did not reproduce the
// stall, and neither did repeated commands. Whether a line error on the half-duplex bus, the
// flash driver or the SDK's interrupt handling stops the receiver has not been established, so
// this is a recovery, not a fix. The "Serial port restarts" sensor counts how often it fires.
#pragma once
#include <Arduino.h>

#include "w41h1_uart_policy.h"

static W41h1UartWatch w41h1_uart_watch;

// Tear the port down and bring it up again: pin mux, interrupt hook and FIFO all start fresh.
// Serial0 is the object ESPHome's uart component holds for PA14 / PA13, so the component keeps
// reading from the same receive buffer afterwards.
static inline void w41h1_uart_restart(uint32_t baud_rate) {
  Serial0.end();
  Serial0.begin(baud_rate, SERIAL_8N1);
}
