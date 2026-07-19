/********************************************************************************
  *
  * hisense_rs485.h
  *
  * RS-485 driver for the Hisense/AirconIntl indoor-unit control bus, replacing
  * the Ameba room_air_conditioner SDK example's stub (DHT11 + PWM fan) hardware
  * I/O with real A/C bus TX/RX.
  *
  * Frame format is FIRMWARE-CONFIRMED (W41H1 dongle disassembly, see
  * hisense-w41h1-re/docs/03-rs485-ac-protocol.md): F4 F5 start, F4 FB end,
  * length + checksum. The payload byte layout, field encodings, and checksum
  * ALGORITHM below are reverse-derived from the community reference
  * implementation `pslawinski/esphome_airconintl` (messages.h / device_status.h
  * / aircon_climate.h), which drives the identical Hisense/AirconIntl bus, and
  * were arithmetically re-verified against all ~75 sample frames in that repo
  * before being encoded here (see hisense-w41h1-re/docs/05-esp32-replacement.md
  * for the human-readable summary this header supersedes in places -- offsets
  * that disagree with docs/05 are called out explicitly below).
  *
  * Anything not directly exercised by a sample frame in the reference repo is
  * marked `// VERIFY` and MUST be confirmed on the bench (see INTEGRATION.md)
  * before being trusted in the field.
  *
  * Plain C-style embedded C++: no STL, no heap churn in the hot path, no
  * exceptions. Matches the style of room_aircon_driver.{h,cpp}.
  *
********************************************************************************/

#pragma once

#include <platform_stdlib.h>
#include <serial_api.h>
#include <PinNames.h>
#include <gpio_api.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Pin configuration -- CONFIRMED from the stock W41H1 firmware dump.
 *
 * The A/C bus UART is brought up inside `uart_ctl_process_main` with
 * serial_init(obj, PA_14 tx, PA_13 rx) -> UART0, then
 * serial_format(obj, 8, ParityNone, 1) (8N1) and serial_baud(obj, 9600)
 * in the A/C-link mode (it boots 115200 and drops to 9600 for the A/C link).
 * See UART_PINS.md for the full disassembly (movs r2 = 0xd / movs r1 = 0xe ->
 * PA_13/PA_14; serial_init/serial_baud offset delta 0x7c cross-checked
 * against the SDK build).
 *
 * DE/RE direction GPIO: PA_17. CORRECTED 2026-07-06 -- an earlier pass wrongly
 * concluded "no direction GPIO / auto-direction". The stock firmware DOES drive
 * a half-duplex direction line, not in the UART bring-up window but in its
 * low-level byte-writer (0x9b6f51c8): it drives PA_17 HIGH to transmit and LOW
 * to receive (DE and /RE tied). Without asserting it the UM3352E's bus driver
 * stays high-Z, so our bytes reach DI but never the A/B wire and the A/C stays
 * silent -- confirmed on hardware across three firmware revisions. Full
 * disassembly + timing in reverse-engineering/docs/09.
 * -------------------------------------------------------------------------*/
#define HISENSE_UART_TX_PIN   PA_14   // firmware-confirmed (UART0 TX, raw 0x0E)
#define HISENSE_UART_RX_PIN   PA_13   // firmware-confirmed (UART0 RX, raw 0x0D)
#define HISENSE_UART_DE_PIN   PA_17   // half-duplex driver-enable (HIGH=TX, LOW=RX)

#define HISENSE_UART_BAUD      9600   // firmware-confirmed A/C-link baud (8N1)
#define HISENSE_UART_DATA_BITS 8
#define HISENSE_UART_STOP_BITS 1

/* ---------------------------------------------------------------------------
 * Frame constants
 * -------------------------------------------------------------------------*/
#define HISENSE_STX1 0xF4
#define HISENSE_STX2 0xF5
#define HISENSE_ETX1 0xF4
#define HISENSE_ETX2 0xFB

// Command (module -> A/C) frames are a fixed 50 bytes in every sample in the
// reference repo (single-field "set" commands); the 16-byte header and total
// length are IDENTICAL across on/off/mode/temp/fan/swing/eco/turbo/display
// commands -- only the 30-byte body (offset 16..45) and the recomputed
// checksum differ.
#define HISENSE_CMD_FRAME_LEN     50
#define HISENSE_CMD_HEADER_LEN    16
#define HISENSE_CMD_CHK_OFFSET    46  // 2 bytes, big-endian
#define HISENSE_CMD_END_OFFSET    48  // F4 FB

