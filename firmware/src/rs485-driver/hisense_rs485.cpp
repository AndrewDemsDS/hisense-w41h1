/********************************************************************************
  *
  * hisense_rs485.cpp
  *
  * See hisense_rs485.h for the full provenance notes. This file implements:
  *   - UART init (9600 8N1 on PA_14/PA_13, firmware-confirmed; no DE/RE)
  *   - an interrupt-driven RX ring buffer + a single "bus" task that
  *     reassembles F4 F5 .. F4 FB frames (un-stuffing doubled F4)
  *   - a paced TX queue (all senders enqueue; the bus task applies the
  *     reference project's send timing so frames never interleave)
  *   - hisense_build_command() / hisense_build_power_frame() / status-request,
  *     including TX-side byte-stuffing of an 0xF4 checksum byte
  *   - hisense_parse_status()
  *
  * Pins, checksum width, framing and the "no DE/RE" fact are all confirmed
  * against the stock W41H1 firmware dump (see UART_PINS.md and the
  * disassembly in reverse-engineering/docs/03-rs485-ac-protocol.md).
  *
********************************************************************************/

#include "hisense_rs485.h"

#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Module state
 * -------------------------------------------------------------------------*/
static serial_t             s_uart;
static gpio_t               s_de;              // half-duplex driver-enable (PA_17)
static hisense_status_cb_t  s_status_cb        = NULL;
static TaskHandle_t         s_bus_task_handle  = NULL;
static bool                 s_initialized      = false;
static hisense_recommission_cb_t s_recommission_cb = NULL;
static uint8_t              s_last_link_req    = 0;   // masked "77" request bits from the last 0x1E reply (diag)
static uint8_t              s_link_req_streak  = 0;   // consecutive 0x1E replies with the request asserted (debounce)
static bool                s_recommission_latched = false; // fired for the current sustained assertion (fire-once)
static bool                s_prov_active      = false; // report prov=1 in outbound 0x1E -> A/C shows "77"
static hisense_link_cb_t   s_link_cb          = NULL;  // fires on link lost/restored edges (#56)
static bool                s_link_down        = false; // link-health latch for the edge detector (#56)
static hisense_features_cb_t s_features_cb    = NULL;  // fires on each 0x66/40 ProductType response
static HisenseFeatures     s_features;                 // last-parsed feature flags
static bool                s_features_valid   = false; // true once a 0x66/40 response has been parsed
// #49: link session token captured from A/C reply envelope [9]/[10] (stock 0x9b6f33ae).
// OBSERVE-ONLY (Phase A): captured + exposed via hisense_get_link_token(), NOT yet stamped
// into outgoing frames. Seeded to 01 01 (this unit's value) so the getter reads sanely pre-link.
static uint8_t             s_link_tok[2]      = { 0x01, 0x01 };
static bool                s_link_tok_seen    = false; // true once a reply has updated the token

/* RX ring buffer, filled from the UART RxIrq, drained by the bus task. */
#define HISENSE_RX_RING_SIZE 256   // power of two
static volatile uint8_t     s_rx_ring[HISENSE_RX_RING_SIZE];
static volatile uint16_t    s_rx_head = 0;   // written by ISR
static volatile uint16_t    s_rx_tail = 0;   // written by task

/* TX queue of whole frames. A frame is a length-prefixed byte blob. */
#define HISENSE_TX_MAX_FRAME 64
typedef struct {
    uint8_t len;
    uint8_t data[HISENSE_TX_MAX_FRAME];
} HisenseTxItem;
static QueueHandle_t        s_tx_queue = NULL;

/* Half-duplex direction (PA_17 DE) timing, from the stock byte-writer
 * (docs/09): assert DE, settle, send, drain, deassert. */
#define HISENSE_DE_SETTLE_MS   5   // DE-high settle before the first byte
#define HISENSE_DE_DRAIN_MS   25   // hold DE high after last byte until the TX
                                   // FIFO+shift drains (<=16B @9600 ~= 17ms);
                                   // releasing early truncates our frame's tail

/* ---------------------------------------------------------------------------
 * Checksum: big-endian sum over bytes [start, end_exclusive). Callers pass
 * start=2 (skip F4 F5) and end_exclusive = offset of the first checksum byte.
 *
 * CONFIRMED against the stock firmware parser: the checksum is a plain running
 * byte-sum (Thumb `ldrb r2,[r3,#1]!; add r1,r2` loop), and its WIDTH is chosen
 * by CTRL & 0xC0 (0x40 -> 2 bytes big-endian for every command/status frame
 * here). Also re-derived arithmetically against all ~75 sample frames in
 * pslawinski/esphome_airconintl messages.h -- matched on every frame,
 * including temp_16_C (whose low checksum byte 0xF4 is byte-stuffed on the
 * wire; that is the escape rule, NOT an upstream typo -- see the stuffing in
 * hisense_stuff_checksum() below).
 * -------------------------------------------------------------------------*/
static uint16_t hisense_checksum_range(const uint8_t *frame, size_t start, size_t end_exclusive)
{
    uint32_t sum = 0;
    for (size_t i = start; i < end_exclusive; i++) {
        sum += frame[i];
    }
    return (uint16_t)(sum & 0xFFFF);
}

/* ---------------------------------------------------------------------------
 * Finalize a fully-populated frame in place: compute the 2-byte big-endian
 * checksum over [2, chk_offset), store it (hi,lo) at chk_offset, then write the
 * F4 FB end tag at end_offset. Behavior-identical to the hand-inlined sequence
 * it replaces -- checksum range/width and the byte offsets are unchanged; the
 * bytes written are exactly the same. (For the 0x1E link frame end_offset lands
 * on the template's own F4 FB, so the tag is rewritten with identical values.)
 * -------------------------------------------------------------------------*/
static void finalize_frame(uint8_t *frame, size_t chk_offset, size_t end_offset)
{
    uint16_t chk = hisense_checksum_range(frame, 2, chk_offset);
    frame[chk_offset]     = (uint8_t)(chk >> 8);
    frame[chk_offset + 1] = (uint8_t)(chk & 0xFF);
    frame[end_offset]     = HISENSE_ETX1;
    frame[end_offset + 1] = HISENSE_ETX2;
}

/* ---------------------------------------------------------------------------
 * TX byte-stuffing.
 *
 * 0xF4 is the frame marker byte; any 0xF4 that occurs INSIDE a frame (i.e. in
 * a checksum byte -- payload bytes of 0x65 command frames never reach 0xF4)
 * must be doubled so the receiver's un-stuffer collapses it back and does not
 * mistake it for a start/end marker. The reference's temp_16_C frame proves
 * this: checksum 0x01F4 is emitted as `01 F4 F4` then the `F4 FB` end tag.
 *
 * `frame` holds a fully-built frame of `len` bytes whose last four bytes are
 * [chk_hi][chk_lo][F4][FB]. This rewrites `out` with the two checksum bytes
 * stuffed if needed, followed by the F4 FB end tag, and returns the new
 * length. `out` must have room for len+2. The F4 FB end tag and the F4 F5
 * start marker are NOT stuffed (they are real markers).
 * -------------------------------------------------------------------------*/
