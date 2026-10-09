// Edge cases of the ESPHome component's pure layers (hisense_map.h, hisense_protocol.*): the rules
// that decide what a status frame may put into the command shadow, what a write may assume about
// the unit, and how the receive side copes with a damaged byte stream.
//
// These rules exist only in the port, so there is nothing in the shared driver to compare them
// with (that is test_esphome_codec_parity.cpp). Each block names the sequence it guards against.

#include <cmath>
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

// A checksum-valid 160-byte status frame with the given byte 18, fan byte and setpoint.
static std::vector<uint8_t> status_frame(uint8_t byte18, uint8_t fan_raw, uint8_t setpoint) {
  std::vector<uint8_t> f(160, 0);
  f[0] = 0xF4;
  f[1] = 0xF5;
  f[2] = 0x01;
  f[3] = 0x40;
  f[4] = 151;
  f[13] = 0x66;
  f[16] = fan_raw;
  f[18] = byte18;
  f[19] = setpoint;
  f[20] = 23;
  uint16_t sum = H::checksum_range(f.data(), 2, 156);
  f[156] = (uint8_t) (sum >> 8);
  f[157] = (uint8_t) sum;
  f[158] = 0xF4;
  f[159] = 0xFB;
  return f;
}

static bool known_mode_byte(uint8_t b) { return b == 0x10 || b == 0x30 || b == 0x50 || b == 0x70 || b == 0x90; }

// Sequence: the A/C reports a byte 18 whose upper nibble is not 0..6 (bit 7 set, or the value 7).
// Copied raw into the shadow, the next combined frame re-sent it as (mode * 2 + 1) << 4, and the
// entity showed cool.
static void test_status_mode() {
  printf("-- status mode into the shadow\n");
  H::Mode out = H::MODE_DRY;
  for (int m = H::MODE_FAN; m <= H::MODE_AUTO; m++)
    CHECK(H::mode_from_status((H::Mode) m, &out) && out == (H::Mode) m, "mode %d passes through", m);

  out = H::MODE_DRY;
  CHECK(!H::mode_from_status((H::Mode) 7, &out) && out == H::MODE_DRY, "7 is no mode, the output is untouched");
  CHECK(!H::mode_from_status((H::Mode) 15, &out) && out == H::MODE_DRY, "15 is 7 with bit 7 set: still no mode");
  for (int m = H::MODE_FAN; m <= H::MODE_AUTO; m++)
    CHECK(H::mode_from_status((H::Mode)(m | 8), &out) && out == (H::Mode) m, "bit 7 dropped from %d", m | 8);
  CHECK(H::mode_from_status((H::Mode) 13, &out) && out == H::MODE_AUTO, "auto as 5 with bit 7 set");
  CHECK(H::mode_from_status((H::Mode) 14, &out) && out == H::MODE_AUTO, "auto as 6 with bit 7 set");

  // The hazard itself, on the raw path the hub used to take: status nibble 13 lands in the frame
  // as 0xB0, which is no mode command.
  {
    H::AcCommand c;
    c.mode = (H::Mode) 13;
    uint8_t f[H::CMD_FRAME_MAX];
    size_t n = H::build_command(c, f, sizeof(f));
    CHECK(n > 0 && !known_mode_byte(f[18]), "raw copy of nibble 13 builds byte 18 = 0x%02X", f[18]);
  }

  // End to end for every byte 18 nibble: parse, validate, sync, build. Whatever the status said,
  // the frame carries one of the five mode commands, and an unusable value leaves the shadow alone.
  for (int nibble = 0; nibble < 16; nibble++) {
    auto frame = status_frame((uint8_t) ((nibble << 4) | 0x04), 0x0A, 24);
    H::AcState st;
    CHECK(H::parse_status(frame.data(), frame.size(), &st), "nibble %d parses", nibble);
    H::AcCommand shadow;
    shadow.mode = H::MODE_HEAT;
    bool took = H::mode_from_status(st.mode, &shadow.mode);
    uint8_t f[H::CMD_FRAME_MAX];
    size_t n = H::build_command(shadow, f, sizeof(f));
    CHECK(n > 0 && known_mode_byte(f[18]), "nibble %d: frame byte 18 = 0x%02X", nibble, f[18]);
    if ((nibble & 7) == 7)
      CHECK(!took && shadow.mode == H::MODE_HEAT, "nibble %d keeps the shadow's mode", nibble);
  }
}