// The 16-byte header on every "write" (message-class 0x65) command frame,
// byte-for-byte from messages.h. Byte[2]=TYPE(0x00), byte[3]=CTRL(0x40),
// byte[13]=0x65 is the message-class ID for "set" commands (0x66 = "status"
// class, see request_status[] / Device_Status).
//
// RECONCILED against the stock firmware parser (docs/03 generic envelope):
// byte[3]=CTRL=0x40. The parser decodes the checksum WIDTH from CTRL & 0xC0
// (firmware `helper 0x6f0958`: 0x00->1 byte, 0x40->2 bytes, 0x80->2, 0xC0->4,
// all big-endian). CTRL=0x40 therefore selects a **2-byte big-endian sum**,
// which is exactly what hisense_checksum_range() computes -- so docs/03's
// tentative "1-byte checksum" note only applies to CTRL&0xC0==0 frames, not
// to these. byte[4]=0x29 is the 8-bit LEN (bit5 "16-bit LEN" clear, as
// expected for a 41-byte payload). The header is thus a genuine, decoded
// envelope, not an opaque blob.
static const uint8_t HISENSE_CMD_HEADER[HISENSE_CMD_HEADER_LEN] = {
    0xF4, 0xF5, 0x00, 0x40, 0x29, 0x00, 0x00, 0x01,
    0x01, 0xFE, 0x01, 0x00, 0x00, 0x65, 0x00, 0x00
};

// Status-request frame (module -> A/C, message-class 0x66), verbatim from
// messages.h `request_status[]`. Send this periodically to poll A/C state.
#define HISENSE_STATUS_REQUEST_LEN 21
static const uint8_t HISENSE_STATUS_REQUEST[HISENSE_STATUS_REQUEST_LEN] = {
    0xF4, 0xF5, 0x00, 0x40, 0x0C, 0x00, 0x00, 0x01, 0x01, 0xFE, 0x01,
    0x00, 0x00, 0x66, 0x00, 0x00, 0x00, 0x01, 0xB3, 0xF4, 0xFB
};

// Link / keepalive frames (module -> A/C), captured VERBATIM from the stock
// AEH-W41H1 dongle's DI line at link-up (live capture, 2026-07-06). The A/C
// stays SILENT to the status poll above until it has seen the module bring the
// link up: two init frames (0x0A, 0x07) once at boot, then a 0x1E LINK
// heartbeat at ~1 Hz. The 0x1E frame is static (22x byte-identical on the wire,
// no rolling counter), so it is safe to replay verbatim. All checksums verified
// OK by the repo decoder. Direction byte[2]=0x00 = module->A/C (the A/C replies
// with byte[2]=0x01).
#define HISENSE_LINK_INIT_0A_LEN 20
static const uint8_t HISENSE_LINK_INIT_0A[HISENSE_LINK_INIT_0A_LEN] = {
    0xF4, 0xF5, 0x00, 0x40, 0x0B, 0x00, 0x00, 0x00, 0x00, 0xFE,
    0x01, 0x00, 0x00, 0x0A, 0x04, 0x00, 0x01, 0x58, 0xF4, 0xFB
};
#define HISENSE_LINK_INIT_07_LEN 20
static const uint8_t HISENSE_LINK_INIT_07[HISENSE_LINK_INIT_07_LEN] = {
    0xF4, 0xF5, 0x00, 0x40, 0x0B, 0x00, 0x00, 0x01, 0x01, 0xFE,
    0x01, 0x00, 0x00, 0x07, 0x01, 0x00, 0x01, 0x54, 0xF4, 0xFB
};
#define HISENSE_LINK_HEARTBEAT_LEN 28
static const uint8_t HISENSE_LINK_HEARTBEAT[HISENSE_LINK_HEARTBEAT_LEN] = {
    0xF4, 0xF5, 0x00, 0x40, 0x13, 0x00, 0x00, 0x01, 0x01, 0xFE,
    0x01, 0x00, 0x00, 0x1E, 0x00, 0x00, 0xB0, 0x80, 0x20, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x03, 0x02, 0xF4, 0xFB
};

// Status (A/C -> module) frame: 16-byte header (TYPE=0x01, class-id
// byte[13]=0x66) + body + [chk_hi][chk_lo][F4][FB].
//
// LENGTH IS LEN-DRIVEN, CONFIRMED on real W41H1 hardware (live capture):
// total length = byte[4] + 9. This unit's status frame is **160 bytes**
// (byte[4]=0x97=151 -> 151+9=160), NOT the 82 bytes the AEH-W4A1 reference's
// `sizeof(Device_Status)` implied. The 82-byte assumption dropped every real
// status frame. The early body fields (fan@16, mode/run@18, setpoint@19,
// current@20, flags1@35, flags2@36) sit at the SAME offsets as the reference
// -- the 160-byte frame is the reference layout plus trailing telemetry.
// The status frame is 160 bytes as observed, but the parser/RX compute length
// from byte[4] so they stay correct regardless.
#define HISENSE_STATUS_HEADER_LEN  16

