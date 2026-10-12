// libFuzzer harness: the ESPHome codec (hisense_protocol.cpp) against the frozen reference driver
// (firmware/test/reference-driver/hisense_rs485.cpp) on the same bytes, wherever both implement
// the same function.
//
// The reference driver's source is #included into this translation unit instead of linked, so
// its static functions are reachable without touching the file: the frame reassembler
// (hisense_rx_feed), the link heartbeat builder and the single-field builder. That closes the gap
// test_esphome_codec_parity.cpp names in its header ("the original's static helpers cannot be
// called from outside").
//
// Input: byte 0 picks the group, the rest feeds it.
//
// Intended differences (see test_esphome_codec_parity.cpp and the two headers). Each is compared
// through a stated rule where one exists, and left out only where the two have no common ground:
//  - Trailer-aware lengths: the port reads a payload byte only when it lies in front of the 4-byte
//    trailer, the reference when `len > offset`. parse_faults, parse_features and
//    devtype_from_reply are compared with the reference's answer for a frame 4 bytes shorter;
//    parse_status with "refused below 49 bytes, the three optional tail fields need 4 more".
//  - AcCommand.power_on has no counterpart: the reference never ORs the run field into byte 18.
//    Checked here as a relation instead: same frame, byte 18 | 0x0C, checksum redone.
//  - build_single_field refuses offset >= 50; the reference's static helper has no guard (its
//    only callers pass 17 and 35). Compared for offsets below 50 only.
//  - The bit 5 lockout: the port keeps (armed, since) and compares elapsed time, the reference
//    keeps an end tick with 0 meaning "none" and a signed comparison. Compared where both are
//    defined: armed, end tick not 0, elapsed below 2^31.
//  - setup_request_step / SetupRequestTracker and the provisioning argument of the heartbeat
//    builder exist only in the port (the reference keeps that state in file statics inside its
//    bus task). The heartbeat is compared through the reference's s_prov_active.
//  - The reference's enums are plain C enums. Values outside their bit range are undefined
//    behaviour to load there, so the harness keeps mode <= 7 and swing/feature/display <= 3.
#include "../reference-driver/hisense_rs485.cpp"  // NOLINT(bugprone-suspicious-include)

#include "fuzz_common.h"

using namespace fz;

static bool same_bytes(const uint8_t *a, size_t na, const uint8_t *b, size_t nb) {
  return na == nb && (na == 0 || memcmp(a, b, na) == 0);
}

static bool same_state(const HisenseState &o, const H::AcState &n) {
  return o.valid == n.valid && o.power_on == n.power_on && static_cast<int>(o.mode) == static_cast<int>(n.mode) &&
         o.temp_unit_f == n.temp_unit_f && o.indoor_temp_c == n.indoor_temp_c && o.setpoint_c == n.setpoint_c &&
         o.fan_raw == n.fan_raw && o.vswing_on == n.vswing_on && o.turbo_on == n.turbo_on && o.eco_on == n.eco_on &&
         o.hswing_on == n.hswing_on && o.heat_relay_on == n.heat_relay_on && o.mute_on == n.mute_on &&
         o.sleep_on == n.sleep_on && o.sleep_raw == n.sleep_raw && o.purify_on == n.purify_on &&
         o.outdoor_temp_c == n.outdoor_temp_c && o.coil_temp_c == n.coil_temp_c &&
         o.compressor_freq == n.compressor_freq && o.current_raw == n.current_raw && o.voltage_raw == n.voltage_raw;
}

static bool same_features(const HisenseFeatures &o, const H::AcFeatures &n) {
  return o.valid == n.valid && o.cool_heat == n.cool_heat && o.ai == n.ai && o.infinite_fan == n.infinite_fan &&
         o.power_save == n.power_save && o.fan_mute == n.fan_mute && o.swing_dir_8 == n.swing_dir_8 &&
         o.swing_follow == n.swing_follow && o.power_display == n.power_display && o.demand_resp == n.demand_resp &&
         o.humidity == n.humidity && o.heat_8c == n.heat_8c && o.purify == n.purify && o.ext_valid == n.ext_valid &&
         o.q_display == n.q_display && o.enable_8heat == n.enable_8heat && o.trans_102_64 == n.trans_102_64 &&
         o.reply_len == n.reply_len;
}

