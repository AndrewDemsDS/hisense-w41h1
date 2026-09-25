#pragma once
// Non-blocking RS-485 bus master for the Hisense A/C, driven from ESPHome's loop().
//
// Port of the bus task in firmware/src/rs485-driver/hisense_rs485.cpp (hisense_bus_task and
// hisense_transact) as a state machine: the same frames, in the same order, with the same timing,
// but no task, no queue object and no module state. Every exchange is a transaction, as on the
// stock module (RE docs/09, docs/10): flush RX, stamp the frame, raise DE, settle 5 ms, send, hold
// DE 25 ms past the last byte, drop DE, then listen up to 500 ms for a reply of the expected
// class. The A/C only ever answers, never initiates; two frames without a reply window collide on
// the half-duplex bus, which is why a free-running replay once got silence.
//
// Boot: DevType (0x0A) handshake, up to 10 tries 500 ms apart, then 0x07.
// Each ~1 s cycle: a DevType re-handshake if 5 status polls in a row went unanswered, the 0x1E
// heartbeat, every queued command, the 0x66 status poll (the link-liveness signal), and the
// 0x66/40 ProductType poll on the first heard cycle and every 60th after.
//
// I/O goes through BusIO so the host tests can drive this against a simulated A/C
// (firmware/test/test_esphome_bus.cpp). No ESPHome includes.

#include <cstddef>
#include <cstdint>

#include "hisense_protocol.h"

namespace esphome::hisense_ac {

// Transport the scheduler talks through. Implemented by the hub over uart::UARTDevice.
class BusIO {
 public:
  virtual ~BusIO() = default;
  // Drive the transceiver's DE line (high = transmit). Only called when a DE pin is configured.
  virtual void bus_set_de(bool high) = 0;
  virtual void bus_write(const uint8_t *data, size_t len) = 0;
  // Block until every written byte has left the shift register (at most a few ms by then).
  virtual void bus_flush() = 0;
  // One received byte, or -1 when none is waiting.
  virtual int bus_read() = 0;
};

// Results, delivered from inside poll(), i.e. on ESPHome's main loop.
class BusListener {
 public:
  virtual ~BusListener() = default;
  virtual void on_bus_status(const AcState &state) = 0;
  virtual void on_bus_features(const AcFeatures &features) = 0;
  virtual void on_bus_link(bool up) = 0;
};

// Timing, from the stock byte-writer and transaction primitive (RE docs/09).
static constexpr uint32_t BUS_DE_SETTLE_MS = 5;  // DE high before the first byte
static constexpr uint32_t BUS_DE_DRAIN_MS = 25;  // DE held after the last byte left
static constexpr uint32_t BUS_REPLY_TIMEOUT_MS = 500;
static constexpr uint32_t BUS_CYCLE_MS = 1000;  // stock master paces at 0x3e8 ms
static constexpr uint32_t BUS_BOOT_RETRY_MS = 500;
static constexpr uint8_t BUS_BOOT_TRIES = 10;
static constexpr uint8_t BUS_LINK_LOST_POLLS = 5;
static constexpr uint8_t BUS_PRODUCTTYPE_CYCLES = 60;
static constexpr size_t BUS_TX_QUEUE_LEN = 8;

// Bytes of wire time for `len` bytes at 9600 8N1 (10 bits each), rounded up.
inline uint32_t bus_tx_time_ms(size_t len) {
  return static_cast<uint32_t>((len * 10 * 1000 + BUS_BAUD_RATE - 1) / BUS_BAUD_RATE);
}

class BusScheduler {
 public:
  // `has_de`: true when this component drives DE itself (the hardware-validated path). False when
  // the UART peripheral owns it (flow_control_pin): then there is no settle or drain, as in the
  // original's CONFIG_HISENSE_RS485_HW_MODE.
  void setup(BusIO *io, BusListener *listener, bool has_de);
  // Advance as far as `now_ms` allows. Call every loop.
  void poll(uint32_t now_ms);
  // Queue a finished frame (from the builders) for the next cycle's command slot. False when the
  // queue is full or the frame is empty or too long, matching hisense_send_frame().
  bool enqueue(const uint8_t *frame, size_t len);
  // True while DE timing is running, so the hub can ask ESPHome for a tight loop.
  bool wants_fast_loop() const { return this->phase_ == Phase::TX_SETTLE || this->phase_ == Phase::TX_DRAIN; }

  bool link_token(uint8_t *hi, uint8_t *lo) const;  // false until a DevType reply supplied it
  uint32_t checksum_mismatches() const { return this->chk_mismatch_; }
  bool faults(AcFaults *out) const;
  bool features(AcFeatures *out) const;
  size_t queued() const { return this->queue_len_; }

 protected:
  enum class Phase : uint8_t { IDLE, TX_SETTLE, TX_DRAIN, LISTEN, WAIT };
  enum class Step : uint8_t {
    BOOT_DEVTYPE,
    BOOT_07,
    CYCLE_BEGIN,
    RECOVER,
    HEARTBEAT,
    DRAIN,
    STATUS,
    PRODUCT,
    PACE,
  };

  void start_step_(uint32_t now);
  void begin_transaction_(const uint8_t *frame, size_t len, uint8_t expect_class, uint32_t now);
  void finish_transaction_(size_t reply_len, uint32_t now);
  void on_step_done_(size_t reply_len, uint32_t now);
  bool consume_(size_t n);
  void discard_rx_();

  BusIO *io_{nullptr};
  BusListener *listener_{nullptr};
  bool has_de_{true};

  Phase phase_{Phase::IDLE};
  Step step_{Step::BOOT_DEVTYPE};
  uint32_t phase_until_{0};
  uint32_t cycle_start_{0};
  uint8_t expect_class_{0};

  uint8_t tx_[TX_FRAME_MAX + 2]{};
  size_t tx_len_{0};
  FrameAssembler rx_;

  uint8_t queue_[BUS_TX_QUEUE_LEN][TX_FRAME_MAX]{};
  uint8_t queue_lens_[BUS_TX_QUEUE_LEN]{};
  size_t queue_head_{0};
  size_t queue_len_{0};

  // #49: outbound envelope bytes 7/8 = the A/C's device type from its DevType reply. Seeded to
  // the known-good 01 01, so a unit whose probe never answers behaves as before #49.
  uint8_t token_[2]{0x01, 0x01};
  bool token_seen_{false};

  uint8_t boot_tries_{0};
  bool heard_ac_{false};
  uint8_t link_miss_{0};
  bool link_down_{false};
  uint8_t pt_poll_{0};
  uint32_t chk_mismatch_{0};
  AcFeatures features_{};
  AcFaults faults_{};
};

}  // namespace esphome::hisense_ac