/* ---------------------------------------------------------------------------
 * Value maps -- CONFIRMED via `raw = value*2 + 1` arithmetic against every
 * sample frame in messages.h (fan speed @ body offset 0 / mode nibble @
 * body offset 2 / target temp @ body offset 3 / sleep level @ body offset 1).
 * This supersedes the AUTO=0,LOWER=5,LOW=6,MEDIUM=7,HIGH=8,HIGHER=9 fan table
 * in docs/05 (that table came from deiger/AirCon and does not match what
 * esphome_airconintl actually sends on the wire for this bus).
 * -------------------------------------------------------------------------*/
typedef enum {
    HISENSE_MODE_FAN  = 0,
    HISENSE_MODE_HEAT = 1,
    HISENSE_MODE_COOL = 2,
    HISENSE_MODE_DRY  = 3,
    HISENSE_MODE_AUTO = 4,   // Enum value = COMMAND index (so the builder's
                              // (mode*2+1)<<4 gives the CONFIRMED AUTO command
                              // byte18 = 0x90, sniffed from the stock dongle).
                              // NOTE the STATUS frame reports AUTO as nibble 5
                              // OR 6 (value 4 is skipped), so hisense_parse_status
                              // remaps status nibble 5/6 -> AUTO. Command index 4
                              // and status value 5/6 genuinely differ for AUTO;
                              // both hardware-confirmed (bus-tap 2026-07-08).
} HisenseMode;

// Enum value = W41H1 fan INDEX, so the command builder's (fan*2+1) yields the
// CONFIRMED command byte16 values sniffed from the stock dongle. The W41H1 has
// SIX speeds (vs the reference's three) and uses indices 5..9 for low..high --
// NOT the reference's 2..4 (which is why the old LOW=2 -> 0x05 was wrong; the
// real LOW command is 0x0B). Status byte16 = index*2 (even); command = index*2+1.
typedef enum {
    HISENSE_FAN_AUTO     = 0,   // cmd 0x01 / status 0x01   CONFIRMED
    HISENSE_FAN_QUIET    = 1,   // cmd 0x03 / status 0x02   (mute; VERIFY cmd)
    HISENSE_FAN_LOW      = 5,   // cmd 0x0B / status 0x0A   CONFIRMED
    HISENSE_FAN_MED_LOW  = 6,   // cmd 0x0D / status 0x0C   CONFIRMED
    HISENSE_FAN_MID      = 7,   // cmd 0x0F / status 0x0E   CONFIRMED
    HISENSE_FAN_MED_HIGH = 8,   // cmd 0x11 / status 0x10   CONFIRMED
    HISENSE_FAN_HIGH     = 9,   // cmd 0x13 / status 0x12   CONFIRMED
    HISENSE_FAN_NOCHANGE = 0xFF, // sentinel: "keep previous fan"; NEVER packed on the wire.
                                 // Returned by hisense_fan_raw_to_cmd() for an unknown raw so a
                                 // transient garbled status can't clobber the shadow to AUTO (#59).
} HisenseFanSpeed;

typedef enum {
    HISENSE_SWING_OFF       = 0,  // fixed / no swing command
    HISENSE_SWING_DIRECTION = 1,  // move to a set position, no oscillation
    HISENSE_SWING_SWING     = 3,  // oscillate (2-bit field value 0b11)
} HisenseSwingMode;

typedef enum {
    HISENSE_FEATURE_NONE    = 0,   // byte33=0x04 (neutral / turbo-off, confirmed)
    HISENSE_FEATURE_ECO     = 1,   // byte33=0x30 (eco-on, confirmed W41H1)
    HISENSE_FEATURE_TURBO   = 2,   // byte33=0x0C (turbo-on, confirmed W41H1)
    HISENSE_FEATURE_ECO_OFF = 3,   // byte33=0x10 explicit eco-clear (P3c). NONE's
                                    // 0x04 clears TURBO but is NOT the eco-off value;
                                    // send ECO_OFF to clear eco. VERIFY on hardware.
    // VERIFY: eco and turbo are mutually exclusive in this driver because no
    // sample frame shows both bits combined in body offset 17 (0x34/0x14 =
    // eco on/off, 0x5C/0x54 = turbo on/off, 0x04 = neither). Requesting both
    // simultaneously will just pick turbo.
} HisenseFeature;

/* Panel display / LED, frame[36] (= payload @20). All three values CONFIRMED on the
 * bench 2026-07-19 against a live W41H1 (issue #52): 0x40 darkened the panel and 0xC0
 * lit it again, with every other byte held at the unit's current state.
 *
 * The tri-state is REQUIRED, not a nicety. `display` rides the combined command frame,
 * so it is present on every mode / setpoint / fan / swing change. A plain bool mapping
 * false->0x40 would force the display off on every one of those commands; NOCHANGE's
 * 0x00 ("leave alone") is what keeps ordinary traffic from touching the panel. Only an
 * explicit display request should send ON/OFF, and it should be one-shot (reset to
 * NOCHANGE after the frame goes out), otherwise later frames keep re-asserting it and
 * fight the user's remote. */