static bool same_faults(const HisenseFaults &o, const H::AcFaults &n) {
  return o.valid == n.valid && o.any == n.any && o.raw_indoor == n.raw_indoor && o.raw_module == n.raw_module &&
         o.raw_outdoor == n.raw_outdoor && o.raw_protect == n.raw_protect && o.in_temp == n.in_temp &&
         o.in_coil_temp == n.in_coil_temp && o.in_humidity == n.in_humidity && o.water_full == n.water_full &&
         o.in_fan_motor == n.in_fan_motor && o.grille == n.grille && o.in_vzero == n.in_vzero && o.in_com == n.in_com &&
         o.in_display == n.in_display && o.in_keys == n.in_keys && o.in_wifi == n.in_wifi && o.in_ele == n.in_ele &&
         o.in_eeprom == n.in_eeprom && o.out_eeprom == n.out_eeprom && o.out_coil_temp == n.out_coil_temp &&
         o.out_gas_temp == n.out_gas_temp && o.out_temp == n.out_temp && o.over_temp == n.over_temp;
}

// ---- group 0: the two reassemblers on one stream -------------------------------------------------
static void diff_assembler(Reader &r) {
  hisense_reset_rx();
  H::FrameAssembler *fa = new H::FrameAssembler();
  size_t pos = 0;
  while (!r.empty()) {
    uint8_t b = r.u8();
    size_t no = hisense_rx_feed(b);
    size_t nn = fa->feed(b);
    FUZZ_CHECK(no == nn, "stream byte %zu (%02X): reference returned %zu, port %zu", pos, b, no, nn);
    if (no != 0)
      FUZZ_CHECK(memcmp(s_msg_buf, fa->data(), no) == 0, "stream byte %zu: the two delivered frames differ", pos);
    pos++;
  }
  delete fa;
}

// The length at which the reference gives the port's answer (trailer-aware lengths, above).
static size_t ref_len(size_t len) { return len >= H::FRAME_TRAILER_LEN ? len - H::FRAME_TRAILER_LEN : 0; }

// ---- group 1: parsers on one buffer --------------------------------------------------------------
static void diff_parsers(Reader &r) {
  uint8_t raw[260];
  size_t len = r.take(raw, sizeof(raw));
  Exact buf(raw, len);
  const uint8_t *f = buf.data();

  HisenseState os;
  H::AcState ns;
  memset(&os, 0, sizeof(os));
  bool ro = hisense_parse_status(f, len, &os);
  bool rn = H::parse_status(f, len, &ns);
  FUZZ_CHECK(rn == (ro && len >= 49), "parse_status: reference %d, port %d (len %zu)", ro, rn, len);
  if (rn) {
    if (len < 50)
      os.coil_temp_c = 0;
    if (len < 55)
      os.voltage_raw = 0;
    if (len < 60)
      os.current_raw = 0;
    FUZZ_CHECK(same_state(os, ns), "parse_status fields differ (len %zu)", len);
  }

  HisenseFeatures ofe;
  H::AcFeatures nfe;
  memset(&ofe, 0, sizeof(ofe));
  bool fo = hisense_parse_features(f, ref_len(len), &ofe);
  bool fn = H::parse_features(f, len, &nfe);
  FUZZ_CHECK(fo == fn, "parse_features: reference %d, port %d (len %zu)", fo, fn, len);
  if (fo) {
    ofe.reply_len = static_cast<uint8_t>(len > 255 ? 255 : len);  // the diagnostic length is the real one
    FUZZ_CHECK(same_features(ofe, nfe), "parse_features fields differ (len %zu)", len);
    FUZZ_CHECK(hisense_features_to_bitmap32(&ofe) == H::features_to_bitmap32(nfe), "feature bitmaps differ");
  }

  HisenseFaults ofa;
  H::AcFaults nfa;
  bool ao = hisense_parse_faults(f, ref_len(len), &ofa);
  bool an = H::parse_faults(f, len, &nfa);
  FUZZ_CHECK(ao == an && same_faults(ofa, nfa), "parse_faults differs (len %zu)", len);
  FUZZ_CHECK(hisense_faults_to_bitmap32(&ofa) == H::faults_to_bitmap32(nfa), "fault bitmaps differ");

  FUZZ_CHECK(hisense_status_checksum_ok(f, len) == H::status_checksum_ok(f, len), "checksum_ok differs (len %zu)", len);

  uint8_t oh = 0, ol = 0, nh = 0, nl = 0;
  bool dvo = hisense_devtype_from_reply(f, ref_len(len), &oh, &ol);
  bool dvn = H::devtype_from_reply(f, len, &nh, &nl);
  FUZZ_CHECK(dvo == dvn && oh == nh && ol == nl, "devtype_from_reply differs (len %zu)", len);
}

