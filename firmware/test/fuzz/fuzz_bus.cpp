// libFuzzer harness: the bus scheduler (hisense_bus.cpp) with the codec underneath it, driven by
// a scripted "A/C" that answers well, answers badly, answers late, sends line noise in arbitrary
// chunks, or stays silent, while time advances in arbitrary steps on a 32 bit clock that may
// start just before the wrap. This is where chunk sizes and timing matter: the assembler takes
// single bytes, but the scheduler decides when it listens, when it gives up and when it resets.
//
// Input: byte 0 = options (bit 0: the component drives DE; bits 1-2: clock start), then a list
// of operations [op, argument bytes...]:
//   0-2  advance the clock (1 ms, 8 ms or 64 ms units) and poll
//   3    the A/C sends up to 63 arbitrary bytes
//   4    the A/C sends a well-formed reply to the frame it last received, optionally damaged
//   5    the hub queues a command built by the codec (combined, power, mute or sleep)
//   6    the hub switches the setup indicator
//   7    the hub queues arbitrary bytes (off contract: enqueue() documents "a finished frame")
//
// Properties:
//  - memory safety everywhere, and every poll() returns;
//  - every write is 1..TX_FRAME_MAX + 2 bytes, happens with DE high (when the component owns DE),
//    and never starts while the previous frame is still shifting out at 9600 baud;
//  - until arbitrary bytes were queued, every frame written is one well-formed frame with a good
//    checksum, and carries the current link token in bytes 7/8 (the DevType probe excepted);
//  - on_bus_status and on_bus_features only follow a frame that passes the model's checks;
//    on_bus_checksum_error only a frame that fails them; on_bus_setup_request only a
//    checksum-good 0x1E reply, with bits that reply carried;
//  - the link token changes only to bytes 16/17 of a DevType reply just received, never to 00 00.
//  STRICT only (KNOWN_FAILURES: bus-token-no-checksum): that DevType reply passes its checksum.
#include "fuzz_common.h"
#include "hisense_bus.h"

using namespace fz;

struct Sim;

struct Io : public H::BusIO {
  Sim *sim{nullptr};
  uint8_t rx[2048];
  size_t head{0}, tail{0};
  bool de{false};
  bool has_de{true};
  void push(const uint8_t *d, size_t n) {
    for (size_t i = 0; i < n && tail < sizeof(rx); i++)
      rx[tail++] = d[i];
  }
  void bus_set_de(bool high) override;
  void bus_write(const uint8_t *data, size_t len) override;
  void bus_flush() override {}
  int bus_read() override {
    if (head == tail) {
      head = tail = 0;
      return -1;
    }
    return rx[head++];
  }
};

struct Sim : public H::BusListener {
  H::BusScheduler bus;
  Io io;
  uint32_t now{0};
  // Last frame the scheduler reported, and what the model thinks of it.
  uint8_t last[H::RX_FRAME_MAX];
  size_t last_len{0};
  bool last_ok{false};
  bool frame_since_poll{false};
  // Last frame written.
  uint8_t tx[80];
  size_t tx_len{0};
  uint32_t tx_at{0};
  bool wrote{false};
  bool raw_queued{false};
  uint8_t tok_hi{0x01}, tok_lo{0x01};

  void on_bus_frame(const uint8_t *frame, size_t len) override {
    FUZZ_CHECK(len >= 9 && len <= H::RX_FRAME_MAX && len == static_cast<size_t>(frame[4]) + 9, "frame length %zu", len);
    FUZZ_CHECK(
        frame[0] == 0xF4 && frame[1] == 0xF5 && frame[2] == 0x01 && frame[len - 2] == 0xF4 && frame[len - 1] == 0xFB,
        "reported frame breaks the framing rules");
    memcpy(last, frame, len);
    last_len = len;
    last_ok = model_frame_ok(frame, len);
    frame_since_poll = true;
  }
  void on_bus_status(const H::AcState &state) override {
    FUZZ_CHECK(state.valid && last_ok && last_len >= 45 && last[13] == 0x66 && last[14] != 0x40,
               "status published from a frame that is not a good 0x66 status (len %zu, ok %d)", last_len, last_ok);
  }
  void on_bus_features(const H::AcFeatures &features) override {
    FUZZ_CHECK(features.valid && last_ok && last[13] == 0x66 && last[14] == 0x40,
               "features published from a frame that is not a good 0x66/40 reply");
  }
  void on_bus_link(bool up) override { (void) up; }
  void on_bus_checksum_error(const uint8_t *frame, size_t len) override {
    FUZZ_CHECK(!H::status_checksum_ok(frame, len) && frame[13] == 0x66, "checksum error reported for a good frame");
  }
  void on_bus_setup_request(uint8_t request) override {
    FUZZ_CHECK(last_ok && last[13] == 0x1E && last_len >= 22, "setup request from a frame that is not a good 0x1E");
    FUZZ_CHECK(request != 0 && (request & ~(last[17] & 0x28)) == 0, "setup request %02X not in reply flags %02X",
               request, last[17]);
  }