typedef enum {
    HISENSE_DISPLAY_NOCHANGE = 0,  // frame[36]=0x00 leave the panel alone (default)
    HISENSE_DISPLAY_ON       = 1,  // frame[36]=0xC0 CONFIRMED lights the panel
    HISENSE_DISPLAY_OFF      = 2,  // frame[36]=0x40 CONFIRMED darkens the panel
} HisenseDisplay;

/* ---------------------------------------------------------------------------
 * Command struct: everything hisense_build_command() can pack into ONE
 * combined "write" frame.
 *
 * NOTE: multi-field frames ARE protocol-legal -- the reference's own
 * `mode_cool`/`mode_heat`/`mode_dry`/`mode_fan` frames each carry mode + fan
 * + temp + several companion bytes in ONE frame (e.g. mode_cool sets fan
 * 0x01 @16, mode 0x50 @18, temp 0x35 @19, plus 0x10/0x01/0x10 companions).
 * The reference sends one canned frame per user action only because it works
 * from a fixed table, not because the bus rejects combined writes. This
 * builder packs mode+temp+fan+swing+eco/turbo+display into one frame; the
 * per-field offsets are individually confirmed and don't collide bitwise.
 * Still bench-test each field alone first, THEN combined, before trusting it
 * -- the companion bytes the reference sets on mode frames (0x31/0x32/0x35 =
 * 0x10/0x01/0x10) are NOT reproduced here and their purpose is unconfirmed.
 * Power on/off is intentionally kept OUT of this struct -- see
 * hisense_build_power_frame().
 * -------------------------------------------------------------------------*/
typedef struct {
    HisenseMode      mode;
    int8_t           setpoint;         // whole degrees, unit per `fahrenheit`
    bool             fahrenheit;       // false = Celsius (16-32 range seen),
                                        // true = Fahrenheit (61-90 range seen)
    HisenseFanSpeed  fan;
    HisenseSwingMode vswing;
    HisenseSwingMode hswing;
    HisenseFeature   feature;          // eco / turbo / none
    HisenseDisplay   display;          // panel display/LED. CONFIRMED on the bench
                                        // 2026-07-19 (see HisenseDisplay).
} HisenseCommand;

/* ---------------------------------------------------------------------------
 * Parsed status, extracted from a 160-byte W41H1 status frame.
 * -------------------------------------------------------------------------*/
typedef struct {
    bool     valid;             // false until first good frame parsed
    bool     power_on;          // run_status != 0 (byte18 bits2-3)
    HisenseMode mode;           // byte18 bits4-7
    int8_t   indoor_temp_c;     // offset 20, DIRECT integer C. CONFIRMED on
                                 // hardware: 21 C room -> 0x15. (NOT the
                                 // reference's (raw-32)*0.5556 -- that was the
                                 // bug that gave -5 C.)
    int8_t   setpoint_c;        // offset 19, DIRECT integer C. CONFIRMED:
                                 // 22 C setpoint -> 0x16.
    uint8_t  fan_raw;           // offset 16 wind_status. CONFIRMED W41H1 values:
                                 // 0x01=AUTO, 0x0A=LOW, 0x0C=MED-LOW, 0x0E=MID,
                                 // 0x10=MED-HIGH, 0x12=HIGH (0x02=quiet). Six
                                 // speeds -- more than the reference's three;
                                 // steps of 2. (COMMAND-side fan encoding is a
                                 // separate 2n+1 scheme, see HisenseFanSpeed.)
    bool     vswing_on;         // offset 35 bit7 (0x80). CONFIRMED on hw.
    bool     turbo_on;          // offset 35 bit1 (0x02). CONFIRMED on hw (app
                                 // Turbo/Boost toggled it; also forces fan high
                                 // + setpoint to 16 C). New W41H1 status field.
    bool     eco_on;            // offset 35 bit2 (0x04). CONFIRMED on hw (app
                                 // Eco/Power-save). NOTE: bit2, not the
                                 // reference's bit3 low_power.
    bool     hswing_on;         // offset 35 bit6 (0x40, left_right). CONFIRMED
                                 // on hw via the remote (0x40<->0x00).
    bool     heat_relay_on;     // offset 35 bit4 (0x10) = aux/PTC electric-heat
                                 // relay. Position confirmed; stays 0 during
                                 // normal heat-pump heating (verified HEAT +
                                 // 50Hz), only asserts in cold/defrost.
    bool     mute_on;           // offset 36 bit2 (0x04). CONFIRMED on hw (app
                                 // Mute/Quiet; also sets fan_raw=0x02 quiet).
    bool     sleep_on;          // offset 17 != 0. CONFIRMED on hw (also drops
                                 // fan to low when on).
    uint8_t  sleep_raw;         // raw offset-17 byte = sleep PROFILE, CONFIRMED
                                 // on hw: 0x00 off, 0x02 General, 0x04 Old,
                                 // 0x06 Young, 0x08 Kids (= profile*2, 1..4).
                                 // The COMMAND side uses profile*2+1 (odd:
                                 // 0x03/05/07/09, matching the reference's
                                 // sleep_1..4 frames).
    bool     purify_on;         // offset 36 bit5 (0x20). Not present on this
                                 // unit's app -- unconfirmed.
    int8_t   outdoor_temp_c;    // offset 44, direct C. CONFIRMED vs a weather
                                 // station (sensor 32-33 / station 33-34).
    int8_t   coil_temp_c;       // offset 45, outdoor/condenser coil temp, direct C.
                                 // CONFIRMED by cool->off->heat reversal (33->25->17;
                                 // reverses, unlike outdoor@44). docs/03 diag map.
    uint8_t  compressor_freq;   // offset 42, Hz. CONFIRMED: 0 stopped, ramps
                                 // 24->42->55 under load. (56/144 = frame
                                 // counter, not Hz; offset 43 = likely target.)
    uint8_t  current_raw;       // offset 55: current PROXY. Calibrated 2026-07-07
                                 // vs a panel meter: active power P[W] = 4.15*raw^2
                                 // (raw tracks sqrt(power)). Feeds the energy
                                 // clusters. See firmware/docs/09 + power_estimate.h.
    uint8_t  voltage_raw;       // offset 50: supply voltage, whole V (coarse ~220,
                                 // reads ~6% low vs the panel meter). docs/09.
} HisenseState;