// Sequence: quiet is on, so status byte 16 reads 0x02. The shadow took FAN_SPEED_QUIET from it,
// and the next combined frame (a setpoint change, say) sent fan byte 0x03, which the unit runs as
// high.
static void test_shadow_fan() {
  printf("-- status fan into the shadow\n");
  CHECK(H::fan_raw_to_cmd(0x02) == H::FAN_SPEED_QUIET, "the shared mapping still names the quiet step");
  CHECK(H::shadow_fan_from_status(0x02) == H::FAN_SPEED_NOCHANGE, "the shadow does not take it");
  CHECK(H::shadow_fan_from_status(0x01) == H::FAN_SPEED_AUTO, "auto is taken");
  for (const auto &row : H::FAN_TABLE) {
    if (row.cmd != H::FAN_SPEED_QUIET)
      CHECK(H::shadow_fan_from_status(row.raw) == row.cmd, "raw 0x%02X is taken", row.raw);
  }
  // No reported fan byte, known or not, can put 0x03 on the wire through the shadow.
  for (int raw = 0; raw < 256; raw++) {
    H::AcCommand shadow;
    shadow.fan = H::FAN_SPEED_MID;  // what the user last chose
    H::FanSpeed fan = H::shadow_fan_from_status((uint8_t) raw);
    if (fan != H::FAN_SPEED_NOCHANGE)
      shadow.fan = fan;
    uint8_t f[H::CMD_FRAME_MAX];
    size_t n = H::build_command(shadow, f, sizeof(f));
    CHECK(n > 0 && f[16] != 0x03, "status fan 0x%02X builds fan byte 0x%02X", raw, f[16]);
    if (raw == 0x02)
      CHECK(f[16] == 0x0F, "quiet keeps the user's last speed (0x%02X)", f[16]);
  }
}

// Sequence: unit running, user switches it off, then asks for a mode before the status frame has
// caught up (inside the hold-off). The power-on decision read the stale status, sent the mode
// alone, and the unit, off by then, ignored it.
static void test_power_intent() {
  printf("-- power intent across the hold-off\n");
  H::PowerIntent p;
  CHECK(H::power_expected(p, true) && !H::power_expected(p, false), "nothing sent: the status decides");

  H::power_intent_sent(&p, false);  // off, while the status still says on
  CHECK(!H::power_expected(p, true), "after off, a mode change needs the power-on frame");
  H::power_intent_status(&p, true);  // a status frame inside the hold-off may predate the command
  CHECK(!H::power_expected(p, true), "a frame inside the hold-off does not override the command");
  H::power_intent_sent(&p, true);  // the mode change powers it back on
  CHECK(H::power_expected(p, false), "after on, the next write does not need a second power-on");
  H::power_intent_status(&p, false);  // first frame after the hold-off
  CHECK(!H::power_expected(p, false), "after the hold-off the status is the truth: a lost power-on shows as off");
  CHECK(H::power_expected(p, true), "and an obeyed one as on");

  // The setpoint rule sees the same value: off was just sent in auto, so the unit is not running
  // and the value goes into the shadow instead of being refused.
  H::PowerIntent q;
  H::power_intent_sent(&q, false);
  CHECK(H::setpoint_request_allowed(H::power_expected(q, true), H::MODE_AUTO), "setpoint kept right after off");
}

// Sequence: a write arrives before the first status frame (boot, or an A/C that has not answered
// yet). The combined frame was built from the default shadow.
static void test_shadow_readiness() {
  printf("-- no combined frame from a default shadow\n");
  CHECK(!H::combined_frame_allowed(false) && H::combined_frame_allowed(true), "gated on the first status");
  // Why: the default shadow is a complete instruction, not a neutral one.
  H::AcCommand c;
  uint8_t f[H::CMD_FRAME_MAX];
  size_t n = H::build_command(c, f, sizeof(f));
  CHECK(n > 0 && f[18] == 0x50 && f[19] == 24 * 2 + 1 && f[16] == 0x01 && f[32] == 0x40,
        "default shadow commands cool, 24, auto fan, louvre held (18=%02X 19=%02X 16=%02X 32=%02X)", f[18], f[19],
        f[16], f[32]);
}