  void poll() {
    frame_since_poll = false;
    bus.poll(now);
    // The stored copies are only ever a parsed, valid result.
    H::AcFaults fl;
    H::AcFeatures ft;
    FUZZ_CHECK(!bus.faults(&fl) || fl.valid, "faults() handed out an invalid struct");
    FUZZ_CHECK(!bus.features(&ft) || ft.valid, "features() handed out an invalid struct");
    uint8_t hi = 0, lo = 0;
    bool seen = bus.link_token(&hi, &lo);
    if (hi != tok_hi || lo != tok_lo) {
      FUZZ_CHECK(seen && frame_since_poll, "link token changed with no frame received");
      // The frame that ends a transaction is the last one reported in that poll, because the next
      // transaction starts by flushing the receive side.
      FUZZ_CHECK(last[13] == 0x0A && last_len > 17 && last[16] == hi && last[17] == lo,
                 "link token %02X %02X is not bytes 16/17 of the DevType reply", hi, lo);
      FUZZ_CHECK(hi != 0 || lo != 0, "link token 00 00 adopted");
      if (STRICT)
        FUZZ_CHECK(last_ok, "link token %02X %02X adopted from a DevType reply with a bad checksum", hi, lo);
      tok_hi = hi;
      tok_lo = lo;
    }
  }
};

void Io::bus_set_de(bool high) {
  FUZZ_CHECK(has_de, "DE driven although the UART owns it");
  de = high;
}

void Io::bus_write(const uint8_t *data, size_t len) {
  FUZZ_CHECK(len >= 1 && len <= H::TX_FRAME_MAX + 2, "write of %zu bytes", len);
  FUZZ_CHECK(!has_de || de, "write with DE low");
  Sim *s = sim;
  if (has_de && s->wrote) {
    uint32_t gap = s->now - s->tx_at;
    FUZZ_CHECK(gap >= H::bus_tx_time_ms(s->tx_len), "new frame %u ms after a %zu byte frame that needs %u ms", gap,
               s->tx_len, H::bus_tx_time_ms(s->tx_len));
  }
  Exact copy(data, len);  // reads every byte: the pointer covers all `len` bytes
  if (!s->raw_queued) {
    uint8_t f[80];
    size_t u = check_tx_wire("scheduler write", copy.data(), len, H::TX_FRAME_MAX + 2, f, sizeof(f));
    FUZZ_CHECK(f[2] == 0x00, "scheduler wrote a frame with direction %02X", f[2]);
    if (f[13] == 0x0A) {
      // The DevType probe goes out verbatim, with the vendor module's pre-link 00 00.
      FUZZ_CHECK(u == H::LINK_INIT_0A_LEN && memcmp(f, H::LINK_INIT_0A, u) == 0, "DevType probe altered");
    } else {
      uint8_t hi = 0, lo = 0;
      s->bus.link_token(&hi, &lo);
      FUZZ_CHECK(f[7] == hi && f[8] == lo, "frame class %02X carries %02X %02X, link token is %02X %02X", f[13], f[7],
                 f[8], hi, lo);
    }
  }
  memcpy(s->tx, data, len < sizeof(s->tx) ? len : sizeof(s->tx));
  s->tx_len = len;
  s->tx_at = s->now;
  s->wrote = true;
}

