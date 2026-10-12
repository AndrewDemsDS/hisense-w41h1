#pragma once
// Non-blocking RS-485 bus master for the Hisense A/C, driven from ESPHome's loop().
//
// A state machine with no task and no blocking waits. Every exchange is a transaction, as on the
// vendor module: flush RX, stamp the frame, raise DE, settle 5 ms, send, hold DE 25 ms past the
// last byte, drop DE, then listen up to 500 ms for a reply of the expected class. The A/C only
// ever answers, never initiates, and two frames without a reply window collide on the half-duplex
// bus.
//
// Boot: DevType (0x0A) handshake, up to 10 tries 500 ms apart, then 0x07.
// Each ~1 s cycle: a DevType re-handshake if 5 status polls in a row went unanswered, the 0x1E
// heartbeat, the queued commands that fit (command_slot_open below), the 0x66 status poll (the
// link-liveness signal), and the 0x66/40 ProductType poll on the first heard cycle and every 60th
// after.
//
// A command frame that gets no reply is sent again in the next cycle's command slot, ahead of
// everything queued behind it, up to COMMAND_SENDS_MAX sends and only while the unit answers its
// status poll (command_resend_allowed below).
//
// I/O goes through BusIO, so the scheduler can be driven against a simulated A/C in a host test.
// No ESPHome includes.

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
  // Diagnostics, all optional. They exist so the owner of the bus can log at the level it likes;
  // the scheduler itself has no logger, which keeps it buildable without ESPHome.
  // Every complete frame received, of any class, before its checksum is judged.
  virtual void on_bus_frame(const uint8_t *frame, size_t len) {}
  // A complete frame, of any class, whose checksum did not match. It is counted and nothing is
  // read from it: no status, no device type, no setup request.
  virtual void on_bus_checksum_error(const uint8_t *frame, size_t len) {}
  // The reply window closed with no frame of the expected class (0 = any class was acceptable).
  virtual void on_bus_timeout(uint8_t expect_class) {}
  // A queued command frame finished one transaction. `answered` is false when the reply window
  // closed with nothing from the A/C; command_resend_pending() then says whether the scheduler
  // sends the frame again. `reply_class` is the class of the frame that ended the window, 0 when
  // there was none, and command_reply() is what that frame says.
  virtual void on_bus_command(bool answered, uint8_t reply_class) {}
  // The remote's "77" gesture, debounced: once per accepted request. `request` is the request bits
  // of the A/C's 0x1E reply (LINK_REQ_* in hisense_protocol.h).
  virtual void on_bus_setup_request(uint8_t request) {}
};

// Timing, from the vendor module's byte-writer and transaction primitive.
static constexpr uint32_t BUS_DE_SETTLE_MS = 5;  // DE high before the first byte
static constexpr uint32_t BUS_DE_DRAIN_MS = 25;  // DE held after the last byte left
static constexpr uint32_t BUS_REPLY_TIMEOUT_MS = 500;
static constexpr uint32_t BUS_CYCLE_MS = 1000;  // the vendor module paces at 0x3e8 ms
static constexpr uint32_t BUS_BOOT_RETRY_MS = 500;
static constexpr uint8_t BUS_BOOT_TRIES = 10;
static constexpr uint8_t BUS_LINK_LOST_POLLS = 5;
static constexpr uint8_t BUS_PRODUCTTYPE_CYCLES = 60;
static constexpr size_t BUS_TX_QUEUE_LEN = 8;

// Bytes of wire time for `len` bytes at 9600 8N1 (10 bits each), rounded up.
inline uint32_t bus_tx_time_ms(size_t len) {
  return static_cast<uint32_t>((len * 10 * 1000 + BUS_BAUD_RATE - 1) / BUS_BAUD_RATE);
}

// ---- Command rules ------------------------------------------------------------------------------
// Pure, so they are tested without a scheduler (test_esphome_confirm.cpp, test_esphome_map_gaps.cpp).

// A frame with no reply goes out again in the next cycle's command slot, in front of anything queued
// behind it, so a newer command can never be overtaken by an older one. Three sends in all. Only
// while the unit is answering its status poll: a unit that answers nothing is a dead bus, and frames
// re-sent into it would pile up and all land when it comes back.
static constexpr uint8_t COMMAND_SENDS_MAX = 3;

inline bool command_resend_allowed(uint8_t sends, bool unit_answering) {
  return unit_answering && sends < COMMAND_SENDS_MAX;
}

// How long into a cycle a queued command may still start. The command slot sits between the
// heartbeat and the status poll, and it used to run until the queue was empty: a queue refilled as
// fast as it drained (about seven frames a second) kept the unit without its 1 Hz heartbeat and
// Home Assistant without status for as long as that lasted. Past this point what is still queued
// waits for the next cycle. 600 ms leaves the status poll and its 160-byte reply (about 260 ms)
// room inside the 1000 ms cycle after a command that started at the last moment, and lets the
// three or four frames through that fit a cycle anyway. The first command of a cycle always
// goes, however late the heartbeat ended, so the queue cannot stall.
static constexpr uint32_t BUS_COMMAND_SLOT_MS = 600;

