// Confirm and retry in the ESPHome component (hisense_map.h): did a command take, should it be sent
// again, and is a difference between what was asked and what the unit reports just the unit being
// itself. Plus power and mode in one frame.
//
// The first half checks each decision on its own. The second half runs the decisions as the hub
// does (HisenseAC::confirm_main_, confirm_special_ and the special-mode queue) against a model of
// the unit that can lose frames, swallow special-mode commands inside its debounce, pin its
// setpoint and force turbo. The hub itself needs ESPHome and cannot be built here, so `Hub` below
// repeats its few lines of glue. If the two drift, the scenarios stop describing the firmware.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "hisense_map.h"
#include "hisense_protocol.h"

namespace H = esphome::hisense_ac;

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

static const H::SpecialState NO_SPECIAL{};

static H::UnitView view(bool on, H::Mode mode, int setpoint, uint8_t fan_raw) {
  H::UnitView v;
  v.power_on = on;
  v.mode_valid = true;
  v.mode = mode;
  v.setpoint_c = (int8_t) setpoint;
  v.fan_raw = fan_raw;
  return v;
}

// ---- Transport: a frame with no reply -----------------------------------------------------------
static void test_resend_rule() {
  printf("-- a command frame with no reply\n");
  CHECK(H::command_resend_allowed(1, true) && H::command_resend_allowed(2, true), "sent again after one and two");
  CHECK(!H::command_resend_allowed(3, true), "three sends is the limit");
  for (int sends = 0; sends < 8; sends++)
    CHECK(!H::command_resend_allowed((uint8_t) sends, false), "never into a unit that is not answering (%d)", sends);
}

static void test_reply_verdict() {
  printf("-- what the reply to a command says\n");
  uint8_t tx[H::CMD_FRAME_MAX];
  size_t n = H::build_command(H::AcCommand{}, tx, sizeof(tx));
  uint8_t reply[20] = {0xF4, 0xF5, 0x01, 0x40, 0x0B};
  reply[13] = 0x65;
  reply[15] = 0x01;
  CHECK(H::command_reply_verdict(tx, n, nullptr, 0) == H::COMMAND_REPLY_NONE, "no frame");
  CHECK(H::command_reply_verdict(tx, n, reply, 0) == H::COMMAND_REPLY_NONE, "empty window");
  CHECK(H::command_reply_verdict(tx, n, reply, sizeof(reply)) == H::COMMAND_REPLY_ACK, "echo with ack 1");
  reply[15] = 0x00;
  CHECK(H::command_reply_verdict(tx, n, reply, sizeof(reply)) == H::COMMAND_REPLY_NAK, "echo without the ack");
  reply[15] = 0x01;
  reply[14] = 0x40;
  CHECK(H::command_reply_verdict(tx, n, reply, sizeof(reply)) == H::COMMAND_REPLY_OTHER, "sub-class differs");
  reply[14] = 0x00;
  reply[13] = 0x66;
  CHECK(H::command_reply_verdict(tx, n, reply, sizeof(reply)) == H::COMMAND_REPLY_OTHER, "a status frame");
  reply[13] = 0x1E;
  CHECK(H::command_reply_verdict(tx, n, reply, sizeof(reply)) == H::COMMAND_REPLY_OTHER, "a link frame");
  reply[13] = 0x65;
  CHECK(H::command_reply_verdict(tx, n, reply, 15) == H::COMMAND_REPLY_OTHER, "too short to hold the ack byte");
}

// ---- Power and mode in one frame ----------------------------------------------------------------
static void test_one_frame_power() {
  printf("-- power and mode in one frame\n");
  uint8_t on[H::CMD_FRAME_MAX];
  CHECK(H::build_power_frame(true, on, sizeof(on)) > 0 && on[H::CMD_MODE_BYTE] == H::CMD_POWER_ON_BITS,
        "the power-on bits are the literal power-on frame's byte 18 (0x%02X)", on[H::CMD_MODE_BYTE]);
  // Byte 18 as the stock module sends it: 0x5C cool and 0x3C heat are in the stock image (RE docs/10
  // section 5b-2). The other three follow from the same field rule and still need a unit.
  const struct {
    H::Mode mode;
    uint8_t byte18;
  } rows[] = {
      {H::MODE_COOL, 0x5C}, {H::MODE_HEAT, 0x3C}, {H::MODE_AUTO, 0x9C}, {H::MODE_DRY, 0x7C}, {H::MODE_FAN, 0x1C}};
  for (const auto &row : rows) {
    H::AcCommand c;
    c.mode = row.mode;
    uint8_t plain[H::CMD_FRAME_MAX], with[H::CMD_FRAME_MAX];
    size_t np = H::build_command(c, plain, sizeof(plain));
    c.power_on = true;
    size_t nw = H::build_command(c, with, sizeof(with));
    CHECK(nw > 0 && with[18] == row.byte18, "mode %d with power-on is 0x%02X (got 0x%02X)", (int) row.mode, row.byte18,
          with[18]);
    CHECK(np > 0 && plain[18] == (row.byte18 & 0xF0), "and without it the run bits stay clear (0x%02X)", plain[18]);
    CHECK(H::status_checksum_ok(with, nw) || nw > H::CMD_FRAME_LEN, "checksum covers the new bits");
    int diff = 0;
    for (size_t i = 0; i < H::CMD_CHK_OFFSET; i++)
      diff += plain[i] != with[i];
    CHECK(diff == 1, "nothing else in the frame moves (%d bytes differ)", diff);
  }
}