/* ---------------------------------------------------------------------------
 * Feature-capability/state flags, parsed from the A/C's 0x66/40 "ProductType"
 * response. Bit positions recovered by static RE of the stock dongle firmware
 * (Ghidra decomp of the ProductType parser @0x9b6f0c4c) -- each field's
 * <payload byte, bit> is from that function. Offsets are RELATIVE to the class
 * byte (frame byte 13 = 0x66); the parser indexes buf[13 + N].
 * -------------------------------------------------------------------------*/
typedef struct {
    bool    valid;              // false until a 0x66/40 response has been parsed
    bool    cool_heat;         // [ 5]&0x80  ac_cool_heat  (heat-pump capable)
    bool    ai;                // [15]&0x40  ac_ai         (AI/SMART mode)
    bool    infinite_fan;      // [12]&0x08  ac_infinite_fan_speed
    bool    power_save;        // [10]&0x40  ac_power_save (eco)
    bool    fan_mute;          // [11]&0x40  ac_fan_mute   (quiet)
    bool    swing_dir_8;       // [15]&0x10  ac_swing_direction_8 (8-pos louvre)
    bool    swing_follow;      // [13]&0x02  ac_swing_follow
    uint8_t power_display;     // [14]>>6    ac_power_display (2-bit: display/LED)
    uint8_t demand_resp;       // [22]&0x03  ac_dr          (2-bit: demand response)
    bool    humidity;          // [19]&0x01  ac_humidity
    // RENAMED 2026-07-16 -- these two were MISLABELED. The byte reads were always correct; the
    // NAMES were swapped-ish, so `purify` reported 8heat and `q_display` reported purify. Caught
    // by RE of the stock printf arg order (0x9b6f0d60) and corroborated by this repo's own
    // [PROVEN] flag table in RE docs/10 §5a. No behaviour change -- same bytes, right names.
    bool    heat_8c;           // [0x0D]&0x80  ac_8heat  (8 C frost-guard heat; was `purify`)
    bool    purify;            // [0x0A]&0x08  ac_purify (was `q_display`)

    // --- Extended tier: payload [0x19]/[0x1A] = frame bytes 38/39 -----------
    // Stock gates the higher fields on payload length (docs/10 §5a: `len-2 ∈
    // {>0x14, >0x17, >0x18}`), so a valid-but-short 0x66/40 reply simply does not
    // carry them. We mirror that: the base tier above parses from len >= 36, and
    // these three need len >= 40. `ext_valid` says which happened -- without it a
    // `0` here is ambiguous between "this unit lacks the feature" and "the frame
    // was too short to say", and docs/11 §5.1's design rule (gate at runtime, per
    // unit) depends on telling those two apart.
    bool    ext_valid;         // false => the three fields below are UNKNOWN, not 0
    bool    q_display;         // [0x1A]&0x40  ac_q_display  (the REAL q_display)
    bool    enable_8heat;      // [0x1A]&0x04  ac_enable_8heat
    bool    trans_102_64;      // [0x19]&0x08  ac_trans_102_64 (set -> stock profile '199')
    uint8_t reply_len;         // raw 0x66/40 frame length that produced this parse (0 = unset,
                               // capped at 255). Diagnostic only: it disambiguates an
                               // `ext_valid == false` -- a 38-byte reply is 2 bytes short of the
                               // tier, whereas a >39-byte reply with ext_valid false would mean a
                               // parser bug. No consumer should gate behaviour on it.
} HisenseFeatures;

