#include "hisense_ac.h"
#include "hisense_climate.h"
#include "esphome/core/log.h"

#include <cinttypes>
#include <cmath>

namespace esphome::hisense_ac {

static const char *const TAG = "hisense_ac";

// What each log level shows:
//   WARN          the bus link dropping, a frame that could not be sent, a bad checksum, a fault,
//                 a command that got no reply, was sent again, or was given up on
//   INFO          the bus link coming back, a fault clearing, a command taken on a re-send, a setup
//                 request from the remote, the "77" indication going on or off
//   DEBUG         a status change, every command sent, the unit's capabilities, a change in the
//                 request bits of the unit's 0x1E reply
//   VERBOSE       every decoded status frame, every poll that got no reply
//   VERY_VERBOSE  the raw bytes of every frame sent and received

// ---- Bus results ------------------------------------------------------------------------------
// These run inside bus_.poll(). Entities are not touched here: the frame is parked and loop()
// publishes it after the special-mode queue has had its turn.
void HisenseAC::on_bus_status(const AcState &state) {
  this->pending_ = state;
  this->pending_valid_ = true;
}

void HisenseAC::on_bus_features(const AcFeatures &features) {}  // polled in publish_diagnostics_

#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERY_VERBOSE
// A status frame is about 160 bytes, more than one log line holds, so frames go out in rows.
static void log_frame(const char *direction, const uint8_t *frame, size_t len) {
  static const size_t ROW = 32;
  char hex[format_hex_pretty_size(ROW)];
  for (size_t at = 0; at < len; at += ROW) {
    const size_t n = len - at < ROW ? len - at : ROW;
    ESP_LOGVV(TAG, "%s %3u: %s", direction, (unsigned) at, format_hex_pretty_to(hex, frame + at, n, ' '));
  }
}
#endif

void HisenseAC::on_bus_frame(const uint8_t *frame, size_t len) {
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERY_VERBOSE
  log_frame("RX", frame, len);
#endif
}

void HisenseAC::on_bus_checksum_error(const uint8_t *frame, size_t len) {
  ESP_LOGW(TAG, "Frame with a bad checksum dropped (%" PRIu32 " since boot)", this->bus_.checksum_mismatches());
}

void HisenseAC::on_bus_timeout(uint8_t expect_class) {
  ESP_LOGV(TAG, "No reply within the window (expected class 0x%02X)", expect_class);
}

#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_DEBUG
static const LogString *command_reply_to_string(uint8_t reply) {
  switch (reply) {
    case COMMAND_REPLY_ACK:
      return LOG_STR("echo, ack");
    case COMMAND_REPLY_NAK:
      return LOG_STR("echo, no ack");
    default:
      return LOG_STR("not an echo");
  }
}
#endif

void HisenseAC::on_bus_command(bool answered, uint8_t reply_class) {
  // The unit answers every command it hears. Silence means the frame was lost on the way or
  // arrived damaged.
  if (answered) {
    ESP_LOGD(TAG, "Command answered (class 0x%02X, %s)", reply_class,
             LOG_STR_ARG(command_reply_to_string(this->bus_.command_reply())));
  } else if (this->bus_.command_resend_pending()) {
    ESP_LOGW(TAG, "Command frame got no reply from the unit, sending it again");
  } else {
    ESP_LOGW(TAG, "Command frame got no reply from the unit, not sent again");
  }
  this->commands_.on_command_done(this->bus_.command_ticket(), answered, millis());
}

void HisenseAC::on_bus_link(bool up) {
  if (!up) {
    // Drop any undrained pre-loss frame, or loop() would publish those stale values (and flip
    // link_up_ back to true) until real data returns.
    this->pending_valid_ = false;
    // A silent unit may be one that lost power, and it comes back with the placeholder.
    outdoor_temp_gate_reset(&this->outdoor_gate_);
  }
  this->link_up_ = up;
  this->link_dirty_ = true;
}

void HisenseAC::on_bus_setup_request(uint8_t request) { this->setup_request_pending_ = request; }

void HisenseAC::set_setup_indicator(bool on) {
  if (on == this->bus_.setup_indicator())
    return;
  ESP_LOGI(TAG, "Panel setup indication (\"77\") %s", on ? LOG_STR_LITERAL("on") : LOG_STR_LITERAL("off"));
  this->bus_.set_setup_indicator(on, millis());
}

void HisenseAC::setup() {
  if (this->de_pin_ != nullptr) {
    this->de_pin_->setup();
    this->de_pin_->digital_write(false);  // idle low = receive, before the UART speaks
  }
  this->commands_.set_sink(this);
  this->bus_.setup(this, this, this->de_pin_ != nullptr);
}

void HisenseAC::loop() {
  this->bus_.poll(millis());
  // DE settle and drain are a few ms each; the default ~16 ms loop would stretch them, so ask for
  // a tight loop only while they run.
  if (this->bus_.wants_fast_loop()) {
    this->fast_loop_.start();
  } else {
    this->fast_loop_.stop();
  }

  AcState state;
  bool have_state = false;
  if (this->pending_valid_) {
    state = this->pending_;
    this->pending_valid_ = false;
    have_state = true;
  }

  const bool link_edge = this->link_dirty_;
  if (link_edge) {
    this->link_dirty_ = false;
    // The warning flag is what a status LED blinks on.
    if (this->link_up_) {
      ESP_LOGI(TAG, "A/C bus link restored");
      this->status_clear_warning();
    } else {
      ESP_LOGW(TAG, "A/C bus link lost: the unit is not answering");
      this->status_set_warning(LOG_STR("A/C bus link down"));
    }
#ifdef USE_BINARY_SENSOR
    // publish_telemetry_() only runs on a decoded frame, so it can only ever publish true; the
    // loss edge has to be published here or the sensor never shows the link down.
    if (this->bus_link_binary_sensor_ != nullptr)
      this->bus_link_binary_sensor_->publish_state(this->link_up_);
#endif
  }
  if (have_state)
    this->log_status_(state);
  // The command tracker takes the link-lost edge, the special-mode queue and the status frame in
  // the one order that is right (CommandTracker::loop_pass). Nothing is re-sent into a dead bus,
  // and nothing still waiting is sent when it comes back.
  this->commands_.loop_pass(link_edge && !this->link_up_, have_state ? &state : nullptr, millis());
  // No status frame arrives while the link is down, so the edge is the one moment the counters
  // can still go out.
  if (link_edge)
    this->publish_bus_counters_(false);

  if (have_state)
    this->process_status_(state);

#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_DEBUG
  // Every change, taken or not: this is how to see what a given unit raises for the gesture.
  if (this->bus_.link_request() != this->logged_link_request_) {
    this->logged_link_request_ = this->bus_.link_request();
    ESP_LOGD(TAG, "0x1E reply request bits now 0x%02X", this->logged_link_request_);
  }
#endif
  if (this->setup_request_pending_ != 0) {
    const uint8_t request = this->setup_request_pending_;
    this->setup_request_pending_ = 0;
    ESP_LOGI(TAG, "Setup requested from the remote (0x1E reply request bits 0x%02X)", request);
    this->setup_request_callback_.call(request);
  }
}

void HisenseAC::log_status_(const AcState &state) {
  ESP_LOGV(TAG, "Status: power=%d mode=%d setpoint=%d indoor=%d fan_raw=0x%02X holdoff=%d", state.power_on,
           (int) state.mode, state.setpoint_c, state.indoor_temp_c, state.fan_raw, this->in_command_holdoff());
  // A frame arrives about once a second, so DEBUG gets it only when something a user would
  // recognise moved. The sleep and mute bytes are in the list because they are how the unit
  // answers those two commands: one that is accepted and then ignored looks the same as one that
  // was never sent unless the raw value is seen to move.
  const AcState &was = this->commands_.last_state();
  if (!was.valid || was.power_on != state.power_on || was.mode != state.mode || was.setpoint_c != state.setpoint_c ||
      was.indoor_temp_c != state.indoor_temp_c || was.fan_raw != state.fan_raw || was.sleep_raw != state.sleep_raw ||
      was.mute_on != state.mute_on || was.eco_on != state.eco_on || was.turbo_on != state.turbo_on) {
    ESP_LOGD(
        TAG,
        "Status changed: power=%d mode=%d setpoint=%d indoor=%d fan_raw=0x%02X eco=%d turbo=%d mute=%d sleep_raw=%u",
        state.power_on, (int) state.mode, state.setpoint_c, state.indoor_temp_c, state.fan_raw, state.eco_on,
        state.turbo_on, state.mute_on, state.sleep_raw);
  }
}

void HisenseAC::process_status_(const AcState &state) {
  // A decoded frame IS the link being up. The link callback only fires on EDGES, so a link that
  // is healthy from boot never produces a "restored" edge and link_up_ would sit at its initial
  // false forever, reporting a working bus as down.
  this->link_up_ = true;
  this->status_clear_warning();
#ifdef USE_CLIMATE
  if (this->climate_ != nullptr)
    this->climate_->update_from_bus(state, this->in_command_holdoff());
#endif
  for (auto *listener : this->listeners_)
    listener->on_status(state);

  this->publish_telemetry_(state);
  this->publish_diagnostics_();
}

#ifdef USE_SENSOR
void HisenseAC::publish_sensor_(sensor::Sensor *sensor, float value, bool refresh_due) {
  if (sensor == nullptr)
    return;
  if (telemetry_publish_due(sensor->has_state(), sensor->get_raw_state(), value, refresh_due))
    sensor->publish_state(value);
}
#endif

void HisenseAC::publish_telemetry_(const AcState &state) {
#ifdef USE_SENSOR
  // An unchanged value is still republished once per refresh period: integrating sensors such as
  // total_daily_energy only advance when their source publishes.
  const uint32_t now = millis();
  const bool refresh = !this->telemetry_refreshed_ || now - this->last_refresh_ms_ >= TELEMETRY_REFRESH_MS;
  if (refresh) {
    this->last_refresh_ms_ = now;
    this->telemetry_refreshed_ = true;
  }
  this->publish_sensor_(this->indoor_temperature_sensor_, state.indoor_temp_c, refresh);
  // Both read -20 C for a while after mains power returns: the unit's "not measured yet" value.
  // Those frames go out as unknown (see outdoor_temps_measured in hisense_map.h).
  const bool outdoor_measured = outdoor_temps_measured(&this->outdoor_gate_, state.outdoor_temp_c, state.coil_temp_c);
  this->publish_sensor_(this->outdoor_temperature_sensor_, outdoor_measured ? state.outdoor_temp_c : NAN, refresh);
  this->publish_sensor_(this->coil_temperature_sensor_, outdoor_measured ? state.coil_temp_c : NAN, refresh);
  this->publish_sensor_(this->compressor_frequency_sensor_, state.compressor_freq, refresh);
  // The bus carries a current PROXY, not amps: active power is 4.15 * raw^2, calibrated against
  // a panel meter. hisense_map.h owns that maths and works in milli-units, so scale back here.
  this->publish_sensor_(this->power_sensor_, active_power_mw(state.current_raw) / 1000.0f, refresh);
  this->publish_sensor_(this->voltage_sensor_, voltage_mv(state.voltage_raw) / 1000.0f, refresh);
  this->publish_sensor_(this->current_sensor_, active_current_ma(state.current_raw, state.voltage_raw) / 1000.0f,
                        refresh);
  this->publish_bus_counters_(refresh);
#endif
#ifdef USE_BINARY_SENSOR
  if (this->aux_heat_binary_sensor_ != nullptr)
    this->aux_heat_binary_sensor_->publish_state(state.heat_relay_on);
  if (this->bus_link_binary_sensor_ != nullptr)
    this->bus_link_binary_sensor_->publish_state(this->link_up_);
#endif
}

void HisenseAC::publish_bus_counters_(bool refresh_due) {
#ifdef USE_SENSOR
  this->publish_sensor_(this->checksum_errors_sensor_, this->bus_.checksum_mismatches(), refresh_due);
  this->publish_sensor_(this->reply_timeouts_sensor_, this->bus_.reply_timeouts(), refresh_due);
  this->publish_sensor_(this->unanswered_commands_sensor_, this->bus_.unanswered_commands(), refresh_due);
  this->publish_sensor_(this->command_retries_sensor_, this->bus_.command_resends() + this->commands_.command_retries(),
                        refresh_due);
  this->publish_sensor_(this->failed_commands_sensor_, this->commands_.failed_commands(), refresh_due);
  this->publish_sensor_(this->link_losses_sensor_, this->bus_.link_losses(), refresh_due);
#endif
}

void HisenseAC::publish_diagnostics_() {
  AcFaults faults;
  if (this->bus_.faults(&faults)) {
    uint32_t bitmap = faults_to_bitmap32(faults);
    if (bitmap != this->logged_faults_) {
      if (bitmap != 0) {
        ESP_LOGW(TAG, "A/C reports a fault: bitmap 0x%08" PRIX32, bitmap);
      } else {
        ESP_LOGI(TAG, "A/C fault cleared");
      }
      this->logged_faults_ = bitmap;
    }
#ifdef USE_BINARY_SENSOR
    if (this->problem_binary_sensor_ != nullptr)
      this->problem_binary_sensor_->publish_state(faults.any);
    for (auto &entry : this->fault_sensors_)
      entry.second->publish_state((bitmap >> entry.first) & 1u);
#endif
  }

  // Capabilities answer once, after the 0x66/40 ProductType exchange, and never change.
  AcFeatures features;
  if (this->features_published_ || !this->bus_.features(&features) || !features.valid)
    return;
  this->features_published_ = true;
  uint32_t bitmap = features_to_bitmap32(features);
  ESP_LOGD(TAG, "A/C capabilities: bitmap 0x%08" PRIX32, bitmap);
#ifdef USE_BINARY_SENSOR
  for (auto &entry : this->capability_sensors_)
    entry.second->publish_state((bitmap >> entry.first) & 1u);
#endif
#ifdef USE_TEXT_SENSOR
  if (this->link_token_text_sensor_ != nullptr) {
    uint8_t hi = 0;
    uint8_t lo = 0;
    this->bus_.link_token(&hi, &lo);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02X %02X", hi, lo);
    this->link_token_text_sensor_->publish_state(buf);
  }
#endif
}

// ---- Commands ---------------------------------------------------------------------------------
BusTicket HisenseAC::send_frame_(const uint8_t *frame, size_t len) {
  // Builders write the buzzer bit set, as the stock module does by default.
  if (this->beeper_)
    return this->bus_.enqueue(frame, len);
  uint8_t quiet[CMD_FRAME_MAX];
  size_t n = stamp_beep(frame, len, false, quiet, sizeof(quiet));
  return n != 0 ? this->bus_.enqueue(quiet, n) : BUS_TICKET_NONE;
}

BusTicket HisenseAC::send_combined(const AcCommand &cmd) {
  uint8_t frame[CMD_FRAME_MAX];
  ESP_LOGD(TAG, "TX combined: power_on=%d mode=%d setpoint=%d fan=0x%02X vswing=%d hswing=%d feature=%d", cmd.power_on,
           (int) cmd.mode, (int) cmd.setpoint, (unsigned) cmd.fan, (int) cmd.vswing, (int) cmd.hswing,
           (int) cmd.feature);
  // Stamp the user's standing display preference on every combined frame. Leaving it at
  // NOCHANGE writes 0x00, which real hardware treats as "on".
  AcCommand stamped = cmd;
  stamped.display = this->display_pref_;
  size_t len = build_command(stamped, frame, sizeof(frame));
  if (len == 0) {
    ESP_LOGW(TAG, "Command frame could not be built");
    return BUS_TICKET_NONE;
  }
  const BusTicket ticket = this->send_frame_(frame, len);
  if (ticket == BUS_TICKET_NONE) {
    ESP_LOGW(TAG, "Command frame dropped: TX queue full");
  }
  return ticket;
}

BusTicket HisenseAC::send_power(bool on) {
  uint8_t frame[CMD_FRAME_MAX];
  size_t len = build_power_frame(on, frame, sizeof(frame));
  const BusTicket ticket = len != 0 ? this->send_frame_(frame, len) : BUS_TICKET_NONE;
  if (ticket == BUS_TICKET_NONE) {
    ESP_LOGW(TAG, "Power %s frame not sent", on ? LOG_STR_LITERAL("on") : LOG_STR_LITERAL("off"));
  } else {
    ESP_LOGD(TAG, "TX power %s", on ? LOG_STR_LITERAL("on") : LOG_STR_LITERAL("off"));
  }
  return ticket;
}

/* Mute and sleep use the MINIMAL single-field frame, which is what the vendor module's generic
 * attribute setter sends: one field set, every other byte left at 0x00, "leave alone".
 *
 * The A/C ignores that frame unless it also carries frame[31] = 0x01, the marker every combined
 * command writes and a zeroed buffer omits. With the marker added in build_single_field(), both
 * attributes work: all four sleep profiles select (sleep_raw 2/4/6/8) and mute engages with
 * fan_raw 0x02.
 *
 * The minimal frame is preferable to patching the combined one, because it leaves mode,
 * setpoint, fan and swing alone instead of re-asserting the shadow on every mute or sleep. */
BusTicket HisenseAC::send_mute(bool on) {
  uint8_t frame[CMD_FRAME_MAX];
  size_t len = build_mute_frame(on, frame, sizeof(frame));
  const BusTicket ticket = len != 0 ? this->send_frame_(frame, len) : BUS_TICKET_NONE;
  if (ticket == BUS_TICKET_NONE) {
    ESP_LOGW(TAG, "Mute frame not sent");
  }
  return ticket;
}

BusTicket HisenseAC::send_sleep(uint8_t profile) {
  uint8_t frame[CMD_FRAME_MAX];
  size_t len = build_sleep_frame(profile, frame, sizeof(frame));
  const BusTicket ticket = len != 0 ? this->send_frame_(frame, len) : BUS_TICKET_NONE;
  if (ticket == BUS_TICKET_NONE) {
    ESP_LOGW(TAG, "Sleep frame not sent");
  }
  return ticket;
}

// ---- What the command tracker did ---------------------------------------------------------------
// Every decision is taken in hisense_commands.h by functions of hisense_map.h, with a host test
// (test_esphome_confirm.cpp). This only says what happened.
void HisenseAC::on_command_event(CommandEvent event, uint16_t fields, uint16_t count) {
  switch (event) {
    case CommandEvent::COMBINED_REFUSED_NO_STATUS:
      ESP_LOGW(TAG, "Command not sent: no status from the unit yet, so the frame would carry defaults");
      break;
    case CommandEvent::REFUSED_LINK_DOWN:
      ESP_LOGW(TAG, "Command refused: the unit is not answering");
      break;
    case CommandEvent::MAIN_CONFIRMED:
      if (count > 0) {
        ESP_LOGI(TAG, "Command taken by the unit after %u re-send(s)", count);
      } else {
        ESP_LOGD(TAG, "Command confirmed by the unit's status");
      }
      break;
    case CommandEvent::MAIN_YIELDED:
      ESP_LOGD(TAG, "Unit was changed from elsewhere (fields 0x%03X), command not sent again", fields);
      break;
    case CommandEvent::MAIN_GIVEN_UP:
      ESP_LOGW(TAG, "Command not taken by the unit after %u re-send(s) (fields 0x%03X), showing what it reports", count,
               fields);
      break;
    case CommandEvent::MAIN_RESENT:
      ESP_LOGW(TAG, "Command not taken by the unit (fields 0x%03X), sending it again (%u of %u)", fields, count,
               COMMAND_RESEND_MAX);
      break;
    case CommandEvent::SPECIAL_CONFIRMED:
      if (count > 0) {
        ESP_LOGI(TAG, "Special-mode command taken by the unit after %u re-send(s)", count);
      }
      break;
    case CommandEvent::SPECIAL_YIELDED:
      ESP_LOGD(TAG, "Special mode was changed from elsewhere (fields 0x%03X), command not sent again", fields);
      break;
    case CommandEvent::SPECIAL_GIVEN_UP:
      ESP_LOGW(TAG, "Special-mode command not taken by the unit after %u re-send(s) (fields 0x%03X)", count, fields);
      break;
    case CommandEvent::SPECIAL_REPLANNED:
      ESP_LOGW(TAG, "Special-mode command not taken by the unit (fields 0x%03X), %u command(s) queued again", fields,
               count);
      break;
    case CommandEvent::SPECIAL_SENDING:
      switch (fields) {
        case SPECIAL_OP_FEATURE:
          ESP_LOGD(TAG, "TX special: byte33 feature %u", count);
          break;
        case SPECIAL_OP_MUTE:
          ESP_LOGD(TAG, "TX special: mute %u", count);
          break;
        case SPECIAL_OP_SLEEP:
          ESP_LOGD(TAG, "TX special: sleep profile %u", count);
          break;
        default:
          break;
      }
      break;
    case CommandEvent::SPECIAL_QUEUE_FULL:
      ESP_LOGW(TAG, "Special-mode queue full, op %u refused", fields);
      break;
    case CommandEvent::SPECIAL_NOT_SENT:
      ESP_LOGW(TAG, "Special-mode command (op %u, value %u) could not be queued for the bus, given up", fields, count);
      break;
    case CommandEvent::PRESET_QUEUED:
      ESP_LOGD(TAG, "Preset %s: %u command(s) queued", PRESETS[fields].name, count);
      break;
    case CommandEvent::DROPPED_ON_LINK_LOSS:
      ESP_LOGW(TAG, "%u command(s) unconfirmed or unsent when the link dropped, not sent again", count);
      break;
    case CommandEvent::SHADOW_MODE_KEPT:
      ESP_LOGV(TAG, "Status mode %u is not a mode, shadow kept", fields);
      break;
    case CommandEvent::SHADOW_SETPOINT_KEPT:
      ESP_LOGV(TAG, "Status setpoint %d C is outside the command range, shadow kept", (int8_t) fields);
      break;
  }
}

void HisenseAC::dump_config() {
  ESP_LOGCONFIG(TAG, "Hisense A/C (RS-485 9600 8N1)");
  LOG_PIN("  DE pin: ", this->de_pin_);
  if (this->de_pin_ == nullptr) {
    ESP_LOGCONFIG(TAG, "  DE: owned by the UART (flow_control_pin) or the transceiver");
  }
  this->check_uart_settings(BUS_BAUD_RATE, 1, uart::UART_CONFIG_PARITY_NONE, 8);
  ESP_LOGCONFIG(TAG, "  link: %s", this->link_up_ ? LOG_STR_LITERAL("up") : LOG_STR_LITERAL("down"));
  ESP_LOGCONFIG(TAG, "  panel setup indication (\"77\"): %s",
                this->bus_.setup_indicator() ? LOG_STR_LITERAL("on") : LOG_STR_LITERAL("off"));
  const AcState &last = this->commands_.last_state();
  if (last.valid) {
    ESP_LOGCONFIG(TAG, "  last status: power=%d mode=%d setpoint=%dC indoor=%dC", last.power_on, (int) last.mode,
                  last.setpoint_c, last.indoor_temp_c);
  } else {
    ESP_LOGCONFIG(TAG, "  no status frame decoded yet");
  }
}

// ---- BusIO ------------------------------------------------------------------------------------
void HisenseAC::bus_set_de(bool high) {
  if (this->de_pin_ != nullptr)
    this->de_pin_->digital_write(high);
}
void HisenseAC::bus_write(const uint8_t *data, size_t len) {
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERY_VERBOSE
  log_frame("TX", data, len);
#endif
  this->write_array(data, len);
}
void HisenseAC::bus_flush() { this->flush(); }
int HisenseAC::bus_read() {
#ifdef USE_LIBRETINY
  // LibreTiny's Serial keeps received bytes in the Arduino RingBufferN, whose reader decrements
  // its element count with the receive interrupt enabled. An interrupt that lands inside that
  // decrement stores a byte and the count loses it: from then on the byte sits in the buffer
  // uncounted and every frame arrives short by one byte, late by one byte, which the assembler
  // can only time out on. Measured on an RTL8710C with the UART in loopback at 9600 baud, read
  // in groups as this loop reads: 5 such losses in 233034 bytes without this lock, none in
  // 233287 with it (firmware/docs/15-esphome-path.md has the runs). The lock covers one buffered
  // byte, a few microseconds.
  InterruptLock lock;
#endif
  uint8_t b;
  if (this->available() > 0 && this->read_byte(&b))
    return b;
  return -1;
}

}  // namespace esphome::hisense_ac