// ---- What a command asks ------------------------------------------------------------------------
static void test_intent() {
  printf("-- what a command asks of the unit\n");
  H::AcCommand c;
  c.mode = H::MODE_HEAT;
  c.setpoint = 22;
  c.fan = H::FAN_SPEED_MID;
  c.vswing = H::SWING_MODE_SWING;
  H::CommandIntent i = H::intent_from_command(c, H::CONFIRM_MODE | H::CONFIRM_SETPOINT);
  CHECK(i.fields == (H::CONFIRM_MODE | H::CONFIRM_SETPOINT), "only what the user asked is checked");
  CHECK(i.want.mode == H::MODE_HEAT && i.want.setpoint_c == 22 && i.want.fan_raw == 0x0E && i.want.vswing,
        "the values are the whole shadow");
  CHECK((i.known & H::CONFIRM_FAN) != 0 && (i.known & H::CONFIRM_POWER) == 0, "known: fan yes, power no");

  i = H::intent_from_command(c, H::CONFIRM_MODE | H::CONFIRM_POWER);
  CHECK((i.fields & H::CONFIRM_POWER) != 0 && i.want.power_on, "a power-on frame asks for power");

  // Every fan the entity can command has a status value to check against.
  for (uint8_t idx = 0; idx <= H::FAN_INDEX_MAX; idx++) {
    if (idx == 1)
      continue;  // quiet is the mute flag, not a fan command
    c.fan = H::fan_index_to_hisense(idx);
    i = H::intent_from_command(c, H::CONFIRM_FAN);
    CHECK(i.fields == H::CONFIRM_FAN && H::fan_raw_to_index(i.want.fan_raw) == idx, "fan index %u checks as raw 0x%02X",
          idx, i.want.fan_raw);
  }
  c.fan = H::FAN_SPEED_NOCHANGE;
  CHECK(H::intent_from_command(c, H::CONFIRM_FAN).fields == 0, "a fan the shadow does not know is not checked");
  c.fan = H::FAN_SPEED_QUIET;
  CHECK(H::intent_from_command(c, H::CONFIRM_FAN).fields == 0, "nor the quiet step");

  // An F panel reports the setpoint through f_to_c: the intent holds what will come back.
  for (int t = H::SETPOINT_MIN_C; t <= H::SETPOINT_MAX_C; t++) {
    H::AcCommand f;
    H::setpoint_to_cmd(t, true, &f);
    i = H::intent_from_command(f, H::CONFIRM_SETPOINT);
    CHECK(i.want.setpoint_c == H::f_to_c(f.setpoint), "F panel, %d C: checked as the status will read", t);
    H::AcCommand back;
    H::intent_to_command(i, true, &back);
    CHECK(back.setpoint == f.setpoint && back.fahrenheit, "and re-sent as the same wire byte (%d)", back.setpoint);
  }

  H::CommandIntent off = H::intent_power_off();
  CHECK(off.fields == H::CONFIRM_POWER && !off.want.power_on, "power off asks for exactly that");

  // Special-mode writes: one flag each.
  i = H::intent_from_special({H::SPECIAL_OP_FEATURE, H::FEATURE_ECO});
  CHECK(i.fields == H::CONFIRM_ECO && i.want.special.eco, "eco on");
  i = H::intent_from_special({H::SPECIAL_OP_FEATURE, H::FEATURE_ECO_OFF});
  CHECK(i.fields == H::CONFIRM_ECO && !i.want.special.eco, "eco off");
  i = H::intent_from_special({H::SPECIAL_OP_FEATURE, H::FEATURE_TURBO});
  CHECK(i.fields == H::CONFIRM_TURBO && i.want.special.turbo, "turbo on");
  i = H::intent_from_special({H::SPECIAL_OP_FEATURE, H::FEATURE_NONE});
  CHECK(i.fields == H::CONFIRM_TURBO && !i.want.special.turbo, "turbo off");
  i = H::intent_from_special({H::SPECIAL_OP_MUTE, 1});
  CHECK(i.fields == H::CONFIRM_MUTE && i.want.special.mute, "quiet on");
  i = H::intent_from_special({H::SPECIAL_OP_SLEEP, 3});
  CHECK(i.fields == H::CONFIRM_SLEEP && i.want.special.sleep == 3, "sleep profile");

  // Putting an intent back into the shadow touches only its own fields.
  H::AcCommand shadow;
  shadow.mode = H::MODE_COOL;
  shadow.setpoint = 30;
  shadow.fan = H::FAN_SPEED_HIGH;
  c = H::AcCommand{};
  c.mode = H::MODE_DRY;
  c.setpoint = 18;
  c.fan = H::FAN_SPEED_LOW;
  H::intent_to_command(H::intent_from_command(c, H::CONFIRM_MODE), false, &shadow);
  CHECK(shadow.mode == H::MODE_DRY && shadow.setpoint == 30 && shadow.fan == H::FAN_SPEED_HIGH, "mode only");
  H::intent_to_command(H::intent_from_command(c, H::CONFIRM_FAN | H::CONFIRM_SETPOINT), false, &shadow);
  CHECK(shadow.setpoint == 18 && shadow.fan == H::FAN_SPEED_LOW, "then setpoint and fan");
}

// ---- The unit being itself ----------------------------------------------------------------------
static void test_expected_fields() {
  printf("-- what the unit is expected to show, and what it overrides by design\n");
  const uint16_t all = H::CONFIRM_POWER | H::CONFIRM_MODE | H::CONFIRM_SETPOINT | H::CONFIRM_FAN | H::CONFIRM_SWING;
  H::AcCommand c;
  H::CommandIntent i = H::intent_from_command(c, all);

  H::UnitView v = view(true, H::MODE_COOL, 24, 0x01);
  CHECK(H::intent_expected_fields(i, v, NO_SPECIAL) == all, "cool: everything");
  v.mode = H::MODE_HEAT;
  CHECK(H::intent_expected_fields(i, v, NO_SPECIAL) == all, "heat: everything");

  for (H::Mode m : {H::MODE_AUTO, H::MODE_DRY, H::MODE_FAN}) {
    v.mode = m;
    uint16_t f = H::intent_expected_fields(i, v, NO_SPECIAL);
    CHECK((f & H::CONFIRM_SETPOINT) == 0, "mode %d: the unit keeps its own setpoint", (int) m);
    CHECK((f & H::CONFIRM_MODE) != 0 && (f & H::CONFIRM_SWING) != 0, "mode %d: mode and swing still checked", (int) m);
    CHECK(((f & H::CONFIRM_FAN) != 0) == (m != H::MODE_DRY), "mode %d: fan checked unless dry", (int) m);
  }

  v = view(false, H::MODE_COOL, 24, 0x01);
  CHECK(H::intent_expected_fields(i, v, NO_SPECIAL) == H::CONFIRM_POWER, "powered down: only power");
  H::CommandIntent no_power = H::intent_from_command(c, H::CONFIRM_SETPOINT | H::CONFIRM_FAN);
  CHECK(H::intent_expected_fields(no_power, v, NO_SPECIAL) == 0, "a setpoint sent to a unit that is off: nothing");

  v = view(true, H::MODE_COOL, 16, 0x12);
  v.special.turbo = true;
  uint16_t f = H::intent_expected_fields(i, v, NO_SPECIAL);
  CHECK(f == (H::CONFIRM_POWER | H::CONFIRM_SWING), "turbo: mode, setpoint and fan are the unit's (0x%03X)", f);
  v = view(true, H::MODE_HEAT, 24, 0x0A);
  H::SpecialState turbo_coming;
  turbo_coming.turbo = true;
  f = H::intent_expected_fields(i, v, turbo_coming);
  CHECK(f == (H::CONFIRM_POWER | H::CONFIRM_SWING), "turbo queued and not yet on: the same");

  v = view(true, H::MODE_COOL, 24, 0x02);
  v.special.mute = true;
  f = H::intent_expected_fields(i, v, NO_SPECIAL);
  CHECK((f & H::CONFIRM_FAN) == 0 && (f & H::CONFIRM_SETPOINT) != 0, "quiet pins the fan, not the setpoint");
  v.special.mute = false;
  v.special.sleep = 2;
  CHECK((H::intent_expected_fields(i, v, NO_SPECIAL) & H::CONFIRM_FAN) == 0, "a sleep profile pins the fan");
  v.special.sleep = 0;
  v.special.eco = true;
  CHECK(H::intent_expected_fields(i, v, NO_SPECIAL) == all, "eco pins nothing");

  v = view(true, H::MODE_COOL, 24, 0x01);
  v.mode_valid = false;
  f = H::intent_expected_fields(i, v, NO_SPECIAL);
  CHECK((f & (H::CONFIRM_MODE | H::CONFIRM_SETPOINT | H::CONFIRM_FAN)) == 0,
        "a status with no mode judges none of them");

  // The status view: the same remap the entity uses.
  H::AcState st;
  st.power_on = true;
  st.mode = (H::Mode) 7;
  CHECK(!H::unit_view_from_status(st).mode_valid, "status mode 7 is no mode");
  st.mode = H::MODE_AUTO;
  st.sleep_raw = 6;
  st.mute_on = true;
  H::UnitView sv = H::unit_view_from_status(st);
  CHECK(sv.mode_valid && sv.mode == H::MODE_AUTO && sv.special.sleep == 3 && sv.special.mute, "view from a status");
}

