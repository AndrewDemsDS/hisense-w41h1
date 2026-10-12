// Gap tests for the codec (hisense_protocol.h and .cpp): temperature conversion, checksums,
// builders, parsers, the setup-request tracker and the receive-side frame assembler.
//
// Every check here closes a hole that coverage or mutation testing found in the other host tests
// (firmware/test/coverage/run.sh and mutate.py): a line no test ran, a branch no test took, or a
// mutant every test let through. The comment on each block names the line or the mutant it is
// there for. Line numbers are those of hisense_protocol.cpp when the test was written: read them
// as "the statement that says this", since they move.
//
// The checks state what the code does today. Byte values are traced to where they come from:
// "capture" is a frame captured from the vendor module and stored verbatim in hisense_protocol.h
// (STATUS_REQUEST, LINK_INIT_0A, LINK_INIT_07, LINK_HEARTBEAT, and the two literal power frames in
// hisense_protocol.cpp), "header" is a constant or a comment of hisense_protocol.h, "RE docs/03"
// is reverse-engineering/docs/03-rs485-ac-protocol.md.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

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

typedef std::vector<uint8_t> Bytes;

// Write the checksum and the end tag of an A/C -> module frame whose other bytes are set.
// Checksum: 16-bit sum over [2, len - 4), high byte first (header, "Checksums and framing").
static void seal(Bytes *f) {
  size_t n = f->size();
  uint32_t sum = 0;
  for (size_t i = 2; i < n - 4; i++)
    sum += (*f)[i];
  (*f)[n - 4] = (uint8_t) (sum >> 8);
  (*f)[n - 3] = (uint8_t) sum;
  (*f)[n - 2] = 0xF4;  // FRAME_ETX1
  (*f)[n - 1] = 0xFB;  // FRAME_ETX2
}

// A sealed A/C -> module frame of `len` bytes: F4 F5, direction 0x01, CTRL 0x40, LEN = len - 9
// (header: "A frame's total length is its LEN byte plus 9"), class at byte 13.
static Bytes frame(size_t len, uint8_t cls) {
  Bytes f(len, 0);
  f[0] = 0xF4;
  f[1] = 0xF5;
  f[2] = 0x01;
  f[3] = 0x40;
  f[4] = (uint8_t) (len - 9);
  if (len > 13)
    f[13] = cls;
  seal(&f);
  return f;
}

// As it travels: every 0xF4 between the start and the end tag doubled.
static Bytes wire(const Bytes &f) {
  Bytes w(f.begin(), f.begin() + 2);
  for (size_t i = 2; i < f.size() - 2; i++) {
    w.push_back(f[i]);
    if (f[i] == 0xF4)
      w.push_back(0xF4);
  }
  w.push_back(0xF4);
  w.push_back(0xFB);
  return w;
}

// Feed a byte stream. Returns the number of frames completed and keeps the last one. Fails the
// test when feed() returns anything but 0 or a plausible length.
static int feed_all(H::FrameAssembler *fa, const Bytes &stream, Bytes *last) {
  int frames = 0;
  for (uint8_t b : stream) {
    size_t n = fa->feed(b);
    if (n != 0) {
      frames++;
      if (last != nullptr)
        last->assign(fa->data(), fa->data() + (n <= H::RX_FRAME_MAX ? n : 0));
    }
  }
  return frames;
}

static Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes out;
  for (const Bytes &p : parts)
    out.insert(out.end(), p.begin(), p.end());
  return out;
}

// ---- Defaults ----------------------------------------------------------------------------------
// What a struct holds before anything is written to it. The hub's command shadow is an AcCommand
// and its state an AcState: until the first status frame these defaults are what it works with
// (mutants on the member initialisers of hisense_protocol.h).
static void test_defaults() {
  printf("-- what each struct holds before it is filled\n");
  H::AcCommand c;
  CHECK(c.mode == H::MODE_COOL && c.setpoint == 24 && !c.fahrenheit && c.fan == H::FAN_SPEED_AUTO,
        "command: cool, 24 C, auto fan");
  CHECK(c.vswing == H::SWING_MODE_OFF && c.hswing == H::SWING_MODE_OFF && c.feature == H::FEATURE_NONE &&
            c.display == H::DISPLAY_NOCHANGE && !c.power_on,
        "command: no swing, neutral byte 33, display left alone, no power-on");
  H::AcState s;
  CHECK(!s.valid && !s.power_on && s.mode == H::MODE_COOL && !s.temp_unit_f, "state: not valid, off, cool, Celsius");
  CHECK(s.indoor_temp_c == 0 && s.setpoint_c == 0 && s.outdoor_temp_c == 0 && s.coil_temp_c == 0,
        "state: temperatures 0");
  CHECK(s.fan_raw == 0 && s.sleep_raw == 0 && s.compressor_freq == 0 && s.current_raw == 0 && s.voltage_raw == 0,
        "state: raw bytes 0");
  CHECK(!s.vswing_on && !s.hswing_on && !s.turbo_on && !s.eco_on && !s.heat_relay_on && !s.mute_on && !s.sleep_on &&
            !s.purify_on,
        "state: no flag set");
  H::AcFeatures f;
  CHECK(!f.valid && !f.ext_valid && f.power_display == 0 && f.demand_resp == 0 && f.reply_len == 0 &&
            H::features_to_bitmap32(f) == 0,
        "features: nothing known, bitmap 0");
  H::AcFaults fl;
  CHECK(!fl.valid && !fl.any && fl.raw_indoor == 0 && fl.raw_module == 0 && fl.raw_outdoor == 0 &&
            fl.raw_protect == 0 && H::faults_to_bitmap32(fl) == 0,
        "faults: nothing known, bitmap 0");
  // The framing constants (header, "confirmed against the vendor W41H1 module's parser").
  CHECK(H::FRAME_STX1 == 0xF4 && H::FRAME_STX2 == 0xF5 && H::FRAME_ETX1 == 0xF4 && H::FRAME_ETX2 == 0xFB,
        "F4 F5 ... F4 FB");
  CHECK(H::FRAME_LEN_OVERHEAD == 9 && H::FRAME_CLASS_OFFSET == 13 && H::CMD_FRAME_LEN == 50 && H::CMD_FRAME_MAX == 52 &&
            H::RX_FRAME_MAX == 200 && H::TX_FRAME_MAX == 64,
        "LEN + 9, class at 13, command 50 (52 stuffed), receive 200, transmit 64");
  CHECK(H::CLASS_DEVTYPE == 0x0A && H::CLASS_LINK == 0x1E && H::CLASS_COMMAND == 0x65 && H::CLASS_STATUS == 0x66 &&
            H::SUBTYPE_PRODUCT_TYPE == 0x40,
        "classes 0A, 1E, 65, 66 and subtype 40");
  CHECK(H::BUS_BAUD_RATE == 9600, "9600 baud");
  // Each captured frame says its own length: LEN byte + 9 (header: poll 0x0C is 21, link 0x13 is
  // 28, command 0x29 is 50).
  CHECK(H::STATUS_REQUEST[4] + 9 == (int) H::STATUS_REQUEST_LEN &&
            H::LINK_INIT_0A[4] + 9 == (int) H::LINK_INIT_0A_LEN &&
            H::LINK_INIT_07[4] + 9 == (int) H::LINK_INIT_07_LEN &&
            H::LINK_HEARTBEAT[4] + 9 == (int) H::LINK_HEARTBEAT_LEN && H::CMD_HEADER[4] + 9 == (int) H::CMD_FRAME_LEN,
        "every frame constant's LEN byte agrees with its length");
  // The fault bit numbers binary_sensor.py reads (header: "uses the same bit indices").
  const uint8_t fault_bits[] = {H::FAULT1_IN_TEMP,      H::FAULT1_IN_COIL_TEMP, H::FAULT1_IN_HUMIDITY,
                                H::FAULT1_WATER_FULL,   H::FAULT1_IN_FAN_MOTOR, H::FAULT1_GRILLE,
                                H::FAULT1_IN_VZERO,     H::FAULT1_IN_COM,       H::FAULT1_IN_DISPLAY,
                                H::FAULT1_IN_KEYS,      H::FAULT1_IN_WIFI,      H::FAULT1_IN_ELE,
                                H::FAULT1_IN_EEPROM,    H::FAULT1_OUT_EEPROM,   H::FAULT1_OUT_COIL_TEMP,
                                H::FAULT1_OUT_GAS_TEMP, H::FAULT1_OUT_TEMP,     H::FAULT1_OVER_TEMP};
  for (uint8_t i = 0; i < 18; i++)
    CHECK(fault_bits[i] == i, "fault constant %u is bit %u", i, i);
  CHECK(H::FAULT1_ANY == 30 && H::FAULT1_VALID == 31, "any is 30, valid is 31");
}

// ---- Temperature -------------------------------------------------------------------------------
// f_to_c and c_to_f round to nearest. The values are plain arithmetic: C = (F - 32) * 5 / 9.
static void test_temperature() {
  printf("-- temperature conversion: rounding, sign, and the int8 clamp\n");
  struct {
    int f, c;
  } to_c[] = {{32, 0},      {33, 1},      {31, -1},     {34, 1},      {30, -1},   {41, 5},    {23, -5},
              {61, 16},     {62, 17},     {63, 17},     {64, 18},     {77, 25},   {90, 32},   {212, 100},
              {-40, -40},   {0, -18},     {255, 124},   {260, 127},   {261, 127}, {262, 127}, {1000, 127},
              {-198, -128}, {-199, -128}, {-200, -128}, {-1000, -128}};
  for (const auto &r : to_c)
    CHECK(H::f_to_c(r.f) == r.c, "%d F is %d C (got %d)", r.f, r.c, H::f_to_c(r.f));
  // 262 F is 127.8 C: without the clamp at line 102 the int8 cast would wrap it to -128.
  // -200 F is -128.9 C: without the clamp at line 104 it would wrap to 127.
  struct {
    int c, f;
  } to_f[] = {{0, 32},    {1, 34},    {-1, 30},    {2, 36},     {3, 37},     {-3, 27},     {16, 61},    {17, 63},
              {24, 75},   {25, 77},   {32, 90},    {-40, -40},  {-18, 0},    {52, 126},    {53, 127},   {54, 127},
              {100, 127}, {127, 127}, {-88, -126}, {-89, -128}, {-90, -128}, {-128, -128}, {1000, 127}, {-1000, -128}};
  for (const auto &r : to_f)
    CHECK(H::c_to_f(r.c) == r.f, "%d C is %d F (got %d)", r.c, r.f, H::c_to_f(r.c));
  // 54 C is 129.2 F and -90 C is -130 F: the two values just past the clamps at lines 114-117.

  // setpoint_in_range, one either side of each of the four bounds (header: 16..32 C, 61..90 F).
  CHECK(!H::setpoint_in_range(15, false) && H::setpoint_in_range(16, false), "C: 15 no, 16 yes");
  CHECK(H::setpoint_in_range(32, false) && !H::setpoint_in_range(33, false), "C: 32 yes, 33 no");
  CHECK(!H::setpoint_in_range(60, true) && H::setpoint_in_range(61, true), "F: 60 no, 61 yes");
  CHECK(H::setpoint_in_range(90, true) && !H::setpoint_in_range(91, true), "F: 90 yes, 91 no");
  CHECK(!H::setpoint_in_range(61, false) && !H::setpoint_in_range(32, true),
        "a value of the other unit is out of range");
  CHECK(!H::setpoint_in_range(-128, false) && !H::setpoint_in_range(127, false) && !H::setpoint_in_range(-128, true) &&
            !H::setpoint_in_range(127, true) && !H::setpoint_in_range(0, false) && !H::setpoint_in_range(0, true),
        "the ends of the byte and zero");

  // shadow_setpoint_from_status. Line 130: `out` may be null (mutants that drop the test crash).
  CHECK(H::shadow_setpoint_from_status(24, false, nullptr), "a valid setpoint with nowhere to put it is still valid");
  CHECK(!H::shadow_setpoint_from_status(5, false, nullptr), "an invalid one is still invalid");
  int8_t out = 99;
  CHECK(!H::shadow_setpoint_from_status(15, false, &out) && out == 99, "15 C refused, the output untouched");
  CHECK(!H::shadow_setpoint_from_status(33, false, &out) && out == 99, "33 C refused");
  CHECK(H::shadow_setpoint_from_status(16, false, &out) && out == 16, "16 C taken");
  CHECK(H::shadow_setpoint_from_status(32, false, &out) && out == 32, "32 C taken");
  out = 99;
  CHECK(!H::shadow_setpoint_from_status(15, true, &out) && out == 99, "F panel: 15 C is 59 F, refused");
  CHECK(!H::shadow_setpoint_from_status(33, true, &out) && out == 99, "F panel: 33 C is 91 F, refused");
  CHECK(H::shadow_setpoint_from_status(16, true, &out) && out == 61, "F panel: 16 C goes in as 61 F");
  CHECK(H::shadow_setpoint_from_status(32, true, &out) && out == 90, "F panel: 32 C goes in as 90 F");
}