// Boundaries of the values a write or a status frame can carry.
static void test_boundaries() {
  printf("-- boundaries\n");
  H::AcCommand c;
  CHECK(H::setpoint_to_cmd(15, false, &c) == 16 && c.setpoint == 16, "below the range clamps to 16");
  CHECK(H::setpoint_to_cmd(33, false, &c) == 32 && c.setpoint == 32, "above the range clamps to 32");
  CHECK(H::setpoint_to_cmd(-300, false, &c) == 16 && H::setpoint_to_cmd(30000, false, &c) == 32,
        "far outside still clamps, no int8 wrap");
  // A Fahrenheit panel: every Celsius step survives the trip to the wire and back.
  for (int t = H::SETPOINT_MIN_C; t <= H::SETPOINT_MAX_C; t++) {
    CHECK(H::setpoint_to_cmd(t, true, &c) == t && c.fahrenheit, "F panel accepts %d C", t);
    CHECK(H::setpoint_in_range(c.setpoint, true), "%d C is %d F, inside the F range", t, c.setpoint);
    CHECK(H::f_to_c(c.setpoint) == t, "%d C -> %d F -> %d C", t, c.setpoint, H::f_to_c(c.setpoint));
  }
  // A reported setpoint outside the command range (frost guard reports 5 C) never reaches the shadow.
  c = H::AcCommand{};
  c.setpoint = 22;
  for (int t : {-128, -1, 0, 5, 15, 33, 127})
    CHECK(!H::sync_shadow_setpoint((int8_t) t, false, &c) && c.setpoint == 22, "status setpoint %d C not taken", t);
  CHECK(H::sync_shadow_setpoint(16, false, &c) && c.setpoint == 16, "16 C taken");
  CHECK(H::sync_shadow_setpoint(32, false, &c) && c.setpoint == 32, "32 C taken");

  // Fan ladder ends and beyond.
  CHECK(H::fan_index_to_hisense(0) == H::FAN_SPEED_AUTO && H::fan_index_to_hisense(6) == H::FAN_SPEED_HIGH,
        "ladder ends");
  CHECK(H::fan_index_to_hisense(7) == H::FAN_SPEED_AUTO && H::fan_index_to_hisense(255) == H::FAN_SPEED_AUTO,
        "past the top falls back to auto");
  CHECK(H::fan_raw_to_index(0x00) == 0 && H::fan_raw_to_index(0x14) == 0 && H::fan_raw_to_index(0xFF) == 0,
        "unknown fan bytes read as auto for display");

  // Sleep byte values the table does not know: no out-of-range preset index, and a plan to `none`
  // still switches sleep off.
  for (int raw = 0; raw < 256; raw++) {
    H::SpecialState s = H::special_from_status(false, false, false, (uint8_t) raw);
    uint8_t idx = H::preset_detect(s, 0x0F);
    CHECK(idx < H::PRESET_COUNT, "sleep_raw %d detects row %u", raw, idx);
    if (s.sleep > 4)
      CHECK(idx == H::PRESET_NONE, "unknown sleep profile %u reads as none", s.sleep);
    H::SpecialOp ops[H::PRESET_PLAN_MAX];
    size_t n = H::preset_plan(s, H::PRESET_NONE, ops);
    CHECK(n <= H::PRESET_PLAN_MAX, "plan fits");
    if (s.sleep != 0)
      CHECK(n == 1 && ops[0].kind == H::SPECIAL_OP_SLEEP && ops[0].value == 0, "sleep_raw %d: none clears it", raw);
  }
  // Every preset to every preset: the plan fits and replaying it reaches the target exactly.
  for (size_t from = 0; from < H::PRESET_COUNT; from++) {
    for (size_t to = 0; to < H::PRESET_COUNT; to++) {
      const H::PresetRow &r = H::PRESETS[from];
      H::SpecialState s;
      s.eco = r.eco;
      s.turbo = r.turbo;
      s.mute = r.mute;
      s.sleep = r.sleep;
      H::SpecialOp ops[H::PRESET_PLAN_MAX];
      size_t n = H::preset_plan(s, (uint8_t) to, ops);
      CHECK(n <= H::PRESET_PLAN_MAX, "%s -> %s: %zu ops", r.name, H::PRESETS[to].name, n);
      for (size_t i = 0; i < n && i < H::PRESET_PLAN_MAX; i++)
        H::special_apply(&s, ops[i]);
      CHECK(H::preset_detect(s, 0x0F) == to, "%s -> %s lands on %s", r.name, H::PRESETS[to].name,
            H::PRESETS[H::preset_detect(s, 0x0F)].name);
    }
  }

  // Power estimate at the ends of the raw byte.
  CHECK(H::active_power_mw(0) == 0, "raw 0 is 0 W");
  CHECK(H::active_power_mw(255) == H::POWER_MW_MAX, "raw 255 is capped");
  CHECK(H::active_current_ma(255, 0) == 0, "no voltage, no current (no divide by zero)");
  CHECK(H::active_current_ma(0, 255) == 0, "no power, no current");

  // Publish-on-change with NaN on either side.
  CHECK(H::telemetry_publish_due(true, NAN, 1.0f, false), "NaN -> value publishes");
  CHECK(H::telemetry_publish_due(true, 1.0f, NAN, false), "value -> NaN publishes");
  CHECK(!H::telemetry_publish_due(true, 1.0f, 1.0f, false), "unchanged does not");
  CHECK(!H::telemetry_publish_due(true, 0.0f, -0.0f, false), "0 and -0 are the same reading");
}

static size_t feed_all(H::FrameAssembler &fa, const std::vector<uint8_t> &bytes, int *frames) {
  size_t last = 0;
  for (uint8_t b : bytes) {
    size_t n = fa.feed(b);
    if (n > 0) {
      last = n;
      (*frames)++;
    }
  }
  return last;
}