// ---- The verdict --------------------------------------------------------------------------------
static H::PendingCommand pending(const H::CommandIntent &i, const H::UnitView &before, uint32_t now) {
  H::PendingCommand p;
  H::pending_begin(&p, i, before, now);
  return p;
}

static void test_decision() {
  printf("-- did it take: wait, done, send again, give up, yield\n");
  H::AcCommand c;
  c.mode = H::MODE_HEAT;
  c.setpoint = 22;
  const H::UnitView before = view(true, H::MODE_COOL, 24, 0x01);
  const H::UnitView after = view(true, H::MODE_HEAT, 22, 0x01);
  H::PendingCommand p = pending(H::intent_from_command(c, H::CONFIRM_MODE | H::CONFIRM_SETPOINT), before, 1000);
  uint16_t unmet = 0xFFFF;

  CHECK(H::confirm_decision(H::PendingCommand{}, before, NO_SPECIAL, 0, 2, &unmet) == H::CONFIRM_DONE && unmet == 0,
        "nothing waiting");
  CHECK(H::confirm_decision(p, before, NO_SPECIAL, 1000 + H::CONFIRM_SETTLE_MS - 1, 2, &unmet) == H::CONFIRM_WAIT,
        "inside the settle time a stale status proves nothing");
  CHECK(H::confirm_decision(p, after, NO_SPECIAL, 1000 + H::CONFIRM_SETTLE_MS - 1, 2, nullptr) == H::CONFIRM_WAIT,
        "and a matching one is not judged early either");
  CHECK(H::confirm_decision(p, after, NO_SPECIAL, 1000 + H::CONFIRM_SETTLE_MS, 2, &unmet) == H::CONFIRM_DONE &&
            unmet == 0,
        "taken");
  CHECK(H::confirm_decision(p, before, NO_SPECIAL, 1000 + H::CONFIRM_SETTLE_MS, 2, &unmet) == H::CONFIRM_RESEND &&
            unmet == (H::CONFIRM_MODE | H::CONFIRM_SETPOINT),
        "not taken: send again (unmet 0x%03X)", unmet);
  H::UnitView half = view(true, H::MODE_HEAT, 24, 0x01);
  CHECK(H::confirm_decision(p, half, NO_SPECIAL, 6000, 2, &unmet) == H::CONFIRM_RESEND && unmet == H::CONFIRM_SETPOINT,
        "half taken: still sent again");

  p.resends = 1;
  CHECK(H::confirm_decision(p, before, NO_SPECIAL, 6000, 2, nullptr) == H::CONFIRM_RESEND, "second re-send allowed");
  p.resends = 2;
  CHECK(H::confirm_decision(p, before, NO_SPECIAL, 6000, 2, nullptr) == H::CONFIRM_GIVE_UP, "then it gives up");
  CHECK(H::confirm_decision(p, after, NO_SPECIAL, 6000, 2, nullptr) == H::CONFIRM_DONE, "unless it took after all");
  p.resends = 0;
  CHECK(H::confirm_decision(p, before, NO_SPECIAL, 1000 + H::CONFIRM_STALE_MS, 2, nullptr) == H::CONFIRM_GIVE_UP,
        "a command this old is not sent again");
  CHECK(H::confirm_decision(p, after, NO_SPECIAL, 1000 + H::CONFIRM_STALE_MS, 2, nullptr) == H::CONFIRM_DONE,
        "but an old one that took is simply done");

  // The remote: a value that is neither the old one nor the asked one.
  H::UnitView remote = view(true, H::MODE_DRY, 24, 0x01);
  CHECK(H::confirm_decision(p, remote, NO_SPECIAL, 6000, 2, nullptr) == H::CONFIRM_YIELD,
        "mode went somewhere else: someone else has the unit");
  remote = view(true, H::MODE_HEAT, 27, 0x01);
  CHECK(H::confirm_decision(p, remote, NO_SPECIAL, 6000, 2, nullptr) == H::CONFIRM_YIELD, "same for the setpoint");
  // Switched off from the remote: nothing can be expected of a unit that is off.
  remote = view(false, H::MODE_COOL, 24, 0x01);
  CHECK(H::confirm_decision(p, remote, NO_SPECIAL, 6000, 2, nullptr) == H::CONFIRM_DONE, "unit switched off meanwhile");

  // The unit's own behaviour is never a lost command.
  c = H::AcCommand{};
  c.mode = H::MODE_AUTO;
  c.setpoint = 19;
  p = pending(H::intent_from_command(c, H::CONFIRM_SETPOINT), view(true, H::MODE_AUTO, 22, 0x01), 0);
  CHECK(H::confirm_decision(p, view(true, H::MODE_AUTO, 22, 0x01), NO_SPECIAL, 5000, 2, nullptr) == H::CONFIRM_DONE,
        "auto keeps its own setpoint: not sent again");
  c.mode = H::MODE_HEAT;
  c.setpoint = 25;
  c.fan = H::FAN_SPEED_LOW;
  p = pending(H::intent_from_command(c, H::CONFIRM_MODE | H::CONFIRM_SETPOINT | H::CONFIRM_FAN),
              view(true, H::MODE_COOL, 24, 0x01), 0);
  H::UnitView turbo = view(true, H::MODE_COOL, 16, 0x12);
  turbo.special.turbo = true;
  CHECK(H::confirm_decision(p, turbo, NO_SPECIAL, 5000, 2, nullptr) == H::CONFIRM_DONE,
        "turbo forces cool, 16 C, high: not sent again");

  // Power.
  c = H::AcCommand{};
  c.mode = H::MODE_AUTO;
  const H::UnitView off = view(false, H::MODE_COOL, 24, 0x01);
  p = pending(H::intent_from_command(c, H::CONFIRM_MODE | H::CONFIRM_POWER), off, 0);
  CHECK(H::confirm_decision(p, off, NO_SPECIAL, 5000, 2, &unmet) == H::CONFIRM_RESEND && unmet == H::CONFIRM_POWER,
        "still off: power is what is missing");
  CHECK(H::resend_frames(p.intent, unmet) == H::RESEND_POWER_ON_PAIR, "sent again as power-on, then the mode");
  // Issue #168: the power part lands and the mode part does not, so the unit runs in its last mode.
  CHECK(H::confirm_decision(p, view(true, H::MODE_COOL, 24, 0x01), NO_SPECIAL, 5000, 2, &unmet) == H::CONFIRM_RESEND &&
            unmet == H::CONFIRM_MODE,
        "on in the old mode: the mode is sent again");
  CHECK(H::resend_frames(p.intent, unmet) == H::RESEND_COMBINED, "as the plain combined frame");
  p = pending(H::intent_power_off(), view(true, H::MODE_COOL, 24, 0x01), 0);
  CHECK(H::confirm_decision(p, view(true, H::MODE_COOL, 24, 0x01), NO_SPECIAL, 5000, 2, &unmet) == H::CONFIRM_RESEND &&
            H::resend_frames(p.intent, unmet) == H::RESEND_POWER_OFF,
        "still on after off: the power-off frame again");
  CHECK(H::confirm_decision(p, off, NO_SPECIAL, 5000, 2, nullptr) == H::CONFIRM_DONE, "off");
  H::CommandIntent on_only;
  on_only.fields = on_only.known = H::CONFIRM_POWER;
  on_only.want.power_on = true;
  CHECK(H::resend_frames(on_only, H::CONFIRM_POWER) == H::RESEND_POWER_ON_ALONE,
        "power-on alone when no mode was asked");

  // millis() wraps after 49.7 days: ages are differences.
  c = H::AcCommand{};
  p = pending(H::intent_from_command(c, H::CONFIRM_MODE), view(true, H::MODE_HEAT, 24, 0x01), 0xFFFFFF00u);
  CHECK(H::confirm_decision(p, view(true, H::MODE_HEAT, 24, 0x01), NO_SPECIAL, 0x00000100u, 2, nullptr) ==
            H::CONFIRM_WAIT,
        "512 ms across the wrap is still inside the settle time");
  CHECK(H::confirm_decision(p, view(true, H::MODE_HEAT, 24, 0x01), NO_SPECIAL, 0x00001000u, 2, nullptr) ==
            H::CONFIRM_RESEND,
        "and 4.3 s across it is judged");

  // The settle time counts from the wire, not from the queue.
  p = pending(H::intent_from_command(c, H::CONFIRM_MODE), view(true, H::MODE_HEAT, 24, 0x01), 1000);
  H::pending_on_wire(&p, 2500);
  CHECK(H::confirm_decision(p, view(true, H::MODE_HEAT, 24, 0x01), NO_SPECIAL, 1000 + H::CONFIRM_SETTLE_MS, 2,
                            nullptr) == H::CONFIRM_WAIT,
        "a frame that left the wire 1.5 s late is judged 1.5 s later");
  H::PendingCommand idle;
  H::pending_on_wire(&idle, 2500);
  CHECK(!idle.active && idle.sent_ms == 0, "nothing waiting, nothing touched");
}