inline bool command_slot_open(uint8_t sent_this_cycle, uint32_t cycle_elapsed_ms) {
  return sent_this_cycle == 0 || cycle_elapsed_ms < BUS_COMMAND_SLOT_MS;
}

// What the reply to a command frame says. The stock transaction primitive accepts a reply as verified
// when its payload repeats the first two payload bytes of the command and carries 1 in the third
// (RE docs/10 section 4.2 step 6). Payload byte N is frame byte 13 + N, so that is frame bytes 13
// and 14 echoed, and the value 1 at frame offset 15.
// VERIFY: the layout is read from the stock image and has not been captured on this bus. Until a
// capture backs it the verdict is logged and nothing is decided on it: the hub's status check
// (confirm_decision in hisense_map.h) is what tells a command that took from one that did not.
static constexpr size_t CMD_REPLY_ACK_OFFSET = 15;
static constexpr uint8_t CMD_REPLY_ACK_VALUE = 0x01;

enum CommandReply : uint8_t {
  COMMAND_REPLY_NONE = 0,   // the window closed empty
  COMMAND_REPLY_ACK = 1,    // echo of the command, ack byte 1
  COMMAND_REPLY_NAK = 2,    // echo of the command, ack byte something else
  COMMAND_REPLY_OTHER = 3,  // a frame of another class (a status or link frame) ended the window
};

inline CommandReply command_reply_verdict(const uint8_t *tx, size_t tx_len, const uint8_t *reply, size_t reply_len) {
  if (reply == nullptr || reply_len == 0)
    return COMMAND_REPLY_NONE;
  if (tx == nullptr || tx_len <= FRAME_CLASS_OFFSET + 1 || reply_len <= CMD_REPLY_ACK_OFFSET)
    return COMMAND_REPLY_OTHER;
  if (reply[FRAME_CLASS_OFFSET] != tx[FRAME_CLASS_OFFSET] ||
      reply[FRAME_CLASS_OFFSET + 1] != tx[FRAME_CLASS_OFFSET + 1])
    return COMMAND_REPLY_OTHER;
  return reply[CMD_REPLY_ACK_OFFSET] == CMD_REPLY_ACK_VALUE ? COMMAND_REPLY_ACK : COMMAND_REPLY_NAK;
}

class BusScheduler {
 public:
  // `has_de`: true when this component drives DE itself (the hardware-validated path). False when
  // the UART peripheral owns it (flow_control_pin): then there is no settle or drain.
  void setup(BusIO *io, BusListener *listener, bool has_de);
  // Advance as far as `now_ms` allows. Call every loop.
  void poll(uint32_t now_ms);
  // Queue a finished frame (from the builders) for the next cycle's command slot. False when the
  // queue is full or the frame is empty or too long.
  bool enqueue(const uint8_t *frame, size_t len);
  // True while DE timing is running, so the hub can ask ESPHome for a tight loop. Always false
  // when the UART peripheral owns DE: there is no timing to keep then.
  bool wants_fast_loop() const { return this->phase_ == Phase::TX_SETTLE || this->phase_ == Phase::TX_DRAIN; }
  // True while a frame is on its way out, as of the last poll(): from DE going high until it is
  // released, or, without a DE pin, from the write until the last byte has had time to leave at
  // 9600 baud. Code that touches the serial port from outside has to leave it alone meanwhile.
  bool transmitting() const { return this->transmitting_; }

  bool link_token(uint8_t *hi, uint8_t *lo) const;  // false until a DevType reply supplied it
  // Complete frames received with a bad checksum, of any class, since boot.
  uint32_t checksum_mismatches() const { return this->chk_mismatch_; }
  // Counters since boot, for diagnostics. A reply window that closed empty, of any kind; a queued
  // command among those (each send counts); a command frame sent again for that reason; a
  // link-lost edge.
  uint32_t reply_timeouts() const { return this->reply_timeouts_; }
  uint32_t unanswered_commands() const { return this->unanswered_commands_; }
  uint32_t command_resends() const { return this->command_resends_; }
  // Valid inside on_bus_command(): the frame just reported is going out again.
  bool command_resend_pending() const { return this->head_resend_; }
  // What the reply to the last command frame says (a CommandReply).
  uint8_t command_reply() const { return this->command_reply_; }
  uint32_t link_losses() const { return this->link_losses_; }
  bool faults(AcFaults *out) const;
  bool features(AcFeatures *out) const;
  size_t queued() const { return this->queue_len_; }