// ---- Checksum ----------------------------------------------------------------------------------
static void test_checksum() {
  printf("-- checksum: empty ranges, the carry into the high byte, the wrap at 16 bits\n");
  Bytes ff(300, 0xFF);
  CHECK(H::checksum_range(ff.data(), 0, 0) == 0 && H::checksum_range(ff.data(), 7, 7) == 0, "an empty range sums to 0");
  // Line 139: a range that ends before it starts is empty too (mutant `i != end` runs off the buffer).
  CHECK(H::checksum_range(ff.data(), 5, 3) == 0 && H::checksum_range(ff.data(), 1, 0) == 0,
        "an inverted range sums to 0");
  CHECK(H::checksum_range(ff.data(), 0, 1) == 0x00FF, "one byte");
  CHECK(H::checksum_range(ff.data(), 3, 5) == 0x01FE, "two bytes of 0xFF carry into the high byte");
  CHECK(H::checksum_range(ff.data(), 0, 257) == 0xFFFF, "257 * 0xFF is exactly 0xFFFF");
  CHECK(H::checksum_range(ff.data(), 0, 258) == 0x00FE, "one more wraps at 16 bits");
  Bytes ramp = {0x00, 0x7F, 0x80, 0x01, 0xFF};
  CHECK(H::checksum_range(ramp.data(), 1, 3) == 0x00FF, "0x7F + 0x80: the last value below the carry");
  CHECK(H::checksum_range(ramp.data(), 1, 4) == 0x0100, "+ 0x01: the carry, low byte back at 0");
  CHECK(H::checksum_range(ramp.data(), 0, 5) == 0x01FF, "the range is [start, end): both ends counted right");
  CHECK(H::checksum_range(ramp.data(), 1, 2) == 0x007F && H::checksum_range(ramp.data(), 4, 5) == 0x00FF,
        "start is included, end is not");

  // The captured frames carry their own checksums: 0x01B3, 0x0158, 0x0154, 0x0302 (capture).
  CHECK(H::status_checksum_ok(H::STATUS_REQUEST, H::STATUS_REQUEST_LEN), "the captured status poll verifies");
  CHECK(H::status_checksum_ok(H::LINK_INIT_0A, H::LINK_INIT_0A_LEN), "the captured 0x0A frame verifies");
  CHECK(H::status_checksum_ok(H::LINK_INIT_07, H::LINK_INIT_07_LEN), "the captured 0x07 frame verifies");
  CHECK(H::status_checksum_ok(H::LINK_HEARTBEAT, H::LINK_HEARTBEAT_LEN), "the captured heartbeat verifies");
  CHECK(H::checksum_range(H::STATUS_REQUEST, 2, 17) == 0x01B3, "and the sum is the captured 01 B3");

  // Line 145: nothing shorter than checksum + end tag + two bytes is looked at.
  const uint8_t six[6] = {0xAA, 0xBB, 0x00, 0x00, 0xF4, 0xFB};  // sum over [2, 2) is 0
  CHECK(H::status_checksum_ok(six, 6), "six bytes is the shortest frame that can verify");
  for (size_t n = 0; n < 6; n++)
    CHECK(!H::status_checksum_ok(six + (6 - n), n), "%zu bytes cannot", n);
  const uint8_t seven[7] = {0xAA, 0xBB, 0x12, 0x00, 0x12, 0xF4, 0xFB};
  CHECK(H::status_checksum_ok(seven, 7), "seven bytes: one byte summed");
  // Each of the four trailer bytes on its own. Poll checksum 01 B3: high and low differ.
  for (size_t at = H::STATUS_REQUEST_LEN - 4; at < H::STATUS_REQUEST_LEN; at++) {
    for (int delta : {-1, 1}) {
      uint8_t f[H::STATUS_REQUEST_LEN];
      std::memcpy(f, H::STATUS_REQUEST, sizeof(f));
      f[at] = (uint8_t) (f[at] + delta);
      CHECK(!H::status_checksum_ok(f, sizeof(f)), "trailer byte %zu off by %d fails", at, delta);
    }
  }
  uint8_t swapped[H::STATUS_REQUEST_LEN];
  std::memcpy(swapped, H::STATUS_REQUEST, sizeof(swapped));
  swapped[17] = 0xB3;
  swapped[18] = 0x01;
  CHECK(!H::status_checksum_ok(swapped, sizeof(swapped)), "the checksum is high byte first");
  // Bytes 0 and 1 are not summed, byte 2 is.
  uint8_t head[H::STATUS_REQUEST_LEN];
  std::memcpy(head, H::STATUS_REQUEST, sizeof(head));
  head[0] = 0x00;
  head[1] = 0x00;
  CHECK(H::status_checksum_ok(head, sizeof(head)), "the two start bytes are outside the sum");
  head[2] = 0x01;
  CHECK(!H::status_checksum_ok(head, sizeof(head)), "byte 2 is inside it");

  CHECK(H::reply_class_ok(0x66, 0x66) && !H::reply_class_ok(0x66, 0x65) && !H::reply_class_ok(0x65, 0x66),
        "a reply must be of the class asked for");
  CHECK(H::reply_class_ok(0x66, 0) && H::reply_class_ok(0x00, 0) && H::reply_class_ok(0xFF, 0), "0 accepts any class");
}

// ---- Builders: arguments and capacity ----------------------------------------------------------
static void test_builder_arguments() {
  printf("-- builders: no buffer, a buffer one byte short, a buffer just big enough\n");
  uint8_t out[80];
  H::AcCommand cmd;

  // build_command (line 266): CMD_FRAME_MAX = 52, the 50-byte frame plus two stuffed bytes.
  CHECK(H::build_command(cmd, nullptr, sizeof(out)) == 0, "command: no buffer");
  CHECK(H::build_command(cmd, out, 51) == 0, "command: 51 bytes is refused even when 50 would do");
  CHECK(H::build_command(cmd, out, 52) == 50, "command: 52 bytes is enough");
  CHECK(H::build_command(cmd, out, 0) == 0, "command: no room");

  // build_power_frame (line 231): the literal frames need exactly 50.
  CHECK(H::build_power_frame(true, nullptr, sizeof(out)) == 0, "power: no buffer");
  CHECK(H::build_power_frame(true, out, 49) == 0 && H::build_power_frame(false, out, 49) == 0, "power: 49 bytes");
  CHECK(H::build_power_frame(true, out, 50) == 50, "power: 50 bytes");
  // Capture: on has byte 18 = 0x0C and checksum 01 DF, off has byte 18 = 0x04 and checksum 02 31.
  CHECK(out[18] == 0x0C && out[46] == 0x01 && out[47] == 0xDF && H::status_checksum_ok(out, 50),
        "power on: byte 18 0x0C, checksum 01 DF, and it verifies");
  CHECK(H::build_power_frame(false, out, 50) == 50 && out[18] == 0x04 && out[46] == 0x02 && out[47] == 0x31 &&
            H::status_checksum_ok(out, 50),
        "power off: byte 18 0x04, checksum 02 31, and it verifies");

  // build_producttype_request (line 222): 21 bytes, the poll with byte 14 = 0x40. The poll's
  // checksum 0x01B3 (capture) plus 0x40 is 0x01F3.
  CHECK(H::build_producttype_request(nullptr, sizeof(out)) == 0, "product type: no buffer");
  CHECK(H::build_producttype_request(out, 20) == 0, "product type: 20 bytes");
  std::memset(out, 0xEE, sizeof(out));
  CHECK(H::build_producttype_request(out, 21) == 21, "product type: 21 bytes");
  CHECK(out[13] == 0x66 && out[14] == 0x40 && out[17] == 0x01 && out[18] == 0xF3 && out[19] == 0xF4 && out[20] == 0xFB,
        "product type: 66 40, checksum 01 F3, end tag");
  CHECK(out[21] == 0xEE, "product type: nothing written past the frame");
  for (size_t i = 0; i < 17; i++)
    if (i != 14)
      CHECK(out[i] == H::STATUS_REQUEST[i], "product type: byte %zu is the poll's", i);

  // build_single_field (line 241).
  CHECK(H::build_single_field(17, 0x03, nullptr, sizeof(out)) == 0, "single field: no buffer");
  CHECK(H::build_single_field(17, 0x03, out, 51) == 0, "single field: 51 bytes");
  CHECK(H::build_single_field(17, 0x03, out, 52) == 50, "single field: 52 bytes");
  CHECK(
      H::build_single_field(50, 0x03, out, sizeof(out)) == 0 && H::build_single_field(255, 0x03, out, sizeof(out)) == 0,
      "single field: an offset at or past the end of the frame");
  // Only a body byte may be named: 0 to 15 are the envelope (offset 0 would give a frame that
  // does not start F4 F5, offset 4 a wrong LEN, offset 13 another class), 46 to 49 the checksum
  // and the end tag, which are written over whatever was put there.
  for (int offset = 0; offset < 256; offset++) {
    const bool body = offset >= 16 && offset <= 45;
    std::memset(out, 0xEE, sizeof(out));
    size_t n = H::build_single_field((uint8_t) offset, 0x03, out, sizeof(out));
    CHECK((n == 50) == body && (body || n == 0), "single field: offset %d is %s", offset, body ? "taken" : "refused");
    if (!body)
      CHECK(out[0] == 0xEE && out[49] == 0xEE, "single field: offset %d refused with nothing written", offset);
  }
  // The baseline: the command header, 0x04 at byte 23, 0x01 at byte 31 (header).
  CHECK(H::build_single_field(20, 0x00, out, sizeof(out)) == 50, "single field: a zero value");
  for (size_t i = 0; i < 46; i++) {
    uint8_t want = i < H::CMD_HEADER_LEN ? H::CMD_HEADER[i] : (i == 23 ? 0x04 : (i == 31 ? 0x01 : 0x00));
    CHECK(out[i] == want, "single field: baseline byte %zu is 0x%02X", i, want);
  }
  // Header sum 0x1CF + 0x04 + 0x01.
  CHECK(out[46] == 0x01 && out[47] == 0xD4 && out[48] == 0xF4 && out[49] == 0xFB,
        "single field: checksum 01 D4, end tag");

  // build_link_heartbeat (line 347): 28 bytes plus room for two stuffed ones.
  CHECK(H::build_link_heartbeat(true, false, nullptr, sizeof(out)) == 0, "heartbeat: no buffer");
  CHECK(H::build_link_heartbeat(true, false, out, 29) == 0 && H::build_link_heartbeat(true, true, out, 29) == 0,
        "heartbeat: 29 bytes");
  CHECK(H::build_link_heartbeat(true, false, out, 27) == 0 && H::build_link_heartbeat(true, false, out, 0) == 0,
        "heartbeat: less");
  std::memset(out, 0xEE, sizeof(out));
  CHECK(H::build_link_heartbeat(true, false, out, 30) == 28, "heartbeat: 30 bytes");
  CHECK(std::memcmp(out, H::LINK_HEARTBEAT, 28) == 0 && out[28] == 0xEE, "heard, not provisioning: the captured frame");
  // Not heard: byte 16 is 0xF0 (header), checksum 0x0302 + 0x40. Provisioning: byte 17 bit 3.
  CHECK(H::build_link_heartbeat(false, false, out, 30) == 28 && out[16] == 0xF0 && out[17] == 0x80 && out[24] == 0x03 &&
            out[25] == 0x42 && out[26] == 0xF4 && out[27] == 0xFB,
        "not heard: F0, checksum 03 42");
  CHECK(H::build_link_heartbeat(true, true, out, 30) == 28 && out[16] == 0xB0 && out[17] == 0x88 && out[24] == 0x03 &&
            out[25] == 0x0A,
        "provisioning: byte 17 is 0x88, checksum 03 0A");
  CHECK(H::build_link_heartbeat(false, true, out, 30) == 28 && out[16] == 0xF0 && out[17] == 0x88 && out[25] == 0x4A,
        "both: F0 88, checksum 03 4A");
  CHECK(H::build_link_heartbeat(true, false, out, 30) == 28 && std::memcmp(out, H::LINK_HEARTBEAT, 28) == 0,
        "the four-argument form with no flag is the captured frame");
}

// The checksum's low byte at 0xF4 (doubled on the wire) and one either side of it, and the carry
// from 0x01FF to 0x0200. build_single_field(20, v) sums to 0x01D4 + v.
static void test_checksum_stuffing() {
  printf("-- a checksum byte of 0xF4, its neighbours, and the carry\n");
  uint8_t out[H::CMD_FRAME_MAX + 4];
  struct {
    uint8_t value;
    uint8_t hi, lo;
    size_t wire_len;
  } rows[] = {{0x1F, 0x01, 0xF3, 50}, {0x20, 0x01, 0xF4, 51}, {0x21, 0x01, 0xF5, 50},
              {0x2B, 0x01, 0xFF, 50}, {0x2C, 0x02, 0x00, 50}, {0xFF, 0x02, 0xD3, 50}};
  for (const auto &r : rows) {
    std::memset(out, 0xEE, sizeof(out));
    size_t n = H::build_single_field(20, r.value, out, H::CMD_FRAME_MAX);
    CHECK(n == r.wire_len, "value 0x%02X: %zu bytes on the wire (got %zu)", r.value, r.wire_len, n);
    CHECK(out[46] == r.hi && out[47] == r.lo, "value 0x%02X: checksum %02X %02X (got %02X %02X)", r.value, r.hi, r.lo,
          out[46], out[47]);
    if (r.wire_len == 51) {
      CHECK(out[48] == 0xF4 && out[49] == 0xF4 && out[50] == 0xFB, "the 0xF4 is doubled, then the end tag");
    } else {
      CHECK(out[48] == 0xF4 && out[49] == 0xFB, "no stuffing: the end tag follows at once");
    }
    CHECK(out[n] == 0xEE, "nothing past the frame");
  }
  // The same through build_command: cool (0x50), 24 C (0x31), auto fan (0x01), hold louvre
  // (0x40), neutral byte 33 (0x04), marker bytes 0x04 and 0x01, on the header's 0x1CF: 0x029A.
  // A setpoint of 27 adds 6: 0x02A0. Display off adds 0x40.
  H::AcCommand c;
  CHECK(H::build_command(c, out, H::CMD_FRAME_MAX) == 50 && out[46] == 0x02 && out[47] == 0x9A,
        "the default command sums to 02 9A (got %02X %02X)", out[46], out[47]);
  // Every command the entity can build in cool, heat or auto on a Celsius panel: the checksum is
  // the sum, and the frames that land on a low byte of 0xF4 are the 51-byte ones.
  int stuffed = 0, built = 0;
  const H::FanSpeed fans[] = {H::FAN_SPEED_AUTO, H::FAN_SPEED_LOW,      H::FAN_SPEED_MED_LOW,
                              H::FAN_SPEED_MID,  H::FAN_SPEED_MED_HIGH, H::FAN_SPEED_HIGH};
  for (H::Mode mode : {H::MODE_HEAT, H::MODE_COOL, H::MODE_AUTO}) {
    for (int t = H::SETPOINT_MIN_C; t <= H::SETPOINT_MAX_C; t++) {
      for (H::FanSpeed fan : fans) {
        for (int feature = 0; feature < 4; feature++) {
          for (int swing = 0; swing < 4; swing++) {
            H::AcCommand k;
            k.mode = mode;
            k.setpoint = (int8_t) t;
            k.fan = fan;
            k.feature = (H::Feature) feature;
            k.vswing = (swing & 1) ? H::SWING_MODE_SWING : H::SWING_MODE_OFF;
            k.hswing = (swing & 2) ? H::SWING_MODE_SWING : H::SWING_MODE_OFF;
            std::memset(out, 0xEE, sizeof(out));
            size_t n = H::build_command(k, out, H::CMD_FRAME_MAX);
            uint16_t sum = H::checksum_range(out, 2, 46);
            const bool f4 = (sum & 0xFF) == 0xF4;
            built++;
            stuffed += f4;
            CHECK(out[46] == (sum >> 8) && out[47] == (sum & 0xFF) && n == (f4 ? 51u : 50u) && out[n - 2] == 0xF4 &&
                      out[n - 1] == 0xFB && out[n] == 0xEE && (!f4 || out[48] == 0xF4),
                  "mode %d, %d C, fan %d, feature %d, swing %d: checksum %04X, %zu bytes", (int) mode, t, (int) fan,
                  feature, swing, sum, n);
          }
        }
      }
    }
  }
  CHECK(stuffed > 0, "some ordinary commands land on a 0xF4 checksum byte (%d of %d do)", stuffed, built);
}