// ---- A newer command takes over -----------------------------------------------------------------
static void test_supersede() {
  printf("-- a newer command takes over from the one still waiting\n");
  const H::UnitView cool24 = view(true, H::MODE_COOL, 24, 0x01);
  H::AcCommand c;
  c.mode = H::MODE_HEAT;
  H::PendingCommand p = pending(H::intent_from_command(c, H::CONFIRM_MODE), cool24, 0);
  p.resends = 2;

  // Heat (lost), then dry: only dry is ever checked or sent again.
  c.mode = H::MODE_DRY;
  H::pending_begin(&p, H::intent_from_command(c, H::CONFIRM_MODE), cool24, 1500);
  CHECK(p.intent.want.mode == H::MODE_DRY && p.resends == 0 && p.sent_ms == 1500, "newest value, fresh allowance");
  uint16_t unmet = 0;
  CHECK(H::confirm_decision(p, view(true, H::MODE_HEAT, 24, 0x01), NO_SPECIAL, 6000, 2, &unmet) == H::CONFIRM_RESEND,
        "the unit sits in the FIRST request: that is a lost second command, not the remote");
  H::AcCommand shadow;
  H::intent_to_command(p.intent, false, &shadow);
  CHECK(shadow.mode == H::MODE_DRY, "and what goes out again is the newest request");

  // A second write to another field joins the first: both are checked, with the newest shadow.
  c = H::AcCommand{};
  c.mode = H::MODE_HEAT;
  p = pending(H::intent_from_command(c, H::CONFIRM_MODE), cool24, 0);
  c.setpoint = 21;
  H::pending_begin(&p, H::intent_from_command(c, H::CONFIRM_SETPOINT), cool24, 800);
  CHECK(p.intent.fields == (H::CONFIRM_MODE | H::CONFIRM_SETPOINT), "mode, then setpoint: both wait");
  CHECK(H::confirm_decision(p, view(true, H::MODE_COOL, 24, 0x01), NO_SPECIAL, 6000, 2, &unmet) == H::CONFIRM_RESEND &&
            unmet == (H::CONFIRM_MODE | H::CONFIRM_SETPOINT),
        "both lost: both unmet");
  CHECK(H::confirm_decision(p, view(true, H::MODE_DRY, 24, 0x01), NO_SPECIAL, 6000, 2, nullptr) == H::CONFIRM_YIELD,
        "the mode was asked once, so a third value there is still the remote");

  // Off drops everything else that was asked.
  H::pending_begin(&p, H::intent_power_off(), cool24, 1200);
  CHECK(p.intent.fields == H::CONFIRM_POWER && !p.intent.want.power_on, "off replaces the lot");
  CHECK(H::confirm_decision(p, view(false, H::MODE_COOL, 24, 0x01), NO_SPECIAL, 6000, 2, nullptr) == H::CONFIRM_DONE,
        "and is all that is checked");

  // Off, then a setpoint for later (the unit stays off): power stays "off", the setpoint is moot.
  c = H::AcCommand{};
  c.setpoint = 27;
  H::pending_begin(&p, H::intent_from_command(c, H::CONFIRM_SETPOINT), cool24, 1500);
  CHECK((p.intent.fields & H::CONFIRM_POWER) != 0 && !p.intent.want.power_on, "off is still what was asked of power");
  CHECK(H::confirm_decision(p, cool24, NO_SPECIAL, 6000, 2, &unmet) == H::CONFIRM_RESEND &&
            H::resend_frames(p.intent, unmet) == H::RESEND_POWER_OFF,
        "still on: the off frame goes out again, not a power-on");

  // Off, then a mode: the mode request switches the unit on, and that is the newest word on power.
  H::pending_begin(&p, H::intent_power_off(), cool24, 0);
  c = H::AcCommand{};
  c.mode = H::MODE_AUTO;
  H::pending_begin(&p, H::intent_from_command(c, H::CONFIRM_MODE | H::CONFIRM_POWER), cool24, 700);
  CHECK(p.intent.want.power_on && p.intent.fields == (H::CONFIRM_POWER | H::CONFIRM_MODE),
        "off then auto: on, in auto");

  // On with a mode, then a setpoint: the power request is kept.
  c = H::AcCommand{};
  c.mode = H::MODE_HEAT;
  p = pending(H::intent_from_command(c, H::CONFIRM_MODE | H::CONFIRM_POWER), view(false, H::MODE_COOL, 24, 0x01), 0);
  c.setpoint = 20;
  H::pending_begin(&p, H::intent_from_command(c, H::CONFIRM_SETPOINT), view(false, H::MODE_COOL, 24, 0x01), 500);
  CHECK(p.intent.want.power_on && (p.intent.fields & H::CONFIRM_POWER) != 0, "power-on survives the second write");
  CHECK(H::confirm_decision(p, view(false, H::MODE_COOL, 24, 0x01), NO_SPECIAL, 6000, 2, &unmet) == H::CONFIRM_RESEND &&
            H::resend_frames(p.intent, unmet) == H::RESEND_POWER_ON_PAIR,
        "and a unit still off gets power-on plus the combined frame");
}

