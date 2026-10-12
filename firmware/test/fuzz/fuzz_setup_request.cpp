// libFuzzer harness: the setup-request tracker (the remote's "77" gesture), fed arbitrary
// sequences of request bits, indicator states and time steps, starting anywhere on the 32 bit
// millisecond clock (so the 49.7 day wrap is crossed all the time).
//
// Input: byte 0 picks the clock's starting point and whether the boot lockout is armed, then
// 4 bytes per step: [control, request byte, dt high, dt low].
//   control bit 0: the module's provisioning flag is on the wire (`indicating`)
//   control bit 1: the indication was switched off just before this reply (setup_request_lockout)
//   control bit 2: dt is in units of 64 ms instead of 1 ms (reaches past the 10 s lockout)
//
// The harness keeps its own 64 bit clock and a few counters, and checks what the tracker returns
// against the rules written in hisense_protocol.h:
//  - the result is 0 or request bits that were in this reply (masked to bits 3 and 5);
//  - bit 5 (smart-config) is never taken within 10 s of an accepted request or of a lockout;
//  - bit 3 (reconfigure) is never taken while the module's own flag is on the wire, never before
//    a reply without it was seen with the flag off, and only on the third reply in a row;
//  - an accepted request arms the lockout at the current time;
//  - liveness: a lone bit 5 after a quiet reply, with no lockout running, is taken at once.
// The two pure helpers under the tracker get the same treatment with arbitrary arguments.
#include "fuzz_common.h"

using namespace fz;

static void fuzz_helpers(Reader &r) {
  // setup_request_debounce with arbitrary state.
  uint8_t req = r.u8();
  uint8_t streak = r.u8();
  uint8_t streak0 = streak;
  bool latched = r.flag();
  bool latched0 = latched;
  uint8_t hold = r.u8();
  int e = H::setup_request_debounce(req, &streak, &latched, hold);
  FUZZ_CHECK(e >= -1 && e <= 1, "debounce returned %d", e);
  if (req == 0) {
    FUZZ_CHECK(streak == 0 && !latched && e == (latched0 ? -1 : 0), "debounce: a quiet reply clears and reports once");
  } else {
    FUZZ_CHECK(streak == (streak0 == 0xFF ? 0xFF : streak0 + 1), "debounce: streak saturates at 255");
    FUZZ_CHECK(e == ((streak >= hold && !latched0) ? 1 : 0), "debounce: rising edge exactly once");
    FUZZ_CHECK(latched == (latched0 || e == 1), "debounce: latch");
  }

  // smartcfg_lockout_mask with arbitrary arguments.
  uint8_t bits = r.u8();
  uint32_t now = r.u32();
  uint32_t since = r.u32();
  bool armed = r.flag();
  uint8_t out = H::smartcfg_lockout_mask(bits, now, armed, since);
  bool running = armed && static_cast<uint32_t>(now - since) < H::SETUP_REQUEST_LOCKOUT_MS;
  FUZZ_CHECK(out == (running ? (bits & ~0x20) : bits), "lockout mask: %02X -> %02X, running %d", bits, out, running);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  Reader r(data, size);
  uint8_t start = r.u8();
  uint64_t t;
  switch (start & 3) {
    case 0:
      t = 0;
      break;
    case 1:
      t = 0xFFFFE000ull;  // 8 s before the wrap
      break;
    case 2:
      t = 0x7FFFF000ull;  // around the sign bit, where a signed comparison would flip
      break;
    default:
      t = 0x12345678ull;
      break;
  }

  H::SetupRequestTracker tr;
  bool locked = false;
  uint64_t lock_at = 0;
  if ((start & 4) != 0) {  // the scheduler arms the lockout on its first poll
    H::setup_request_lockout(&tr, static_cast<uint32_t>(t));
    locked = true;
    lock_at = t;
  }
  bool quiet = false;  // a reply without bit 3 was seen while the flag was off the wire
  unsigned run3 = 0;   // replies in a row carrying bit 3 with the flag off
  uint8_t prev_eff = 0;

  if ((start & 8) != 0)
    fuzz_helpers(r);

  while (!r.empty()) {
    uint8_t ctl = r.u8();
    uint8_t raw = r.u8();
    uint64_t dt = r.u16();
    if ((ctl & 4) != 0)
      dt *= 64;
    t += dt;
    const uint32_t now = static_cast<uint32_t>(t);
    const bool indicating = (ctl & 1) != 0;

    if ((ctl & 2) != 0) {
      H::setup_request_lockout(&tr, now);
      locked = true;
      lock_at = t;
    }

    // The model's view of this reply, before the tracker sees it.
    if (indicating) {
      quiet = false;
    } else if ((raw & 0x08) == 0) {
      quiet = true;
    }
    run3 = ((raw & 0x08) != 0 && !indicating) ? run3 + 1 : 0;
    uint8_t eff = raw & 0x28;
    if (!quiet)
      eff &= static_cast<uint8_t>(~0x08);
    const bool lock_running = locked && (t - lock_at) < H::SETUP_REQUEST_LOCKOUT_MS;

    const uint8_t got = H::setup_request_step(&tr, raw, indicating, now);

    FUZZ_CHECK((got & ~0x28) == 0 && (got & ~raw) == 0, "result %02X is not a subset of the request %02X", got, raw);
    if ((got & 0x20) != 0)
      FUZZ_CHECK(!lock_running, "smart-config taken %llu ms into the 10 s lockout",
                 static_cast<unsigned long long>(t - lock_at));
    if ((got & 0x08) != 0) {
      FUZZ_CHECK(!indicating, "reconfigure taken while the module's own flag is on the wire");
      FUZZ_CHECK(quiet, "reconfigure taken before any reply without it");
      FUZZ_CHECK(run3 >= H::SETUP_REQUEST_HOLD_FRAMES, "reconfigure taken after %u replies in a row", run3);
    }
    if (got != 0) {
      FUZZ_CHECK(tr.lockout_armed && tr.lockout_since == now, "an accepted request did not arm the lockout");
      locked = true;
      lock_at = t;
    } else if (eff == 0x20 && prev_eff == 0 && !lock_running) {
      FUZZ_CHECK(false, "a lone smart-config pulse after a quiet reply, no lockout running, was not taken");
    }
    FUZZ_CHECK(tr.last_req == eff, "tracker's last request %02X, model %02X", tr.last_req, eff);
    prev_eff = eff;
  }
  return 0;
}