// ---- group 2: the stamp functions on arbitrary input and capacity --------------------------------
static void diff_stamps(Reader &r) {
  uint8_t ctl = r.u8();
  size_t cap = r.u8() % 96;
  uint8_t hi = r.u8(), lo = r.u8();
  uint8_t raw[96];
  size_t len = r.take(raw, sizeof(raw));
  if ((ctl & 1) != 0 && len >= 2) {
    raw[0] = 0xF4;
    raw[1] = 0xF5;
  }
  if ((ctl & 2) != 0 && len > 13)
    raw[13] = 0x65;
  if ((ctl & 4) != 0 && len > 4)
    raw[4] = static_cast<uint8_t>(len - 9);  // a LEN that matches the input
  Exact in(raw, len);
  Exact a(cap), b(cap);
  size_t na = hisense_stamp_link_token(in.data(), len, hi, lo, a.data(), cap);
  size_t nb = H::stamp_link_token(in.data(), len, hi, lo, b.data(), cap);
  FUZZ_CHECK(same_bytes(a.data(), na, b.data(), nb), "stamp_link_token: reference %zu bytes, port %zu", na, nb);
  FUZZ_CHECK(nb <= cap, "stamp_link_token wrote %zu into %zu", nb, cap);

  Exact c(cap), d(cap);
  bool beep = (ctl & 8) != 0;
  na = hisense_stamp_beep(in.data(), len, beep, c.data(), cap);
  nb = H::stamp_beep(in.data(), len, beep, d.data(), cap);
  FUZZ_CHECK(same_bytes(c.data(), na, d.data(), nb), "stamp_beep: reference %zu bytes, port %zu", na, nb);
  FUZZ_CHECK(nb <= cap, "stamp_beep wrote %zu into %zu", nb, cap);
}

// ---- group 3: builders ---------------------------------------------------------------------------
static void diff_builders(Reader &r) {
  size_t cap = r.u8() % 80;
  Exact a(cap), b(cap);

  H::AcCommand n;
  n.mode = static_cast<H::Mode>(r.u8() & 7);
  n.setpoint = static_cast<int8_t>(r.u8());
  n.fahrenheit = r.flag();
  n.fan = static_cast<H::FanSpeed>(r.u8());
  n.vswing = static_cast<H::SwingMode>(r.u8() & 3);
  n.hswing = static_cast<H::SwingMode>(r.u8() & 3);
  n.feature = static_cast<H::Feature>(r.u8() & 3);
  n.display = static_cast<H::Display>(r.u8() & 3);
  HisenseCommand o;
  memset(&o, 0, sizeof(o));
  o.mode = static_cast<HisenseMode>(n.mode);
  o.setpoint = n.setpoint;
  o.fahrenheit = n.fahrenheit;
  o.fan = static_cast<HisenseFanSpeed>(n.fan);
  o.vswing = static_cast<HisenseSwingMode>(n.vswing);
  o.hswing = static_cast<HisenseSwingMode>(n.hswing);
  o.feature = static_cast<HisenseFeature>(n.feature);
  o.display = static_cast<HisenseDisplay>(n.display);
  size_t na = hisense_build_command(&o, a.data(), cap);
  size_t nb = H::build_command(n, b.data(), cap);
  FUZZ_CHECK(same_bytes(a.data(), na, b.data(), nb), "build_command: reference %zu bytes, port %zu", na, nb);

  // power_on is the port's own: the reference frame with the run field set and the checksum redone.
  if (na != 0) {
    n.power_on = true;
    uint8_t with_power[H::CMD_FRAME_MAX];
    size_t np = H::build_command(n, with_power, sizeof(with_power));
    uint8_t ref_f[80], port_f[80];
    size_t ur = model_unstuff_tx(a.data(), na, ref_f, sizeof(ref_f));
    size_t up = model_unstuff_tx(with_power, np, port_f, sizeof(port_f));
    FUZZ_CHECK(ur == 50 && up == 50, "command frames un-stuff to 50 bytes");
    ref_f[18] |= 0x0C;
    model_seal(ref_f, 50);
    FUZZ_CHECK(memcmp(ref_f, port_f, 50) == 0, "power_on changes more than the run field of byte 18");
  }

  bool on = r.flag();
  na = hisense_build_power_frame(on, a.data(), cap);
  nb = H::build_power_frame(on, b.data(), cap);
  FUZZ_CHECK(same_bytes(a.data(), na, b.data(), nb), "build_power_frame differs");

  na = hisense_build_mute_frame(on, a.data(), cap);
  nb = H::build_mute_frame(on, b.data(), cap);
  FUZZ_CHECK(same_bytes(a.data(), na, b.data(), nb), "build_mute_frame differs");

  uint8_t profile = r.u8();
  na = hisense_build_sleep_frame(profile, a.data(), cap);
  nb = H::build_sleep_frame(profile, b.data(), cap);
  FUZZ_CHECK(same_bytes(a.data(), na, b.data(), nb), "build_sleep_frame differs");

  na = hisense_build_producttype_request(a.data(), cap);
  nb = H::build_producttype_request(b.data(), cap);
  FUZZ_CHECK(same_bytes(a.data(), na, b.data(), nb), "build_producttype_request differs");

  uint8_t off = r.u8() % 50;
  uint8_t val = r.u8();
  na = hisense_build_single_field(off, val, a.data(), cap);
  nb = H::build_single_field(off, val, b.data(), cap);
  FUZZ_CHECK(same_bytes(a.data(), na, b.data(), nb), "build_single_field(%u, %02X) differs", off, val);

  bool heard = r.flag();
  s_prov_active = r.flag();
  na = hisense_build_link_1e(a.data(), cap, heard);
  nb = H::build_link_heartbeat(heard, s_prov_active, b.data(), cap);
  FUZZ_CHECK(same_bytes(a.data(), na, b.data(), nb), "link heartbeat differs (heard %d, provisioning %d)", heard,
             s_prov_active);
  s_prov_active = false;
}