// ---- Special modes ------------------------------------------------------------------------------
static void test_special_plan() {
  printf("-- special modes: the plan to a state, and the plan again after a lost write\n");
  // The refactor: a preset plan is the plan to that row's state.
  int steps = 0;
  for (size_t from = 0; from < H::PRESET_COUNT; from++) {
    for (size_t to = 0; to < H::PRESET_COUNT; to++) {
      H::SpecialState now;
      now.eco = H::PRESETS[from].eco;
      now.turbo = H::PRESETS[from].turbo;
      now.mute = H::PRESETS[from].mute;
      now.sleep = H::PRESETS[from].sleep;
      H::SpecialState want;
      want.eco = H::PRESETS[to].eco;
      want.turbo = H::PRESETS[to].turbo;
      want.mute = H::PRESETS[to].mute;
      want.sleep = H::PRESETS[to].sleep;
      H::SpecialOp a[H::PRESET_PLAN_MAX], b[H::PRESET_PLAN_MAX];
      size_t na = H::preset_plan(now, (uint8_t) to, a);
      size_t nb = H::special_plan(now, want, b);
      bool same = na == nb && na <= H::PRESET_PLAN_MAX;
      for (size_t i = 0; same && i < na; i++)
        same = a[i].kind == b[i].kind && a[i].value == b[i].value;
      CHECK(same, "preset %zu -> %zu: same plan either way", from, to);
      // Lose any one write of the plan: planning again from what the unit then reports still ends
      // on the target, in no more writes than a plan holds.
      for (size_t lost = 0; lost < na; lost++) {
        H::SpecialState unit = now;
        for (size_t i = 0; i < lost; i++)
          H::special_apply(&unit, a[i]);
        H::SpecialState projected = unit;
        H::special_apply(&projected, a[lost]);  // the hub believes it landed
        H::SpecialState wanted = H::special_wanted(projected, a + lost + 1, na - lost - 1);
        CHECK(wanted.eco == want.eco && wanted.turbo == want.turbo && wanted.mute == want.mute &&
                  wanted.sleep == want.sleep,
              "preset %zu -> %zu: the wanted state is the target whatever was lost", from, to);
        // A lost write that only re-asserted a value (the sleep profile sent again after a byte 33
        // write) leaves this model on the target already, and then nothing is planned.
        const bool there =
            unit.eco == want.eco && unit.turbo == want.turbo && unit.mute == want.mute && unit.sleep == want.sleep;
        H::SpecialOp again[H::PRESET_PLAN_MAX];
        size_t n = H::special_plan(unit, wanted, again);
        for (size_t i = 0; i < n; i++)
          H::special_apply(&unit, again[i]);
        CHECK((n >= 1 || there) && n <= H::PRESET_PLAN_MAX && unit.eco == want.eco && unit.turbo == want.turbo &&
                  unit.mute == want.mute && unit.sleep == want.sleep,
              "preset %zu -> %zu, write %zu lost: the second plan lands", from, to, lost);
        steps++;
      }
    }
  }
  printf("  %d lost-write cases planned again\n", steps);

  // A write the user has since reversed is not planned again.
  H::SpecialState unit;  // nothing on, and the eco write was lost
  H::SpecialState projected;
  projected.eco = true;
  H::SpecialOp queued[1] = {{H::SPECIAL_OP_FEATURE, H::FEATURE_ECO_OFF}};
  H::SpecialOp out[H::PRESET_PLAN_MAX];
  CHECK(H::special_plan(unit, H::special_wanted(projected, queued, 1), out) == 0,
        "eco on (lost), then eco off: nothing left to send");
}

// ---- Scenarios ----------------------------------------------------------------------------------
enum FrameKind { F_COMBINED, F_POWER_ON, F_POWER_OFF, F_MUTE, F_SLEEP };
struct Frame {
  uint32_t t;
  FrameKind kind;
  H::AcCommand cmd;
  uint8_t value;
};

// The unit, with the behaviours that must not be mistaken for a lost command.
struct Unit {
  H::UnitView v;
  int lose = 0;                 // frames to lose from now on
  bool takes_nothing = false;   // answers and ignores every command
  bool one_frame_power = true;  // false: a unit that ignores a combined frame while off, power bits or not
  uint32_t last_special = 0;
  bool special_seen = false;
  std::vector<Frame> heard;

  // The unit swallows a special-mode command within about 8 s of the previous one.
  bool debounced(uint32_t now) {
    bool swallow = special_seen && now - last_special < 8000;
    if (!swallow) {
      special_seen = true;
      last_special = now;
    }
    return swallow;
  }