static size_t hisense_stuff_checksum(const uint8_t *frame, size_t len, uint8_t *out, size_t out_cap)
{
    // Layout: [0 .. len-5] body, [len-4] chk_hi, [len-3] chk_lo, [len-2]=F4, [len-1]=FB.
    if (len < 4 || out_cap < len + 2) {
        return 0;
    }
    size_t body = len - 4;          // bytes up to and excluding the checksum
    size_t o = 0;
    memcpy(out, frame, body);       // copy body verbatim (start marker included)
    o = body;
    // Stuff each checksum byte if it equals 0xF4.
    for (size_t i = body; i < body + 2; i++) {
        out[o++] = frame[i];
        if (frame[i] == HISENSE_STX1) {   // 0xF4
            out[o++] = HISENSE_STX1;      // double it
        }
    }
    out[o++] = HISENSE_ETX1;        // F4
    out[o++] = HISENSE_ETX2;        // FB
    return o;
}

/* ---------------------------------------------------------------------------
 * Frame builders
 * -------------------------------------------------------------------------*/
size_t hisense_build_status_request(uint8_t *out, size_t out_cap)
{
    if (out == NULL || out_cap < HISENSE_STATUS_REQUEST_LEN) {
        return 0;
    }
    memcpy(out, HISENSE_STATUS_REQUEST, HISENSE_STATUS_REQUEST_LEN);
    return HISENSE_STATUS_REQUEST_LEN;
}

// 0x66/40 ProductType feature-flag poll: the status request with subtype 0x40 and
// a recomputed 2-byte checksum. The stock builds body `66 40 00 00` (RE'd from the
// dongle, FUN_9b6f2b0c); our status request already carries `66 00 ...`, so flip
// byte14 and re-finalize (chk @17/18, F4 FB @19/20) via the validated machinery.
size_t hisense_build_producttype_request(uint8_t *out, size_t out_cap)
{
    if (out == NULL || out_cap < HISENSE_STATUS_REQUEST_LEN) {
        return 0;
    }
    memcpy(out, HISENSE_STATUS_REQUEST, HISENSE_STATUS_REQUEST_LEN);
    out[14] = 0x40;                 // subtype 0x00 (status) -> 0x40 (ProductType)
    finalize_frame(out, 17, 19);    // recompute checksum @17/18; F4 FB @19/20
    return HISENSE_STATUS_REQUEST_LEN;
}

// Parse a 0x66/40 ProductType response into the feature flags. buf is the full
// frame (class 0x66 @ buf[13]); offsets are buf[13 + N] per the stock parser
// (FUN_9b6f0c4c). Needs the payload out to byte 35 (buf[13+0x16]). Pure.
bool hisense_parse_features(const uint8_t *buf, size_t len, HisenseFeatures *out)
{
    if (buf == NULL || out == NULL) {
        return false;
    }
    if (len <= 35 || buf[13] != 0x66 || buf[14] != 0x40) {
        return false;
    }
    out->cool_heat     = (buf[18] & 0x80) != 0;   // [ 5]&0x80
    out->power_save    = (buf[23] & 0x40) != 0;   // [10]&0x40
    out->q_display     = (buf[23] & 0x08) != 0;   // [10]&0x08
    out->fan_mute      = (buf[24] & 0x40) != 0;   // [11]&0x40
    out->infinite_fan  = (buf[25] & 0x08) != 0;   // [12]&0x08
    out->purify        = (buf[26] & 0x80) != 0;   // [13]&0x80
    out->swing_follow  = (buf[26] & 0x02) != 0;   // [13]&0x02
    out->power_display = (uint8_t)((buf[27] >> 6) & 0x03); // [14]>>6
    out->ai            = (buf[28] & 0x40) != 0;   // [15]&0x40
    out->swing_dir_8   = (buf[28] & 0x10) != 0;   // [15]&0x10
    out->humidity      = (buf[32] & 0x01) != 0;   // [19]&0x01
    out->demand_resp   = (uint8_t)(buf[35] & 0x03); // [22]&0x03
    out->valid = true;
    return true;
}

void hisense_set_features_cb(hisense_features_cb_t cb)
{
    s_features_cb = cb;
}

bool hisense_get_features(HisenseFeatures *out)
{
    if (out == NULL || !s_features_valid) {
        return false;
    }
    *out = s_features;
    return true;
}

// Literal command templates ported byte-for-byte from messages.h `on[]` /
// `off[]`. NOT synthesized: several bytes in the `off` frame differ from every
// other single-purpose command and are not understood field-by-field -- only
// that this exact byte sequence powers the unit off. Their checksum low bytes
// (0xDF / 0x31) are not 0xF4, so no stuffing is needed for these two.
static const uint8_t HISENSE_FRAME_ON[HISENSE_CMD_FRAME_LEN] = {
    0xF4, 0xF5, 0x00, 0x40, 0x29, 0x00, 0x00, 0x01, 0x01, 0xFE, 0x01,
    0x00, 0x00, 0x65, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00,
    0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0xDF, 0xF4, 0xFB
};
static const uint8_t HISENSE_FRAME_OFF[HISENSE_CMD_FRAME_LEN] = {
    0xF4, 0xF5, 0x00, 0x40, 0x29, 0x00, 0x00, 0x01, 0x01, 0xFE, 0x01,
    0x00, 0x00, 0x65, 0x00, 0x00, 0x00, 0x01, 0x04, 0x00, 0x00, 0x00,
    0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01,
    0x55, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x02, 0x31, 0xF4, 0xFB
};

size_t hisense_build_power_frame(bool power_on, uint8_t *out, size_t out_cap)
{
    if (out == NULL || out_cap < HISENSE_CMD_FRAME_LEN) {
        return 0;
    }
    memcpy(out, power_on ? HISENSE_FRAME_ON : HISENSE_FRAME_OFF, HISENSE_CMD_FRAME_LEN);
    return HISENSE_CMD_FRAME_LEN;
}