/* Feature-flags callback: invoked (bus-task context) each time a 0x66/40
 * ProductType response is parsed. Optional. */
typedef void (*hisense_features_cb_t)(const HisenseFeatures *features);

/* ---------------------------------------------------------------------------
 * RX callback: invoked from the RX task context (NOT an ISR) whenever a
 * complete, checksum-valid status frame has been parsed.
 * -------------------------------------------------------------------------*/
typedef void (*hisense_status_cb_t)(const HisenseState *state);

/* ---------------------------------------------------------------------------
 * Recommission ("77") callback: invoked from the bus-task context when the A/C's
 * 0x1E LINK reply carries a re-provision request. The remote "Horizon Airflow x6
 * -> 77" makes the mainboard set a request bit in payload[4] (frame byte[17]) of
 * its 0x1E reply -- firmware-RE of the stock dongle, see
 * reverse-engineering/docs/02 "Recommission (77)":
 *     bit3 (0x08) = reset / reconfigure      bit5 (0x20) = smart-config pairing
 * `reason` is the raw payload[4] byte so the handler can tell the two apart. It
 * fires ONCE per sustained assertion -- the request must persist for
 * HISENSE_RECOMMISSION_HOLD_FRAMES consecutive ~1Hz replies before it fires (a
 * debounce; bit3 doubles as our outbound prov_status, so a single reflected/
 * glitched frame must not trip a window). The handler opens an on-network
 * commissioning window (see matter_drivers.cpp).
 * -------------------------------------------------------------------------*/
#define HISENSE_LINK_REQ_RECONFIG      0x08   // payload[4] bit3: reset / reconfigure
#define HISENSE_LINK_REQ_SMARTCFG      0x20   // payload[4] bit5: smart-config pairing
#define HISENSE_LINK_REQ_RECOMMISSION  (HISENSE_LINK_REQ_RECONFIG | HISENSE_LINK_REQ_SMARTCFG)
#define HISENSE_RECOMMISSION_HOLD_FRAMES 3    // ~3s at ~1Hz: a held "77" press, not a glitch/echo
typedef void (*hisense_recommission_cb_t)(uint8_t reason);

// Pure debounce step for the "77" request (exposed for host tests). Advances the
// streak/latch state for one 0x1E reply's masked request bits and returns true
// exactly once, when the request has been asserted for >= hold_frames in a row.
bool hisense_recommission_debounce(uint8_t req_bits, uint8_t *streak,
                                   bool *latched, uint8_t hold_frames);

// Pure reply-class gate (exposed for host tests). A transaction accepts a parsed
// frame only when its class (frame byte[13]) matches what the request expected;
// expect_class == 0 means "accept any class" (#60). Prevents a late/stale reply of
// the wrong class from being consumed as the answer to the current request.
static inline bool hisense_reply_class_ok(uint8_t got_class, uint8_t expect_class)
{
    return expect_class == 0 || got_class == expect_class;
}

// ---------------------------------------------------------------------------
// Bus-link health (#56). The bus task tracks consecutive missed status polls;
// when the count crosses HISENSE_LINK_LOST_POLLS the link is considered lost, and
// restored on the next good poll. A registered callback fires ONCE on each edge so
// the Matter layer can mark liveness attributes unavailable instead of holding
// stale values (stock returns status-7 on sustained silence). The callback runs in
// the bus-task context (NOT an ISR).
// ---------------------------------------------------------------------------
typedef void (*hisense_link_cb_t)(bool link_up);  // true = restored, false = lost
void hisense_set_link_cb(hisense_link_cb_t cb);

// Pure link-health edge detector (exposed for host tests). `silent` = link currently
// considered lost; `*was_down` carries the latch across calls. Returns +1 on the
// lost edge, -1 on the restored edge, 0 otherwise (fires each edge exactly once).
int hisense_link_health_edge(bool silent, bool *was_down);

/* ---------------------------------------------------------------------------
 * Public API
 *
 * Threading model (matches the stock firmware and the reference project):
 *   - RX is INTERRUPT-driven. hisense_init() installs a UART RxIrq handler
 *     that pushes bytes into a ring buffer; a single "bus" task drains the
 *     ring, reassembles F4 F5 .. F4 FB frames (un-stuffing doubled F4), and
 *     invokes `cb` on each valid status frame.
 *   - TX is SERIALIZED through a queue. Callers never touch the UART
 *     directly; they enqueue whole frames with hisense_send_frame() and the
 *     bus task sends each as a transaction (send, then listen up to a 500 ms
 *     ACK timeout for the A/C's reply) and paces the cycle to ~1 Hz -- so the
 *     poll task and the Matter uplink handler can both submit frames without
 *     their bytes interleaving on the wire.
 *
 * The bus is half-duplex: the bus task drives the PA_17 DE line HIGH to
 * transmit and LOW to receive (see the pin notes above); RX is muted while
 * we send.
 * -------------------------------------------------------------------------*/