// build_command: the values next to the valid enums, and the bytes it leaves at zero.
static void test_command_bytes() {
  printf("-- command bytes for values at and past the ends of the enums\n");
  uint8_t out[H::CMD_FRAME_MAX];
  // Byte 33 (header, Feature): 0x04 neutral, 0x30 eco, 0x0C turbo, 0x10 eco clear. A value past
  // the enum is refused: no frame.
  struct {
    int feature;
    uint8_t byte33;
  } features[] = {{0, 0x04}, {1, 0x30}, {2, 0x0C}, {3, 0x10}};
  for (const auto &r : features) {
    H::AcCommand c;
    c.feature = (H::Feature) r.feature;
    CHECK(H::build_command(c, out, sizeof(out)) > 0 && out[33] == r.byte33,
          "feature %d is byte 33 = 0x%02X (got 0x%02X)", r.feature, r.byte33, out[33]);
  }
  // Byte 36 (header, Display): 0x00 leave alone, 0xC0 on, 0x40 off.
  struct {
    int display;
    uint8_t byte36;
  } displays[] = {{0, 0x00}, {1, 0xC0}, {2, 0x40}};
  for (const auto &r : displays) {
    H::AcCommand c;
    c.display = (H::Display) r.display;
    CHECK(H::build_command(c, out, sizeof(out)) > 0 && out[36] == r.byte36, "display %d is byte 36 = 0x%02X", r.display,
          r.byte36);
  }
  // Every value of every enum field, one field at a time: a named value builds, any other is
  // refused with nothing written. The packing rules would otherwise turn it into a byte no capture
  // backs: mode 8 into 0x10 (fan-only), fan 2 to 4 into 0x05 to 0x09, hswing 2 into byte 37 = 0x14
  // with no direction bits.
  for (int field = 0; field < 6; field++) {
    int built = 0;
    for (int v = 0; v < 256; v++) {
      H::AcCommand c;
      bool named = false;
      switch (field) {
        case 0:
          c.mode = (H::Mode) v;
          named = v <= 4;
          break;
        case 1:
          c.fan = (H::FanSpeed) v;
          named = v == 0 || v == 1 || (v >= 5 && v <= 9) || v == 0xFF;
          break;
        case 2:
          c.vswing = (H::SwingMode) v;
          named = v == 0 || v == 1 || v == 3;
          break;
        case 3:
          c.hswing = (H::SwingMode) v;
          named = v == 0 || v == 1 || v == 3;
          break;
        case 4:
          c.feature = (H::Feature) v;
          named = v <= 3;
          break;
        default:
          c.display = (H::Display) v;
          named = v <= 2;
          break;
      }
      std::memset(out, 0xEE, sizeof(out));
      size_t n = H::build_command(c, out, sizeof(out));
      CHECK(H::command_enums_valid(c) == named, "enum field %d = %d is %s", field, v, named ? "named" : "not named");
      CHECK((n != 0) == named, "enum field %d = %d: %s", field, v, named ? "built" : "refused");
      if (!named)
        CHECK(out[0] == 0xEE && out[18] == 0xEE, "enum field %d = %d refused with nothing written", field, v);
      built += n != 0 ? 1 : 0;
    }
    static const int NAMED[6] = {5, 8, 3, 3, 4, 3};
    CHECK(built == NAMED[field], "enum field %d has %d named values (%d built)", field, NAMED[field], built);
  }
  // Byte 32: vertical in bits 6-7 (0x3 swing, 0x1 hold), horizontal in bits 4-5 (0x3 swing, 0x1
  // direction, 0 off). Byte 37 = 0x14 whenever horizontal is not off. SwingMode has no value 2:
  // vertical treats it as hold, horizontal sends no bits.
  struct {
    int v, h;
    uint8_t byte32, byte37;
  } swings[] = {{0, 0, 0x40, 0x00}, {3, 0, 0xC0, 0x00}, {1, 0, 0x40, 0x00}, {0, 3, 0x70, 0x14},
                {0, 1, 0x50, 0x14}, {3, 3, 0xF0, 0x14}, {3, 1, 0xD0, 0x14}};
  for (const auto &r : swings) {
    H::AcCommand c;
    c.vswing = (H::SwingMode) r.v;
    c.hswing = (H::SwingMode) r.h;
    CHECK(H::build_command(c, out, sizeof(out)) > 0 && out[32] == r.byte32 && out[37] == r.byte37,
          "vswing %d, hswing %d: byte 32 = 0x%02X, byte 37 = 0x%02X (got 0x%02X, 0x%02X)", r.v, r.h, r.byte32, r.byte37,
          out[32], out[37]);
    CHECK(out[31] == 0x01, "byte 31 is the marker");
  }
  // Values SwingMode does not have (2, and anything past 3) used to be packed: horizontal as
  // "active" in byte 37 with no direction bits in byte 32. They are refused.
  for (int odd : {2, 4, 255}) {
    H::AcCommand c;
    c.hswing = (H::SwingMode) odd;
    CHECK(H::build_command(c, out, sizeof(out)) == 0, "hswing %d (no such mode): no frame", odd);
    H::AcCommand v;
    v.vswing = (H::SwingMode) odd;
    CHECK(H::build_command(v, out, sizeof(out)) == 0, "vswing %d (no such mode): no frame", odd);
  }
  // The bytes a combined frame leaves at zero: 17 (sleep has its own frame), 35 (mute has its
  // own frame), and everything not named. Lines 280 and 328 write those zeros.
  H::AcCommand c;
  c.mode = H::MODE_HEAT;
  c.setpoint = 30;
  c.fan = H::FAN_SPEED_HIGH;
  c.vswing = c.hswing = H::SWING_MODE_SWING;
  c.feature = H::FEATURE_ECO;
  c.display = H::DISPLAY_ON;
  c.power_on = true;
  CHECK(H::build_command(c, out, sizeof(out)) == 50, "a command with every field set");
  const uint8_t named[] = {16, 18, 19, 23, 31, 32, 33, 36, 37};
  for (size_t i = H::CMD_HEADER_LEN; i < H::CMD_CHK_OFFSET; i++) {
    bool is_named = false;
    for (uint8_t n : named)
      is_named = is_named || n == i;
    if (!is_named)
      CHECK(out[i] == 0x00, "byte %zu is not a combined frame's and stays 0 (got 0x%02X)", i, out[i]);
  }
  // Fan high 9 * 2 + 1, heat with power-on (1 * 2 + 1) << 4 | 0x0C (header: 0x3C is the stock
  // module's own byte), 30 C * 2 + 1.
  CHECK(out[16] == 0x13 && out[18] == 0x3C && out[19] == 0x3D && out[23] == 0x04,
        "fan 0x13, mode 0x3C, setpoint 0x3D, baseline 0x04");
  CHECK(std::memcmp(out, H::CMD_HEADER, H::CMD_HEADER_LEN) == 0, "behind the command header");

  // The setpoint gate, at the bounds, in both units. Dry and fan-only carry no setpoint, so an
  // out-of-range one must not drop the frame there (line 271).
  for (int unit_f = 0; unit_f < 2; unit_f++) {
    const int lo = unit_f ? 61 : 16, hi = unit_f ? 90 : 32;
    for (int t : {lo - 1, lo, hi, hi + 1}) {
      const bool in = t >= lo && t <= hi;
      for (int mode = H::MODE_FAN; mode <= H::MODE_AUTO; mode++) {
        H::AcCommand k;
        k.mode = (H::Mode) mode;
        k.setpoint = (int8_t) t;
        k.fahrenheit = unit_f != 0;
        const bool stripped = mode == H::MODE_DRY || mode == H::MODE_FAN;
        size_t n = H::build_command(k, out, sizeof(out));
        CHECK((n > 0) == (in || stripped), "mode %d, setpoint %d %s: %s", mode, t, unit_f ? "F" : "C",
              (in || stripped) ? "built" : "refused");
        if (n > 0)
          CHECK(out[19] == (stripped ? 0x00 : (uint8_t) (t * 2 + 1)), "byte 19 is %s",
                stripped ? "0" : "value * 2 + 1");
      }
    }
  }

  // build_sleep_frame and build_mute_frame (header: byte 17 = profile * 2 + 1, byte 35 = 0x30
  // on, 0x10 off). A profile past 4 is sent as off.
  const uint8_t sleep_bytes[] = {0x01, 0x03, 0x05, 0x07, 0x09, 0x01, 0x01};
  const uint8_t profiles[] = {0, 1, 2, 3, 4, 5, 255};
  for (size_t i = 0; i < sizeof(profiles); i++)
    CHECK(H::build_sleep_frame(profiles[i], out, sizeof(out)) == 50 && out[17] == sleep_bytes[i] && out[35] == 0x00,
          "sleep profile %u is byte 17 = 0x%02X (got 0x%02X)", profiles[i], sleep_bytes[i], out[17]);
  CHECK(H::build_mute_frame(true, out, sizeof(out)) == 50 && out[35] == 0x30 && out[17] == 0x00,
        "mute on: byte 35 = 0x30");
  CHECK(H::build_mute_frame(false, out, sizeof(out)) == 50 && out[35] == 0x10 && out[17] == 0x00, "mute off: 0x10");
  CHECK(H::build_sleep_frame(1, out, 51) == 0 && H::build_mute_frame(true, out, 51) == 0 &&
            H::build_sleep_frame(1, nullptr, 60) == 0 && H::build_mute_frame(true, nullptr, 60) == 0,
        "both refuse a short or missing buffer");
}