// Single-field 0x65 frame: header + the 0x04 baseline @23 + one named byte set,
// then checksum + F4-stuffing. For toggles the combined HisenseCommand doesn't
// carry (mute, sleep). VERIFY: only the named byte is hardware-confirmed (mute
// @35=0x30/0x10, sleep @17=profile*2+1); the rest of the frame is the minimal
// baseline -- bench-check the dongle's exact mute/sleep frames if either is
// ignored by the A/C.
static size_t hisense_build_single_field(uint8_t off, uint8_t val, uint8_t *out, size_t cap)
{
    if (out == NULL || cap < HISENSE_CMD_FRAME_LEN + 2) {
        return 0;
    }
    uint8_t f[HISENSE_CMD_FRAME_LEN];
    memset(f, 0, sizeof(f));
    memcpy(f, HISENSE_CMD_HEADER, HISENSE_CMD_HEADER_LEN);
    f[23]  = 0x04;
    f[off] = val;
    finalize_frame(f, HISENSE_CMD_CHK_OFFSET, HISENSE_CMD_END_OFFSET);
    return hisense_stuff_checksum(f, HISENSE_CMD_FRAME_LEN, out, cap);
}

size_t hisense_build_mute_frame(bool on, uint8_t *out, size_t out_cap)
{
    return hisense_build_single_field(35, on ? 0x30 : 0x10, out, out_cap);
}

size_t hisense_build_sleep_frame(uint8_t profile, uint8_t *out, size_t out_cap)
{
    // profile 0 = off (0x01); 1..4 = General/Old/Young/Kids -> profile*2+1.
    // Reject out-of-range profiles instead of emitting an undefined wire byte
    // (e.g. profile=200 -> 0x91); unknown -> treat as OFF.
    if (profile > 4) profile = 0;
    uint8_t v = (profile == 0) ? 0x01 : (uint8_t)(profile * 2 + 1);
    return hisense_build_single_field(17, v, out, out_cap);
}

size_t hisense_build_command(const HisenseCommand *cmd, uint8_t *out, size_t out_cap)
{
    if (cmd == NULL || out == NULL || out_cap < HISENSE_CMD_FRAME_LEN + 2) {
        return 0;
    }

    // Range checks matching the sample-confirmed ranges (16-32C / 61-90F). Skipped
    // for Dry / Fan-only, where the setpoint is stripped from the wire anyway (#53)
    // -- an out-of-range (or stale) setpoint must not fail-and-drop the whole frame.
    if (cmd->mode != HISENSE_MODE_DRY && cmd->mode != HISENSE_MODE_FAN) {
        if (!cmd->fahrenheit && (cmd->setpoint < 16 || cmd->setpoint > 32)) {
            return 0;
        }
        if (cmd->fahrenheit && (cmd->setpoint < 61 || cmd->setpoint > 90)) {
            return 0;
        }
    }

    uint8_t frame[HISENSE_CMD_FRAME_LEN];
    memset(frame, 0, sizeof(frame));
    memcpy(frame, HISENSE_CMD_HEADER, HISENSE_CMD_HEADER_LEN);

    // Body offsets below are ABSOLUTE (from frame start). All CONFIRMED via
    // `raw = value*2+1` arithmetic against messages.h samples unless noted.

    // offset 16: fan speed, encode(level) = level*2 + 1 (auto/mute/low/med/max
    // = 01/03/05/07/09). 0x01 (=AUTO) is the "unchanged" filler in every
    // non-fan sample -- also emitted for the HISENSE_FAN_NOCHANGE sentinel, which
    // means "keep the A/C's current fan" and must never be packed literally (#59).
    frame[16] = (cmd->fan == HISENSE_FAN_NOCHANGE) ? 0x01 : (uint8_t)(cmd->fan * 2 + 1);

    // offset 17: sleep. We DON'T expose sleep in HisenseCommand, so write 0x00
    // = "don't touch". CONFIRMED command encoding (sniffed dongle) for when it
    // IS exposed: profile*2+1 -> General 0x03, Old 0x05, Young 0x07, Kids 0x09,
    // sleep-off 0x01. (Status side reports profile*2 = 0x02/04/06/08.)
    frame[17] = 0x00;
    // NOTE (mute command, not yet exposed): the stock dongle toggles MUTE via
    // byte35 = 0x30 (on) / 0x10 (off) -- add that if a mute command is wired.

    // offset 18 (upper nibble): mode, encode(mode) = mode*2 + 1, in bits 4-7.
    frame[18] = (uint8_t)((cmd->mode * 2 + 1) << 4);

    // offset 19: target temperature, encode(temp) = temp*2 + 1 (same scheme
    // for Celsius 16-32 and Fahrenheit 61-90; unit is selected by a SEPARATE
    // persistent switch command, not sent here).
    frame[19] = (uint8_t)(cmd->setpoint * 2 + 1);

    // #53 mode lockouts (match the stock app): Dry and Fan-only carry NO setpoint on
    // the wire, and Dry additionally carries no fan change. Use the confirmed
    // "unchanged" fillers -- setpoint byte19 = 0x00 ("don't touch"), fan byte16 = 0x01
    // (AUTO/unchanged) -- overriding whatever was packed above.
    if (cmd->mode == HISENSE_MODE_DRY || cmd->mode == HISENSE_MODE_FAN) {
        frame[19] = 0x00;              // strip setpoint
    }
    if (cmd->mode == HISENSE_MODE_DRY) {
        frame[16] = 0x01;              // strip fan
    }

    // offset 23: baseline 0x04 in every non-unit-switch sample.
    frame[23] = 0x04;

    // offset 31/32/37: swing. vert enable @31, packed axes @32 (vert bits 6-7,
    // hor bits 4-5), hor companion/enable 0x14 @37. Individually confirmed;
    // combined byte32 is a bitwise-non-overlapping construction (VERIFY the
    // combination on the bench).
    // Vertical command is ALWAYS active: "swing off" is not a no-op on this A/C,
    // it means "hold the louver at a fixed position" (byte32 vert=0x40). Emitting
    // 0x00 for OFF (the old behaviour) left the unit swinging -> off never worked.
    // So OFF and DIRECTION both map to the fixed value 0x1 (=>0x40); SWING=0x3(=>0xC0).
    frame[31] = 0x01;
    uint8_t vswing_bits = (cmd->vswing == HISENSE_SWING_SWING) ? 0x3 : 0x1;
    uint8_t hswing_bits = (cmd->hswing == HISENSE_SWING_SWING) ? 0x3 :
                           (cmd->hswing == HISENSE_SWING_DIRECTION) ? 0x1 : 0x0;
    frame[32] = (uint8_t)((vswing_bits << 6) | (hswing_bits << 4));
    if (cmd->hswing != HISENSE_SWING_OFF) {
        frame[37] = 0x14;
    }

    // offset 33: turbo / eco, mutually exclusive.
    switch (cmd->feature) {
    case HISENSE_FEATURE_TURBO:
        frame[33] = 0x0C; // turbo on. CONFIRMED on W41H1 (sniffed dongle cmd;
                           // was 0x5C from the reference). off = 0x04 (= NONE).
        break;
    case HISENSE_FEATURE_ECO:
        frame[33] = 0x30; // eco on. CONFIRMED on W41H1 (was 0x34).
        break;
    case HISENSE_FEATURE_ECO_OFF:
        frame[33] = 0x10; // explicit eco-clear (P3c). NONE's 0x04 clears turbo but
                           // is not the confirmed eco-off. Callers set feature to
                           // ECO_OFF once to clear eco, then back to NONE. VERIFY hw.
        break;
    case HISENSE_FEATURE_NONE:
    default:
        frame[33] = 0x04; // neutral / turbo-off baseline (confirmed clears turbo).
        break;
    }

    // offset 35: baseline filler 0x00 (majority of single-purpose samples).
    frame[35] = 0x00;

    // offset 36: display. 0xC0 on / 0x40 off / 0x00 "leave alone".
    frame[36] = cmd->display_on ? 0xC0 : 0x00;

    finalize_frame(frame, HISENSE_CMD_CHK_OFFSET, HISENSE_CMD_END_OFFSET);

    // Byte-stuff an 0xF4 checksum byte before it goes on the wire.
    return hisense_stuff_checksum(frame, HISENSE_CMD_FRAME_LEN, out, out_cap);
}