  // While on, every heartbeat carries the module's provisioning flag and the panel shows "77".
  // Switching it off starts the lockout that keeps the closing press from opening it again.
  // `now_ms` is the same clock poll() gets.
  void set_setup_indicator(bool on, uint32_t now_ms);
  bool setup_indicator() const { return this->setup_indicator_; }
  // Request bits of the last 0x1E reply with a good checksum, before any debounce. For diagnostics:
  // it tells "the unit never asked" from "the request was not taken".
  uint8_t link_request() const { return this->link_request_; }

 protected:
  // Where one transaction is: waiting to start, DE settling, bytes leaving, reply window, or the
  // pause that fills a cycle.
  enum class Phase : uint8_t { IDLE, TX_SETTLE, TX_DRAIN, LISTEN, WAIT };
  // What is sent next. Boot runs the first two once, then the rest repeat as one cycle.
  enum class Step : uint8_t {
    BOOT_DEVTYPE,  // DevType probe, up to BUS_BOOT_TRIES
    BOOT_07,       // 0x07, once
    CYCLE_BEGIN,   // sends nothing: notes the time and picks RECOVER or HEARTBEAT
    RECOVER,       // DevType probe, only while the link is lost
    HEARTBEAT,     // 0x1E
    COMMANDS,      // the queue, one frame per transaction, while command_slot_open()
    STATUS,        // 0x66 poll
    PRODUCT,       // 0x66/40 poll, on the first heard cycle and every BUS_PRODUCTTYPE_CYCLES
    PACE,          // sends nothing: waits out the rest of BUS_CYCLE_MS
  };
  // Called when a step's reply window closes. `reply_len` is the length of the reply in rx_, 0
  // when there was none.
  using StepDone = void (BusScheduler::*)(size_t reply_len, uint32_t now);

  bool advance_(uint32_t now);
  bool listen_(uint32_t now);
  bool frame_received_(size_t n, uint32_t now);
  void start_step_(uint32_t now);
  void begin_transaction_(const uint8_t *frame, size_t len, uint8_t expect_class, StepDone done, uint32_t now);
  void finish_transaction_(size_t reply_len, uint32_t now);
  void boot_devtype_done_(size_t reply_len, uint32_t now);
  void boot_07_done_(size_t reply_len, uint32_t now);
  void recover_done_(size_t reply_len, uint32_t now);
  void heartbeat_done_(size_t reply_len, uint32_t now);
  void command_done_(size_t reply_len, uint32_t now);
  void status_done_(size_t reply_len, uint32_t now);
  void product_done_(size_t reply_len, uint32_t now);
  bool consume_(size_t n);
  void check_link_reply_(const uint8_t *frame, size_t n, uint32_t now);
  void discard_rx_();

  BusIO *io_{nullptr};
  BusListener *listener_{nullptr};
  bool has_de_{true};

  Phase phase_{Phase::IDLE};
  Step step_{Step::BOOT_DEVTYPE};
  uint32_t phase_until_{0};
  uint32_t cycle_start_{0};
  uint8_t expect_class_{CLASS_ANY};
  // For transmitting(). Without a DE pin nothing marks the end of a transmission, so the time the
  // bytes need is kept.
  bool transmitting_{false};
  uint32_t tx_done_at_{0};
  StepDone on_done_{nullptr};
  // The frame in rx_ passed its checksum. Set for every frame received, read by its consumers.
  bool reply_sound_{false};

  uint8_t tx_[TX_FRAME_MAX + 2]{};
  size_t tx_len_{0};
  FrameAssembler rx_;

  uint8_t queue_[BUS_TX_QUEUE_LEN][TX_FRAME_MAX]{};
  uint8_t queue_lens_[BUS_TX_QUEUE_LEN]{};
  size_t queue_head_{0};
  size_t queue_len_{0};
  // Sends of the frame at the head of the queue so far, and whether it is waiting for another.
  uint8_t head_sends_{0};
  bool head_resend_{false};
  // Command frames started in the current cycle, for command_slot_open().
  uint8_t cycle_commands_{0};
  uint8_t command_reply_{0};

  // Outbound envelope bytes 7/8 = the A/C's device type from its DevType reply. Seeded to the
  // known-good 01 01, which stays in use for a unit whose probe never answers.
  uint8_t token_[2]{0x01, 0x01};
  bool token_seen_{false};

  uint8_t boot_tries_{0};
  bool heard_ac_{false};
  uint8_t link_miss_{0};
  bool link_down_{false};
  SetupRequestTracker setup_{};
  bool setup_started_{false};
  bool setup_indicator_{false};
  uint8_t link_request_{0};
  uint8_t pt_poll_{0};
  uint32_t chk_mismatch_{0};
  uint32_t reply_timeouts_{0};
  uint32_t unanswered_commands_{0};
  uint32_t command_resends_{0};
  bool status_seen_{false};
  uint32_t link_losses_{0};
  AcFeatures features_{};
  AcFaults faults_{};
};

}  // namespace esphome::hisense_ac