// ---- stamp_link_token and stamp_beep -----------------------------------------------------------
static void test_stamps() {
  printf("-- re-stamping a finished frame: arguments, the shortest and the longest frame\n");
  uint8_t out[80];
  const uint8_t *poll = H::STATUS_REQUEST;
  const size_t poll_len = H::STATUS_REQUEST_LEN;

  // Lines 184 and 201: each argument on its own.
  CHECK(H::stamp_link_token(nullptr, poll_len, 1, 1, out, sizeof(out)) == 0, "token: no input");
  CHECK(H::stamp_link_token(poll, poll_len, 1, 1, nullptr, sizeof(out)) == 0, "token: no output");
  CHECK(H::stamp_link_token(poll, 12, 1, 1, out, sizeof(out)) == 0, "token: 12 bytes of input");
  CHECK(H::stamp_beep(nullptr, poll_len, true, out, sizeof(out)) == 0, "beep: no input");
  CHECK(H::stamp_beep(poll, poll_len, true, nullptr, sizeof(out)) == 0, "beep: no output");
  CHECK(H::stamp_beep(poll, 12, true, out, sizeof(out)) == 0, "beep: 12 bytes of input");

  // Every length below 13, from a buffer of exactly that size: nothing past it may be read. The
  // start bytes and the LEN byte sit at 0, 1 and 4, so the length is tested before them. Only a
  // sanitizer build can see a read past the end (mutate.py --sanitize): the result is 0 either way.
  for (size_t len = 0; len < 13; len++) {
    Bytes exact(H::STATUS_REQUEST, H::STATUS_REQUEST + len);
    CHECK(H::stamp_link_token(exact.data(), len, 1, 1, out, sizeof(out)) == 0, "token: %zu bytes of input", len);
    CHECK(H::stamp_beep(exact.data(), len, true, out, sizeof(out)) == 0, "beep: %zu bytes of input", len);
  }
  // Lines 186 and 203: the start bytes, one either side of each.
  for (int at = 0; at < 2; at++) {
    for (int delta : {-1, 1}) {
      uint8_t bad[H::STATUS_REQUEST_LEN];
      std::memcpy(bad, poll, poll_len);
      bad[at] = (uint8_t) (bad[at] + delta);
      CHECK(H::stamp_link_token(bad, poll_len, 1, 1, out, sizeof(out)) == 0, "token: start byte %d off by %d", at,
            delta);
      CHECK(H::stamp_beep(bad, poll_len, true, out, sizeof(out)) == 0, "beep: start byte %d off by %d", at, delta);
    }
  }

  // The shortest frame both accept is 13 bytes (LEN 4): 9 envelope bytes, checksum, end tag.
  uint8_t f13[13] = {0xF4, 0xF5, 0x00, 0x40, 0x04, 0x00, 0x00, 0x01, 0x01, 0x00, 0x46, 0xF4, 0xFB};
  CHECK(H::status_checksum_ok(f13, 13), "(the 13-byte frame is well formed)");
  std::memset(out, 0xEE, sizeof(out));
  CHECK(H::stamp_link_token(f13, 13, 0x02, 0x05, out, sizeof(out)) == 13, "token: a 13-byte frame is stamped");
  // 0x40 + 0x04 + 0x02 + 0x05 = 0x4B.
  CHECK(out[7] == 0x02 && out[8] == 0x05 && out[9] == 0x00 && out[10] == 0x4B && out[11] == 0xF4 && out[12] == 0xFB &&
            out[13] == 0xEE,
        "token: bytes 7 and 8, checksum 00 4B, end tag, nothing after");
  CHECK(H::stamp_beep(f13, 13, false, out, sizeof(out)) == 13 && std::memcmp(out, f13, 13) == 0,
        "beep: a 13-byte frame is copied through");
  // LEN 3 says 12 bytes: refused even with 13 supplied.
  uint8_t f12[13];
  std::memcpy(f12, f13, 13);
  f12[4] = 0x03;
  CHECK(
      H::stamp_link_token(f12, 13, 1, 1, out, sizeof(out)) == 0 && H::stamp_beep(f12, 13, true, out, sizeof(out)) == 0,
      "a LEN byte that says 12 bytes is refused");
  // LEN says one byte more than supplied.
  CHECK(H::stamp_link_token(poll, poll_len - 1, 1, 1, out, sizeof(out)) == 0 &&
            H::stamp_beep(poll, poll_len - 1, true, out, sizeof(out)) == 0,
        "a frame cut one byte short of its LEN is refused");
  // Supplied longer than LEN says (a stuffed frame is): the extra is ignored.
  uint8_t padded[30];
  std::memset(padded, 0x99, sizeof(padded));
  std::memcpy(padded, poll, poll_len);
  CHECK(H::stamp_link_token(padded, 30, 1, 1, out, sizeof(out)) == 21 && std::memcmp(out, poll, 21) == 0,
        "token: bytes past the frame's own length are ignored");

  // The longest: TX_FRAME_MAX = 64 bytes (LEN 55) is taken, 65 (LEN 56) is not. Line 189.
  uint8_t f64[70];
  std::memset(f64, 0, sizeof(f64));
  f64[0] = 0xF4;
  f64[1] = 0xF5;
  f64[3] = 0x40;
  f64[4] = 55;
  f64[13] = 0x66;
  std::memset(out, 0xEE, sizeof(out));
  CHECK(H::stamp_link_token(f64, 64, 0x01, 0x01, out, 66) == 64, "token: a 64-byte frame is stamped");
  // 0x40 + 55 + 0x66 + 1 + 1 = 0xDF.
  CHECK(out[60] == 0x00 && out[61] == 0xDF && out[62] == 0xF4 && out[63] == 0xFB && out[64] == 0xEE,
        "token: its checksum and end tag are at 60..63, nothing after");
  CHECK(H::stamp_beep(f64, 64, true, out, 66) == 64, "beep: a 64-byte frame is taken");
  f64[4] = 56;
  CHECK(
      H::stamp_link_token(f64, 70, 1, 1, out, sizeof(out)) == 0 && H::stamp_beep(f64, 70, true, out, sizeof(out)) == 0,
      "a 65-byte frame is refused");
  // Output room: the frame plus two, as for every builder.
  CHECK(H::stamp_link_token(poll, poll_len, 1, 1, out, 22) == 0 &&
            H::stamp_link_token(poll, poll_len, 1, 1, out, 23) == 21,
        "token: 21 bytes need room for 23");
  CHECK(H::stamp_beep(poll, poll_len, true, out, 22) == 0 && H::stamp_beep(poll, poll_len, true, out, 23) == 21,
        "beep: the same");

  // Stamping the poll. It already carries 01 01 (capture), so stamping those is the identity.
  CHECK(H::stamp_link_token(poll, poll_len, 0x01, 0x01, out, sizeof(out)) == 21 && std::memcmp(out, poll, 21) == 0,
        "token 01 01 on the poll gives the poll back");
  // 0x01B3 - 2 + 0x02 + 0x03 = 0x01B6.
  CHECK(H::stamp_link_token(poll, poll_len, 0x02, 0x03, out, sizeof(out)) == 21 && out[7] == 0x02 && out[8] == 0x03 &&
            out[17] == 0x01 && out[18] == 0xB6,
        "token 02 03: checksum 01 B6");
  // 0x01B3 - 2 + 0x40 + 0x03 = 0x01F4: the low byte is doubled, 22 bytes.
  std::memset(out, 0xEE, sizeof(out));
  CHECK(H::stamp_link_token(poll, poll_len, 0x40, 0x03, out, sizeof(out)) == 22 && out[17] == 0x01 && out[18] == 0xF4 &&
            out[19] == 0xF4 && out[20] == 0xF4 && out[21] == 0xFB && out[22] == 0xEE,
        "token 40 03: checksum 01 F4 goes out as 01 F4 F4, then F4 FB");
  // And that stuffed frame can be stamped again: its LEN still says 21.
  uint8_t stuffed[22];
  std::memcpy(stuffed, out, 22);
  CHECK(H::stamp_link_token(stuffed, 22, 0x01, 0x01, out, sizeof(out)) == 21 && std::memcmp(out, poll, 21) == 0,
        "a stuffed frame is re-stamped from its own LEN");
  // 0x01B3 - 2 + 0xFF + 0xFF = 0x03AF: the carry reaches the high byte.
  CHECK(H::stamp_link_token(poll, poll_len, 0xFF, 0xFF, out, sizeof(out)) == 21 && out[17] == 0x03 && out[18] == 0xAF,
        "token FF FF: checksum 03 AF");

  // stamp_beep: only a 0x65 frame of full length has a buzzer bit (byte 23 bit 2, header).
  uint8_t cmd[H::CMD_FRAME_MAX];
  size_t n = H::build_command(H::AcCommand{}, cmd, sizeof(cmd));
  CHECK(n == 50 && cmd[23] == 0x04, "(a command frame carries the bit set)");
  CHECK(H::stamp_beep(cmd, n, true, out, sizeof(out)) == 50 && std::memcmp(out, cmd, 50) == 0, "beep on: unchanged");
  // Checksum 0x029A (the default command) less 4.
  CHECK(H::stamp_beep(cmd, n, false, out, sizeof(out)) == 50 && out[23] == 0x00 && out[46] == 0x02 && out[47] == 0x96 &&
            H::status_checksum_ok(out, 50),
        "beep off: byte 23 = 0x00, checksum 02 96");
  int moved = 0;
  for (size_t i = 0; i < 46; i++)
    moved += out[i] != cmd[i];
  CHECK(moved == 1, "and no other byte moves");
  uint8_t silent[H::CMD_FRAME_MAX];
  std::memcpy(silent, out, 50);
  CHECK(H::stamp_beep(silent, 50, false, out, sizeof(out)) == 50 && std::memcmp(out, silent, 50) == 0,
        "off twice is off");
  CHECK(H::stamp_beep(silent, 50, true, out, sizeof(out)) == 50 && std::memcmp(out, cmd, 50) == 0,
        "and on again restores it");
  // Only bit 2 of byte 23 is touched.
  uint8_t odd[50];
  std::memcpy(odd, cmd, 50);
  odd[23] = 0xFF;
  CHECK(H::stamp_beep(odd, 50, false, out, sizeof(out)) == 50 && out[23] == 0xFB, "the other bits of byte 23 are kept");
  odd[23] = 0xFB;
  CHECK(H::stamp_beep(odd, 50, true, out, sizeof(out)) == 50 && out[23] == 0xFF, "both ways");
  // The literal power-off frame (capture, checksum 02 31) with the buzzer cleared: 02 2D.
  uint8_t off[50];
  H::build_power_frame(false, off, sizeof(off));
  CHECK(H::stamp_beep(off, 50, false, out, sizeof(out)) == 50 && out[23] == 0x00 && out[46] == 0x02 && out[47] == 0x2D,
        "power off, silent: checksum 02 2D");
  // A frame of another class is copied through whatever the flag: the poll's byte 23 does not
  // exist, and the heartbeat's is data.
  CHECK(H::stamp_beep(poll, poll_len, false, out, sizeof(out)) == 21 && std::memcmp(out, poll, 21) == 0,
        "the poll passes through");
  CHECK(H::stamp_beep(H::LINK_HEARTBEAT, 28, false, out, sizeof(out)) == 28 &&
            std::memcmp(out, H::LINK_HEARTBEAT, 28) == 0,
        "the heartbeat passes through");
  // A 0x65 frame shorter than a command (line 210, second half) passes through too: 28 bytes.
  uint8_t short65[28];
  std::memcpy(short65, H::LINK_HEARTBEAT, 28);
  short65[13] = 0x65;
  short65[23] = 0x04;
  short65[24] = 0x03;  // 0x0302 - 0x1E + 0x65 + 0x04 = 0x034D
  short65[25] = 0x4D;
  CHECK(H::status_checksum_ok(short65, 28), "(the 28-byte class 0x65 frame is well formed)");
  CHECK(H::stamp_beep(short65, 28, false, out, sizeof(out)) == 28 && std::memcmp(out, short65, 28) == 0,
        "a class 0x65 frame shorter than 50 bytes keeps its byte 23");
  // Clearing the bit can land the checksum on 0xF4: 0x01D4 + 0x24 = 0x01F8, less 4.
  uint8_t pre[H::CMD_FRAME_MAX];
  CHECK(H::build_single_field(20, 0x24, pre, sizeof(pre)) == 50 && pre[47] == 0xF8, "(a frame summing to 01 F8)");
  std::memset(out, 0xEE, sizeof(out));
  CHECK(H::stamp_beep(pre, 50, false, out, sizeof(out)) == 51 && out[46] == 0x01 && out[47] == 0xF4 &&
            out[48] == 0xF4 && out[49] == 0xF4 && out[50] == 0xFB && out[51] == 0xEE,
        "silenced, its checksum 01 F4 is stuffed: 51 bytes");
  // And a stuffed command going in comes out right.
  uint8_t in51[H::CMD_FRAME_MAX];
  CHECK(H::build_single_field(20, 0x20, in51, sizeof(in51)) == 51, "(a stuffed 51-byte command)");
  CHECK(H::stamp_beep(in51, 51, true, out, sizeof(out)) == 51 && std::memcmp(out, in51, 51) == 0,
        "a stuffed command with the bit already set comes back as it was");
  CHECK(H::stamp_beep(in51, 51, false, out, sizeof(out)) == 50 && out[46] == 0x01 && out[47] == 0xF0,
        "and silenced it is 50 bytes, checksum 01 F0");
}