/* ---------------------------------------------------------------------------
 * Status parsing (input already un-stuffed by the bus task)
 * -------------------------------------------------------------------------*/
bool hisense_parse_status(const uint8_t *buf, size_t len, HisenseState *out_state)
{
    if (buf == NULL || out_state == NULL) {
        return false;
    }
    // Length is LEN-driven, CONFIRMED on real W41H1 hardware (live capture):
    // total frame length == byte[4] + 9  (= 7 + LEN + F4 FB). Verified across
    // every frame class -- poll(0x0C->21), link(0x13->28), cmd(0x29->50),
    // status(0x97->160). The stock driver's fixed 82 was wrong for THIS unit
    // (its status frame is 160 bytes) and dropped every status frame.
    if (len < 45) {                       // need at least through offset 44
        return false;
    }
    size_t expected = (size_t)buf[4] + 9;
    if (len != expected) {
        return false;
    }
    if (buf[0] != HISENSE_STX1 || buf[1] != HISENSE_STX2) {
        return false;
    }
    if (buf[len - 2] != HISENSE_ETX1 || buf[len - 1] != HISENSE_ETX2) {
        return false;
    }

    uint16_t computed = hisense_checksum_range(buf, 2, len - 4);
    uint16_t received = (uint16_t)((buf[len - 4] << 8) | buf[len - 3]);
    if (computed != received) {
        return false;
    }

    // Device_Status body starts after the 16-byte header. Offsets ABSOLUTE.
    memset(out_state, 0, sizeof(*out_state));

    uint8_t wind_status      = buf[16];
    uint8_t packed           = buf[18]; // direction:2, run:2, mode:4
    uint8_t run_status       = (packed >> 2) & 0x3;
    uint8_t mode_status      = (packed >> 4) & 0xF;
    // Status reports AUTO as nibble 5 OR 6 (value 4 is skipped) -> remap to the
    // HISENSE_MODE_AUTO enum (=4, the command index). Bus-tap-confirmed 2026-07-08:
    // the stock AUTO command (byte18=0x90) drives the A/C to status nibble **6**
    // (compressor running) -- decoding only nibble 5 made HA fall back to Cool
    // ("Auto reverts to Cool", issue I1). CONFIRMED on hardware.
    if (mode_status == 5 || mode_status == 6) {
        mode_status = HISENSE_MODE_AUTO;
    }
    uint8_t temp_setting_raw = buf[19];
    uint8_t temp_status_raw  = buf[20];
    uint8_t flags1           = buf[35];
    uint8_t flags2           = buf[36];

    out_state->valid = true;
    out_state->power_on = (run_status != 0);
    out_state->mode = (HisenseMode)mode_status;
    out_state->fan_raw = wind_status;

    // Status-side temperature decode: DIRECT integer degrees C. CONFIRMED on
    // real W41H1 hardware -- with the unit set to 22 C in a 21 C room, offset
    // 19 read 0x16 (=22) and offset 20 read 0x15 (=21) exactly. This is a
    // DIFFERENT encoding than the AEH-W4A1 reference's (raw-32)*0.5556, which
    // is what produced the bogus -5 C readings before this was captured.
    out_state->setpoint_c    = (int8_t)temp_setting_raw;   // offset 19
    out_state->indoor_temp_c = (int8_t)temp_status_raw;    // offset 20

    // flags1 @35 / flags2 @36 -- bit assignments CONFIRMED on hardware by
    // toggling each control and diffing (vswing/turbo/eco/mute). NOTE the eco
    // bit is 0x04 (bit2), NOT the reference's 0x08 (bit3); turbo has its own
    // bit 0x02 (bit1), which the reference didn't model as a status flag.
    out_state->vswing_on     = (flags1 & 0x80) != 0; // bit7  CONFIRMED
    out_state->hswing_on     = (flags1 & 0x40) != 0; // bit6  CONFIRMED (remote)
    out_state->turbo_on      = (flags1 & 0x02) != 0; // bit1  CONFIRMED
    out_state->eco_on        = (flags1 & 0x04) != 0; // bit2  CONFIRMED
    out_state->heat_relay_on = (flags1 & 0x10) != 0; // bit4 = aux/PTC electric-
                                                      // heat relay. Stayed 0
                                                      // during heat-pump heating
                                                      // at 50Hz (mild weather);
                                                      // only fires in cold/defrost.

    out_state->mute_on       = (flags2 & 0x04) != 0; // bit2  CONFIRMED
    out_state->purify_on     = (flags2 & 0x20) != 0; // bit5  (feature ABSENT on
                                                      // this unit -- no app/
                                                      // remote control; always
                                                      // 0. Kept for units that
                                                      // do have it.)

    // Sleep @17. CONFIRMED on hw: 0x00 off / 0x02 on (also forces fan low).
    out_state->sleep_raw = buf[17];
    out_state->sleep_on  = (buf[17] != 0);

    // Outdoor temp @44, DIRECT integer C. CONFIRMED against real ambient:
    // sensor read 32-33 while a weather station reported 33-34 (the ~1 C gap is
    // normal for a wall-mounted condenser sensor). Same offset as the reference.
    out_state->outdoor_temp_c  = (int8_t)buf[44];

    // Outdoor/condenser coil temp @45, DIRECT integer C. CONFIRMED by the
    // cool->off->heat three-state correlation (33->25->17 C; it REVERSES between
    // modes, which is how it was told apart from outdoor@44). docs/03 diag map.
    out_state->coil_temp_c = (len > 45) ? (int8_t)buf[45] : 0;

    // Compressor frequency @42, in Hz. CONFIRMED on hardware: with the
    // compressor stopped it read 0x00, then climbed 0x18->0x2A->0x37 (24->42->
    // 55 Hz) the moment cooling demand started after a setpoint drop. (The
    // mirrored 56/144 that first looked like Hz were a rolling frame counter.)
    // Offset 43 is a likely companion (target/send frequency) -- not wired yet.
    out_state->compressor_freq = buf[42];

    // Energy telemetry: @50 supply voltage (whole V), @55 current-proxy. Bounds-
    // guarded (these sit past the 45-byte core gate). Power model in power_estimate.h.
    out_state->voltage_raw = (len > 50) ? buf[50] : 0;
    out_state->current_raw = (len > 55) ? buf[55] : 0;

    return true;
}

