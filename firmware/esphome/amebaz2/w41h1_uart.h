// Serial port recovery for the A/C bus on the stock AEH-W41H1 module under LibreTiny (UART0).
//
// What was seen, on one module in an A/C, 2026-10-10: twice the receive side of the bus port
// stopped and stayed stopped until the unit was power cycled. Once within the first minute of the
// first boot, once right after the first command sent to a unit that had been idle for five
// minutes. Transmit kept working both times: the unit obeyed commands while the node reported
// the link as lost.
//
// A cause that gives exactly this, found on a second module the same day: LibreTiny 1.13.0 keeps
// received bytes in the Arduino RingBufferN. Its reader does `_numElems--` with the receive
// interrupt enabled, and the interrupt does `_numElems++`. When the interrupt lands between the
// load and the store of that decrement, the byte it stored is never counted. The byte stays in
// the buffer, the count says one less, and from then on every frame is read one byte late and
// one byte short. The frame assembler never completes a frame again: reply timeouts, no checksum
// error, transmit fine.
// Measured with the UART in internal loopback (the real interrupt and buffer, nothing on the bus):
//   - 115200 baud, single reads at random times, no lock: the count fell behind 21 times in
//     142558 bytes (first after 0.8 s) and stayed behind. With interrupts masked around the read
//     (PRIMASK, or ESPHome's InterruptLock): 0 times in 142551 and 142485 bytes.
//   - 9600 baud, reads in groups every 2 to 10 ms as the main loop does, no lock: 5 times in
//     233034 bytes, one a minute. With InterruptLock: 0 times in 233287 bytes.
// No byte was lost or out of order in any run. The component now reads under InterruptLock on
// LibreTiny (HisenseAC::bus_read), which removes the cause. Reopening the port also clears a
// buffer that has fallen behind, because begin() makes a new one: that is why the restart below
// brought the link back.
//
// The restart stays as a fallback for a stall with any other cause. It cannot help a unit that
// does not answer at all: on the second module the bus went silent at the pin itself (no low
// level on RX in 2.3 million samples after our frames), and neither about 100 port restarts,
// eight software resets nor a power-on reset brought a reply.
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