// ---- Parsers -----------------------------------------------------------------------------------
static void test_parse_status() {
  printf("-- status parser: arguments, the length gates, each field's bit\n");
  H::AcState st;
  Bytes good = frame(160, 0x66);
  CHECK(H::parse_status(good.data(), good.size(), &st) && st.valid, "(a 160-byte frame of zeros parses)");
  // Line 437: each pointer on its own.
  CHECK(!H::parse_status(nullptr, 160, &st), "no frame");
  CHECK(!H::parse_status(good.data(), good.size(), nullptr), "nowhere to put it");

  // Line 439: the LEN byte must say exactly the length handed in. The checksum is made to agree
  // with the wrong LEN, so only that test can refuse the frame.
  for (int delta : {-1, 1}) {
    Bytes f = frame(160, 0x66);
    f[4] = (uint8_t) (f[4] + delta);
    seal(&f);
    CHECK(H::status_checksum_ok(f.data(), f.size()), "(LEN off by %d, checksum valid)", delta);
    CHECK(!H::parse_status(f.data(), f.size(), &st), "a LEN byte off by %d is refused", delta);
  }
  // The start and end bytes, one either side of each.
  for (size_t at : {(size_t) 0, (size_t) 1, (size_t) 158, (size_t) 159}) {
    for (int delta : {-1, 1}) {
      Bytes f = good;
      f[at] = (uint8_t) (f[at] + delta);
      CHECK(!H::parse_status(f.data(), f.size(), &st), "framing byte %zu off by %d is refused", at, delta);
    }
  }
  // The checksum bytes, each alone. Give the frame a sum with two different bytes first.
  Bytes summed = frame(160, 0x66);
  summed[60] = 0xFF;
  summed[61] = 0xFF;
  seal(&summed);
  CHECK(summed[156] == 0x03 && summed[157] == 0x3C && H::parse_status(summed.data(), 160, &st),
        "(a frame summing to 03 3C)");
  for (size_t at : {(size_t) 156, (size_t) 157}) {
    Bytes f = summed;
    f[at] ^= 0x01;
    CHECK(!H::parse_status(f.data(), f.size(), &st), "checksum byte %zu wrong is refused", at);
  }
  std::swap(summed[156], summed[157]);
  CHECK(!H::parse_status(summed.data(), 160, &st), "the checksum is high byte first");

  // The shortest status is 49 bytes: byte 44 (outdoor) is the last one always read, and it has to
  // lie in front of the 4-byte trailer. Then byte 45 (coil), 50 (voltage) and 55 (the power proxy)
  // each appear when the frame is long enough for them to be payload.
  for (size_t len : {(size_t) 44, (size_t) 45, (size_t) 48, (size_t) 49, (size_t) 50, (size_t) 54, (size_t) 55,
                     (size_t) 59, (size_t) 60}) {
    Bytes f(len, 0);
    f[0] = 0xF4;
    f[1] = 0xF5;
    f[2] = 0x01;
    f[3] = 0x40;
    f[4] = (uint8_t) (len - 9);
    f[13] = 0x66;
    // Put a value in every byte a reading could come from, so a reading taken from the checksum
    // or the end tag would show. Bytes inside the trailer are overwritten by seal().
    for (size_t i = 16; i < len; i++)
      f[i] = (uint8_t) (i + 1);
    seal(&f);
    H::AcState s;
    s.coil_temp_c = 99;
    s.voltage_raw = 99;
    s.current_raw = 99;
    bool ok = H::parse_status(f.data(), len, &s);
    if (len < 49) {
      // 45 to 48 bytes: byte 44 would be a checksum byte or the end tag.
      CHECK(!ok, "%zu bytes: refused", len);
      continue;
    }
    CHECK(ok, "%zu bytes: parsed", len);
    // A reading is taken only from a payload byte, never from the checksum or the end tag.
    CHECK(s.coil_temp_c == (len >= 50 ? 46 : 0), "%zu bytes: coil is %s", len, len >= 50 ? "byte 45" : "0");
    CHECK(s.voltage_raw == (len >= 55 ? 51 : 0), "%zu bytes: voltage is %s", len, len >= 55 ? "byte 50" : "0");
    CHECK(s.current_raw == (len >= 60 ? 56 : 0), "%zu bytes: power proxy is %s", len, len >= 60 ? "byte 55" : "0");
    CHECK(s.outdoor_temp_c == 45 && s.compressor_freq == 43, "%zu bytes: outdoor and compressor", len);
  }

  // Byte 18: direction in bits 0-1, run in bits 2-3, mode in bits 4-6 (RE docs/03, and the
  // t_work_mode record cited in the parser).
  struct {
    uint8_t byte18;
    bool on;
    int mode;
  } b18[] = {{0x00, false, 0}, {0x03, false, 0}, {0x04, true, 0},  {0x08, true, 0},  {0x0C, true, 0},
             {0x10, false, 1}, {0x24, true, 2},  {0x30, false, 3}, {0x40, false, 4}, {0x50, false, 4},
             {0x60, false, 4}, {0x70, false, 7}, {0x80, false, 0}, {0xA4, true, 2},  {0xFF, true, 7}};
  for (const auto &r : b18) {
    Bytes f = frame(160, 0x66);
    f[18] = r.byte18;
    seal(&f);
    CHECK(H::parse_status(f.data(), 160, &st) && st.power_on == r.on && (int) st.mode == r.mode,
          "byte 18 = 0x%02X: %s, mode %d (got %d, %d)", r.byte18, r.on ? "on" : "off", r.mode, st.power_on,
          (int) st.mode);
  }

  // One bit at a time in the flag bytes: each raises its own field and no other.
  struct {
    size_t at;
    uint8_t bit;
    const char *what;
    bool H::AcState::*field;
  } flags[] = {{35, 0x80, "vertical swing", &H::AcState::vswing_on},
               {35, 0x40, "horizontal swing", &H::AcState::hswing_on},
               {35, 0x10, "heater relay", &H::AcState::heat_relay_on},
               {35, 0x04, "eco", &H::AcState::eco_on},
               {35, 0x02, "turbo", &H::AcState::turbo_on},
               {36, 0x04, "quiet", &H::AcState::mute_on},
               {36, 0x80, "purify", &H::AcState::purify_on},
               {26, 0x02, "Fahrenheit", &H::AcState::temp_unit_f}};
  bool H::AcState::*all_flags[] = {&H::AcState::vswing_on, &H::AcState::hswing_on,   &H::AcState::heat_relay_on,
                                   &H::AcState::eco_on,    &H::AcState::turbo_on,    &H::AcState::mute_on,
                                   &H::AcState::purify_on, &H::AcState::temp_unit_f, &H::AcState::power_on,
                                   &H::AcState::sleep_on};
  for (const auto &r : flags) {
    Bytes f = frame(160, 0x66);
    f[r.at] = r.bit;
    f[19] = 77;  // a setpoint valid in either unit's arithmetic
    seal(&f);
    CHECK(H::parse_status(f.data(), 160, &st) && st.*(r.field), "byte %zu bit 0x%02X is %s", r.at, r.bit, r.what);
    int raised = 0;
    for (auto other : all_flags)
      raised += st.*other;
    CHECK(raised == 1, "%s raises nothing else (%d flags set)", r.what, raised);
    // And every other bit of that byte leaves it clear.
    f[r.at] = (uint8_t) ~r.bit;
    seal(&f);
    CHECK(H::parse_status(f.data(), 160, &st) && !(st.*(r.field)), "byte %zu = 0x%02X: no %s", r.at, (uint8_t) ~r.bit,
          r.what);
  }

  // Temperatures. Celsius panel: bytes 19 and 20 are signed. Fahrenheit panel: both converted.
  // Bytes 44 and 45 are Celsius in either case.
  Bytes t = frame(160, 0x66);
  t[19] = 24;
  t[20] = 0xFB;  // -5 as int8
  t[44] = 0xEC;  // -20
  t[45] = 33;
  t[42] = 45;
  t[16] = 0x0E;
  t[17] = 6;
  t[50] = 221;
  t[55] = 29;
  seal(&t);
  CHECK(H::parse_status(t.data(), 160, &st) && st.setpoint_c == 24 && st.indoor_temp_c == -5,
        "C panel: 24 and -5 as sent");
  CHECK(st.outdoor_temp_c == -20 && st.coil_temp_c == 33 && st.compressor_freq == 45 && st.fan_raw == 0x0E,
        "outdoor, coil, compressor, fan");
  CHECK(st.sleep_raw == 6 && st.sleep_on && st.voltage_raw == 221 && st.current_raw == 29,
        "sleep, voltage, power proxy");
  t[26] = 0x02;
  t[19] = 77;  // 25 C
  t[20] = 68;  // 20 C
  seal(&t);
  CHECK(H::parse_status(t.data(), 160, &st) && st.temp_unit_f && st.setpoint_c == 25 && st.indoor_temp_c == 20,
        "F panel: 77 F and 68 F read as 25 C and 20 C");
  CHECK(st.outdoor_temp_c == -20 && st.coil_temp_c == 33, "outdoor and coil are not converted");
  t[17] = 0;
  seal(&t);
  CHECK(H::parse_status(t.data(), 160, &st) && !st.sleep_on && st.sleep_raw == 0, "sleep byte 0 is off");
}

static void test_parse_features_and_faults() {
  printf("-- product type and fault parsers: arguments, length gates, stale output\n");
  H::AcFeatures ft;
  // Lines 495-496, never run: each pointer on its own.
  Bytes pt = frame(60, 0x66);
  pt[14] = 0x40;
  seal(&pt);
  CHECK(!H::parse_features(nullptr, 60, &ft), "features: no frame");
  CHECK(!H::parse_features(pt.data(), 60, nullptr), "features: nowhere to put it");
  CHECK(H::parse_features(pt.data(), 60, &ft) && ft.valid && ft.ext_valid && ft.reply_len == 60, "features: 60 bytes");
  // Byte 35 is the last byte of the basic tier and has to lie in front of the 4-byte trailer: 40
  // bytes is the shortest reply. The extended tier ends at byte 39, so it needs 44. The units
  // measured answer with 45 bytes (RE docs/11 section 5.1).
  CHECK(!H::parse_features(pt.data(), 35, &ft), "features: 35 bytes is too short");
  CHECK(!H::parse_features(pt.data(), 39, &ft), "features: 39 bytes would read byte 35 out of the trailer");
  CHECK(H::parse_features(pt.data(), 40, &ft) && !ft.ext_valid && ft.reply_len == 40,
        "features: 40 bytes, no extended tier");
  CHECK(H::parse_features(pt.data(), 43, &ft) && !ft.ext_valid, "features: 43 bytes, still none");
  CHECK(H::parse_features(pt.data(), 44, &ft) && ft.ext_valid, "features: 44 bytes holds byte 39 as payload");
  CHECK(H::parse_features(pt.data(), 45, &ft) && ft.ext_valid && ft.reply_len == 45, "features: the measured 45 bytes");
  // Class and subtype, one either side.
  for (size_t at : {(size_t) 13, (size_t) 14}) {
    for (int delta : {-1, 1}) {
      Bytes f = pt;
      f[at] = (uint8_t) (f[at] + delta);
      CHECK(!H::parse_features(f.data(), 60, &ft), "features: byte %zu off by %d is not a product type reply", at,
            delta);
    }
  }
  // The extended bits are read only when the tier is there.
  Bytes ext = pt;
  ext[38] = 0x08;
  ext[39] = 0x44;
  CHECK(H::parse_features(ext.data(), 60, &ft) && ft.q_display && ft.enable_8heat && ft.trans_102_64,
        "extended bits read");
  CHECK(H::parse_features(ext.data(), 43, &ft) && !ft.q_display && !ft.enable_8heat && !ft.trans_102_64,
        "and not read from a 43-byte reply");
  // Line 515: reply_len is a byte, clamped. The assembler never hands over more than 200.
  Bytes big(300, 0);
  big[13] = 0x66;
  big[14] = 0x40;
  for (size_t len : {(size_t) 254, (size_t) 255, (size_t) 256, (size_t) 257, (size_t) 300}) {
    CHECK(H::parse_features(big.data(), len, &ft) && ft.reply_len == (len > 255 ? 255 : len),
          "features: reply_len for %zu bytes is %zu (got %u)", len, len > 255 ? (size_t) 255 : len, ft.reply_len);
  }

  // parse_faults. Lines 523-525.
  H::AcFaults fl;
  Bytes fb(160, 0);
  CHECK(!H::parse_faults(nullptr, 160, &fl), "faults: no frame");
  CHECK(!H::parse_faults(fb.data(), 160, nullptr), "faults: nowhere to put it");
  // A frame too short for the first fault byte clears what was there (line 525).
  fl.valid = true;
  fl.any = true;
  fl.raw_indoor = 0xFF;
  fl.in_temp = true;
  CHECK(!H::parse_faults(fb.data(), 43, &fl) && !fl.valid && !fl.any && fl.raw_indoor == 0 && !fl.in_temp,
        "faults: a 43-byte frame is refused and the output cleared");
  // Each byte group appears when the byte that holds it (39, 40, 64, 66: header) is payload, 4
  // bytes in front of the end of the frame.
  fb[39] = 0x01;
  fb[40] = 0x08;
  fb[64] = 0x08;
  fb[66] = 0x10;
  struct {
    size_t len;
    bool module, outdoor, protect;
  } gates[] = {{44, false, false, false}, {45, true, false, false}, {68, true, false, false},
               {69, true, true, false},   {70, true, true, false},  {71, true, true, true}};
  for (const auto &g : gates) {
    // Stale values from a longer frame must not survive a shorter one.
    fl = H::AcFaults{};
    fl.raw_module = 0xAA;
    fl.raw_outdoor = 0xAA;
    fl.raw_protect = 0xAA;
    fl.in_eeprom = fl.out_temp = fl.over_temp = true;
    CHECK(H::parse_faults(fb.data(), g.len, &fl) && fl.valid && fl.raw_indoor == 0x01 && fl.in_com,
          "faults: %zu bytes parse", g.len);
    CHECK(fl.raw_module == (g.module ? 0x08 : 0) && fl.in_eeprom == g.module, "faults: %zu bytes, module byte %s",
          g.len, g.module ? "read" : "absent and cleared");
    CHECK(fl.raw_outdoor == (g.outdoor ? 0x08 : 0) && fl.out_temp == g.outdoor, "faults: %zu bytes, outdoor byte %s",
          g.len, g.outdoor ? "read" : "absent and cleared");
    CHECK(fl.raw_protect == (g.protect ? 0x10 : 0) && fl.over_temp == g.protect,
          "faults: %zu bytes, protection byte %s", g.len, g.protect ? "read" : "absent and cleared");
  }
  // `any` is the OR of the four raw bytes, less the frost-guard flag (byte 66 bit 7, header).
  // Line 558: each byte alone must raise it, so that an `&` in place of an `|` shows.
  struct {
    size_t at;
    uint8_t value;
    bool any;
  } any_rows[] = {{39, 0x01, true}, {40, 0x01, true}, {64, 0x01, true}, {66, 0x01, true}, {66, 0x80, false},
                  {66, 0x90, true}, {64, 0x80, true}, {40, 0x04, true}, {39, 0x00, false}};
  for (const auto &r : any_rows) {
    Bytes f(160, 0);
    f[r.at] = r.value;
    CHECK(H::parse_faults(f.data(), 160, &fl) && fl.any == r.any, "faults: byte %zu = 0x%02X alone: any is %d", r.at,
          r.value, r.any);
  }
  // An outdoor fault with the frost-guard flag also set is still a fault.
  Bytes both(160, 0);
  both[64] = 0x08;
  both[66] = 0x80;
  CHECK(H::parse_faults(both.data(), 160, &fl) && fl.any && fl.out_temp && !fl.over_temp,
        "outdoor fault under frost guard");

  // devtype_from_reply (lines 567-571): the pair at frame 16/17 of a class 0x0A reply.
  Bytes dt(24, 0);
  dt[13] = 0x0A;
  dt[16] = 0x12;
  dt[17] = 0x34;
  uint8_t hi = 0xEE, lo = 0xEE;
  CHECK(!H::devtype_from_reply(nullptr, 24, &hi, &lo), "device type: no reply");
  CHECK(!H::devtype_from_reply(dt.data(), 17, &hi, &lo) && hi == 0xEE && lo == 0xEE,
        "device type: 17 bytes, outputs untouched");
  // Bytes 16/17 are payload only from 22 bytes on: below that they are the checksum or the tag.
  for (size_t n = 14; n < 22; n++) {
    Bytes exact(dt.begin(), dt.begin() + (long) n);
    hi = lo = 0xEE;
    CHECK(!H::devtype_from_reply(exact.data(), n, &hi, &lo) && hi == 0xEE && lo == 0xEE,
          "device type: %zu bytes is too short", n);
  }
  CHECK(H::devtype_from_reply(dt.data(), 22, &hi, &lo) && hi == 0x12 && lo == 0x34,
        "device type: 22 bytes holds bytes 16/17 as payload");
  CHECK(H::devtype_from_reply(dt.data(), 22, nullptr, nullptr), "device type: both outputs optional");
  hi = lo = 0xEE;
  CHECK(H::devtype_from_reply(dt.data(), 24, &hi, nullptr) && hi == 0x12, "device type: only the high byte asked");
  CHECK(H::devtype_from_reply(dt.data(), 24, nullptr, &lo) && lo == 0x34, "device type: only the low byte asked");
  for (int delta : {-1, 1}) {
    Bytes f = dt;
    f[13] = (uint8_t) (0x0A + delta);
    hi = 0xEE;
    CHECK(!H::devtype_from_reply(f.data(), 24, &hi, &lo) && hi == 0xEE, "device type: class 0x%02X is not the reply",
          f[13]);
  }

  // devtype_reply_usable: what may be stamped into bytes 7/8 of every later frame. The checksum
  // verdict comes from the caller, which has judged the frame once already.
  Bytes du = frame(24, 0x0A);
  du[16] = 0x36;
  du[17] = 0x05;
  seal(&du);
  hi = lo = 0xEE;
  CHECK(H::devtype_reply_usable(true, du.data(), du.size(), &hi, &lo) && hi == 0x36 && lo == 0x05,
        "a whole DevType reply: usable");
  CHECK(H::devtype_reply_usable(true, du.data(), du.size(), nullptr, nullptr), "usable: both outputs optional");
  hi = lo = 0xEE;
  CHECK(H::devtype_reply_usable(true, du.data(), du.size(), &hi, nullptr) && hi == 0x36 &&
            H::devtype_reply_usable(true, du.data(), du.size(), nullptr, &lo) && lo == 0x05,
        "usable: each output alone");
  hi = lo = 0xEE;
  CHECK(!H::devtype_reply_usable(false, du.data(), du.size(), &hi, &lo) && hi == 0xEE && lo == 0xEE,
        "a failed checksum: not usable, outputs untouched");
  CHECK(!H::devtype_reply_usable(true, du.data(), 21, &hi, &lo) && hi == 0xEE,
        "too short for bytes 16/17 to be payload: not usable");
  Bytes other = du;
  other[13] = 0x66;
  CHECK(!H::devtype_reply_usable(true, other.data(), other.size(), &hi, &lo) && hi == 0xEE,
        "another class: not usable");
  // 00 00 is "not linked yet" to the stock module. Either byte alone non-zero is a type.
  struct {
    uint8_t type, sub;
    bool usable;
  } pairs[] = {{0x00, 0x00, false}, {0x00, 0x01, true}, {0x01, 0x00, true}, {0x01, 0x01, true}, {0xFF, 0xFF, true}};
  for (const auto &pr : pairs) {
    du[16] = pr.type;
    du[17] = pr.sub;
    hi = lo = 0xEE;
    bool ok = H::devtype_reply_usable(true, du.data(), du.size(), &hi, &lo);
    CHECK(ok == pr.usable && hi == (ok ? pr.type : 0xEE) && lo == (ok ? pr.sub : 0xEE), "device type %02X %02X is %s",
          pr.type, pr.sub, pr.usable ? "adopted" : "never adopted");
  }

  // Bitmaps: one fault at a time lands on its FAULT1_ bit (binary_sensor.py reads these).
  bool H::AcFaults::*fault_fields[] = {
      &H::AcFaults::in_temp,      &H::AcFaults::in_coil_temp, &H::AcFaults::in_humidity,   &H::AcFaults::water_full,
      &H::AcFaults::in_fan_motor, &H::AcFaults::grille,       &H::AcFaults::in_vzero,      &H::AcFaults::in_com,
      &H::AcFaults::in_display,   &H::AcFaults::in_keys,      &H::AcFaults::in_wifi,       &H::AcFaults::in_ele,
      &H::AcFaults::in_eeprom,    &H::AcFaults::out_eeprom,   &H::AcFaults::out_coil_temp, &H::AcFaults::out_gas_temp,
      &H::AcFaults::out_temp,     &H::AcFaults::over_temp};
  for (size_t i = 0; i < 18; i++) {
    H::AcFaults one;
    one.*(fault_fields[i]) = true;
    CHECK(H::faults_to_bitmap32(one) == (1u << i), "fault %zu alone is bit %zu", i, i);
  }
  H::AcFaults flagged;
  flagged.any = true;
  CHECK(H::faults_to_bitmap32(flagged) == (1u << 30), "any is bit 30");
  flagged = H::AcFaults{};
  flagged.valid = true;
  CHECK(H::faults_to_bitmap32(flagged) == (1u << 31), "valid is bit 31");
  flagged.raw_indoor = flagged.raw_module = flagged.raw_outdoor = flagged.raw_protect = 0xFF;
  CHECK(H::faults_to_bitmap32(flagged) == (1u << 31), "the raw bytes are not in the bitmap");

  // Feature bitmap: the two-bit fields are masked, and the extended bits need ext_valid.
  H::AcFeatures f;
  f.power_display = 0xFF;
  CHECK(H::features_to_bitmap32(f) == (3u << 16), "power_display keeps two bits, at 16");
  f = H::AcFeatures{};
  f.demand_resp = 0xFF;
  CHECK(H::features_to_bitmap32(f) == (3u << 18), "demand_resp keeps two bits, at 18");
  f = H::AcFeatures{};
  f.q_display = f.enable_8heat = f.trans_102_64 = true;
  CHECK(H::features_to_bitmap32(f) == 0, "extended bits without ext_valid are not published");
  f.ext_valid = true;
  CHECK(H::features_to_bitmap32(f) == ((1u << 30) | (1u << 10) | (1u << 11) | (1u << 12)),
        "with it: bits 10, 11, 12 and 30");
  f = H::AcFeatures{};
  f.reply_len = 0xFF;
  CHECK(H::features_to_bitmap32(f) == 0, "reply_len is not in the bitmap");
  // Every field alone lands on its own bit, and no bit outside the documented ones is ever set.
  // (binary_sensor.py reads these indices.)
  bool H::AcFeatures::*flag_fields[] = {
      &H::AcFeatures::cool_heat, &H::AcFeatures::ai,          &H::AcFeatures::infinite_fan, &H::AcFeatures::power_save,
      &H::AcFeatures::fan_mute,  &H::AcFeatures::swing_dir_8, &H::AcFeatures::swing_follow, &H::AcFeatures::humidity,
      &H::AcFeatures::heat_8c,   &H::AcFeatures::purify};
  for (int bit = 0; bit < 10; bit++) {
    H::AcFeatures one;
    one.*(flag_fields[bit]) = true;
    CHECK(H::features_to_bitmap32(one) == (1u << bit), "feature field %d alone is bit %d", bit, bit);
  }
  H::AcFeatures all;
  for (auto field : flag_fields)
    all.*field = true;
  all.valid = all.ext_valid = all.q_display = all.enable_8heat = all.trans_102_64 = true;
  all.power_display = 0xFF;
  all.demand_resp = 0xFF;
  all.reply_len = 0xFF;
  CHECK(H::features_to_bitmap32(all) == 0xC00F1FFFu, "everything set: bits 0-12, 16-19, 30, 31 (got 0x%08X)",
        (unsigned) H::features_to_bitmap32(all));
}

