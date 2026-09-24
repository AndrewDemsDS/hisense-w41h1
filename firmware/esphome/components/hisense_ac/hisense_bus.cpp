#include "hisense_bus.h"

#include <cstring>

// See hisense_bus.h. Each transaction and cycle step below cites the line of the original bus
// task it reproduces; behaviour is held to it by firmware/test/test_esphome_bus.cpp.

namespace esphome::hisense_ac {

static bool reached(uint32_t now, uint32_t until) { return static_cast<int32_t>(now - until) >= 0; }

void BusScheduler::setup(BusIO *io, BusListener *listener, bool has_de) {
  this->io_ = io;
  this->listener_ = listener;
  this->has_de_ = has_de;
  if (this->has_de_)
    this->io_->set_de(false);  // idle low = receive, before the UART speaks (stock hs_driver_init)
  this->step_ = Step::BOOT_DEVTYPE;
  this->phase_ = Phase::IDLE;
}

bool BusScheduler::enqueue(const uint8_t *frame, size_t len) {
  if (frame == nullptr || len == 0 || len > TX_FRAME_MAX || this->queue_len_ >= BUS_TX_QUEUE_LEN)
    return false;
  size_t slot = (this->queue_head_ + this->queue_len_) % BUS_TX_QUEUE_LEN;
  std::memcpy(this->queue_[slot], frame, len);
  this->queue_lens_[slot] = static_cast<uint8_t>(len);
  this->queue_len_++;
  return true;
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

void BusScheduler::discard_rx_() {
  while (this->io_->read() >= 0) {
  }
  this->rx_.reset();
}

// hisense_transact(): flush stale RX, stamp the A/C's device type into envelope bytes 7/8 (except
// on the DevType probe, which bootstraps the link with the stock's pre-link 00 00), then send.
void BusScheduler::begin_transaction_(const uint8_t *frame, size_t len, uint8_t expect_class, uint32_t now) {
  this->discard_rx_();
  this->expect_class_ = expect_class;
  size_t stamped = 0;
  if (!(len > FRAME_CLASS_OFFSET && frame[FRAME_CLASS_OFFSET] == CLASS_DEVTYPE))
    stamped = stamp_link_token(frame, len, this->token_[0], this->token_[1], this->tx_, sizeof(this->tx_));
  if (stamped > 0) {
    this->tx_len_ = stamped;
  } else {
    this->tx_len_ = len;
    std::memcpy(this->tx_, frame, len);
  }

  if (this->has_de_) {
    // hisense_tx_raw(): assert DE, then let the transceiver settle before the first byte.
    this->io_->set_de(true);
    this->phase_ = Phase::TX_SETTLE;
    this->phase_until_ = now + BUS_DE_SETTLE_MS;
  } else {
    // Peripheral-owned DE: no settle, no drain, and the reply window opens as the bytes are queued.
    this->io_->write(this->tx_, this->tx_len_);
    this->phase_ = Phase::LISTEN;
    this->phase_until_ = now + BUS_REPLY_TIMEOUT_MS;
  }
}

void BusScheduler::finish_transaction_(size_t reply_len, uint32_t now) {
  this->phase_ = Phase::IDLE;
  this->on_step_done_(reply_len, now);
}

void BusScheduler::poll(uint32_t now) {
  if (this->io_ == nullptr)
    return;
  // Several phases can complete in one call (a reply already buffered, a zero wait); the bound
  // only guards against a logic error spinning here.
  for (int guard = 0; guard < 16; guard++) {
    switch (this->phase_) {
      case Phase::IDLE:
        this->start_step_(now);
        if (this->phase_ == Phase::IDLE)
          return;
        continue;

      case Phase::TX_SETTLE:
        if (!reached(now, this->phase_until_))
          return;
        this->io_->write(this->tx_, this->tx_len_);
        // The original waited for every byte to shift out, then held DE a further 25 ms.
        this->phase_ = Phase::TX_DRAIN;
        this->phase_until_ = now + bus_tx_time_ms(this->tx_len_) + BUS_DE_DRAIN_MS;
        continue;

      case Phase::TX_DRAIN:
        if (!reached(now, this->phase_until_))
          return;
        this->io_->flush_tx();   // already empty by now; guarantees it before DE drops
        this->io_->set_de(false);  // release: back to receive
        this->phase_ = Phase::LISTEN;
        this->phase_until_ = now + BUS_REPLY_TIMEOUT_MS;
        continue;

      case Phase::LISTEN: {
        int c;
        while ((c = this->io_->read()) >= 0) {
          size_t n = this->rx_.feed(static_cast<uint8_t>(c));
          if (n == 0)
            continue;
          const uint8_t *f = this->rx_.data();
          // A completed frame of the wrong class is a late reply to something else (#60): drop
          // it and keep listening within this window.
          if (n > FRAME_CLASS_OFFSET && reply_class_ok(f[FRAME_CLASS_OFFSET], this->expect_class_)) {
            // #49: latch the device type from a DevType reply. A 00 00 pair means "not linked
            // yet" to stock, so it is never adopted.
            uint8_t hi;
            uint8_t lo;
            if (devtype_from_reply(f, n, &hi, &lo) && (hi != 0 || lo != 0)) {
              this->token_[0] = hi;
              this->token_[1] = lo;
              this->token_seen_ = true;
            }
            this->finish_transaction_(n, now);
            break;
          }
        }
        if (this->phase_ != Phase::LISTEN)
          continue;
        if (!reached(now, this->phase_until_))
          return;
        this->finish_transaction_(0, now);  // timeout, no reply
        continue;
      }

      case Phase::WAIT:
        if (!reached(now, this->phase_until_))
          return;
        this->phase_ = Phase::IDLE;
        continue;
    }
  }
}

// Start whatever the current step sends. Steps that send nothing advance immediately.
void BusScheduler::start_step_(uint32_t now) {
  uint8_t f[TX_FRAME_MAX];
  switch (this->step_) {
    case Step::BOOT_DEVTYPE:
      this->begin_transaction_(LINK_INIT_0A, LINK_INIT_0A_LEN, CLASS_DEVTYPE, now);
      return;
    case Step::BOOT_07:
      this->begin_transaction_(LINK_INIT_07, LINK_INIT_07_LEN, 0x00, now);  // reply ignored
      return;
    case Step::CYCLE_BEGIN:
      this->cycle_start_ = now;
      // Cooperative link recovery: one DevType handshake per cycle while the A/C is silent.
      if (this->link_miss_ >= BUS_LINK_LOST_POLLS) {
        this->heard_ac_ = false;
        this->step_ = Step::RECOVER;
        this->begin_transaction_(LINK_INIT_0A, LINK_INIT_0A_LEN, CLASS_DEVTYPE, now);
        return;
      }
      this->step_ = Step::HEARTBEAT;
      this->start_step_(now);
      return;
    case Step::RECOVER:
      // Only reachable from CYCLE_BEGIN; start_step_ is not re-entered for it.
      return;
    case Step::HEARTBEAT: {
      // Byte 16 bit 6 says whether we have heard the A/C's 0x1E. Any reply class is accepted:
      // the A/C sometimes answers the heartbeat with a 0x66.
      size_t n = build_link_heartbeat(this->heard_ac_, f, sizeof(f));
      this->begin_transaction_(f, n, 0x00, now);
      return;
    }
    case Step::DRAIN: {
      if (this->queue_len_ == 0) {
        this->step_ = Step::STATUS;
        this->start_step_(now);
        return;
      }
      size_t slot = this->queue_head_;
      size_t len = this->queue_lens_[slot];
      std::memcpy(f, this->queue_[slot], len);
      this->queue_head_ = (this->queue_head_ + 1) % BUS_TX_QUEUE_LEN;
      this->queue_len_--;
      this->begin_transaction_(f, len, 0x00, now);  // the A/C may echo a status frame
      return;
    }
    case Step::STATUS:
      // Correlated on 0x66, so a late 0x1E cannot reset the miss counter.
      this->begin_transaction_(STATUS_REQUEST, STATUS_REQUEST_LEN, CLASS_STATUS, now);
      return;
    case Step::PRODUCT:
      if (this->heard_ac_ && (!this->features_.valid || this->pt_poll_ == 0)) {
        size_t n = build_producttype_request(f, sizeof(f));
        if (n > 0) {
          this->begin_transaction_(f, n, CLASS_STATUS, now);
          return;
        }
      }
      this->on_step_done_(0, now);
      return;
    case Step::PACE:
      if (!reached(now, this->cycle_start_ + BUS_CYCLE_MS)) {
        this->phase_ = Phase::WAIT;
        this->phase_until_ = this->cycle_start_ + BUS_CYCLE_MS;
      }
      this->step_ = Step::CYCLE_BEGIN;
      return;
  }
}

void BusScheduler::on_step_done_(size_t reply_len, uint32_t now) {
  switch (this->step_) {
    case Step::BOOT_DEVTYPE:
      if (reply_len > 0) {
        this->step_ = Step::BOOT_07;
        return;
      }
      // Every failed try is followed by the 500 ms wait, the tenth included.
      this->boot_tries_++;
      this->phase_ = Phase::WAIT;
      this->phase_until_ = now + BUS_BOOT_RETRY_MS;
      if (this->boot_tries_ >= BUS_BOOT_TRIES)
        this->step_ = Step::BOOT_07;
      return;
    case Step::BOOT_07:
      this->step_ = Step::CYCLE_BEGIN;
      return;
    case Step::CYCLE_BEGIN:
    case Step::RECOVER:
      this->step_ = Step::HEARTBEAT;
      return;
    case Step::HEARTBEAT:
      if (reply_len > 0) {
        this->heard_ac_ = true;
        this->consume_(reply_len);  // no-op unless the A/C answered with a 0x66
      }
      this->step_ = Step::DRAIN;
      return;
    case Step::DRAIN:
      if (reply_len > 0)
        this->consume_(reply_len);
      return;  // stay in DRAIN until the queue is empty
    case Step::STATUS: {
      // Only a checksum-valid 0x66 counts as the link being alive (#12).
      if (reply_len > 0 && this->consume_(reply_len)) {
        this->link_miss_ = 0;
      } else if (this->link_miss_ < 0xFF) {
        this->link_miss_++;
      }
      bool silent = this->link_miss_ >= BUS_LINK_LOST_POLLS;
      if (silent != this->link_down_) {
        this->link_down_ = silent;
        if (this->listener_ != nullptr)
          this->listener_->on_bus_link(!silent);
      }
      this->step_ = Step::PRODUCT;
      return;
    }
    case Step::PRODUCT:
      if (reply_len > 0)
        this->consume_(reply_len);
      if (++this->pt_poll_ >= BUS_PRODUCTTYPE_CYCLES)
        this->pt_poll_ = 0;
      this->step_ = Step::PACE;
      return;
    case Step::PACE:
      return;
  }
}

// hisense_consume_status(): a 0x66 reply is either the ProductType answer (subtype 0x40) or the
// status frame. A well-framed frame with a bad checksum is counted and not parsed (#12).
bool BusScheduler::consume_(size_t n) {
  const uint8_t *f = this->rx_.data();
  if (n < CMD_HEADER_LEN || f[FRAME_CLASS_OFFSET] != CLASS_STATUS)
    return false;
  if (!status_checksum_ok(f, n)) {
    this->chk_mismatch_++;
    return false;
  }
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