// Initializes the UART (9600 8N1, PA_14/PA_13), installs the RX interrupt,
// and starts the single bus task. `cb` is invoked (from the bus task, NOT an
// ISR) each time a valid status frame arrives. Returns pdPASS/pdFAIL.
int hisense_init(hisense_status_cb_t cb);

// Registers the "77" recommission callback (optional; see the typedef above).
// Safe to call before or after hisense_init.
void hisense_set_recommission_cb(hisense_recommission_cb_t cb);

// Report provisioning state to the A/C: while true, the outbound 0x1E carries
// prov_status=1 (payload[4] bit3), which lights "77" on the A/C panel (the module
// telling the mainboard it's pairing). Set true when the commissioning window opens,
// false when it closes. Bench-confirmed 2026-07-09 (injecting that bit showed "77").
void hisense_set_provisioning(bool on);

// Promptly clear provisioning + push one online 0x1E, so the A/C drops "77" (e.g.
// after a commissioning window times out with no new pairing) without waiting for
// the next ~1Hz heartbeat.
void hisense_send_exit_77(void);

void hisense_deinit(void);

// Builds a combined command frame (see HisenseCommand note above).
// Writes into `out` (must be >= HISENSE_CMD_FRAME_LEN + 2 bytes = 52; the +2 covers a
// possible byte-stuffed 0xF4 checksum) and returns the
// frame length, or 0 on error (buffer too small / setpoint out of range).
// The returned length INCLUDES any byte-stuffing of an 0xF4 checksum byte,
// so it may be HISENSE_CMD_FRAME_LEN or slightly larger -- always use the
// return value, never assume HISENSE_CMD_FRAME_LEN.
/* Sample-confirmed setpoint ranges. Exposed (rather than hardcoded in the builder)
 * because CALLERS must validate too: a setpoint outside these bounds makes
 * hisense_build_command() return 0, and an app layer that copies an out-of-range
 * setpoint into its command shadow will then silently drop EVERY later combined
 * frame. That is not hypothetical -- see hisense_setpoint_in_range(). */
#define HISENSE_SETPOINT_MIN_C   16
#define HISENSE_SETPOINT_MAX_C   32
#define HISENSE_SETPOINT_MIN_F   61
#define HISENSE_SETPOINT_MAX_F   90

/* True if `setpoint` is a value the command builder will accept for the given unit.
 *
 * Call this before copying an A/C-reported setpoint into a command shadow. The A/C can
 * legitimately report a value outside these bounds: this hardware answers ac_8heat=1
 * (8 C frost-guard heat), and the bench confirmed it will report, and hold, setpoints
 * well below 16 C (5 C was accepted verbatim). Copying such a value into the shadow
 * poisons every subsequent hisense_build_command() call, which returns 0 and takes the
 * whole frame with it -- mode, fan, swing and all -- with no error on the wire.
 *
 * Dry / Fan-only strip the setpoint from the frame entirely, so the builder skips the
 * range check for those modes (#53); this helper reports the raw range only. */
static inline bool hisense_setpoint_in_range(int8_t setpoint, bool fahrenheit)
{
    return fahrenheit
        ? (setpoint >= HISENSE_SETPOINT_MIN_F && setpoint <= HISENSE_SETPOINT_MAX_F)
        : (setpoint >= HISENSE_SETPOINT_MIN_C && setpoint <= HISENSE_SETPOINT_MAX_C);
}

size_t hisense_build_command(const HisenseCommand *cmd, uint8_t *out, size_t out_cap);

// BENCH ONLY (#52 display-byte hunt). Same frame as hisense_build_command(), with
// exactly ONE pre-checksum byte replaced. Every other byte stays at the caller's
// current known-good state, so a failed probe is a no-op rather than a surprise
// mode/setpoint change -- that is what makes an offset sweep safe to run against a
// live A/C.
//
// `ovr_off` is an ABSOLUTE frame offset and must land in the payload:
// [HISENSE_CMD_HEADER_LEN, HISENSE_CMD_CHK_OFFSET). Header and checksum/terminator
// offsets are rejected (returns 0) because patching them yields a frame the A/C
// simply drops, which is indistinguishable from "this byte does nothing" and would
// poison a sweep with false negatives.
//
// Returns the on-the-wire length (byte-stuffing included), or 0 on a rejected
// offset / the same errors as hisense_build_command().
size_t hisense_build_command_override(const HisenseCommand *cmd, uint8_t *out, size_t out_cap,
                                      int ovr_off, uint8_t ovr_val);

