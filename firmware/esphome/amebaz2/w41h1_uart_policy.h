// When to restart the A/C bus serial port on the stock AEH-W41H1 module under LibreTiny.
//
// Plain integers only, no Arduino and no ESPHome, so the host tests compile it as it is
// (firmware/test/test_esphome_uart_policy.cpp). The restart itself is in w41h1_uart.h.
#pragma once
#include <stdint.h>

// While the link stays down the port is restarted this often. A restart costs one poll at most.
// A unit that is switched off at the mains while the node stays powered gets a restart four
// times a minute for nothing, which is harmless.
static const uint32_t W41H1_UART_RETRY_MS = 15000;

struct W41h1UartWatch {
  bool started{false};   // the retry clock is running
  bool was_up{false};    // the link was up at the previous look
  uint32_t last_ms{0};   // when the link dropped, or the last restart
  uint32_t restarts{0};  // for the diagnostic sensor
};

// Call about once a second with the hub's link state. True means: restart the port now.
//   - the link has just dropped: at once, since a stopped receiver is the one cause a restart cures;
//   - the link is still down: again every W41H1_UART_RETRY_MS;
//   - the link has never been up (boot, or a unit that is off): the first restart comes one retry
//     period after the first look, not at once, so a healthy boot is left alone.
// Elapsed-time arithmetic, so the 49.7-day millis() wrap needs no special case.
static inline bool w41h1_uart_restart_due(W41h1UartWatch *w, bool link_up, uint32_t now_ms) {
  if (link_up) {
    w->was_up = true;
    w->started = true;
    return false;
  }
  if (w->was_up) {
    w->was_up = false;
    w->last_ms = now_ms;
    w->restarts++;
    return true;
  }
  if (!w->started) {
    w->started = true;
    w->last_ms = now_ms;
    return false;
  }
  if (now_ms - w->last_ms < W41H1_UART_RETRY_MS)
    return false;
  w->last_ms = now_ms;
  w->restarts++;
  return true;
}