// The receive side against a damaged byte stream.
static void test_frame_stream() {
  printf("-- frame assembly on a damaged stream\n");
  const auto good = status_frame(0x24, 0x0A, 24);

  {  // Two frames back to back are two frames.
    H::FrameAssembler fa;
    std::vector<uint8_t> s(good);
    s.insert(s.end(), good.begin(), good.end());
    int frames = 0;
    CHECK(feed_all(fa, s, &frames) == 160 && frames == 2, "concatenated frames: %d", frames);
  }
  {  // Garbage, including lone start bytes and a stray end tag, ahead of a frame.
    H::FrameAssembler fa;
    std::vector<uint8_t> s = {0x00, 0xF4, 0x12, 0xF4, 0xFB, 0xF5, 0xFF, 0x33};
    s.insert(s.end(), good.begin(), good.end());
    int frames = 0;
    size_t n = feed_all(fa, s, &frames);
    CHECK(n == 160 && frames == 1 && H::status_checksum_ok(fa.data(), n), "frame found after garbage (%d)", frames);
  }
  {  // The node's own command, echoed by a transceiver that listens while it sends, then the reply.
    H::FrameAssembler fa;
    uint8_t cmd[H::CMD_FRAME_MAX];
    size_t n = H::build_command(H::AcCommand{}, cmd, sizeof(cmd));
    std::vector<uint8_t> s(cmd, cmd + n);
    s.insert(s.end(), good.begin(), good.end());
    int frames = 0;
    CHECK(feed_all(fa, s, &frames) == 160 && frames == 1, "own echo dropped, the reply behind it kept (%d)", frames);
  }
  {  // A well-framed frame with one flipped body bit: assembled, then refused by the checksum.
    H::FrameAssembler fa;
    std::vector<uint8_t> s(good);
    s[40] ^= 0x10;
    int frames = 0;
    size_t n = feed_all(fa, s, &frames);
    H::AcState st;
    CHECK(n == 160 && !H::status_checksum_ok(fa.data(), n) && !H::parse_status(fa.data(), n, &st),
          "bad checksum is framed but not parsed");
  }
  {
    // A frame cut short takes the frame after it down too: the assembler is still counting bytes
    // for the first. The scheduler resets the assembler at every transaction, which bounds the
    // loss to that one reply window (test_esphome_bus.cpp covers the recovery).
    H::FrameAssembler fa;
    std::vector<uint8_t> s(good.begin(), good.begin() + 60);
    s.insert(s.end(), good.begin(), good.end());
    int frames = 0;
    feed_all(fa, s, &frames);
    CHECK(frames == 0, "truncated frame swallows its successor (%d)", frames);
    fa.reset();
    frames = 0;
    CHECK(feed_all(fa, good, &frames) == 160 && frames == 1, "and a reset recovers");
  }
  {  // Lengths at and past the buffer.
    H::FrameAssembler fa;
    int frames = 0;
    std::vector<uint8_t> s = {0xF4, 0xF5, 0x01, 0x40, 0xC0};  // LEN 0xC0 -> 201 bytes, one too many
    s.resize(300, 0x00);
    feed_all(fa, s, &frames);
    CHECK(frames == 0, "a 201-byte claim is dropped");
    // The shortest frame the length byte can describe is 9 bytes, too short to carry a class.
    std::vector<uint8_t> tiny = {0xF4, 0xF5, 0x01, 0x40, 0x00, 0x00, 0x00, 0xF4, 0xFB};
    fa.reset();
    frames = 0;
    size_t n = feed_all(fa, tiny, &frames);
    H::AcState st;
    H::AcFeatures ft;
    CHECK(n == 9 && n <= H::FRAME_CLASS_OFFSET, "a 9-byte frame assembles and is too short for the class check");
    CHECK(!H::parse_status(fa.data(), n, &st) && !H::parse_features(fa.data(), n, &ft), "and parses as nothing");
  }
  // Parsers refuse anything shorter than the fields they read.
  for (size_t len = 0; len < 45; len++) {
    std::vector<uint8_t> s(good.begin(), good.begin() + (long) len);
    H::AcState st;
    CHECK(!H::parse_status(s.data(), s.size(), &st), "status of %zu bytes refused", len);
  }
}

int main() {
  printf("== ESPHome component edge cases ==\n");
  test_status_mode();
  test_shadow_fan();
  test_power_intent();
  test_shadow_readiness();
  test_boundaries();
  test_frame_stream();
  printf("  %d checks, %d failed\n", g_checks, g_fail);
  printf(g_fail ? "== EDGE CASES FAILED ==\n" : "== EDGE CASES OK ==\n");
  return g_fail ? 1 : 0;
}