// ---- Setup request ("77") ----------------------------------------------------------------------
static void test_setup_request() {
  printf("-- setup request: the shortest reply, the debounce count, the lockout to the millisecond\n");
  uint8_t bits = 0xEE;
  // link_request_bits (line 369): a 0x1E reply of at least 22 bytes (flags at 17, then checksum
  // and end tag) with a valid checksum. Bits 3 and 5 of byte 17 (header, RE docs/03 "77").
  Bytes r22 = frame(22, 0x1E);
  r22[17] = 0xFF;
  seal(&r22);
  CHECK(H::link_request_bits(r22.data(), 22, &bits) && bits == 0x28, "22 bytes: bits 3 and 5 of byte 17, nothing else");
  bits = 0xEE;
  CHECK(!H::link_request_bits(nullptr, 22, &bits) && bits == 0xEE, "no frame");
  CHECK(!H::link_request_bits(r22.data(), 22, nullptr), "nowhere to put the bits");
  // 21 bytes with a valid checksum of its own: only the length test refuses it. Byte 17 is then
  // the checksum's first byte.
  Bytes r21 = frame(21, 0x1E);
  CHECK(H::status_checksum_ok(r21.data(), 21) && !H::link_request_bits(r21.data(), 21, &bits) && bits == 0xEE,
        "21 bytes is too short, though its checksum is right");
  for (int delta : {-1, 1}) {
    Bytes f = r22;
    f[13] = (uint8_t) (0x1E + delta);
    seal(&f);
    CHECK(!H::link_request_bits(f.data(), 22, &bits) && bits == 0xEE, "class 0x%02X is not a link reply", f[13]);
  }
  Bytes bad = r22;
  bad[19] ^= 0x01;
  CHECK(!H::link_request_bits(bad.data(), 22, &bits) && bits == 0xEE, "a bad checksum does not count");
  for (int value : {0x00, 0x08, 0x20, 0x28, 0xD7}) {
    Bytes f = frame(28, 0x1E);
    f[17] = (uint8_t) value;
    seal(&f);
    CHECK(H::link_request_bits(f.data(), 28, &bits) && bits == (value & 0x28), "byte 17 = 0x%02X gives 0x%02X", value,
          value & 0x28);
  }

  // setup_request_debounce (line 381): the streak, the latch, the end.
  uint8_t streak = 0;
  bool latched = false;
  CHECK(H::setup_request_debounce(0x08, &streak, &latched, 3) == 0 && streak == 1 && !latched, "first of three");
  CHECK(H::setup_request_debounce(0x08, &streak, &latched, 3) == 0 && streak == 2 && !latched, "second");
  CHECK(H::setup_request_debounce(0x08, &streak, &latched, 3) == 1 && streak == 3 && latched, "third: taken, once");
  CHECK(H::setup_request_debounce(0x08, &streak, &latched, 3) == 0 && streak == 4 && latched, "fourth: already taken");
  // The streak stops at 255 instead of wrapping to 0 and counting up to a second +1.
  int fired = 0;
  for (int i = 0; i < 600; i++)
    fired += H::setup_request_debounce(0x08, &streak, &latched, 3) == 1;
  CHECK(fired == 0 && streak == 0xFF && latched,
        "held for ten minutes: never taken a second time, streak stays at 255");
  CHECK(H::setup_request_debounce(0, &streak, &latched, 3) == -1 && streak == 0 && !latched, "gone: reported once");
  CHECK(H::setup_request_debounce(0, &streak, &latched, 3) == 0, "and not again");
  // One short of the hold and gone: never reported at all.
  H::setup_request_debounce(0x08, &streak, &latched, 3);
  H::setup_request_debounce(0x08, &streak, &latched, 3);
  CHECK(H::setup_request_debounce(0, &streak, &latched, 3) == 0 && streak == 0 && !latched,
        "two replies then none: nothing");
  // A hold of one takes the first reply; of none, too (the streak is past it at once).
  CHECK(H::setup_request_debounce(0x20, &streak, &latched, 1) == 1, "hold 1: the first reply");
  streak = 0;
  latched = false;
  CHECK(H::setup_request_debounce(0x20, &streak, &latched, 0) == 1, "hold 0: the first reply");
  // A hold of 255 is reached exactly where the streak saturates.
  streak = 0;
  latched = false;
  fired = 0;
  int at = 0;
  for (int i = 1; i <= 300; i++) {
    if (H::setup_request_debounce(0x08, &streak, &latched, 255) == 1) {
      fired++;
      at = i;
    }
  }
  CHECK(fired == 1 && at == 255, "hold 255: taken at the 255th reply (at %d, %d times)", at, fired);

  // smartcfg_lockout_mask (line 395): bit 5 is removed for SETUP_REQUEST_LOCKOUT_MS = 10 s from
  // `since`. The start is not 0 here, so that elapsed time and remainder differ.
  CHECK(H::SETUP_REQUEST_LOCKOUT_MS == 10000 && H::SETUP_REQUEST_HOLD_FRAMES == 3, "10 s lockout, 3 replies");
  CHECK(H::smartcfg_lockout_mask(0x28, 25000, true, 20000) == 0x08, "5 s in: bit 5 removed, bit 3 kept");
  CHECK(H::smartcfg_lockout_mask(0x28, 29999, true, 20000) == 0x08, "9.999 s in: still removed");
  CHECK(H::smartcfg_lockout_mask(0x28, 30000, true, 20000) == 0x28, "10 s in: back");
  CHECK(H::smartcfg_lockout_mask(0x28, 45000, true, 20000) == 0x28, "25 s in: back (45000 mod 20000 is under 10 s)");
  CHECK(H::smartcfg_lockout_mask(0x28, 25000, false, 20000) == 0x28, "not armed: never removed");
  CHECK(H::smartcfg_lockout_mask(0x20, 20000, true, 20000) == 0x00, "at the instant it starts");
  CHECK(H::smartcfg_lockout_mask(0x20, 4000, true, 0xFFFFF000u) == 0x00, "across the clock wrap: 8.096 s in");
  CHECK(H::smartcfg_lockout_mask(0x20, 6000, true, 0xFFFFF000u) == 0x20, "across the wrap: 10.096 s in");

  // setup_request_lockout sets both fields.
  H::SetupRequestTracker tr;
  CHECK(!tr.lockout_armed && tr.lockout_since == 0 && !tr.reconfig_quiet && tr.streak == 0 && !tr.latched &&
            !tr.smartcfg_seen && !tr.smartcfg_live && !tr.taken,
        "a new tracker is the boot state");
  H::setup_request_lockout(&tr, 1000);
  CHECK(tr.lockout_armed && tr.lockout_since == 1000, "the lockout is armed from the time given");

  // setup_request_step, line 424: an expired lockout is disarmed at exactly 10 s, not after.
  CHECK(H::setup_request_step(&tr, 0, false, 10999) == 0 && tr.lockout_armed, "9.999 s: still armed");
  CHECK(H::setup_request_step(&tr, 0, false, 11000) == 0 && !tr.lockout_armed, "10 s: disarmed");
  H::setup_request_lockout(&tr, 1000);
  CHECK(H::setup_request_step(&tr, 0, false, 11001) == 0 && !tr.lockout_armed, "10.001 s: disarmed too");
  H::setup_request_lockout(&tr, 20000);
  CHECK(H::setup_request_step(&tr, 0, false, 45000) == 0 && !tr.lockout_armed, "25 s after a start at 20 s: disarmed");

  // Bit 5 alone is taken from one reply, and that starts the lockout by itself.
  H::SetupRequestTracker a;
  CHECK(H::setup_request_step(&a, 0x20, false, 50000) == 0x20, "bit 5, one reply: taken");
  CHECK(a.lockout_armed && a.lockout_since == 50000, "the lockout runs from then");
  CHECK(H::setup_request_step(&a, 0x20, false, 51000) == 0, "the same request still standing: not again");
  // Line 429: the reply that ends a request does not restart the lockout.
  CHECK(H::setup_request_step(&a, 0, false, 52000) == 0 && a.lockout_armed && a.lockout_since == 50000,
        "the request going away leaves the lockout where it was");
  CHECK(H::setup_request_step(&a, 0x20, false, 59999) == 0, "9.999 s after it was taken: bit 5 still ignored");
  CHECK(H::setup_request_step(&a, 0, false, 59999) == 0, "(reply without it)");
  CHECK(H::setup_request_step(&a, 0x20, false, 60000) == 0x20, "10 s after: taken again");

  // Line 411: while the module's own flag is on the wire, bit 3 is never "quiet", whatever the
  // reply carries. A reply without bit 3 seen then must not open the gate.
  H::SetupRequestTracker q;
  CHECK(H::setup_request_step(&q, 0, true, 1000) == 0 && !q.reconfig_quiet,
        "indicating, reply without bit 3: gate stays shut");
  for (int i = 0; i < 5; i++)
    CHECK(H::setup_request_step(&q, 0x08, false, 2000 + i * 1000) == 0, "bit 3 right after: not a request (reply %d)",
          i);
  CHECK(H::setup_request_step(&q, 0, false, 8000) == 0 && q.reconfig_quiet,
        "a reply without it, flag off the wire: gate opens");
  CHECK(H::setup_request_step(&q, 0x08, false, 9000) == 0 && H::setup_request_step(&q, 0x08, false, 10000) == 0,
        "bit 3 now counts: first and second reply");
  CHECK(H::setup_request_step(&q, 0x08, false, 11000) == 0x08, "third reply: taken");
  CHECK(H::setup_request_step(&q, 0x08, true, 12000) == 0 && !q.reconfig_quiet, "indicating again: the gate shuts");

  // The count of bit 3 is its own: bit 5 arriving on top does not start it again. Two replies of
  // bit 3, then bits 3 and 5 together: that is the third reply with bit 3, and both are reported.
  H::SetupRequestTracker k;
  H::setup_request_step(&k, 0, false, 1000);
  CHECK(H::setup_request_step(&k, 0x08, false, 2000) == 0 && H::setup_request_step(&k, 0x08, false, 3000) == 0,
        "two replies of bit 3");
  CHECK(H::setup_request_step(&k, 0x28, false, 4000) == 0x28 && k.streak == 3 && k.latched && k.taken,
        "then bits 3 and 5: the third reply with bit 3, taken with both bits");
  CHECK(H::setup_request_step(&k, 0x28, false, 5000) == 0 && H::setup_request_step(&k, 0x08, false, 6000) == 0 &&
            H::setup_request_step(&k, 0x20, false, 7000) == 0,
        "and nothing more while the unit keeps asking, in whatever form");
  CHECK(H::setup_request_step(&k, 0, false, 8000) == 0 && !k.taken && k.streak == 0 && !k.latched && !k.smartcfg_seen,
        "a reply that asks nothing ends the run");
  // Bit 5 with a bit 3 that counts waits for bit 3's hold, and is reported with it only when it
  // is still there then.
  H::SetupRequestTracker w;
  H::setup_request_step(&w, 0, false, 1000);
  CHECK(H::setup_request_step(&w, 0x28, false, 2000) == 0 && w.smartcfg_live && !w.taken, "both bits: bit 5 waits");
  CHECK(H::setup_request_step(&w, 0x28, false, 3000) == 0, "second");
  CHECK(H::setup_request_step(&w, 0x28, false, 4000) == 0x28, "third: both");
  H::SetupRequestTracker w2;
  H::setup_request_step(&w2, 0, false, 1000);
  H::setup_request_step(&w2, 0x28, false, 2000);
  CHECK(H::setup_request_step(&w2, 0x20, false, 3000) == 0x20,
        "bit 3 gone again after one reply: bit 5 is taken alone");
  // Bit 5 taken, then bit 3 held with no quiet reply between: the same run, no second request.
  H::SetupRequestTracker d;
  H::setup_request_step(&d, 0, false, 1000);
  CHECK(H::setup_request_step(&d, 0x20, false, 2000) == 0x20 && d.taken, "bit 5 taken");
  CHECK(H::setup_request_step(&d, 0x08, false, 3000) == 0 && d.streak == 1 && !d.latched,
        "bit 3 next: counted from one");
  CHECK(H::setup_request_step(&d, 0x08, false, 4000) == 0, "second");
  CHECK(H::setup_request_step(&d, 0x08, false, 5000) == 0 && d.streak == 3 && d.latched,
        "third: its hold is complete, but the run has had its request");
  // Bit 5 that appears inside the lockout is spent, also when it outlives the lockout.
  H::SetupRequestTracker s;
  H::setup_request_lockout(&s, 1000);
  CHECK(H::setup_request_step(&s, 0x20, false, 10999) == 0 && s.smartcfg_seen && !s.smartcfg_live,
        "bit 5 at 9.999 s: seen, not live");
  CHECK(H::setup_request_step(&s, 0x20, false, 11000) == 0 && H::setup_request_step(&s, 0x20, false, 60000) == 0,
        "still up when the lockout is over: not taken");
  CHECK(H::setup_request_step(&s, 0, false, 61000) == 0 && !s.smartcfg_seen, "gone");
  CHECK(H::setup_request_step(&s, 0x20, false, 62000) == 0x20, "a new pulse is taken");
  // Bit 5 that appeared outside the lockout, with the lockout restarted (the indication switched
  // off) before bit 3's hold is complete: bit 3 is reported alone.
  H::SetupRequestTracker x;
  H::setup_request_step(&x, 0, false, 1000);
  H::setup_request_step(&x, 0x28, false, 2000);
  H::setup_request_step(&x, 0x28, false, 3000);
  H::setup_request_lockout(&x, 3500);
  CHECK(H::setup_request_step(&x, 0x28, false, 4000) == 0x08, "bit 5 is held back by the lockout that began meanwhile");
  // Bits outside the mask never reach the tracker's state.
  H::SetupRequestTracker m;
  CHECK(H::setup_request_step(&m, 0xD7, false, 1000) == 0 && !m.smartcfg_seen && !m.taken && m.streak == 0,
        "other bits are no request");

  // setup_request_expire: disarms at exactly 10 s, leaves the rest alone, and is safe to repeat.
  H::SetupRequestTracker e;
  H::setup_request_expire(&e, 5);
  CHECK(!e.lockout_armed && e.lockout_since == 0, "expire on a tracker with no lockout does nothing");
  H::setup_request_lockout(&e, 0xFFFFFF00u);
  H::setup_request_expire(&e, 0xFFFFFF00u + 9999);
  CHECK(e.lockout_armed, "9.999 s, across the wrap: armed");
  H::setup_request_expire(&e, 0xFFFFFF00u + 10000);
  CHECK(!e.lockout_armed && e.lockout_since == 0xFFFFFF00u, "10 s: disarmed");
  H::setup_request_expire(&e, 0xFFFFFF00u + 5);
  CHECK(!e.lockout_armed, "and it stays disarmed whatever the clock says later");

  // setup_request_flag_sent: the gate for bit 3 shuts without a reply having been read.
  H::SetupRequestTracker fs;
  H::setup_request_step(&fs, 0, false, 1000);
  CHECK(fs.reconfig_quiet, "(gate open)");
  H::setup_request_flag_sent(&fs);
  CHECK(!fs.reconfig_quiet, "the flag went out: the gate is shut");
  int fires = 0;
  for (int i = 0; i < 5; i++)
    fires += H::setup_request_step(&fs, 0x08, false, 2000 + i * 1000) != 0;
  CHECK(fires == 0, "the flag handed back for five replies with the indication already off: no request");
  CHECK(H::setup_request_step(&fs, 0, false, 8000) == 0 && fs.reconfig_quiet,
        "a reply without it opens the gate again");

  // setup_request_link_lost: the hold of bit 3 starts again, nothing else changes.
  H::SetupRequestTracker ll;
  H::setup_request_step(&ll, 0, false, 1000);
  H::setup_request_step(&ll, 0x08, false, 2000);
  H::setup_request_step(&ll, 0x08, false, 3000);
  H::setup_request_lockout(&ll, 3000);
  H::setup_request_link_lost(&ll);
  CHECK(ll.streak == 0 && !ll.latched && ll.reconfig_quiet && ll.lockout_armed && ll.lockout_since == 3000,
        "link lost: the count is gone, the gate and the lockout are as they were");
  CHECK(H::setup_request_step(&ll, 0x08, false, 70000) == 0 && H::setup_request_step(&ll, 0x08, false, 71000) == 0,
        "back: first and second reply");
  CHECK(H::setup_request_step(&ll, 0x08, false, 72000) == 0x08, "third: taken");
  H::setup_request_link_lost(&ll);
  CHECK(ll.taken, "a request already taken stays taken across a lost link");
  CHECK(H::setup_request_step(&ll, 0x08, false, 73000) == 0 && H::setup_request_step(&ll, 0x08, false, 74000) == 0 &&
            H::setup_request_step(&ll, 0x08, false, 75000) == 0,
        "and the bit still standing afterwards is not reported again");
}