  void receive(const Frame &f) {
    heard.push_back(f);
    if (lose > 0) {
      lose--;
      return;
    }
    if (takes_nothing)
      return;
    switch (f.kind) {
      case F_POWER_ON:
        v.power_on = true;
        return;
      case F_POWER_OFF:
        v.power_on = false;
        return;
      case F_MUTE:
        if (!v.power_on || debounced(f.t))
          return;
        v.special.mute = f.value != 0;
        if (v.special.mute)
          v.special.sleep = 0;
        return;
      case F_SLEEP:
        if (!v.power_on || debounced(f.t))
          return;
        v.special.sleep = f.value;
        if (f.value != 0)
          v.special.mute = false;
        return;
      case F_COMBINED:
        break;
    }
    if (!v.power_on) {
      if (!f.cmd.power_on || !one_frame_power)
        return;
      v.power_on = true;
    }
    // Byte 33: a change of eco or turbo is a special-mode command.
    H::SpecialState s = v.special;
    H::special_apply(&s, {H::SPECIAL_OP_FEATURE, (uint8_t) f.cmd.feature});
    if ((s.eco != v.special.eco || s.turbo != v.special.turbo) && !debounced(f.t)) {
      v.special.eco = s.eco;
      v.special.turbo = s.turbo;
    }
    v.mode = f.cmd.mode;
    if (v.mode == H::MODE_COOL || v.mode == H::MODE_HEAT)
      v.setpoint_c = f.cmd.setpoint;  // auto, dry and fan-only keep the unit's own
    if (v.mode != H::MODE_DRY) {
      for (const auto &row : H::FAN_TABLE)
        if (row.cmd == f.cmd.fan)
          v.fan_raw = row.raw;
      if (f.cmd.fan == H::FAN_SPEED_AUTO)
        v.fan_raw = H::FAN_RAW_AUTO;
    }
    v.vswing = f.cmd.vswing == H::SWING_MODE_SWING;
  }

  // What the status frame reports: turbo, quiet and sleep show through.
  H::UnitView report() const {
    H::UnitView r = v;
    if (r.special.turbo) {
      r.mode = H::MODE_COOL;
      r.setpoint_c = 16;
      r.fan_raw = 0x12;
    } else if (r.special.mute) {
      r.fan_raw = 0x02;
    } else if (r.special.sleep != 0) {
      r.fan_raw = 0x0A;
    }
    return r;
  }
};

// HisenseAC's command paths, line for line where it matters. Frames reach the unit at once: the
// scheduler's part (the wait for the command slot, the re-send of an unanswered frame) is covered
// by test_esphome_bus.cpp.
struct Hub {
  static constexpr uint32_t HOLDOFF_MS = 4000;
  Unit *unit;
  uint32_t now = 100000;
  H::AcCommand cmd;
  H::UnitView last;
  H::PendingCommand main_pending, special_pending;
  H::SpecialOp queue[8];
  size_t queue_len = 0;
  uint32_t last_special_ms = 0;
  bool special_sent = false;
  uint8_t special_resends = 0;
  H::SpecialState projected;
  uint32_t holdoff_start = 0;
  bool holdoff_armed = false;
  bool link_up = true;
  int retries = 0, failed = 0;
  H::UnitView shown;  // what the entities show

  explicit Hub(Unit *u) : unit(u) {
    last = shown = u->report();
    sync_shadow();
  }
  void note() {
    holdoff_start = now;
    holdoff_armed = true;
  }
  bool holdoff() {
    if (holdoff_armed && now - holdoff_start >= HOLDOFF_MS)
      holdoff_armed = false;
    return holdoff_armed;
  }
  bool busy() const { return queue_len > 0 || special_pending.active; }
  void tx(FrameKind kind, uint8_t value = 0) {
    if (link_up)
      unit->receive({now, kind, cmd, value});
  }
  void sync_shadow() {
    H::FanSpeed fan = H::shadow_fan_from_status(last.fan_raw);
    if (fan != H::FAN_SPEED_NOCHANGE)
      cmd.fan = fan;
    if (last.mode_valid)
      cmd.mode = last.mode;
    cmd.setpoint = last.setpoint_c;
    cmd.vswing = last.vswing ? H::SWING_MODE_SWING : H::SWING_MODE_OFF;
    cmd.feature = H::feature_from_status(last.special.eco, last.special.turbo);
  }

  // climate control() + send_user_command()
  void user(uint16_t fields, bool power_on) {
    cmd.power_on = power_on;
    tx(F_COMBINED);
    cmd.power_on = false;
    if (power_on)
      fields |= H::CONFIRM_POWER;
    H::pending_begin(&main_pending, H::intent_from_command(cmd, fields), last, now);
    note();
  }
  void user_off() {
    tx(F_POWER_OFF);
    H::pending_begin(&main_pending, H::intent_power_off(), last, now);
    note();
  }
  void push(const H::SpecialOp &op) {
    queue[queue_len++] = op;
    note();
  }
  void user_special(const H::SpecialOp &op) {
    special_resends = 0;
    push(op);
  }
  void user_preset(uint8_t target) {
    H::SpecialOp ops[H::PRESET_PLAN_MAX];
    size_t n = H::preset_plan(projected, target, ops);
    queue_len = 0;
    special_resends = 0;
    for (size_t i = 0; i < n; i++)
      push(ops[i]);
    note();
  }
  void drain() {
    if (queue_len == 0 || special_pending.active)
      return;
    if (special_sent && now - last_special_ms < H::SPECIAL_SETTLE_MS)
      return;
    H::SpecialOp op = queue[0];
    for (size_t i = 1; i < queue_len; i++)
      queue[i - 1] = queue[i];
    queue_len--;
    switch (op.kind) {
      case H::SPECIAL_OP_FEATURE:
        cmd.feature = (H::Feature) op.value;
        tx(F_COMBINED);
        cmd.feature = H::feature_after_send(cmd.feature);
        break;
      case H::SPECIAL_OP_MUTE:
        tx(F_MUTE, op.value);
        break;
      default:
        tx(F_SLEEP, op.value);
        break;
    }
    H::special_apply(&projected, op);
    last_special_ms = now;
    special_sent = true;
    note();
    H::pending_begin(&special_pending, H::intent_from_special(op), last, now);
    special_pending.resends = special_resends;
  }
  void confirm(const H::UnitView &v) {
    const H::SpecialState wanted = H::special_wanted(projected, queue, queue_len);
    uint16_t unmet = 0;
    if (main_pending.active) {
      H::PendingCommand &p = main_pending;
      switch (H::confirm_decision(p, v, wanted, now, H::COMMAND_RESEND_MAX, &unmet)) {
        case H::CONFIRM_WAIT:
          break;
        case H::CONFIRM_GIVE_UP:
          failed++;
          p.active = false;
          break;
        case H::CONFIRM_DONE:
        case H::CONFIRM_YIELD:
          p.active = false;
          break;
        case H::CONFIRM_RESEND:
          p.resends++;
          retries++;
          H::intent_to_command(p.intent, false, &cmd);
          switch (H::resend_frames(p.intent, unmet)) {
            case H::RESEND_POWER_OFF:
              tx(F_POWER_OFF);
              break;
            case H::RESEND_POWER_ON_ALONE:
              tx(F_POWER_ON);
              break;
            case H::RESEND_POWER_ON_PAIR:
              tx(F_POWER_ON);
              tx(F_COMBINED);
              break;
            case H::RESEND_COMBINED:
              tx(F_COMBINED);
              break;
          }
          p.sent_ms = now;
          note();
          break;
      }
    }
    if (special_pending.active) {
      H::PendingCommand &p = special_pending;
      switch (H::confirm_decision(p, v, wanted, now, H::SPECIAL_RESEND_MAX, &unmet)) {
        case H::CONFIRM_WAIT:
          break;
        case H::CONFIRM_GIVE_UP:
          failed++;
          p.active = false;
          break;
        case H::CONFIRM_DONE:
        case H::CONFIRM_YIELD:
          p.active = false;
          break;
        case H::CONFIRM_RESEND: {
          p.active = false;
          special_resends++;
          retries++;
          H::SpecialOp ops[H::PRESET_PLAN_MAX];
          size_t n = H::special_plan(v.special, wanted, ops);
          projected = v.special;
          queue_len = 0;
          for (size_t i = 0; i < n; i++)
            push(ops[i]);
          note();
          break;
        }
      }
    }
  }
  // process_status_()
  void status() {
    if (!link_up)
      return;
    last = unit->report();
    confirm(last);
    if (!holdoff()) {
      sync_shadow();
      if (!busy())
        projected = last.special;
      shown = last;
    }
  }
  void link_lost() {
    link_up = false;
    failed += (main_pending.active ? 1 : 0) + (special_pending.active ? 1 : 0) + (queue_len > 0 ? 1 : 0);
    main_pending.active = special_pending.active = false;
    queue_len = 0;
  }
  // One bus cycle per second: the special queue gets its turn every 100 ms, a status frame at the end.
  void run(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 100) {
      now += 100;
      drain();
      if (now % 1000 == 0)
        status();
    }
  }
};

