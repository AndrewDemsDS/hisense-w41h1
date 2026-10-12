#pragma once
// Hisense / AirconIntl indoor-unit RS-485 codec: frame constants, builders and parsers.
//
// Plain constants and free functions, plus the receive-side frame assembler. Every byte value comes
// from a bus capture or from the vendor Wi-Fi module's firmware. `VERIFY` marks values not yet
// confirmed on hardware.
//
// The remote's "77" gesture is carried as a setup request: the request bits of the A/C's 0x1E
// reply, their debounce and lockout, and the heartbeat bit that puts "77" on the panel. What the
// request means is left to whoever listens (packages/node.yaml opens the Wi-Fi setup hotspot).
// Deliberately NOT implemented: the rest of the vendor module's cloud pairing flow (smart-config
// itself, the binding token of bit 0) and the raw 0x1E link-frame diagnostics.
//
// No ESPHome includes, so this file can be compiled and tested on a host without ESPHome.

#include <cstddef>
#include <cstdint>

namespace esphome::hisense_ac {

// ---- Framing (confirmed against the vendor W41H1 module's parser) ------------------------------
static constexpr uint8_t FRAME_STX1 = 0xF4;
static constexpr uint8_t FRAME_STX2 = 0xF5;
static constexpr uint8_t FRAME_ETX1 = 0xF4;
static constexpr uint8_t FRAME_ETX2 = 0xFB;
static constexpr uint32_t BUS_BAUD_RATE = 9600;  // 8N1, firmware-confirmed A/C-link baud

// A frame's total length is its LEN byte (frame[4]) plus 9. Confirmed on hardware for every
// class: poll 0x0C -> 21, link 0x13 -> 28, command 0x29 -> 50, status 0x97 -> 160.
static constexpr size_t FRAME_LEN_OVERHEAD = 9;
static constexpr size_t FRAME_CLASS_OFFSET = 13;
// Every frame ends with a 2-byte checksum and the F4 FB tag.
static constexpr size_t FRAME_TRAILER_LEN = 4;
// True when byte `offset` of a `len`-byte frame is payload: it exists and lies in front of the
// trailer. In a frame shorter than that the same offset holds a checksum byte or the end tag, so a
// parser that tested only `len > offset` would read those as data.
constexpr bool frame_has_payload_byte(size_t len, size_t offset) { return len >= offset + 1 + FRAME_TRAILER_LEN; }

// Message classes (frame[13]).
static constexpr uint8_t CLASS_DEVTYPE = 0x0A;
static constexpr uint8_t CLASS_LINK = 0x1E;
static constexpr uint8_t CLASS_COMMAND = 0x65;
static constexpr uint8_t CLASS_STATUS = 0x66;
static constexpr uint8_t SUBTYPE_PRODUCT_TYPE = 0x40;  // 0x66/40 feature-flag poll

// Combined command (0x65) frame: 16-byte header, 30-byte body, 2-byte checksum, F4 FB.
static constexpr size_t CMD_FRAME_LEN = 50;
static constexpr size_t CMD_HEADER_LEN = 16;
static constexpr size_t CMD_CHK_OFFSET = 46;
static constexpr size_t CMD_END_OFFSET = 48;
// A builder's output can grow by up to two bytes: each 0xF4 checksum byte is doubled on the wire.
static constexpr size_t CMD_FRAME_MAX = CMD_FRAME_LEN + 2;

// CTRL (frame[3]) = 0x40 selects a 2-byte big-endian checksum in the vendor module's parser;
// frame[4] = 0x29 is the 8-bit LEN; frame[13] = 0x65.
static constexpr uint8_t CMD_HEADER[CMD_HEADER_LEN] = {0xF4, 0xF5, 0x00, 0x40, 0x29, 0x00, 0x00, 0x01,
                                                       0x01, 0xFE, 0x01, 0x00, 0x00, 0x65, 0x00, 0x00};

// Status poll (0x66/00).
static constexpr size_t STATUS_REQUEST_LEN = 21;
static constexpr uint8_t STATUS_REQUEST[STATUS_REQUEST_LEN] = {0xF4, 0xF5, 0x00, 0x40, 0x0C, 0x00, 0x00,
                                                               0x01, 0x01, 0xFE, 0x01, 0x00, 0x00, 0x66,
                                                               0x00, 0x00, 0x00, 0x01, 0xB3, 0xF4, 0xFB};

// Link bring-up and keepalive, captured verbatim from the vendor module's transmit line. The A/C
// ignores the status poll until it has seen 0x0A and 0x07 once, then a 0x1E heartbeat at about
// 1 Hz.
static constexpr size_t LINK_INIT_0A_LEN = 20;
static constexpr uint8_t LINK_INIT_0A[LINK_INIT_0A_LEN] = {0xF4, 0xF5, 0x00, 0x40, 0x0B, 0x00, 0x00, 0x00, 0x00, 0xFE,
                                                           0x01, 0x00, 0x00, 0x0A, 0x04, 0x00, 0x01, 0x58, 0xF4, 0xFB};
static constexpr size_t LINK_INIT_07_LEN = 20;
static constexpr uint8_t LINK_INIT_07[LINK_INIT_07_LEN] = {0xF4, 0xF5, 0x00, 0x40, 0x0B, 0x00, 0x00, 0x01, 0x01, 0xFE,
                                                           0x01, 0x00, 0x00, 0x07, 0x01, 0x00, 0x01, 0x54, 0xF4, 0xFB};
static constexpr size_t LINK_HEARTBEAT_LEN = 28;
static constexpr uint8_t LINK_HEARTBEAT[LINK_HEARTBEAT_LEN] = {
    0xF4, 0xF5, 0x00, 0x40, 0x13, 0x00, 0x00, 0x01, 0x01, 0xFE, 0x01, 0x00, 0x00, 0x1E,
    0x00, 0x00, 0xB0, 0x80, 0x20, 0x00, 0x00, 0x00, 0x40, 0x00, 0x03, 0x02, 0xF4, 0xFB};

// Receive buffer: the 160-byte status with room to spare. A frame whose LEN claims more is dropped,
// so this is behaviour, not just a size.
static constexpr size_t RX_FRAME_MAX = 200;
static constexpr size_t TX_FRAME_MAX = 64;

// ---- Setpoint ranges (sample-confirmed) --------------------------------------------------------
static constexpr int8_t SETPOINT_MIN_C = 16;
static constexpr int8_t SETPOINT_MAX_C = 32;
static constexpr int8_t SETPOINT_MIN_F = 61;
static constexpr int8_t SETPOINT_MAX_F = 90;

// ---- Enums -------------------------------------------------------------------------------------
// Enum value = COMMAND index, so (mode * 2 + 1) << 4 is the command byte 18 (AUTO -> 0x90, captured
// from the vendor module). Status reports AUTO as nibble 5 or 6; parse_status() remaps both to AUTO.
enum Mode : uint8_t {
  MODE_FAN = 0,
  MODE_HEAT = 1,
  MODE_COOL = 2,
  MODE_DRY = 3,
  MODE_AUTO = 4,
};

// Enum value = W41H1 fan index: command byte 16 is index * 2 + 1, status byte 16 is index * 2.
enum FanSpeed : uint8_t {
  FAN_SPEED_AUTO = 0,         // cmd 0x01 / status 0x01, confirmed
  FAN_SPEED_QUIET = 1,        // cmd 0x03 / status 0x02 (mute). VERIFY cmd
  FAN_SPEED_LOW = 5,          // cmd 0x0B / status 0x0A, confirmed
  FAN_SPEED_MED_LOW = 6,      // cmd 0x0D / status 0x0C, confirmed
  FAN_SPEED_MID = 7,          // cmd 0x0F / status 0x0E, confirmed
  FAN_SPEED_MED_HIGH = 8,     // cmd 0x11 / status 0x10, confirmed
  FAN_SPEED_HIGH = 9,         // cmd 0x13 / status 0x12, confirmed
  FAN_SPEED_NOCHANGE = 0xFF,  // "keep the A/C's fan"; never packed literally
};

enum SwingMode : uint8_t {
  SWING_MODE_OFF = 0,        // hold the louvre at a fixed position
  SWING_MODE_DIRECTION = 1,  // move to a set position, no oscillation
  SWING_MODE_SWING = 3,      // oscillate (2-bit field 0b11)
};

// Byte 33. Eco and turbo share it, so they are mutually exclusive on the wire.
enum Feature : uint8_t {
  FEATURE_NONE = 0,     // 0x04, neutral and turbo-off, confirmed
  FEATURE_ECO = 1,      // 0x30, confirmed on W41H1
  FEATURE_TURBO = 2,    // 0x0C, confirmed on W41H1
  FEATURE_ECO_OFF = 3,  // 0x10, explicit eco clear (NONE does not clear eco). VERIFY
};

// Byte 36. NOCHANGE rides ordinary frames.
enum Display : uint8_t {
  DISPLAY_NOCHANGE = 0,  // 0x00, leave the panel alone
  DISPLAY_ON = 1,        // 0xC0
  DISPLAY_OFF = 2,       // 0x40
};

// ---- Structs -----------------------------------------------------------------------------------
// Command byte 18 bits 2-3, the run field. 0b11 is "switch on", 0b01 "switch off", 0b00 "leave
// alone". The literal power frames carry 0x0C and 0x04 there (hardware-proven), and the stock module
// ORs the same 0x0C into the mode byte so that power and mode travel in one frame: 0x5C for cool and
// 0x3C for heat (stock image file 0x25bf2 and 0x25bdc, RE docs/10 section 5b-2).
static constexpr uint8_t CMD_MODE_BYTE = 18;
static constexpr uint8_t CMD_POWER_ON_BITS = 0x0C;

// Everything one combined 0x65 frame carries. Switching off is deliberately absent: it has its own
// literal frame.
struct AcCommand {
  Mode mode{MODE_COOL};
  int8_t setpoint{24};  // whole degrees in the unit named by `fahrenheit`
  bool fahrenheit{false};
  FanSpeed fan{FAN_SPEED_AUTO};
  SwingMode vswing{SWING_MODE_OFF};
  SwingMode hswing{SWING_MODE_OFF};
  Feature feature{FEATURE_NONE};
  Display display{DISPLAY_NOCHANGE};
  // Also switch the unit on, in this frame (byte 18 |= CMD_POWER_ON_BITS). Set for the one frame
  // that powers the unit on, never kept in the shadow. Cool 0x5C and heat 0x3C are the stock
  // module's own bytes. VERIFY on hardware: auto 0x9C, dry 0x7C and fan 0x1C, which follow from the
  // same field rule and which this project's earlier firmware never sent.
  bool power_on{false};
};

// Decoded 0x66/00 status frame (160 bytes on the W41H1). Offsets are absolute frame offsets.
struct AcState {
  bool valid{false};
  bool power_on{false};        // byte 18 bits 2-3 != 0
  Mode mode{MODE_COOL};        // byte 18 bits 4-7, AUTO remapped
  bool temp_unit_f{false};     // byte 26 bit 1, confirmed
  int8_t indoor_temp_c{0};     // byte 20, display unit, converted to C
  int8_t setpoint_c{0};        // byte 19, display unit, converted to C
  uint8_t fan_raw{0};          // byte 16: 0x01 auto, 0x02 quiet, 0x0A..0x12 low..high
  bool vswing_on{false};       // byte 35 bit 7
  bool turbo_on{false};        // byte 35 bit 1
  bool eco_on{false};          // byte 35 bit 2
  bool hswing_on{false};       // byte 35 bit 6
  bool heat_relay_on{false};   // byte 35 bit 4, aux/PTC heater relay
  bool mute_on{false};         // byte 36 bit 2
  bool sleep_on{false};        // byte 17 != 0
  uint8_t sleep_raw{0};        // byte 17 holds profile * 2
  bool purify_on{false};       // byte 36 bit 7. VERIFY (no test unit had purify)
  int8_t outdoor_temp_c{0};    // byte 44, always C
  int8_t coil_temp_c{0};       // byte 45, always C
  uint8_t compressor_freq{0};  // byte 42, Hz
  uint8_t current_raw{0};      // byte 55, power proxy (see hisense_map.h)
  uint8_t voltage_raw{0};      // byte 50, whole volts
};

// 0x66/40 ProductType reply. Offsets from the vendor module's parser.
struct AcFeatures {
  bool valid{false};
  bool cool_heat{false};
  bool ai{false};
  bool infinite_fan{false};
  bool power_save{false};
  bool fan_mute{false};
  bool swing_dir_8{false};
  bool swing_follow{false};
  uint8_t power_display{0};
  uint8_t demand_resp{0};
  bool humidity{false};
  bool heat_8c{false};
  bool purify{false};
  bool ext_valid{false};  // false => the three fields below are UNKNOWN, not absent
  bool q_display{false};
  bool enable_8heat{false};
  bool trans_102_64{false};
  uint8_t reply_len{0};  // diagnostic only
};

// f_e_* fault bits from the same status frame. Wire byte = 15 + payload offset.
static constexpr size_t FAULT_BYTE_INDOOR = 39;
static constexpr size_t FAULT_BYTE_MODULE = 40;
static constexpr size_t FAULT_BYTE_OUTDOOR = 64;
static constexpr size_t FAULT_BYTE_PROTECT = 66;
// Byte 66 bit 7 is the 8 C frost-guard mode flag, proven on hardware, not a fault.
static constexpr uint8_t FAULT_NONFAULT_PROTECT = 0x80;

struct AcFaults {
  bool valid{false};
  bool any{false};  // ORs the RAW bytes: an unnamed bit still counts as a fault
  uint8_t raw_indoor{0};
  uint8_t raw_module{0};
  uint8_t raw_outdoor{0};
  uint8_t raw_protect{0};
  bool in_temp{false};
  bool in_coil_temp{false};
  bool in_humidity{false};
  bool water_full{false};
  bool in_fan_motor{false};
  bool grille{false};
  bool in_vzero{false};
  bool in_com{false};
  bool in_display{false};
  bool in_keys{false};
  bool in_wifi{false};
  bool in_ele{false};
  bool in_eeprom{false};
  bool out_eeprom{false};
  bool out_coil_temp{false};
  bool out_gas_temp{false};
  bool out_temp{false};
  bool over_temp{false};
};

// ---- Packed bitmaps (binary_sensor.py uses the same bit indices) -------------------------------
static constexpr uint8_t FEAT1_COOL_HEAT = 0;
static constexpr uint8_t FEAT1_AI = 1;
static constexpr uint8_t FEAT1_INFINITE_FAN = 2;
static constexpr uint8_t FEAT1_POWER_SAVE = 3;
static constexpr uint8_t FEAT1_FAN_MUTE = 4;
static constexpr uint8_t FEAT1_SWING_DIR_8 = 5;
static constexpr uint8_t FEAT1_SWING_FOLLOW = 6;
static constexpr uint8_t FEAT1_HUMIDITY = 7;
static constexpr uint8_t FEAT1_HEAT_8C = 8;
static constexpr uint8_t FEAT1_PURIFY = 9;
static constexpr uint8_t FEAT1_Q_DISPLAY = 10;
static constexpr uint8_t FEAT1_ENABLE_8HEAT = 11;
static constexpr uint8_t FEAT1_TRANS_102_64 = 12;
static constexpr uint8_t FEAT1_POWER_DISPLAY_SHIFT = 16;
static constexpr uint8_t FEAT1_DEMAND_RESP_SHIFT = 18;
static constexpr uint8_t FEAT1_EXT_VALID = 30;
static constexpr uint8_t FEAT1_VALID = 31;

static constexpr uint8_t FAULT1_IN_TEMP = 0;
static constexpr uint8_t FAULT1_IN_COIL_TEMP = 1;
static constexpr uint8_t FAULT1_IN_HUMIDITY = 2;
static constexpr uint8_t FAULT1_WATER_FULL = 3;
static constexpr uint8_t FAULT1_IN_FAN_MOTOR = 4;
static constexpr uint8_t FAULT1_GRILLE = 5;
static constexpr uint8_t FAULT1_IN_VZERO = 6;
static constexpr uint8_t FAULT1_IN_COM = 7;
static constexpr uint8_t FAULT1_IN_DISPLAY = 8;
static constexpr uint8_t FAULT1_IN_KEYS = 9;
static constexpr uint8_t FAULT1_IN_WIFI = 10;
static constexpr uint8_t FAULT1_IN_ELE = 11;
static constexpr uint8_t FAULT1_IN_EEPROM = 12;
static constexpr uint8_t FAULT1_OUT_EEPROM = 13;
static constexpr uint8_t FAULT1_OUT_COIL_TEMP = 14;
static constexpr uint8_t FAULT1_OUT_GAS_TEMP = 15;
static constexpr uint8_t FAULT1_OUT_TEMP = 16;
static constexpr uint8_t FAULT1_OVER_TEMP = 17;
static constexpr uint8_t FAULT1_ANY = 30;
static constexpr uint8_t FAULT1_VALID = 31;

uint32_t features_to_bitmap32(const AcFeatures &f);
AcFeatures features_from_bitmap32(uint32_t b);
uint32_t faults_to_bitmap32(const AcFaults &f);

// ---- Temperature helpers -----------------------------------------------------------------------
// Rounded to nearest, integer maths, correct for negatives.
int8_t f_to_c(int f);
int8_t c_to_f(int c);
bool setpoint_in_range(int8_t setpoint, bool fahrenheit);
// Validate an A/C-reported setpoint for the command shadow, in the WIRE unit. False (out untouched)
// when the builder would reject it, e.g. a 5 C frost-guard setpoint.
bool shadow_setpoint_from_status(int8_t setpoint_c, bool temp_unit_f, int8_t *out);

// ---- Checksums and framing ---------------------------------------------------------------------
// Big-endian running byte sum over [start, end), 16 bits (vendor parser: CTRL & 0xC0 == 0x40).
uint16_t checksum_range(const uint8_t *frame, size_t start, size_t end);
// Verify a received, un-stuffed frame's checksum: sum over [2, n - 4) against bytes n-4, n-3.
bool status_checksum_ok(const uint8_t *frame, size_t n);
// A transaction accepts a reply only when its class matches; 0 accepts any class.
inline bool reply_class_ok(uint8_t got_class, uint8_t expect_class) {
  return expect_class == 0 || got_class == expect_class;
}

// ---- Builders: each returns the on-the-wire length (stuffing included), or 0 on error ----------
size_t build_command(const AcCommand &cmd, uint8_t *out, size_t out_cap);
// Literal on/off frames, sent byte for byte.
size_t build_power_frame(bool power_on, uint8_t *out, size_t out_cap);
// Minimal single-field 0x65 frame: header, 0x04 at byte 23, 0x01 at byte 31, one named byte.
size_t build_single_field(uint8_t offset, uint8_t value, uint8_t *out, size_t out_cap);
size_t build_mute_frame(bool on, uint8_t *out, size_t out_cap);           // byte 35: 0x30 on, 0x10 off
size_t build_sleep_frame(uint8_t profile, uint8_t *out, size_t out_cap);  // byte 17: profile*2+1
size_t build_producttype_request(uint8_t *out, size_t out_cap);
// 0x1E heartbeat. Byte 16 = 0xB0 once the A/C's own 0x1E has been heard, 0xF0 before.
size_t build_link_heartbeat(bool heard_ac, uint8_t *out, size_t out_cap);
// The same heartbeat with the module's provisioning flag (byte 17 bit 3) set or clear. While it is
// set the panel shows "77".
size_t build_link_heartbeat(bool heard_ac, bool provisioning, uint8_t *out, size_t out_cap);
// Re-stamp a finished frame's envelope bytes 7/8 with the A/C's device type and fix the checksum.
// 0 when the envelope is not recognised.
size_t stamp_link_token(const uint8_t *in, size_t len, uint8_t hi, uint8_t lo, uint8_t *out, size_t out_cap);
// Set or clear the buzzer bit of a finished 0x65 command frame and fix the checksum. The stock
// module ORs `t_beep << 2` into payload byte 10, which is frame byte 23 (get_dev_control_cmd, stock
// image file 0x1f96e), and defaults it to 1; every builder here writes it as 1 too. It is a flag
// on the one frame, not a stored setting. VERIFY on hardware: that the unit stays silent with the
// bit clear. Any other frame class is copied through unchanged. 0 on error.
static constexpr uint8_t CMD_BEEP_BYTE = 23;
static constexpr uint8_t CMD_BEEP_BIT = 0x04;
size_t stamp_beep(const uint8_t *in, size_t len, bool beep, uint8_t *out, size_t out_cap);

// ---- Parsers (input already un-stuffed) --------------------------------------------------------
bool parse_status(const uint8_t *buf, size_t len, AcState *out);
bool parse_features(const uint8_t *buf, size_t len, AcFeatures *out);
bool parse_faults(const uint8_t *buf, size_t len, AcFaults *out);
// Device type and sub-type from a 0x0A DevType reply: inner payload [3]/[4] = frame 16/17. Reads
// the two bytes and judges nothing else; see devtype_reply_usable() for what may be adopted.
bool devtype_from_reply(const uint8_t *reply, size_t n, uint8_t *hi, uint8_t *lo);
// Whether a DevType reply may be adopted as the pair stamped into bytes 7/8 of every later frame.
// `checksum_ok` is status_checksum_ok() of the same frame. The stock module runs every reply, this
// one included, through one parser that rejects a checksum mismatch before any handler sees the
// payload (RE docs/03 "Parser facts", docs/10 section 4.2 step 5), and a wrong pair in bytes 7/8
// makes the unit reject every frame (docs/10 section 4.5). 00 00 means "not linked yet" to the
// stock module and is never adopted. VERIFY: no capture of a real DevType reply is in the
// repository. That it carries the 2-byte sum follows from the frame being LEN + 9 bytes long
// (every deployed unit learns its type through an assembler that requires exactly that), which in
// the stock parser's layout leaves two bytes between the payload and the end tag.
bool devtype_reply_usable(bool checksum_ok, const uint8_t *reply, size_t n, uint8_t *hi, uint8_t *lo);

// ---- Setup request: the remote's "77" gesture --------------------------------------------------
// Horizontal airflow pressed six times on the remote makes the mainboard set a request bit in
// payload[4] (frame byte 17) of its reply to the 0x1E heartbeat. The vendor module tests exactly
// these two bits there (reverse-engineering/docs/03-rs485-ac-protocol.md, "Recommission (77)").
static constexpr size_t LINK_FLAGS_OFFSET = 17;
// Bit 5: smart-config pairing. Bench unit, 2026-07-20: set for exactly one reply per gesture, about
// one second, and never transmitted by the module, so the unit cannot be handing it back.
static constexpr uint8_t LINK_REQ_SMARTCFG = 0x20;
// Bit 3: reset and reconfigure, from the vendor firmware's handler. VERIFY: not seen from the bench
// unit, which raises bit 5 instead. The module transmits the same bit position as its provisioning
// flag (below), so a single reply carrying it proves nothing.
static constexpr uint8_t LINK_REQ_RECONFIG = 0x08;
static constexpr uint8_t LINK_REQ_MASK = LINK_REQ_RECONFIG | LINK_REQ_SMARTCFG;
// Outbound, same byte: the module's "I am provisioning" flag. Injected on the bench on 2026-07-09,
// it lit "77" on the panel.
static constexpr uint8_t LINK_FLAG_PROVISIONING = 0x08;
// Replies in a row (about 1 s apart) that must carry a request before it counts, when bit 3 is
// part of it. Bit 5 alone counts at once: one reply is all the unit ever sends.
static constexpr uint8_t SETUP_REQUEST_HOLD_FRAMES = 3;
// How long bit 5 is ignored after a request was taken and after the indication was switched off.
// The gesture is also how a user leaves the mode, and the press that ends it makes a pulse of its
// own a reply later, which reopened what had just been closed (project issue 69).
static constexpr uint32_t SETUP_REQUEST_LOCKOUT_MS = 10000;

// The request bits of a 0x1E reply, masked to LINK_REQ_MASK. False for any other class, a short
// frame or a bad checksum: one reply is enough to act on, so a damaged one must not count.
bool link_request_bits(const uint8_t *frame, size_t len, uint8_t *bits);

// One step of the debounce, for one reply's request bits. +1 once when the request has been there
// for `hold_frames` replies in a row, -1 when a request that had counted goes away, 0 otherwise.
int setup_request_debounce(uint8_t req_bits, uint8_t *streak, bool *latched, uint8_t hold_frames);

// `req_bits` without bit 5 while the lockout runs: `armed`, and less than SETUP_REQUEST_LOCKOUT_MS
// since `since_ms`. Bit 3 is never removed here. Elapsed time on a 32 bit clock, so it is right
// through the 49.7 day wrap as long as the caller disarms an expired lockout (the tracker does).
uint8_t smartcfg_lockout_mask(uint8_t req_bits, uint32_t now_ms, bool armed, uint32_t since_ms);

// All of the above for a stream of replies. Zero-initialised is the boot state.
struct SetupRequestTracker {
  uint8_t streak{0};
  bool latched{false};
  uint8_t last_req{0};
  // Bit 3 only counts after a reply without it, seen while the module's own flag was off the
  // wire. That covers a unit that hands the flag back, and a request still standing at boot,
  // which would otherwise be taken again after every restart.
  bool reconfig_quiet{false};
  bool lockout_armed{false};
  uint32_t lockout_since{0};
};
// Start (or restart) the bit 5 lockout. Call at boot and whenever the indication is switched off.
void setup_request_lockout(SetupRequestTracker *tracker, uint32_t now_ms);
// Feed one reply's request bits (from link_request_bits). `indicating`: the module's provisioning
// flag is on the wire. Returns the request bits once per accepted request, 0 otherwise. An
// accepted request starts the lockout by itself.
uint8_t setup_request_step(SetupRequestTracker *tracker, uint8_t req_bits, bool indicating, uint32_t now_ms);

// ---- Receive-side frame assembler --------------------------------------------------------------
// Direction byte (frame[2]) of a frame from the A/C. Our own frames carry 0x00 there, which is how
// an echo of them is told apart (RX gate in the vendor module's parser, RE docs/10 section 3.2).
static constexpr uint8_t FRAME_DIR_FROM_AC = 0x01;

// What the newest byte made of a frame that is being received.
enum FrameProgress : uint8_t {
  FRAME_PROGRESS_MORE = 0,      // nothing wrong so far, not finished
  FRAME_PROGRESS_COMPLETE = 1,  // LEN + 9 bytes, ending F4 FB
  FRAME_PROGRESS_BROKEN = 2,    // not a frame from the A/C
};
// Judge the first `n` un-stuffed bytes of a possible frame (n >= 2) by its newest byte only: F5
// at byte 1, the A/C's direction at byte 2, a LEN at byte 4 that fits RX_FRAME_MAX, the F4 FB tag
// at the end. `*expected` is 0 until byte 4 has been judged, then LEN + 9. Call it for every
// length in turn.
FrameProgress frame_progress(const uint8_t *frame, size_t n, size_t *expected);

// Feeds one byte at a time. Accepts only A/C -> module frames (which also drops any echo of our
// own frames), un-stuffs doubled 0xF4, sizes the frame from its LEN byte, and checks the F4 FB end
// tag. The checksum is NOT judged here: whoever takes the frame calls status_checksum_ok().
//
// Resynchronisation. A byte that breaks a frame is looked at again as a possible start byte. And
// while a frame is being received, the newest F4 F5 inside it is followed as a second candidate:
// if the frame in front turns out not to be one (cut short, or a start tag that noise produced),
// the candidate carries on from the bytes already received instead of being lost with it. A frame
// in front that completes always wins, so a stream of whole frames is read exactly as before.
class FrameAssembler {
 public:
  // Returns the length of a complete frame, now readable through data(); 0 otherwise.
  size_t feed(uint8_t b);
  const uint8_t *data() const { return this->buf_; }
  void reset();

 protected:
  void start_(uint8_t b);
  size_t deliver_(size_t start);

  uint8_t buf_[RX_FRAME_MAX]{};
  size_t len_{0};       // un-stuffed bytes held; 0 = between frames
  size_t expected_{0};  // LEN + 9 of the frame in front, 0 until its byte 4 is in
  // Where in buf_ the second candidate starts (never 0, which means there is none), and its LEN + 9.
  size_t later_{0};
  size_t later_expected_{0};
  bool prev_was_f4_{false};
};

}  // namespace esphome::hisense_ac
