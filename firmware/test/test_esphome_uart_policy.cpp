// When the stock module's ESPHome build restarts the A/C bus serial port
// (firmware/esphome/amebaz2/w41h1_uart_policy.h). The restart is the recovery for a receiver that
// stops while transmit keeps working, seen twice on one module on 2026-10-10 and reproduced there
// by switching the receive interrupt off: link lost after 5 silent polls, back 0.6 s after the
// restart. These checks pin the timing of that decision, not the port.

#include <cstdint>
#include <cstdio>

#include "w41h1_uart_policy.h"

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(cond, ...) \
  do { \
    g_checks++; \
    if (!(cond)) { \
      g_fail++; \
      printf("  FAIL %s:%d ", __FILE__, __LINE__); \
      printf(__VA_ARGS__); \
      printf("\n"); \
    } \
  } while (0)

// Looks once a second from `from` to `to` (exclusive) with the link in one state. Returns the
// number of restarts asked for and stores when the first one came.
static int look(W41h1UartWatch *w, bool up, uint32_t from, uint32_t to, uint32_t *first) {
  int n = 0;
  for (uint32_t t = from; t != to; t += 1000) {
    if (w41h1_uart_restart_due(w, up, t)) {
      if (n == 0 && first != nullptr)
        *first = t;
      n++;
    }
  }
  return n;
}

int main() {
  printf("== AmebaZ2 serial port restart policy ==\n");

  // A healthy bus: up from the first frame, never restarted.
  {
    W41h1UartWatch w;
    CHECK(look(&w, false, 0, 3000, nullptr) == 0, "the seconds before the first frame cause no restart");
    CHECK(look(&w, true, 3000, 3600000, nullptr) == 0, "an hour of link up causes no restart");
    CHECK(w.restarts == 0, "counter stays at 0");
  }

  // The fault: link up, then lost. Restart at once, then every retry period while it stays down.
  {
    W41h1UartWatch w;
    look(&w, true, 0, 60000, nullptr);
    CHECK(w41h1_uart_restart_due(&w, false, 60000), "the look that finds the link lost restarts the port");
    CHECK(w.restarts == 1, "and counts it");
    uint32_t first = 0;
    CHECK(look(&w, false, 61000, 75000, &first) == 0, "no second restart inside the retry period");
    CHECK(look(&w, false, 75000, 76000, &first) == 1 && first == 75000, "the next one comes 15 s after the first");
    CHECK(look(&w, false, 76000, 136000, nullptr) == 4, "then four a minute while the link stays down");
    CHECK(w.restarts == 6, "counter follows (got %u)", (unsigned) w.restarts);
    // The restart worked: quiet again, and a later loss restarts at once again.
    CHECK(look(&w, true, 136000, 200000, nullptr) == 0, "link back: no restart");
    CHECK(w41h1_uart_restart_due(&w, false, 200000), "a second loss restarts at once");
  }

  // The first-boot case: the receiver never delivers a frame. First restart one period after the
  // first look, so a boot that is merely slow is left alone.
  {
    W41h1UartWatch w;
    uint32_t first = 0;
    CHECK(look(&w, false, 5000, 60000, &first) == 3, "a link that never came up is restarted every period");
    CHECK(first == 5000 + W41H1_UART_RETRY_MS, "first at one period after the first look (got %u)", (unsigned) first);
  }

  // millis() wraps after 49.7 days: the retry period spans the wrap.
  {
    W41h1UartWatch w;
    const uint32_t near_wrap = 0xFFFFFFFFu - 4999;  // 5 s before the wrap
    w41h1_uart_restart_due(&w, true, near_wrap - 1000);
    CHECK(w41h1_uart_restart_due(&w, false, near_wrap), "loss just before the wrap restarts at once");
    CHECK(!w41h1_uart_restart_due(&w, false, near_wrap + 14000), "14 s later, across the wrap: not yet");
    CHECK(w41h1_uart_restart_due(&w, false, near_wrap + 15000), "15 s later, across the wrap: restart");
  }

  printf("  %d checks, %d failed\n", g_checks, g_fail);
  printf(g_fail ? "== UART POLICY FAILED ==\n" : "== UART POLICY OK ==\n");
  return g_fail ? 1 : 0;
}