/* ---------------------------------------------------------------------------
 * RX interrupt -> ring buffer
 *
 * The stock firmware drives this UART with serial_irq_handler/serial_irq_set
 * (RxIrq), NOT a polling serial_getc() loop -- so do the same. The ISR pulls
 * every readable byte into a lock-free single-producer/single-consumer ring
 * and the bus task drains it. This avoids the busy-spin that a blocking
 * serial_getc() would cause (it never returns a "no data" sentinel on this
 * HAL) and keeps the bus task off the CPU while the line is idle.
 * -------------------------------------------------------------------------*/
static void hisense_uart_irq(uint32_t id, SerialIrq event)
{
    (void)id;
    if (event != RxIrq) {
        return;
    }
    while (serial_readable(&s_uart)) {
        uint8_t b = (uint8_t)serial_getc(&s_uart);
        uint16_t next = (uint16_t)((s_rx_head + 1) & (HISENSE_RX_RING_SIZE - 1));
        if (next != s_rx_tail) {          // drop on overflow rather than clobber
            s_rx_ring[s_rx_head] = b;
            s_rx_head = next;
        }
    }
}

// Returns -1 if the ring is empty, else the next byte (0..255).
static int hisense_ring_getc(void)
{
    if (s_rx_tail == s_rx_head) {
        return -1;
    }
    uint8_t b = s_rx_ring[s_rx_tail];
    s_rx_tail = (uint16_t)((s_rx_tail + 1) & (HISENSE_RX_RING_SIZE - 1));
    return b;
}

/* ---------------------------------------------------------------------------
 * Frame reassembly (drains the ring; un-stuffs doubled F4)
 * -------------------------------------------------------------------------*/
// Must hold the largest frame. The real W41H1 STATUS frame is 160 bytes
// (confirmed on hardware), so the old 128 overflowed and dropped every status
// frame before it even reached the parser. 200 leaves headroom + stuffing.
#define HISENSE_RX_BUF_SIZE 200

// Reassembly state, kept across calls so the bus task can interleave RX
// processing with TX pacing.
static uint8_t  s_msg_buf[HISENSE_RX_BUF_SIZE];
static size_t   s_msg_len      = 0;
static bool     s_in_message   = false;
static bool     s_prev_was_f4  = false;   // proper escape flag (fixes the
                                          // "drop the next F4 too" bug)
static int      s_expected_len = 0;

static void hisense_reset_rx(void)
{
    s_in_message  = false;
    s_msg_len     = 0;
    s_prev_was_f4 = false;
    s_expected_len = 0;
}

// Generic frame reassembler. Feeds one received byte; when a complete
// `F4 F5 <dir=01> 40 <LEN> ... F4 FB` frame from the A/C is assembled
// (LEN-driven: total = byte[4] + 9, doubled-F4 un-stuffed), returns its length
// with the bytes in s_msg_buf; otherwise 0. Unlike the old status-only version
// this accepts ANY class (0x0A DevType, 0x1E LINK, 0x66 status, ...), because
// the reactive handshake must read the A/C's DevType and LINK replies -- not
// just the 160-byte status. dir byte[2] must be 0x01 (A/C -> module), which
// also rejects any echo of our own outgoing frames (byte[2]=0x00).
static size_t hisense_rx_feed(uint8_t b)
{
    if (!s_in_message) {
        if (b == HISENSE_STX1) {            // F4 -- possible frame start
            s_msg_buf[0]   = b;
            s_msg_len      = 1;
            s_in_message   = true;
            s_prev_was_f4  = true;
            s_expected_len = 0;
        }
        return 0;
    }

    // Un-stuff a doubled F4 (escape), mirror of hisense_stuff_checksum().
    if (b == HISENSE_STX1 && s_prev_was_f4) {
        s_prev_was_f4 = false;
        return 0;
    }
    s_prev_was_f4 = (b == HISENSE_STX1);

    if (s_msg_len >= HISENSE_RX_BUF_SIZE) {
        hisense_reset_rx();
        return 0;
    }
    s_msg_buf[s_msg_len++] = b;
    size_t idx = s_msg_len - 1;

    if (idx == 1 && s_msg_buf[1] != HISENSE_STX2) {   // require F4 F5
        hisense_reset_rx();
        return 0;
    }
    if (idx == 2 && s_msg_buf[2] != 0x01) {           // A/C -> module only
        hisense_reset_rx();
        return 0;
    }
    if (idx == 4) {                                    // LEN-driven total length
        s_expected_len = (int)s_msg_buf[4] + 9;
        if (s_expected_len < 6 || s_expected_len > HISENSE_RX_BUF_SIZE) {
            hisense_reset_rx();
            return 0;
        }
    }
    if (s_expected_len > 0 && idx == (size_t)(s_expected_len - 1)) {
        size_t n = s_msg_len;
        if (s_msg_buf[n - 2] != HISENSE_ETX1 || s_msg_buf[n - 1] != HISENSE_ETX2) {
            hisense_reset_rx();          // no F4 FB end tag -- resync
            return 0;
        }
        hisense_reset_rx();
        return n;                        // complete frame in s_msg_buf[0..n)
    }
    return 0;
}

// Build the 0x1E LINK ("num30") frame. The only field that must react to the
// A/C is wire[16] bit6 = "I have NOT yet received the A/C's num30" (stock
// global recv_num30_flag): 0xF0 before we have heard the A/C's 0x1E, then 0xB0
// after. Everything else is the captured "connected" state. The 2-byte BE
// checksum over [2,24) is recomputed because wire[16] changes it.
static size_t hisense_build_link_1e(uint8_t *out, size_t cap, bool heard_ac)
{
    if (cap < HISENSE_LINK_HEARTBEAT_LEN + 2) {   // +2: defensive checksum byte-stuffing (#62)
        return 0;
    }
    uint8_t f[HISENSE_LINK_HEARTBEAT_LEN];
    memcpy(f, HISENSE_LINK_HEARTBEAT, HISENSE_LINK_HEARTBEAT_LEN);
    f[16] = heard_ac ? 0xB0 : 0xF0;
    if (s_prov_active) {
        f[17] |= 0x08;   // payload[4] bit3 = prov_status -> A/C lights "77" (bench-confirmed 2026-07-09)
    }
    finalize_frame(f, 24, 26);           // chk @24/25; F4 FB @26/27 (== template)
    // Byte-stuff an 0xF4 checksum byte before the wire, like every other frame builder.
    // Dormant today (both reachable link checksums avoid 0xF4) but defensive if the
    // template's reactive bytes change. No-op passthrough when no 0xF4 is present.
    return hisense_stuff_checksum(f, HISENSE_LINK_HEARTBEAT_LEN, out, cap);
}