// ---- group 4: the small pure helpers -------------------------------------------------------------
static void diff_helpers(Reader &r) {
  // Temperatures over the 16 bit range. Callers only ever pass a byte; beyond about 4e8 the
  // multiplication overflows int in both implementations.
  int t = static_cast<int16_t>(r.u16());
  FUZZ_CHECK(hisense_f_to_c(t) == H::f_to_c(t), "f_to_c(%d)", t);
  FUZZ_CHECK(hisense_c_to_f(t) == H::c_to_f(t), "c_to_f(%d)", t);

  int8_t sp = static_cast<int8_t>(r.u8());
  bool unit_f = r.flag();
  FUZZ_CHECK(hisense_setpoint_in_range(sp, unit_f) == H::setpoint_in_range(sp, unit_f), "setpoint_in_range");
  int8_t oa = 99, ob = 99;
  bool ra = hisense_shadow_setpoint_from_status(sp, unit_f, &oa);
  bool rb = H::shadow_setpoint_from_status(sp, unit_f, &ob);
  FUZZ_CHECK(ra == rb && oa == ob, "shadow_setpoint_from_status(%d, %d)", sp, unit_f);

  uint8_t req = r.u8(), hold = r.u8();
  uint8_t s1 = r.u8(), s2 = s1;
  bool l1 = r.flag(), l2 = l1;
  int e1 = hisense_recommission_debounce(req, &s1, &l1, hold);
  int e2 = H::setup_request_debounce(req, &s2, &l2, hold);
  FUZZ_CHECK(e1 == e2 && s1 == s2 && l1 == l2, "debounce step differs");

  uint32_t since = r.u32();
  uint32_t elapsed = r.u32() & 0x7FFFFFFFu;
  uint32_t now = since + elapsed;
  uint32_t until = since + HISENSE_SMARTCFG_REENTRY_LOCKOUT_MS;
  if (until != 0) {
    uint8_t m1 = hisense_smartcfg_lockout_mask(req, now, until);
    uint8_t m2 = H::smartcfg_lockout_mask(req, now, true, since);
    FUZZ_CHECK(m1 == m2, "lockout mask differs %u ms into the lockout", elapsed);
  }
  FUZZ_CHECK(hisense_smartcfg_lockout_mask(req, now, 0) == H::smartcfg_lockout_mask(req, now, false, since),
             "lockout mask differs with no lockout");

  uint8_t c1 = r.u8(), c2 = r.u8();
  FUZZ_CHECK(hisense_reply_class_ok(c1, c2) == H::reply_class_ok(c1, c2), "reply_class_ok");

  uint32_t w = r.u32();
  HisenseFeatures of;
  memset(&of, 0, sizeof(of));
  hisense_features_from_bitmap32(w, &of);
  H::AcFeatures nf = H::features_from_bitmap32(w);
  FUZZ_CHECK(same_features(of, nf), "features_from_bitmap32(%08X)", w);
  FUZZ_CHECK(hisense_features_to_bitmap32(&of) == H::features_to_bitmap32(nf), "features_to_bitmap32");
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  Reader r(data, size);
  switch (r.u8() % 5) {
    case 0:
      diff_assembler(r);
      break;
    case 1:
      diff_parsers(r);
      break;
    case 2:
      diff_stamps(r);
      break;
    case 3:
      diff_builders(r);
      break;
    default:
      diff_helpers(r);
      break;
  }
  return 0;
}
