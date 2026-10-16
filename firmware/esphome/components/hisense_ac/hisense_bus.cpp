#include "hisense_bus.h"

#include <cstring>

// See hisense_bus.h for the transaction sequence and the polling cycle implemented here.

namespace esphome::hisense_ac {

static bool reached(uint32_t now, uint32_t until) { return static_cast<int32_t>(now - until) >= 0; }

void BusScheduler::setup(BusIO *io, BusListener *listener, bool has_de) {
  this->io_ = io;
  this->listener_ = listener;
  this->has_de_ = has_de;
  if (this->has_de_)
    this->io_->bus_set_de(false);  // idle low = receive, before the UART speaks
  this->step_ = Step::BOOT_DEVTYPE;
  this->phase_ = Phase::IDLE;
}

BusTicket BusScheduler::enqueue(const uint8_t *frame, size_t len) {
  if (frame == nullptr || len == 0 || len > TX_FRAME_MAX || this->queue_len_ >= BUS_TX_QUEUE_LEN)
    return BUS_TICKET_NONE;
  const size_t slot = (this->queue_head_ + this->queue_len_) % BUS_TX_QUEUE_LEN;
  std::memcpy(this->queue_[slot], frame, len);
  this->queue_lens_[slot] = static_cast<uint8_t>(len);
  if (++this->last_ticket_ == BUS_TICKET_NONE)
    this->last_ticket_++;
  this->queue_tickets_[slot] = this->last_ticket_;
  this->queue_len_++;
  return this->last_ticket_;
}

bool BusScheduler::link_token(uint8_t *hi, uint8_t *lo) const {
  if (hi != nullptr)
    *hi = this->token_[0];
  if (lo != nullptr)
    *lo = this->token_[1];
  return this->token_seen_;
}

bool BusScheduler::faults(AcFaults *out) const {
  if (!this->faults_.valid)
    return false;
  *out = this->faults_;
  return true;
}

bool BusScheduler::features(AcFeatures *out) const {
  if (!this->features_.valid)
    return false;
  *out = this->features_;
  return true;
}

void BusScheduler::set_setup_indicator(bool on, uint32_t now_ms) {
  if (on == this->setup_indicator_)
    return;
  this->setup_indicator_ = on;
  // Whatever closed the mode, the press that did it must not open it again (issue 69).
  if (!on)
    setup_request_lockout(&this->setup_, now_ms);
}

// The A/C's 0x1E reply carries the remote's setup request ("77"). Any 0x1E frame counts, whichever
// transaction it arrived in: the unit answers the heartbeat with it about once a second.
void BusScheduler::check_link_reply_(const uint8_t *frame, size_t n, uint32_t now) {
  uint8_t bits;
  if (!link_request_bits(frame, n, &bits))
    return;
  this->link_request_ = bits;
  const uint8_t request = setup_request_step(&this->setup_, bits, this->setup_indicator_, now);
  if (request != 0 && this->listener_ != nullptr)
    this->listener_->on_bus_setup_request(request);
}

void BusScheduler::discard_rx_() {
  while (this->io_->bus_read() >= 0) {
  }
  this->rx_.reset();
}

// Flush stale RX, stamp the A/C's device type into envelope bytes 7/8 (except on the DevType
// probe, which bootstraps the link with the vendor module's pre-link 00 00), then send. `done` is
// called when the reply window closes, with the length of the reply in rx_ or 0.
void BusScheduler::begin_transaction_(const uint8_t *frame, size_t len, uint8_t expect_class, StepDone done,
                                      uint32_t now) {
  this->discard_rx_();
  this->expect_class_ = expect_class;
  this->on_done_ = done;
  size_t stamped = 0;
  // NOLINTNEXTLINE(readability-simplify-boolean-expr): reads as "not the DevType probe"
  if (!(len > FRAME_CLASS_OFFSET && frame[FRAME_CLASS_OFFSET] == CLASS_DEVTYPE))
    stamped = stamp_link_token(frame, len, this->token_[0], this->token_[1], this->tx_, sizeof(this->tx_));
  if (stamped > 0) {
    this->tx_len_ = stamped;
  } else {
    this->tx_len_ = len;
    std::memcpy(this->tx_, frame, len);
  }

  if (this->has_de_) {
    // Assert DE, then let the transceiver settle before the first byte.
    this->io_->bus_set_de(true);
    this->phase_ = Phase::TX_SETTLE;
    this->phase_until_ = now + BUS_DE_SETTLE_MS;
  } else {
    // Peripheral-owned DE: no settle, no drain, and the reply window opens as the bytes are queued.
    this->io_->bus_write(this->tx_, this->tx_len_);
    this->phase_ = Phase::LISTEN;
    this->phase_until_ = now + BUS_REPLY_TIMEOUT_MS;
    this->tx_done_at_ = now + bus_tx_time_ms(this->tx_len_);
  }
  this->transmitting_ = true;
}

void BusScheduler::finish_transaction_(size_t reply_len, uint32_t now) {
  this->phase_ = Phase::IDLE;
  (this->*this->on_done_)(reply_len, now);
}

void BusScheduler::poll(uint32_t now_ms) {
  if (this->io_ == nullptr)
    return;
  if (!this->setup_started_) {
    // A smart-config pulse in the first seconds after a start is not acted on: leaving setup mode
    // usually ends in a restart, and the press that asked for it may still be on its way.
    this->setup_started_ = true;
    setup_request_lockout(&this->setup_, now_ms);
  }
  // Here and not only when a reply arrives: a silent unit must not keep an old lockout armed.
  setup_request_expire(&this->setup_, now_ms);
  if (!this->has_de_ && this->transmitting_ && reached(now_ms, this->tx_done_at_))
    this->transmitting_ = false;
  // Several phases can complete in one call (a reply already buffered, a zero wait); the bound
  // only guards against a logic error spinning here.
  for (int guard = 0; guard < 16 && this->advance_(now_ms); guard++) {
  }
}

// One phase, as far as it goes now. False when nothing more can happen until time has passed or
// bytes have arrived.
bool BusScheduler::advance_(uint32_t now) {
  switch (this->phase_) {
    case Phase::IDLE:
      // A step that sends nothing leaves the phase as it is, and the next one starts on the next call.
      this->start_step_(now);
      return this->phase_ != Phase::IDLE;

    case Phase::TX_SETTLE:
      if (!reached(now, this->phase_until_))
        return false;
      this->io_->bus_write(this->tx_, this->tx_len_);
      // Wait for every byte to shift out, then hold DE a further 25 ms.
      this->phase_ = Phase::TX_DRAIN;
      this->phase_until_ = now + bus_tx_time_ms(this->tx_len_) + BUS_DE_DRAIN_MS;
      return true;

    case Phase::TX_DRAIN:
      if (!reached(now, this->phase_until_))
        return false;
      this->io_->bus_flush();        // already empty by now; guarantees it before DE drops
      this->io_->bus_set_de(false);  // release: back to receive
      this->transmitting_ = false;
      this->phase_ = Phase::LISTEN;
      this->phase_until_ = now + BUS_REPLY_TIMEOUT_MS;
      return true;

    case Phase::LISTEN:
      return this->listen_(now);

    case Phase::WAIT:
      break;
  }
  // WAIT: the pause after a failed boot try, or the rest of the cycle.
  if (!reached(now, this->phase_until_))
    return false;
  this->phase_ = Phase::IDLE;
  return true;
}

// Read what has arrived. True when the reply window closed, with a frame or by running out.
bool BusScheduler::listen_(uint32_t now) {
  int c;
  while ((c = this->io_->bus_read()) >= 0) {
    const size_t n = this->rx_.feed(static_cast<uint8_t>(c));
    if (n != 0 && this->frame_received_(n, now))
      return true;
  }
  if (!reached(now, this->phase_until_))
    return false;
  // One unanswered poll on a live link, with the cycle before and this one otherwise answered,
  // is the unit skipping a reply (see skipped_replies()). Anything else is a timeout.
  const bool poll = this->step_ == Step::HEARTBEAT || this->step_ == Step::STATUS;
  const bool isolated =
      poll && this->status_seen_ && !this->link_down_ && !this->cycle_missed_ && !this->prev_cycle_missed_;
  if (isolated) {
    this->skipped_replies_++;
  } else {
    this->reply_timeouts_++;
  }
  this->cycle_missed_ = true;
  if (this->listener_ != nullptr)
    this->listener_->on_bus_timeout(this->expect_class_);
  this->finish_transaction_(0, now);  // timeout, no reply
  return true;
}

// A complete frame is in rx_. True when it was the reply this transaction waits for.
bool BusScheduler::frame_received_(size_t n, uint32_t now) {
  const uint8_t *f = this->rx_.data();
  if (this->listener_ != nullptr)
    this->listener_->on_bus_frame(f, n);
  // The assembler delivers on framing alone. The checksum is judged here, once, for every frame
  // and every consumer below.
  this->reply_sound_ = status_checksum_ok(f, n);
  if (!this->reply_sound_) {
    this->chk_mismatch_++;
    if (this->listener_ != nullptr)
      this->listener_->on_bus_checksum_error(f, n);
  }
  this->check_link_reply_(f, n, now);
  // A completed frame of the wrong class is a late reply to something else: drop it and keep
  // listening within this window.
  if (n <= FRAME_CLASS_OFFSET || !reply_class_ok(f[FRAME_CLASS_OFFSET], this->expect_class_))
    return false;
  // Latch the device type from a DevType reply. A 00 00 pair means "not linked yet" to the vendor
  // module, so it is never adopted.
  uint8_t hi;
  uint8_t lo;
  if (devtype_reply_usable(this->reply_sound_, f, n, &hi, &lo)) {
    this->token_[0] = hi;
    this->token_[1] = lo;
    this->token_seen_ = true;
  }
  this->finish_transaction_(n, now);
  return true;
}

// Start whatever the current step sends. A step that sends nothing moves on to the next one.
void BusScheduler::start_step_(uint32_t now) {
  switch (this->step_) {
    case Step::BOOT_DEVTYPE:
      this->begin_transaction_(LINK_INIT_0A, LINK_INIT_0A_LEN, CLASS_DEVTYPE, &BusScheduler::boot_devtype_done_, now);
      return;
    case Step::BOOT_07:
      // The reply is not looked at.
      this->begin_transaction_(LINK_INIT_07, LINK_INIT_07_LEN, CLASS_ANY, &BusScheduler::boot_07_done_, now);
      return;
    case Step::CYCLE_BEGIN:
      this->cycle_start_ = now;
      this->cycle_commands_ = 0;
      this->prev_cycle_missed_ = this->cycle_missed_;
      this->cycle_missed_ = false;
      // Cooperative link recovery: one DevType handshake per cycle while the A/C is silent, and
      // one after it if the unit came back without having answered any.
      this->step_ = this->link_silent_() || this->handshake_owed_ ? Step::RECOVER : Step::HEARTBEAT;
      this->start_step_(now);
      return;
    case Step::RECOVER:
      if (this->link_silent_()) {
        this->heard_ac_ = false;
        // A new handshake gets a new ProductType poll, like the stock module's bring-up.
        this->features_.valid = false;
      }
      this->begin_transaction_(LINK_INIT_0A, LINK_INIT_0A_LEN, CLASS_DEVTYPE, &BusScheduler::recover_done_, now);
      return;
    case Step::HEARTBEAT: {
      // Byte 16 bit 6 says whether we have heard the A/C's 0x1E. Any reply class is accepted:
      // the A/C sometimes answers the heartbeat with a 0x66.
      uint8_t f[LINK_HEARTBEAT_LEN + 2];
      const size_t n = build_link_heartbeat(this->heard_ac_, this->setup_indicator_, f, sizeof(f));
      if (this->setup_indicator_)
        setup_request_flag_sent(&this->setup_);
      this->begin_transaction_(f, n, CLASS_ANY, &BusScheduler::heartbeat_done_, now);
      return;
    }
    case Step::COMMANDS: {
      // What does not fit this cycle waits for the next: the heartbeat and the status poll keep
      // their pace however many commands are queued.
      if (this->queue_len_ == 0 || !command_slot_open(this->cycle_commands_, now - this->cycle_start_)) {
        this->step_ = Step::STATUS;
        this->start_step_(now);
        return;
      }
      // The frame stays at the head of the queue until its transaction has been judged, so a
      // re-send keeps its place in front of everything queued after it. Any reply class is
      // accepted: the A/C may answer with a status frame.
      const size_t slot = this->queue_head_;
      this->head_sends_++;
      if (this->cycle_commands_ < 0xFF)
        this->cycle_commands_++;
      this->begin_transaction_(this->queue_[slot], this->queue_lens_[slot], CLASS_ANY, &BusScheduler::command_done_,
                               now);
      return;
    }
    case Step::STATUS:
      // Correlated on 0x66, so a late 0x1E cannot reset the miss counter.
      this->begin_transaction_(STATUS_REQUEST, STATUS_REQUEST_LEN, CLASS_STATUS, &BusScheduler::status_done_, now);
      return;
    case Step::PRODUCT: {
      // The stock module asks ProductType once per link bring-up and never again on a live link
      // (RE docs 10 and 15, 2026-08-19 dump). Asking it periodically produced one reply timeout
      // per re-poll on the ESP32 nodes (#186).
      if (!this->heard_ac_ || this->features_.valid) {
        this->product_done_(0, now);
        return;
      }
      uint8_t f[STATUS_REQUEST_LEN];
      const size_t n = build_producttype_request(f, sizeof(f));
      this->begin_transaction_(f, n, CLASS_STATUS, &BusScheduler::product_done_, now);
      return;
    }
    case Step::PACE:
      if (!reached(now, this->cycle_start_ + BUS_CYCLE_MS)) {
        this->phase_ = Phase::WAIT;
        this->phase_until_ = this->cycle_start_ + BUS_CYCLE_MS;
      }
      this->step_ = Step::CYCLE_BEGIN;
      return;
  }
}

// ---- What each step does with its reply (`reply_len` is 0 when the window closed empty) ---------
void BusScheduler::boot_devtype_done_(size_t reply_len, uint32_t now) {
  // Only a reply that passes its checksum is the handshake answer; a damaged one is a failed try.
  if (reply_len > 0 && this->reply_sound_) {
    this->step_ = Step::BOOT_07;
    return;
  }
  // Every failed try is followed by the 500 ms wait, the tenth included.
  this->boot_tries_++;
  this->phase_ = Phase::WAIT;
  this->phase_until_ = now + BUS_BOOT_RETRY_MS;
  if (this->boot_tries_ >= BUS_BOOT_TRIES)
    this->step_ = Step::BOOT_07;
}

void BusScheduler::boot_07_done_(size_t reply_len, uint32_t now) { this->step_ = Step::CYCLE_BEGIN; }

// The device type, if the probe was answered, was latched as the reply arrived. An unanswered
// probe is owed again only while the unit is silent: once it answers its status poll it gets this
// one more and no further, so a unit that never answers the probe is not asked every second.
void BusScheduler::recover_done_(size_t reply_len, uint32_t now) {
  const bool answered = reply_len > 0 && this->reply_sound_;
  this->handshake_owed_ = this->link_silent_() && !answered;
  this->step_ = Step::HEARTBEAT;
}

void BusScheduler::heartbeat_done_(size_t reply_len, uint32_t now) {
  if (reply_len > 0) {
    // Byte 16 of the next heartbeat says "heard you", so the frame has to be a whole one.
    if (this->reply_sound_)
      this->heard_ac_ = true;
    this->consume_(reply_len);  // no-op unless the A/C answered with a 0x66
  }
  this->step_ = Step::COMMANDS;
}

void BusScheduler::command_done_(size_t reply_len, uint32_t now) {
  const bool answered = reply_len > 0;
  this->command_reply_ = command_reply_verdict(this->tx_, this->tx_len_, this->rx_.data(), reply_len);
  this->command_ticket_ = this->queue_tickets_[this->queue_head_];
  if (!answered)
    this->unanswered_commands_++;
  // A frame with no reply at all goes out again, while the unit shows it is there by answering
  // its status poll (command_resend_allowed).
  this->head_resend_ =
      !answered && command_resend_allowed(this->head_sends_, this->status_seen_ && this->link_miss_ == 0);
  if (this->head_resend_) {
    this->command_resends_++;
  } else {
    this->queue_head_ = (this->queue_head_ + 1) % BUS_TX_QUEUE_LEN;
    this->queue_len_--;
    this->head_sends_ = 0;
  }
  if (this->listener_ != nullptr)
    this->listener_->on_bus_command(answered, answered ? this->rx_.data()[FRAME_CLASS_OFFSET] : 0);
  if (answered)
    this->consume_(reply_len);
  // The re-send waits for the next cycle's command slot: a unit that missed a frame is not asked
  // again within the same second. Otherwise the step stays COMMANDS until the queue is empty or
  // the slot closes.
  if (this->head_resend_)
    this->step_ = Step::STATUS;
}

void BusScheduler::status_done_(size_t reply_len, uint32_t now) {
  // Only a checksum-valid 0x66 counts as the link being alive.
  if (reply_len > 0 && this->consume_(reply_len)) {
    this->link_miss_ = 0;
    this->status_seen_ = true;
  } else if (this->link_miss_ < 0xFF) {
    this->link_miss_++;
  }
  const bool silent = this->link_silent_();
  if (silent != this->link_down_) {
    this->link_down_ = silent;
    if (silent) {
      this->link_losses_++;
      setup_request_link_lost(&this->setup_);
    }
    if (this->listener_ != nullptr)
      this->listener_->on_bus_link(!silent);
  }
  this->step_ = Step::PRODUCT;
}

void BusScheduler::product_done_(size_t reply_len, uint32_t now) {
  if (reply_len > 0)
    this->consume_(reply_len);
  this->step_ = Step::PACE;
}

// A 0x66 reply is either the ProductType answer (subtype 0x40) or the status frame. A well-framed
// frame with a bad checksum was counted when it arrived and is not parsed.
bool BusScheduler::consume_(size_t n) {
  const uint8_t *f = this->rx_.data();
  if (n < CMD_HEADER_LEN || f[FRAME_CLASS_OFFSET] != CLASS_STATUS || !this->reply_sound_)
    return false;
  if (f[14] == SUBTYPE_PRODUCT_TYPE) {
    AcFeatures ft;
    if (parse_features(f, n, &ft)) {
      this->features_ = ft;
      if (this->listener_ != nullptr)
        this->listener_->on_bus_features(ft);
    }
    return true;
  }
  AcFaults fl;
  if (parse_faults(f, n, &fl))
    this->faults_ = fl;
  AcState st;
  if (parse_status(f, n, &st) && this->listener_ != nullptr)
    this->listener_->on_bus_status(st);
  return true;
}

}  // namespace esphome::hisense_ac