static size_t count(const Unit &u, FrameKind k, size_t from = 0) {
  size_t n = 0;
  for (size_t i = from; i < u.heard.size(); i++)
    n += u.heard[i].kind == k;
  return n;
}

static Unit running(H::Mode mode = H::MODE_COOL) {
  Unit u;
  u.v = view(true, mode, 24, 0x01);
  return u;
}

static void test_scenarios() {
  printf("-- scenarios: the hub's decisions against a unit that loses and overrides commands\n");
  {  // The plain case costs nothing: one frame, no re-send.
    Unit u = running();
    Hub h(&u);
    h.cmd.setpoint = 26;
    h.user(H::CONFIRM_SETPOINT, false);
    h.run(20000);
    CHECK(u.heard.size() == 1 && h.retries == 0 && h.failed == 0 && u.v.setpoint_c == 26, "taken: one frame");
  }
  {  // The 1-in-52 case: the unit answers and does not apply.
    Unit u = running();
    Hub h(&u);
    u.lose = 1;
    h.cmd.setpoint = 26;
    h.user(H::CONFIRM_SETPOINT, false);
    h.run(3000);
    CHECK(h.shown.setpoint_c == 24 && h.holdoff(), "entity still holds the request");
    h.run(17000);
    CHECK(u.heard.size() == 2 && h.retries == 1 && h.failed == 0, "lost once: sent again once (%zu frames)",
          u.heard.size());
    CHECK(u.v.setpoint_c == 26 && h.shown.setpoint_c == 26 && !h.main_pending.active, "and it lands");
    CHECK(u.heard[1].t - u.heard[0].t >= H::CONFIRM_SETTLE_MS && u.heard[1].t - u.heard[0].t <= 5000,
          "re-send follows the first by %u ms", (unsigned) (u.heard[1].t - u.heard[0].t));
  }
  {  // Never taken: bounded, loud, and the entity shows the truth.
    Unit u = running();
    Hub h(&u);
    u.takes_nothing = true;
    h.cmd.mode = H::MODE_HEAT;
    h.user(H::CONFIRM_MODE, false);
    h.run(60000);
    CHECK(u.heard.size() == 3 && h.retries == 2 && h.failed == 1, "three frames, then it gives up (%zu, %d, %d)",
          u.heard.size(), h.retries, h.failed);
    CHECK(h.shown.mode == H::MODE_COOL && h.cmd.mode == H::MODE_COOL, "entity and shadow are back on the real mode");
    CHECK(u.heard.back().t - u.heard.front().t <= 10000, "all within %u ms",
          (unsigned) (u.heard.back().t - u.heard.front().t));
  }
  {  // A newer command supersedes the pending retry.
    Unit u = running();
    Hub h(&u);
    u.lose = 1;
    h.cmd.mode = H::MODE_HEAT;
    h.user(H::CONFIRM_MODE, false);
    h.run(2000);
    h.cmd.mode = H::MODE_DRY;
    h.user(H::CONFIRM_MODE, false);
    h.run(30000);
    bool heat_after = false;
    for (size_t i = 1; i < u.heard.size(); i++)
      heat_after |= u.heard[i].cmd.mode == H::MODE_HEAT;
    CHECK(!heat_after && u.v.mode == H::MODE_DRY && h.retries == 0, "heat (lost) then dry: heat is never sent again");
  }
  {  // Rapid toggling inside the hold-off: the last word wins and is the only thing re-sent.
    Unit u = running();
    Hub h(&u);
    h.user_off();
    h.run(500);
    h.cmd.mode = H::MODE_AUTO;
    u.lose = 1;
    h.user(H::CONFIRM_MODE, true);
    h.run(30000);
    CHECK(u.v.power_on && u.v.mode == H::MODE_AUTO && h.failed == 0, "off, then auto (lost): on, in auto");
    CHECK(count(u, F_POWER_OFF) == 1, "the off is not sent again against the newer request");
  }
  {  // Power and mode in one frame.
    Unit u;
    u.v = view(false, H::MODE_COOL, 24, 0x01);
    Hub h(&u);
    h.cmd.mode = H::MODE_HEAT;
    h.user(H::CONFIRM_MODE, true);
    h.run(20000);
    CHECK(u.heard.size() == 1 && u.heard[0].kind == F_COMBINED && u.heard[0].cmd.power_on, "one frame carries both");
    CHECK(u.v.power_on && u.v.mode == H::MODE_HEAT && h.retries == 0, "and the unit is on, in heat");
  }
  {  // A unit that does not take the one-frame form gets the proven pair on the re-send.
    Unit u;
    u.v = view(false, H::MODE_COOL, 24, 0x01);
    u.one_frame_power = false;
    Hub h(&u);
    h.cmd.mode = H::MODE_AUTO;
    h.user(H::CONFIRM_MODE, true);
    h.run(20000);
    CHECK(u.heard.size() == 3 && u.heard[1].kind == F_POWER_ON && u.heard[2].kind == F_COMBINED &&
              !u.heard[2].cmd.power_on,
          "fallback: literal power-on, then the plain combined frame (%zu frames)", u.heard.size());
    CHECK(u.v.power_on && u.v.mode == H::MODE_AUTO && h.retries == 1 && h.failed == 0, "on, in auto, one re-send");
  }
  {  // Power off that the unit misses.
    Unit u = running();
    Hub h(&u);
    u.lose = 1;
    h.user_off();
    h.run(20000);
    CHECK(count(u, F_POWER_OFF) == 2 && !u.v.power_on && h.retries == 1, "off lost once: sent again, unit off");
  }
  {  // Auto pins its own setpoint: no re-send, no failure.
    Unit u = running(H::MODE_AUTO);
    Hub h(&u);
    h.cmd.setpoint = 19;
    h.user(H::CONFIRM_SETPOINT, false);
    h.run(20000);
    CHECK(u.heard.size() == 1 && h.retries == 0 && h.failed == 0, "auto keeps 24: not a lost command");
    CHECK(h.shown.setpoint_c == 24, "and the entity shows the unit's setpoint");
  }
  {  // Someone uses the remote while a command waits.
    Unit u = running();
    Hub h(&u);
    u.lose = 1;
    h.cmd.mode = H::MODE_HEAT;
    h.user(H::CONFIRM_MODE, false);
    h.run(1500);
    u.v.mode = H::MODE_FAN;  // the remote
    h.run(30000);
    CHECK(u.heard.size() == 1 && h.retries == 0 && u.v.mode == H::MODE_FAN, "the remote's choice is left alone");
  }
  {  // Dead bus: nothing is re-sent, and nothing is sent when it comes back.
    Unit u = running();
    Hub h(&u);
    u.lose = 1;
    h.cmd.mode = H::MODE_HEAT;
    h.user(H::CONFIRM_MODE, false);
    h.user_special({H::SPECIAL_OP_MUTE, 1});
    h.run(1000);
    size_t before = u.heard.size();
    h.link_lost();
    h.run(30000);
    h.link_up = true;
    h.run(30000);
    CHECK(u.heard.size() == before && h.retries == 0, "no frame after the link dropped (%zu)", u.heard.size() - before);
    CHECK(h.failed >= 1 && !h.main_pending.active && !h.special_pending.active && h.queue_len == 0,
          "the open commands are counted as failed and forgotten");
  }
  {  // The observed failure: "preset none, got quiet". The mute-off write is swallowed.
    Unit u = running();
    u.v.special.mute = true;
    Hub h(&u);
    h.projected = u.v.special;
    u.lose = 1;
    h.user_preset(H::PRESET_NONE);
    h.run(8000);
    CHECK(u.v.special.mute && h.busy(), "lost, and the preset entity is still held");
    h.run(30000);
    CHECK(!u.v.special.mute && count(u, F_MUTE) == 2 && h.retries == 1 && h.failed == 0, "sent again: quiet is off");
    CHECK(u.heard[1].t - u.heard[0].t >= H::SPECIAL_SETTLE_MS, "the re-send respects the debounce (%u ms apart)",
          (unsigned) (u.heard[1].t - u.heard[0].t));
    CHECK(!h.busy() && !h.shown.special.mute, "and the entity reads none");
  }
  {  // A multi-write preset with its first write lost: the plan is made again and lands.
    Unit u = running();
    u.v.special.mute = true;
    u.v.special.eco = true;
    Hub h(&u);
    h.projected = u.v.special;
    u.lose = 1;
    h.user_preset(H::PRESET_FIRST_SLEEP + 2);  // eco_quiet -> sleep_old: eco off, mute off, sleep 2
    h.run(90000);
    CHECK(!u.v.special.eco && !u.v.special.mute && u.v.special.sleep == 2 && h.failed == 0,
          "eco_quiet to sleep_old with a lost write: lands (eco=%d mute=%d sleep=%u)", u.v.special.eco,
          u.v.special.mute, u.v.special.sleep);
    for (size_t i = 1; i < u.heard.size(); i++)
      CHECK(u.heard[i].t - u.heard[i - 1].t >= H::SPECIAL_SETTLE_MS, "special write %zu is %u ms after the last", i,
            (unsigned) (u.heard[i].t - u.heard[i - 1].t));
  }
  {  // A special write the unit never takes: one re-send, then it gives up.
    Unit u = running();
    Hub h(&u);
    u.takes_nothing = true;
    h.user_special({H::SPECIAL_OP_FEATURE, H::FEATURE_ECO});
    h.run(120000);
    CHECK(u.heard.size() == 2 && h.retries == 1 && h.failed == 1, "eco never taken: two frames, one failure (%zu)",
          u.heard.size());
    CHECK(!h.busy() && !h.shown.special.eco && !h.projected.eco, "entity and projection are back on the truth");
  }
  {  // The user changes their mind while the first special write is unconfirmed.
    Unit u = running();
    Hub h(&u);
    u.lose = 1;
    h.user_preset(H::PRESET_QUIET);  // mute on, lost
    h.run(2000);
    h.user_preset(H::PRESET_ECO);  // planned from "quiet is on": eco on, mute off
    h.run(90000);
    CHECK(u.v.special.eco && !u.v.special.mute && h.failed == 0, "quiet (lost) then eco: ends on eco");
    bool mute_on_again = false;
    for (size_t i = 1; i < u.heard.size(); i++)
      mute_on_again |= u.heard[i].kind == F_MUTE && u.heard[i].value == 1;
    CHECK(!mute_on_again, "the superseded quiet is never sent again");
  }
  {  // Turbo forces cool, 16 C, high. A setpoint written just before it engages is not chased.
    Unit u = running(H::MODE_HEAT);
    Hub h(&u);
    h.cmd.setpoint = 28;
    h.user(H::CONFIRM_SETPOINT, false);
    h.user_special({H::SPECIAL_OP_FEATURE, H::FEATURE_TURBO});
    h.run(40000);
    CHECK(u.v.special.turbo && h.retries == 0 && h.failed == 0, "turbo on: nothing re-sent, nothing failed");
    size_t frames = u.heard.size();
    h.user_special({H::SPECIAL_OP_FEATURE, H::FEATURE_NONE});
    h.run(40000);
    CHECK(!u.v.special.turbo && h.retries == 0 && h.failed == 0 && u.heard.size() == frames + 1,
          "turbo off: one frame, the unit staying in cool is not a fault");
  }
  {  // Quiet pins the fan: a mode change under quiet does not chase the fan.
    Unit u = running();
    u.v.special.mute = true;
    Hub h(&u);
    h.projected = u.v.special;
    h.cmd.mode = H::MODE_HEAT;
    h.cmd.fan = H::FAN_SPEED_HIGH;
    h.user(H::CONFIRM_MODE | H::CONFIRM_FAN, false);
    h.run(20000);
    CHECK(u.heard.size() == 1 && h.retries == 0 && h.failed == 0, "fan under quiet reads 0x02: not a lost command");
  }
}

int main() {
  printf("== ESPHome confirm and retry, power and mode in one frame ==\n");
  test_resend_rule();
  test_reply_verdict();
  test_one_frame_power();
  test_intent();
  test_expected_fields();
  test_decision();
  test_supersede();
  test_special_plan();
  test_scenarios();
  printf("  %d checks, %d failed\n", g_checks, g_fail);
  printf(g_fail ? "== CONFIRM AND RETRY FAILED ==\n" : "== CONFIRM AND RETRY OK ==\n");
  return g_fail ? 1 : 0;
}