// ---- Frame assembler ---------------------------------------------------------------------------
static void test_assembler() {
  printf("-- frame assembler: the longest frame, damaged starts and ends, bytes next to 0xF4\n");
  Bytes got;
  Bytes ok20 = frame(20, 0x66);

  // feed() returns 0 for every byte but the last, and then the un-stuffed length (line 598).
  {
    H::FrameAssembler fa;
    Bytes f = frame(30, 0x66);
    f[10] = 0xF4;  // a data byte that is doubled on the wire
    f[11] = 0xF4;
    seal(&f);
    Bytes w = wire(f);
    CHECK(w.size() == 32, "(two doubled bytes: 32 on the wire)");
    for (size_t i = 0; i + 1 < w.size(); i++)
      CHECK(fa.feed(w[i]) == 0, "byte %zu of %zu completes nothing", i, w.size());
    CHECK(fa.feed(w.back()) == 30 && std::memcmp(fa.data(), f.data(), 30) == 0,
          "the last byte completes 30 un-stuffed bytes");
  }
  // The longest frame is RX_FRAME_MAX = 200 bytes (LEN 191). Line 619.
  {
    H::FrameAssembler fa;
    Bytes f = frame(200, 0x66);
    f[100] = 0x5A;
    seal(&f);
    CHECK(feed_all(&fa, wire(f), &got) == 1 && got == f, "a 200-byte frame is assembled whole");
  }
  // LEN 192 says 201 bytes: dropped at the LEN byte, so the frame right behind it is not lost.
  {
    H::FrameAssembler fa;
    Bytes head = {0xF4, 0xF5, 0x01, 0x40, 192};
    CHECK(feed_all(&fa, cat({head, ok20}), &got) == 1 && got == ok20,
          "a 201-byte claim is dropped, the next frame is taken");
    Bytes head255 = {0xF4, 0xF5, 0x01, 0x40, 0xFF};
    CHECK(feed_all(&fa, cat({head255, ok20}), &got) == 1 && got == ok20, "and a 264-byte claim");
  }
  // The shortest: LEN 0 is 9 bytes.
  {
    H::FrameAssembler fa;
    Bytes f9 = {0xF4, 0xF5, 0x01, 0x40, 0x00, 0x00, 0x41, 0xF4, 0xFB};
    CHECK(feed_all(&fa, f9, &got) == 1 && got == f9, "a 9-byte frame (LEN 0) is the shortest");
  }
  // Line 590: a stray 0xF4 in front of a frame. The frame's own F4 is taken as its double, and
  // the frame still starts.
  {
    H::FrameAssembler fa;
    Bytes stray = {0xF4};
    CHECK(feed_all(&fa, cat({stray, ok20}), &got) == 1 && got == ok20,
          "a stray F4 before a frame does not cost the frame");
  }
  // Line 586: a byte above 0xF4 does not start a frame.
  for (int junk : {0xF3, 0xF5, 0xFB, 0xFF, 0x00}) {
    H::FrameAssembler fa;
    Bytes j = {(uint8_t) junk};
    CHECK(feed_all(&fa, cat({j, ok20}), &got) == 1 && got == ok20 && got[0] == 0xF4,
          "junk 0x%02X before a frame: the frame is taken and starts with F4", junk);
  }
  // Line 609: the second byte must be exactly 0xF5.
  for (int second : {0x00, 0x10, 0xF3, 0xF6, 0xFB, 0xFF}) {
    H::FrameAssembler fa;
    Bytes j = {0xF4, (uint8_t) second};
    CHECK(feed_all(&fa, cat({j, ok20}), &got) == 1 && got == ok20,
          "F4 0x%02X is no start, the frame behind it is taken", second);
  }
  // Line 613: the third byte must be exactly 0x01 (A/C -> module). 0x00 is our own echo: it is
  // dropped at that byte, so the reply that follows the echo at once is not lost.
  for (int dir : {0x00, 0x02, 0xFF}) {
    H::FrameAssembler fa;
    Bytes j = {0xF4, 0xF5, (uint8_t) dir};
    CHECK(feed_all(&fa, cat({j, ok20}), &got) == 1 && got == ok20,
          "direction 0x%02X is dropped, the next frame is taken", dir);
    H::FrameAssembler fb;
    Bytes whole = frame(20, 0x66);
    whole[2] = (uint8_t) dir;
    seal(&whole);
    CHECK(feed_all(&fb, whole, &got) == 0, "a whole frame with direction 0x%02X is never returned", dir);
  }
  {
    // The module's own poll (direction 0x00, capture) echoed, then the reply.
    H::FrameAssembler fa;
    Bytes echo(H::STATUS_REQUEST, H::STATUS_REQUEST + H::STATUS_REQUEST_LEN);
    CHECK(feed_all(&fa, cat({echo, ok20}), &got) == 1 && got == ok20,
          "an echo of our own poll, then a reply: only the reply");
  }
  // Line 626: the end tag, one either side of each byte. A frame with a wrong tag is dropped.
  for (size_t at : {(size_t) 18, (size_t) 19}) {
    for (int delta : {-1, 1}) {
      H::FrameAssembler fa;
      Bytes f = ok20;
      f[at] = (uint8_t) (f[at] + delta);
      // Not wire(): the damaged tag is sent as it is. 0xF3, 0xF5, 0xFA and 0xFC need no doubling.
      CHECK(feed_all(&fa, f, &got) == 0, "end tag byte %zu off by %d: no frame", at, delta);
      CHECK(feed_all(&fa, ok20, &got) == 1 && got == ok20, "and the assembler is ready for the next one");
    }
  }
  // Line 600: only 0xF4 arms the "doubled" test. A data byte above it, then a doubled F4.
  {
    H::FrameAssembler fa;
    Bytes f = frame(24, 0x66);
    f[14] = 0xFF;
    f[15] = 0xF4;
    f[16] = 0xF5;
    f[17] = 0xF4;
    seal(&f);
    CHECK(feed_all(&fa, wire(f), &got) == 1 && got == f, "FF F4 F5 F4 as data survive the wire");
  }
  // And a checksum whose low byte is above 0xF4, directly in front of the end tag's F4.
  {
    H::FrameAssembler fa;
    Bytes f = frame(24, 0x66);
    f[14] = 0xFF;
    seal(&f);
    // Raise byte 15 by what the low checksum byte lacks to 0xFF.
    f[15] = (uint8_t) (0xFF - f[21]);
    seal(&f);
    CHECK(f[21] == 0xFF, "(a frame whose checksum low byte is 0xFF, got 0x%02X)", f[21]);
    CHECK(feed_all(&fa, wire(f), &got) == 1 && got == f, "a checksum byte of 0xFF before the end tag");
  }
  // Line 596: a lone 0xF4 in the data (not doubled) followed by a smaller byte keeps both.
  {
    H::FrameAssembler fa;
    Bytes f = frame(24, 0x66);
    f[14] = 0xF4;
    f[15] = 0x10;
    seal(&f);
    CHECK(feed_all(&fa, f, &got) == 1 && got == f, "an F4 that was not doubled, then 0x10: both kept");
  }
  // A checksum low byte of 0xF4, doubled, then the end tag: F4 F4 F4 FB.
  {
    H::FrameAssembler fa;
    Bytes f = frame(24, 0x66);
    seal(&f);
    f[15] = (uint8_t) (0xF4 - f[21]);
    seal(&f);
    CHECK(f[21] == 0xF4, "(a frame whose checksum low byte is 0xF4)");
    Bytes w = wire(f);
    CHECK(w.size() == 25 && feed_all(&fa, w, &got) == 1 && got == f, "checksum F4 doubled before the end tag");
  }
  // reset() in the middle of a frame: what was collected is gone.
  {
    H::FrameAssembler fa;
    Bytes half(ok20.begin(), ok20.begin() + 10);
    feed_all(&fa, half, nullptr);
    fa.reset();
    Bytes rest(ok20.begin() + 10, ok20.end());
    CHECK(feed_all(&fa, rest, &got) == 0, "the second half alone is no frame");
    CHECK(feed_all(&fa, ok20, &got) == 1 && got == ok20, "a whole one after it is");
  }
  // Two frames back to back, and a cut frame followed by a whole one.
  {
    H::FrameAssembler fa;
    Bytes other = frame(45, 0x66);
    CHECK(feed_all(&fa, cat({ok20, other}), &got) == 2 && got == other, "two frames back to back");
  }
}