// A well-formed reply to the last frame written, as the A/C would send it.
static void good_reply(Sim *s, Reader &r) {
  uint8_t f[200];
  size_t n;
  uint8_t cls = s->tx_len > 14 ? s->tx[13] : 0x66;
  uint8_t pick = r.u8();
  if ((pick & 0x80) != 0)
    cls = (pick & 0x40) != 0 ? 0x1E : 0x66;  // an answer to something else (late, or unsolicited)
  switch (cls) {
    case 0x0A: {
      uint8_t hi = r.u8();
      uint8_t lo = r.u8();
      n = gold_devtype_reply(f, hi, lo);
      break;
    }
    case 0x1E:
      n = gold_link_reply(f, r.u8());
      break;
    case 0x65: {
      // The A/C echoes a command: same class and subtype, ack byte at 15.
      n = 50;
      memset(f, 0, n);
      f[2] = 0x01;
      f[3] = 0x40;
      f[13] = 0x65;
      f[15] = r.u8();
      model_seal(f, n);
      break;
    }
    default: {
      n = gold_status(f);
      if (s->tx_len > 14 && s->tx[13] == 0x66 && s->tx[14] == 0x40)
        f[14] = 0x40;
      size_t k = r.u8() % 8;
      for (size_t i = 0; i < k; i++) {
        size_t at = 16 + r.u8() % 140;
        f[at] = r.u8();
      }
      model_seal(f, n);
      break;
    }
  }
  uint8_t damage = r.u8();
  if ((damage & 3) == 1)
    f[damage % n] ^= static_cast<uint8_t>(1u << (damage >> 5));  // one flipped bit
  uint8_t wire[420];
  size_t w = model_stuff(f, n, 2, wire);
  if ((damage & 3) == 2)
    w -= (damage >> 2) % w;  // cut short
  s->io.push(wire, w);
}

static void queue_command(Sim *s, Reader &r) {
  uint8_t frame[H::CMD_FRAME_MAX];
  size_t n = 0;
  uint8_t kind = r.u8();
  switch (kind & 3) {
    case 0: {
      H::AcCommand c;
      c.mode = static_cast<H::Mode>(r.u8() % 5);
      c.setpoint = static_cast<int8_t>(16 + r.u8() % 17);
      c.fan = static_cast<H::FanSpeed>(5 + r.u8() % 5);
      c.power_on = r.flag();
      n = H::build_command(c, frame, sizeof(frame));
      break;
    }
    case 1:
      n = H::build_power_frame(r.flag(), frame, sizeof(frame));
      break;
    case 2:
      n = H::build_mute_frame(r.flag(), frame, sizeof(frame));
      break;
    default:
      n = H::build_sleep_frame(r.u8(), frame, sizeof(frame));
      break;
  }
  if ((kind & 4) != 0 && n != 0) {  // the hub's quiet path
    uint8_t quiet[H::CMD_FRAME_MAX];
    size_t q = H::stamp_beep(frame, n, false, quiet, sizeof(quiet));
    FUZZ_CHECK(q != 0, "stamp_beep refused a builder's frame");
    memcpy(frame, quiet, q);
    n = q;
  }
  size_t before = s->bus.queued();
  bool ok = s->bus.enqueue(frame, n);
  FUZZ_CHECK(ok == (n != 0 && before < H::BUS_TX_QUEUE_LEN), "enqueue returned %d with %zu queued", ok, before);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  Reader r(data, size);
  uint8_t opt = r.u8();
  Sim *s = new Sim();
  s->io.sim = s;
  s->io.has_de = (opt & 1) != 0;
  static const uint32_t starts[4] = {0, 0xFFFFFC00u, 0x7FFFFE00u, 0x00ABCDEFu};
  s->now = starts[(opt >> 1) & 3];
  s->bus.setup(&s->io, s, s->io.has_de);
  s->poll();

  int budget = 600;
  while (!r.empty() && budget-- > 0) {
    uint8_t op = r.u8();
    switch (op % 8) {
      case 0:
        s->now += r.u8();
        s->poll();
        break;
      case 1:
        s->now += static_cast<uint32_t>(r.u8()) * 8;
        s->poll();
        break;
      case 2:
        s->now += static_cast<uint32_t>(r.u8()) * 64;
        s->poll();
        break;
      case 3: {
        uint8_t chunk[63];
        size_t k = r.take(chunk, r.u8() % 64);
        s->io.push(chunk, k);
        break;
      }
      case 4:
        good_reply(s, r);
        break;
      case 5:
        queue_command(s, r);
        break;
      case 6:
        s->bus.set_setup_indicator(r.flag(), s->now);
        break;
      default: {
        uint8_t raw[80];
        size_t want = r.u8() % 80;
        size_t k = r.take(raw, want);
        size_t before = s->bus.queued();
        Exact e(raw, k);
        bool ok = s->bus.enqueue(e.data(), k);
        FUZZ_CHECK(ok == (k != 0 && k <= H::TX_FRAME_MAX && before < H::BUS_TX_QUEUE_LEN), "enqueue(%zu bytes) = %d", k,
                   ok);
        if (ok)
          s->raw_queued = true;
        break;
      }
    }
  }
  delete s;
  return 0;
}