/* ---------------------------------------------------------------------------
 * Raw TX with half-duplex direction control (PA_17 DE). The UM3352E only drives
 * A/B while DE is HIGH; DE//RE are tied, so RX is muted during our send (we do
 * not hear our own frame). Sequence mirrors the stock byte-writer (docs/09):
 * assert DE -> settle -> send -> drain the FIFO+shift -> release DE (listen).
 * Skipping the assert is why every earlier revision put bytes on DI but nothing
 * on the wire, and the A/C stayed silent.
 * -------------------------------------------------------------------------*/
static void hisense_tx_raw(const uint8_t *buf, size_t len)
{
    gpio_write(&s_de, 1);                              // drive the bus
    vTaskDelay(pdMS_TO_TICKS(HISENSE_DE_SETTLE_MS));   // let the driver settle

    for (size_t i = 0; i < len; i++) {
        serial_putc(&s_uart, buf[i]);
    }

    // serial_putc only guarantees the byte reached the TX FIFO, not that it has
    // shifted out. Hold DE high until the whole FIFO+shift register drains, or
    // we truncate our frame's tail on the wire and the A/C rejects it.
    vTaskDelay(pdMS_TO_TICKS(HISENSE_DE_DRAIN_MS));

    gpio_write(&s_de, 0);                              // release: back to receive
}

/* ---------------------------------------------------------------------------
 * #49: extract the link session token from an A/C reply. The A/C->module
 * envelope carries the token the module must echo at bytes [9]/[10] (stock
 * session-id capture 0x9b6f33a0 / per-transaction echo 0x9b6f33ae). PURE ->
 * host-testable; the offset is the #49-critical bit, so it lives in one function.
 * -------------------------------------------------------------------------*/
bool hisense_link_token_from_reply(const uint8_t *reply, size_t n, uint8_t *hi, uint8_t *lo)
{
    if (reply == NULL || n <= 10) {
        return false;
    }
    if (hi) *hi = reply[9];
    if (lo) *lo = reply[10];
    return true;
}

// #49 getter: last token captured from a reply (Phase A, observe-only). Returns
// false until a reply has updated it (the returned bytes are then the 01 01 seed).
bool hisense_get_link_token(uint8_t *hi, uint8_t *lo)
{
    if (hi) *hi = s_link_tok[0];
    if (lo) *lo = s_link_tok[1];
    return s_link_tok_seen;
}

/* ---------------------------------------------------------------------------
 * Transaction primitive (mirrors stock 0x9b6f335c / 0x9b6f7f38): send one
 * frame, then listen up to timeout_ms for a complete A/C reply. Returns the
 * reply length (bytes in s_msg_buf) or 0 on timeout.
 *
 * The A/C is a pure slave that answers each poll within ~500ms. NEVER send two
 * frames without a reply window, or the A/C's reply collides with our next TX
 * on the half-duplex bus and the link never comes up -- that is exactly why a
 * free-running-timer replay got silence (reverse-engineering/docs/09).
 *
 * `expect_class` correlates the reply to the request (#60): a completed frame whose
 * class byte (s_msg_buf[13]) doesn't match is a late/stale reply and is discarded
 * (we keep listening within the window) rather than consumed as this poll's answer.
 * Pass 0 to accept any class.
 * -------------------------------------------------------------------------*/