// frame_progress(): the one rule set the assembler applies to the frame in front and to the
// candidate behind it.
static void test_frame_progress() {
  printf("-- frame_progress: each header byte, the LEN limit, the end tag\n");
  size_t expected = 0;
  uint8_t f[200] = {0xF4, 0xF5, 0x01, 0x40, 0x00};
  for (int second = 0; second < 256; second++) {
    f[1] = (uint8_t) second;
    CHECK(H::frame_progress(f, 2, &expected) == (second == 0xF5 ? H::FRAME_PROGRESS_MORE : H::FRAME_PROGRESS_BROKEN),
          "byte 1 = 0x%02X", second);
  }
  f[1] = 0xF5;
  for (int dir = 0; dir < 256; dir++) {
    f[2] = (uint8_t) dir;
    CHECK(H::frame_progress(f, 3, &expected) == (dir == 0x01 ? H::FRAME_PROGRESS_MORE : H::FRAME_PROGRESS_BROKEN),
          "direction 0x%02X (0x00 is our own echo)", dir);
  }
  f[2] = 0x01;
  CHECK(H::FRAME_DIR_FROM_AC == 0x01 && expected == 0, "nothing sets the length before byte 4");
  for (int ctrl = 0; ctrl < 256; ctrl++) {
    f[3] = (uint8_t) ctrl;
    CHECK(H::frame_progress(f, 4, &expected) == H::FRAME_PROGRESS_MORE && expected == 0, "CTRL 0x%02X is not judged",
          ctrl);
  }
  for (int len = 0; len < 256; len++) {
    f[4] = (uint8_t) len;
    expected = 0;
    H::FrameProgress p = H::frame_progress(f, 5, &expected);
    CHECK(expected == (size_t) len + 9, "LEN %d sets %zu", len, expected);
    CHECK(p == (len <= 191 ? H::FRAME_PROGRESS_MORE : H::FRAME_PROGRESS_BROKEN), "LEN %d: 200 bytes is the most", len);
  }
  // Walk a whole frame of every length: MORE up to the last byte, COMPLETE on it.
  for (size_t n = 9; n <= 200; n++) {
    Bytes g = frame(n, 0x66);
    expected = 0;
    bool ok = true;
    for (size_t k = 2; k < n; k++)
      ok = ok && H::frame_progress(g.data(), k, &expected) == H::FRAME_PROGRESS_MORE;
    CHECK(ok && H::frame_progress(g.data(), n, &expected) == H::FRAME_PROGRESS_COMPLETE && expected == n,
          "a whole %zu-byte frame", n);
    for (size_t at : {n - 2, n - 1}) {
      for (int delta : {-1, 1}) {
        Bytes bad = g;
        bad[at] = (uint8_t) (bad[at] + delta);
        size_t e = n;
        CHECK(H::frame_progress(bad.data(), n, &e) == H::FRAME_PROGRESS_BROKEN, "%zu bytes, end tag byte %zu off by %d",
              n, at, delta);
      }
    }
  }
}

// What the assembler makes of a stream in which a frame is damaged, cut short or preceded by a
// start tag that noise produced. Every case used to cost the frame behind the damage.
static void test_assembler_resync() {
  printf("-- frame assembler: a false start or a cut frame does not cost the frame behind it\n");
  Bytes got;
  const Bytes link = frame(28, 0x1E);
  const Bytes status = frame(160, 0x66);
  const Bytes ok20 = frame(20, 0x66);
  // Stubs of a header, 2 to 4 bytes, directly ahead of a frame. The byte that breaks the stub is
  // the frame's own F4 (as byte 2, or as a LEN that claims 253 bytes).
  for (size_t stub = 2; stub <= 4; stub++) {
    H::FrameAssembler fa;
    Bytes head(status.begin(), status.begin() + (long) stub);
    CHECK(feed_all(&fa, cat({head, link}), &got) == 1 && got == link, "a %zu-byte stub, then a frame", stub);
    CHECK(feed_all(&fa, status, &got) == 1 && got == status, "and the frame after it");
  }
  // Longer stubs have a LEN: the assembler is inside a frame when the next one starts.
  for (size_t cut = 5; cut < 159; cut++) {
    H::FrameAssembler fa;
    Bytes head(status.begin(), status.begin() + (long) cut);
    if (cut == 160 - 28) {
      // The one length at which the cut frame's own count ends on the reply's end tag: the frame
      // in front completes, and it is the one delivered (132 bytes of status and the reply,
      // which no checksum will pass). The frame after it is read as usual.
      CHECK(feed_all(&fa, cat({head, link}), &got) == 1 && got.size() == 160 &&
                !H::status_checksum_ok(got.data(), got.size()),
            "status cut at %zu: the reply's end tag completes the cut frame", cut);
      CHECK(feed_all(&fa, status, &got) == 1 && got == status, "status cut at %zu: clean afterwards", cut);
      continue;
    }
    // A short frame behind a long cut one: it is delivered on its own last byte, long before
    // the cut frame's 160 bytes are in.
    Bytes s = cat({head, link});
    int frames = 0;
    for (size_t i = 0; i < s.size(); i++) {
      size_t n = fa.feed(s[i]);
      if (n != 0) {
        frames++;
        CHECK(i + 1 == s.size() && n == 28 && std::memcmp(fa.data(), link.data(), 28) == 0,
              "status cut at %zu: the 0x1E reply behind it is delivered on its last byte (byte %zu)", cut, i);
      }
    }
    CHECK(frames == 1, "status cut at %zu, then a 0x1E reply: %d frame(s)", cut, frames);
    CHECK(feed_all(&fa, status, &got) == 1 && got == status, "status cut at %zu: clean afterwards", cut);
  }
  {
    // Cut frame, then a whole one of the same length: the cut frame's count runs out inside the
    // second, which then takes its place with the bytes already received.
    H::FrameAssembler fa;
    Bytes head(status.begin(), status.begin() + 60);
    CHECK(feed_all(&fa, cat({head, status}), &got) == 1 && got == status, "cut status, then a whole status");
  }
  {
    // A header that claims 200 bytes, then seven 28-byte frames: all seven.
    H::FrameAssembler fa;
    Bytes greedy = {0xF4, 0xF5, 0x01, 0x40, 0xBF};
    Bytes s = greedy;
    for (int i = 0; i < 7; i++)
      s.insert(s.end(), link.begin(), link.end());
    CHECK(feed_all(&fa, s, &got) == 7 && got == link, "seven frames behind a header that claims 200 bytes");
  }
  {
    // Two cut frames, then a whole one: the newest start is the one followed.
    H::FrameAssembler fa;
    Bytes a(status.begin(), status.begin() + 60);
    Bytes b(status.begin(), status.begin() + 30);
    CHECK(feed_all(&fa, cat({a, b, link}), &got) == 1 && got == link, "two cut frames, then a whole one");
  }
  {
    // A candidate that turns out not to be a frame from the unit is dropped again, and the frame
    // in front completes as if nothing had been there. The frame in front is sent by a sender
    // that doubles only its checksum (as this project's builders do), so the F4 inside is bare.
    for (int third : {0x00, 0x02}) {
      H::FrameAssembler fa;
      Bytes f = frame(40, 0x66);
      f[20] = 0xF4;
      f[21] = 0xF5;
      f[22] = (uint8_t) third;
      seal(&f);
      CHECK(feed_all(&fa, f, &got) == 1 && got == f, "F4 F5 %02X as data inside a frame: the frame is whole", third);
    }
    // The same with a candidate that looks right for longer: it is still the frame in front that
    // completes first, and it wins.
    H::FrameAssembler fa;
    Bytes f = frame(40, 0x66);
    f[20] = 0xF4;
    f[21] = 0xF5;
    f[22] = 0x01;
    f[24] = 0x40;  // the candidate's LEN: 73 bytes, more than the frame in front has left
    seal(&f);
    CHECK(feed_all(&fa, f, &got) == 1 && got == f, "F4 F5 01 as data inside a frame: the frame is whole");
    CHECK(feed_all(&fa, link, &got) == 1 && got == link, "and nothing of the candidate is left behind");
  }
  {
    // Both end on the same byte: the frame in front is the one delivered.
    H::FrameAssembler fa;
    Bytes f = frame(40, 0x66);
    const uint8_t inner[] = {0xF4, 0xF5, 0x01, 0x40, 0x00, 0x00, 0x41};  // 9 bytes with the tag of the outer frame
    std::memcpy(&f[31], inner, sizeof(inner));
    f[38] = 0xF4;
    f[39] = 0xFB;
    CHECK(feed_all(&fa, f, &got) == 1 && got.size() == 40 && got == f, "two candidates ending together: the outer one");
  }
  {
    // The limit of the scheme, stated: a whole frame inside a longer one that is sent with bare
    // 0xF4 bytes is delivered first, and the outer one is lost. A unit that doubles its data
    // bytes cannot produce this, and the checksum of the outer frame would be judged otherwise.
    H::FrameAssembler fa;
    Bytes f = frame(40, 0x66);
    const uint8_t inner[] = {0xF4, 0xF5, 0x01, 0x40, 0x00, 0x00, 0x41, 0xF4, 0xFB};
    std::memcpy(&f[16], inner, sizeof(inner));
    seal(&f);
    Bytes want(inner, inner + sizeof(inner));
    CHECK(feed_all(&fa, f, &got) == 1 && got == want, "a whole 9-byte frame inside an un-doubled one is taken first");
    CHECK(feed_all(&fa, link, &got) == 1 && got == link, "and the stream is read on from there");
  }
  {
    // The one cut point that still costs the next frame: between the F4 and the FB of the end
    // tag. The next frame's F4 is the second of a pair, which is one escaped data byte.
    H::FrameAssembler fa;
    Bytes head(ok20.begin(), ok20.begin() + 19);
    CHECK(feed_all(&fa, cat({head, link}), &got) == 0, "cut inside the end tag: the next frame's start is not seen");
    CHECK(feed_all(&fa, link, &got) == 1 && got == link, "the one after it is");
  }
}

int main() {
  printf("== ESPHome codec: coverage and mutation gaps ==\n");
  test_defaults();
  test_temperature();
  test_checksum();
  test_builder_arguments();
  test_checksum_stuffing();
  test_command_bytes();
  test_stamps();
  test_parse_status();
  test_parse_features_and_faults();
  test_setup_request();
  test_assembler();
  test_frame_progress();
  test_assembler_resync();
  printf("  %d checks, %d failed\n", g_checks, g_fail);
  printf(g_fail ? "== CODEC GAPS FAILED ==\n" : "== CODEC GAPS OK ==\n");
  return g_fail ? 1 : 0;
}