// Builds the literal power on/off frame, ported byte-for-byte from
// messages.h `on[]`/`off[]` (NOT synthesized -- these carry several bytes
// whose individual semantics are unconfirmed, see hisense_rs485.cpp).
size_t hisense_build_power_frame(bool power_on, uint8_t *out, size_t out_cap);

// Builds the status-request poll frame (verbatim HISENSE_STATUS_REQUEST).
size_t hisense_build_status_request(uint8_t *out, size_t out_cap);

// Builds the 0x66/40 "ProductType" feature-flag poll -- the status request with
// subtype 0x40 and a recomputed checksum (stock body `66 40 00 00`, RE'd from the
// dongle firmware). Same length as the status request.
size_t hisense_build_producttype_request(uint8_t *out, size_t out_cap);

// Parse a received 0x66/40 ProductType response (full frame, class @buf[13]) into
// the feature flags. Returns false on a too-short/wrong-class/wrong-subtype frame.
// Pure -> host-testable.
bool hisense_parse_features(const uint8_t *buf, size_t len, HisenseFeatures *out);

// Registers the feature-flags callback (optional; fired on each 0x66/40 response).
void hisense_set_features_cb(hisense_features_cb_t cb);

// Copies the last-parsed feature flags into *out. Returns false if none received yet.
bool hisense_get_features(HisenseFeatures *out);

// Single-field toggle frames for controls the combined HisenseCommand doesn't
// carry (used by the manufacturer-cluster glue). Return the (possibly stuffed)
// frame length, 0 on error. Confirmed bytes: mute @35 = 0x30 on / 0x10 off;
// sleep @17 = profile*2+1 (profile 0=off->0x01, 1..4=General/Old/Young/Kids).
size_t hisense_build_mute_frame(bool on, uint8_t *out, size_t out_cap);
size_t hisense_build_sleep_frame(uint8_t profile, uint8_t *out, size_t out_cap);

// Enqueues `len` bytes for transmission by the paced bus task. Copies the
// buffer, so `buf` may be a local. Returns true if queued, false if the
// queue is full or the driver isn't initialized. This is the ONLY supported
// way to transmit -- use it from any task; never write the UART directly.
bool hisense_send_frame(const uint8_t *buf, size_t len);

// Validates framing (F4 F5 start / F4 FB end / length / checksum) and
// extracts fields from a caller-supplied status frame buffer (already
// un-stuffed). Returns true on a fully valid, parsed frame.
bool hisense_parse_status(const uint8_t *buf, size_t len, HisenseState *out_state);

// #49 (outbound envelope [7]/[8]) -- LEARNED FROM THE A/C, not hardcoded.
//
// These bytes are NOT a per-session token. Stock seeds them from the reply envelope
// [9]/[10] but then overwrites them with the DevType (class 0x0A) reply's INNER
// payload [3]/[4] = (device-type code, sub-type), and post-handshake that meaning is
// authoritative (RE docs/10 §4.5 [PROVEN]). They are a static per-model identifier --
// which is why hardcoding this unit's 01 01 worked at all.
//
// ⚠ Stamping the raw envelope [9]/[10] instead (docs/10's one INFERRED claim: that the
// two sources coincide on the DevType frame) was shipped as v10207 and KILLED the link
// -- the A/C rejected every frame and no status came back. They do not coincide.
//
// hisense_transact() therefore: sends the DevType probe verbatim (pre-link 00 00),
// latches the type from its reply, and stamps it on every later frame. Defaults to the
// known-good 01 01, so a unit whose probe never answers behaves as the pre-#49 build.
//   - hisense_devtype_from_reply(): PURE extractor of inner [3]/[4] from a 0x0A reply
//     (host-testable; the offset is the risky bit). False for any other class.
//   - hisense_stamp_link_token(): PURE re-stamp of a finished frame's [7]/[8] +
//     checksum fixup (the checksum sums over [7]/[8]). Returns the new length, or
//     0 if the envelope is unrecognized.
//   - hisense_get_link_token(): the live pair; false until a 0x0A reply supplied it
//     (the bytes are then the 01 01 default).
bool hisense_devtype_from_reply(const uint8_t *reply, size_t n, uint8_t *hi, uint8_t *lo);
size_t hisense_stamp_link_token(const uint8_t *in, size_t len, uint8_t hi, uint8_t lo,
                                uint8_t *out, size_t out_cap);
bool hisense_get_link_token(uint8_t *hi, uint8_t *lo);
//   - hisense_get_devtype_envelope(): DIAG. The 0x0A reply's raw envelope [9]/[10] -- the
//     value v10207 stamped and died on. If this differs from hisense_get_link_token()'s
//     pair, the "session token" reading is disproven on the wire. False until a 0x0A reply.
bool hisense_get_devtype_envelope(uint8_t *hi, uint8_t *lo);

#ifdef __cplusplus
}
#endif