static int hisense_transact(const uint8_t *frame, size_t len, int timeout_ms, uint8_t expect_class)
{
    hisense_reset_rx();
    while (hisense_ring_getc() >= 0) { /* flush stale RX before we speak */ }

    hisense_tx_raw(frame, len);

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while ((int)(xTaskGetTickCount() - deadline) < 0) {
        int c;
        while ((c = hisense_ring_getc()) >= 0) {
            size_t n = hisense_rx_feed((uint8_t)c);
            if (n > 0) {
                if (n > 13 && hisense_reply_class_ok(s_msg_buf[13], expect_class)) {
                    // #49 Phase A (observe-only): capture the session token from the
                    // reply envelope [9]/[10]. NOT yet stamped into outgoing frames —
                    // that framing change is gated on bench confirmation via recon.
                    if (hisense_link_token_from_reply(s_msg_buf, n, &s_link_tok[0], &s_link_tok[1]))
                        s_link_tok_seen = true;
                    return (int)n;  // correlated reply now in s_msg_buf[0..n)
                }
                // Wrong-class / late reply: discard and keep listening this window.
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return 0;                       // timeout -- no reply
}

// If the reply in s_msg_buf (length n) is a 0x66 status frame, parse + push it
// to the Matter side via the status callback.
static void hisense_consume_status(size_t n)
{
    if (n >= HISENSE_STATUS_HEADER_LEN && s_msg_buf[13] == 0x66) {
        if (s_msg_buf[14] == 0x40) {   // ProductType feature-flag response, not status
            HisenseFeatures ft;
            if (hisense_parse_features(s_msg_buf, n, &ft)) {
                s_features = ft;
                s_features_valid = true;
                if (s_features_cb != NULL) {
                    s_features_cb(&ft);
                }
            }
            return;
        }
        HisenseState st;
        if (hisense_parse_status(s_msg_buf, n, &st) && s_status_cb != NULL) {
            s_status_cb(&st);
        }
    }
}

// Debounce decision for the "77" recommission request (pure -> host-testable).
// Given this frame's masked request bits and the running streak/latch state,
// advance the state and return true EXACTLY ONCE per sustained assertion (>=
// hold_frames consecutive asserted ~1Hz replies). A bare rising edge is NOT
// enough: payload[4] bit3 (0x08) doubles as the value we transmit for prov_status,
// so a single reflected/glitched frame must not trip a commissioning window. The
// request stays asserted while the user holds "77", so a few frames of hold cleanly
// separates a deliberate press from a 1-frame artefact. Not static: linked by tests.
bool hisense_recommission_debounce(uint8_t req_bits, uint8_t *streak,
                                   bool *latched, uint8_t hold_frames)
{
    if (req_bits != 0) {
        if (*streak < 0xFF) {
            (*streak)++;
        }
        if (*streak >= hold_frames && !*latched) {
            *latched = true;
            return true;                 // fire once for this sustained assertion
        }
        return false;
    }
    *streak  = 0;                        // request cleared -> reset + re-arm
    *latched = false;
    return false;
}

// Link-health edge detector (pure -> host-testable, #56). `silent` = link currently
// considered lost; `*was_down` latches the state across calls. Returns +1 exactly on
// the transition into silence (lost edge), -1 on the transition back (restored edge),
// 0 while steady. Not static: linked by tests.
int hisense_link_health_edge(bool silent, bool *was_down)
{
    if (silent && !*was_down) { *was_down = true;  return  1; }   // just went silent
    if (!silent && *was_down) { *was_down = false; return -1; }   // recovered
    return 0;
}

void hisense_set_link_cb(hisense_link_cb_t cb)
{
    s_link_cb = cb;
}

// If the reply is the A/C's 0x1E LINK reply, check payload[4] (frame byte[17]) for
// a re-provision request -- the remote "77" recommission. Firmware-RE of the stock
// dongle (reverse-engineering/docs/02): the mainboard sets bit3 (0x08 reset/
// reconfig) or bit5 (0x20 smart-config) there. Debounced (see above) so a request
// must persist >= HISENSE_RECOMMISSION_HOLD_FRAMES frames before it fires the
// handler ONCE. byte[13]=class, payload[k]=byte[13+k] -> byte[17].
static void hisense_check_link_reply(size_t n)
{
    if (n <= 17 || s_msg_buf[13] != 0x1E) {
        return;
    }
    uint8_t req = s_msg_buf[17] & HISENSE_LINK_REQ_RECOMMISSION;   // payload[4]
    // Re-arm if the request TYPE changes while still asserted (e.g. RECONFIG->SMARTCFG
    // with no intervening all-zero frame) so the second reason isn't swallowed by the latch.
    if (req != 0 && req != s_last_link_req) {
        s_link_req_streak = 0;
        s_recommission_latched = false;
    }
    if (hisense_recommission_debounce(req, &s_link_req_streak, &s_recommission_latched,
                                      HISENSE_RECOMMISSION_HOLD_FRAMES)
        && s_recommission_cb != NULL) {
        s_recommission_cb(s_msg_buf[17]);
    }
    s_last_link_req = req;   // retained for diagnostics + reason-change detection
}

void hisense_set_recommission_cb(hisense_recommission_cb_t cb)
{
    s_recommission_cb = cb;
}

void hisense_set_provisioning(bool on)
{
    // While on, every outbound 0x1E carries prov_status=1 (payload[4] bit3), which
    // lights "77" on the A/C panel -- bench-confirmed 2026-07-09 (injecting that bit
    // showed "77"). Set true when the commissioning window opens, false when it closes.
    s_prov_active = on;
}

void hisense_send_exit_77(void)
{
    // Stop reporting prov=1, then push one online 0x1E now so the A/C clears "77"
    // promptly instead of waiting for the next ~1Hz heartbeat.
    s_prov_active = false;
    uint8_t f[HISENSE_LINK_HEARTBEAT_LEN + 4];
    size_t n = hisense_build_link_1e(f, sizeof(f), true);
    if (n > 0) {
        hisense_send_frame(f, n);
    }
}

/* ---------------------------------------------------------------------------
 * Hardware bring-up: DE GPIO (PA_17) + UART0. MUST run in TASK context (after
 * the RTOS scheduler + SDK HAL init), never from hisense_init's Matter-app-init
 * caller: mbed gpio_init/serial_init dispatch through HAL/ROM function-tables
 * that aren't populated that early, so calling them there faults the chip before
 * boot (no UART TX / no BLE beacon -- confirmed on hardware). Order mirrors the
 * stock hs_driver_init: DE gpio (idle low = RX) BEFORE the UART. docs/10.
 * -------------------------------------------------------------------------*/
static void hisense_hw_bringup(void)
{
    gpio_init(&s_de, HISENSE_UART_DE_PIN);
    gpio_dir(&s_de, PIN_OUTPUT);
    gpio_mode(&s_de, PullNone);
    gpio_write(&s_de, 0);                 // idle DE low = receive

    serial_init(&s_uart, HISENSE_UART_TX_PIN, HISENSE_UART_RX_PIN);
    serial_baud(&s_uart, HISENSE_UART_BAUD);
    serial_format(&s_uart, HISENSE_UART_DATA_BITS, ParityNone, HISENSE_UART_STOP_BITS);
    serial_irq_handler(&s_uart, hisense_uart_irq, 0);   // IRQ-driven RX
    serial_irq_set(&s_uart, RxIrq, 1);
}

/* ---------------------------------------------------------------------------
 * Bus task: the reactive transactional master. One task owns the UART.
 *   0. bring up the DE line + UART IN THIS TASK CONTEXT (the boot-fault fix).
 *   1. boot: DevType (0x0A) handshake, then 0x07 -- bring the link up.
 *   2. steady ~1Hz: 0x1E LINK transaction (bit6 reacts to having heard the
 *      A/C), drain any queued command frames, then a 0x66 status poll.
 * Every send is a transaction (send -> listen <=500ms). Recovered from the
 * stock firmware -- see reverse-engineering/docs/09 and docs/10.
 * -------------------------------------------------------------------------*/
// Consecutive silent ~1Hz status polls before we treat the link as lost and
// re-run the DevType handshake (link recovery, docs/07 Tier-4).
#define HISENSE_LINK_LOST_POLLS 5

// How often (in ~1 Hz poll cycles) to re-request the 0x66/40 ProductType feature
// flags. They are static per model, so a slow refresh is plenty; the first poll
// after link-up happens immediately (guarded by !s_features_valid).
#define HISENSE_PRODUCTTYPE_POLL_CYCLES 60

// Bus-task stack depth. NOTE: xTaskCreate's unit differs by platform -- WORDS on
// AmebaZ2/standard-FreeRTOS (1024 -> 4KB), but BYTES on ESP-IDF (1024 -> only 1KB,
// which overflows once the status callback runs printf). Default preserves AmebaZ2;
// the ESP-IDF build overrides it to a byte count via a -D compile definition (see
// esp32-matter/components/hisense_rs485/CMakeLists.txt).
#ifndef HISENSE_BUS_TASK_STACK
#define HISENSE_BUS_TASK_STACK 1024
#endif

static void hisense_bus_task(void *pvParameters)
{
    (void)pvParameters;

    hisense_hw_bringup();   // DE GPIO + UART, in task context -- fixes the v4 boot fault

    // Boot handshake: DevType, wait for the A/C's 0x0A reply; retry a few times.
    for (int i = 0; i < 10; i++) {
        if (hisense_transact(HISENSE_LINK_INIT_0A, HISENSE_LINK_INIT_0A_LEN, 500, 0x0A) > 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    hisense_transact(HISENSE_LINK_INIT_07, HISENSE_LINK_INIT_07_LEN, 500, 0x00);  // reply ignored

    bool    heard_ac  = false;
    int     link_miss = 0;   // consecutive silent status polls -> link lost
    int     pt_poll   = 0;   // ProductType (0x66/40) refresh counter
    uint8_t f[HISENSE_TX_MAX_FRAME];

    for (;;) {
        TickType_t t0 = xTaskGetTickCount();

        // 0) COOPERATIVE link recovery. If the A/C has gone silent for several
        // cycles (drop / reconfig / OTA-reboot), re-run ONE DevType handshake this
        // cycle -- never a multi-second blocking spin. The TX queue is still
        // drained every cycle below, so Matter commands issued during a flap are
        // not stuck behind a ~10s stall and dropped once the depth-8 queue fills.
        // heard_ac re-latches when the A/C answers again. (docs/08 HIGH)
        if (link_miss >= HISENSE_LINK_LOST_POLLS) {
            heard_ac = false;
            hisense_transact(HISENSE_LINK_INIT_0A, HISENSE_LINK_INIT_0A_LEN, 500, 0x0A);
        }

        // 1) LINK keepalive: wire[16] bit6 = !heard_ac until the A/C answers. Accept
        // any class -- the A/C may answer the 0x1E poll with a 0x1E or a 0x66.
        size_t fl = hisense_build_link_1e(f, sizeof(f), heard_ac);
        int n = hisense_transact(f, fl, 500, 0x00);
        if (n > 0) {
            // Any A/C-directed reply means we've been heard; if it's a status
            // frame (the A/C sometimes answers the 0x1E poll with a 0x66) consume
            // it instead of discarding it. (docs/08 MED)
            heard_ac = true;
            hisense_consume_status((size_t)n);      // no-op unless it's a 0x66 status frame
            hisense_check_link_reply((size_t)n);    // no-op unless it's a 0x1E LINK reply (F1 "77")
        }

        // 2) Any queued command frames (Matter uplink), each as a transaction.
        if (s_tx_queue != NULL) {
            HisenseTxItem item;
            while (xQueueReceive(s_tx_queue, &item, 0) == pdTRUE) {
                n = hisense_transact(item.data, item.len, 500, 0x00);  // may echo a 0x66 status
                if (n > 0) hisense_consume_status((size_t)n);
            }
        }

        // 3) Status poll -- the primary link-liveness signal. Correlate on 0x66 so a
        // late 0x1E can't falsely reset link_miss while consume_status silently no-ops.
        n = hisense_transact(HISENSE_STATUS_REQUEST, HISENSE_STATUS_REQUEST_LEN, 500, 0x66);
        if (n > 0) {
            hisense_consume_status((size_t)n);
            link_miss = 0;
        } else {
            link_miss++;
        }

        // Link-health edge -> fire the callback once on lost/restored (#56).
        int edge = hisense_link_health_edge(link_miss >= HISENSE_LINK_LOST_POLLS, &s_link_down);
        if (edge != 0 && s_link_cb != NULL) {
            s_link_cb(edge < 0);   // edge<0 = restored, edge>0 = lost
        }

        // 4) ProductType (0x66/40) feature-flag poll. Feature CAPABILITY flags are
        // static per model, so poll rarely: once soon after the link is up, then a
        // slow refresh. hisense_consume_status routes the 0x40 subtype to the
        // feature parser. Correlate on class 0x66 (subtype checked on consume).
        if (heard_ac && (!s_features_valid || pt_poll == 0)) {
            size_t pl = hisense_build_producttype_request(f, sizeof(f));
            if (pl > 0) {
                n = hisense_transact(f, pl, 500, 0x66);
                if (n > 0) hisense_consume_status((size_t)n);
            }
        }
        if (++pt_poll >= HISENSE_PRODUCTTYPE_POLL_CYCLES) pt_poll = 0;

        // Pace the cycle to ~1Hz, like the stock master (0x3e8 ms).
        TickType_t dt = xTaskGetTickCount() - t0;
        if (dt < pdMS_TO_TICKS(1000)) {
            vTaskDelay(pdMS_TO_TICKS(1000) - dt);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Public TX entry point: copy the frame into the queue.
 * -------------------------------------------------------------------------*/
bool hisense_send_frame(const uint8_t *buf, size_t len)
{
    if (!s_initialized || buf == NULL || len == 0 || len > HISENSE_TX_MAX_FRAME || s_tx_queue == NULL) {
        return false;
    }
    HisenseTxItem item;
    item.len = (uint8_t)len;
    memcpy(item.data, buf, len);
    return xQueueSend(s_tx_queue, &item, 0) == pdTRUE;
}

/* ---------------------------------------------------------------------------
 * Init / deinit
 * -------------------------------------------------------------------------*/
int hisense_init(hisense_status_cb_t cb)
{
    if (s_initialized) {
        return pdFAIL;   // not reentrant: a 2nd call would leak the TX queue + race a 2nd bus task on shared state
    }
    s_status_cb = cb;
    s_rx_head = s_rx_tail = 0;
    hisense_reset_rx();

    // IMPORTANT: the DE GPIO and the UART are brought up in the bus TASK
    // (hisense_hw_bringup), NOT here. mbed gpio_init/serial_init dispatch through
    // SDK HAL/ROM vtables that are only valid once the RTOS scheduler + SDK HAL
    // init have run. Calling them from this (Matter app-init) context faults the
    // chip before boot completes -- confirmed on hardware (no UART TX, no BLE
    // beacon). The stock firmware brings the DE line + UART up late, inside its
    // own driver task; we do the same. See reverse-engineering/docs/10.
    s_tx_queue = xQueueCreate(8, sizeof(HisenseTxItem));
    if (s_tx_queue == NULL) {
        return pdFAIL;
    }

    s_initialized = true;

    BaseType_t ok = xTaskCreate(hisense_bus_task, "hisense_bus", HISENSE_BUS_TASK_STACK, NULL,
                                 tskIDLE_PRIORITY + 2, &s_bus_task_handle);
    if (ok != pdPASS) {
        s_initialized = false;
        vQueueDelete(s_tx_queue);
        s_tx_queue = NULL;
        return pdFAIL;
    }
    return pdPASS;
}

void hisense_deinit(void)
{
    if (s_bus_task_handle != NULL) {
        vTaskDelete(s_bus_task_handle);
        s_bus_task_handle = NULL;
    }
    if (s_initialized) {
        serial_irq_set(&s_uart, RxIrq, 0);
        serial_free(&s_uart);
    }
    if (s_tx_queue != NULL) {
        vQueueDelete(s_tx_queue);
        s_tx_queue = NULL;
    }
    s_initialized = false;
}
