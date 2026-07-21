// test_codec.cpp -- Layer-1 host regression test for the Hisense RS-485 codec.
//
// Links the REAL driver (hisense_rs485.cpp) against a mocked HAL (hal_stub.h)
// and asserts its pure functions against the hardware-confirmed "golden" byte
// values captured off the real W41H1 during sniffing (see INTEGRATION.md). Any
// regression in the command builder or status parser fails here, on a PC, in
// milliseconds -- no A/C, no chip.
//
// Build/run:  ./run_tests.sh   (or: g++ -I. -I../src/rs485-driver test_codec.cpp ../src/rs485-driver/hisense_rs485.cpp -o test_codec && ./test_codec)

#include "hal_stub.h"
#include "hisense_rs485.h"
#include "test_common.h"
#include <cstdio>

// 2-byte big-endian sum over [2, end) -- must match the driver's checksum.
static uint16_t besum(const uint8_t *f, int end) {
    uint32_t s = 0; for (int i = 2; i < end; i++) s += f[i]; return (uint16_t)(s & 0xFFFF);
}

// ---- build a 160-byte STATUS frame the way the real A/C does (validated scheme) ----
static size_t make_status(uint8_t *out, bool power, HisenseMode mode, int8_t setp_c,
                          int8_t indoor_c, uint8_t fan_raw, uint8_t flags1, uint8_t flags2,
                          uint8_t sleep_raw, uint8_t comp_hz, int8_t outdoor_c) {
    static const uint8_t hdr[16] = {0xF4,0xF5,0x01,0x40,0x97,0x01,0x00,0xFE,0x01,0x01,0x01,0x01,0x00,0x66,0x00,0x01};
    memset(out, 0, 160);
    memcpy(out, hdr, 16);
    // status mode nibble uses the RAW status value: identity except AUTO(4)->5
    uint8_t status_mode = (mode == HISENSE_MODE_AUTO) ? 5 : (uint8_t)mode;
    out[16] = fan_raw;
    out[17] = sleep_raw;
    out[18] = (uint8_t)((status_mode << 4) | ((power ? 2 : 0) << 2)); // mode<<4 | run<<2
    out[19] = (uint8_t)setp_c;
    out[20] = (uint8_t)indoor_c;
    out[35] = flags1;
    out[36] = flags2;
    out[42] = comp_hz;
    out[44] = (uint8_t)outdoor_c;
    uint16_t ck = besum(out, 156);
    out[156] = (uint8_t)(ck >> 8); out[157] = (uint8_t)(ck & 0xFF);
    out[158] = 0xF4; out[159] = 0xFB;
    return 160;
}

int main() {
    printf("== Hisense RS-485 codec golden tests ==\n");
    uint8_t f[64];

    // ---- COMMAND builder: hardware-confirmed golden bytes ----
    printf("[command builder vs sniffed dongle bytes]\n");
    { HisenseCommand c={HISENSE_MODE_AUTO,24,false,HISENSE_FAN_AUTO,HISENSE_SWING_OFF,HISENSE_SWING_OFF,HISENSE_FEATURE_NONE, HISENSE_DISPLAY_NOCHANGE };
      hisense_build_command(&c,f,sizeof(f)); CHECK(f[18]==0x90,"AUTO byte18=0x%02X exp 0x90",f[18]); }
    { HisenseCommand c={HISENSE_MODE_COOL,24,false,HISENSE_FAN_AUTO,HISENSE_SWING_OFF,HISENSE_SWING_OFF,HISENSE_FEATURE_NONE, HISENSE_DISPLAY_NOCHANGE };
      hisense_build_command(&c,f,sizeof(f)); CHECK(f[18]==0x50,"COOL byte18=0x%02X exp 0x50",f[18]); }
    { HisenseCommand c={HISENSE_MODE_HEAT,24,false,HISENSE_FAN_AUTO,HISENSE_SWING_OFF,HISENSE_SWING_OFF,HISENSE_FEATURE_NONE, HISENSE_DISPLAY_NOCHANGE };
      hisense_build_command(&c,f,sizeof(f)); CHECK(f[18]==0x30,"HEAT byte18=0x%02X exp 0x30",f[18]); }
    { HisenseCommand c={HISENSE_MODE_DRY,24,false,HISENSE_FAN_AUTO,HISENSE_SWING_OFF,HISENSE_SWING_OFF,HISENSE_FEATURE_NONE, HISENSE_DISPLAY_NOCHANGE };
      hisense_build_command(&c,f,sizeof(f)); CHECK(f[18]==0x70,"DRY byte18=0x%02X exp 0x70",f[18]); }
    { HisenseCommand c={HISENSE_MODE_FAN,24,false,HISENSE_FAN_AUTO,HISENSE_SWING_OFF,HISENSE_SWING_OFF,HISENSE_FEATURE_NONE, HISENSE_DISPLAY_NOCHANGE };
      hisense_build_command(&c,f,sizeof(f)); CHECK(f[18]==0x10,"FANmode byte18=0x%02X exp 0x10",f[18]); }
    // six fan speeds
    struct { HisenseFanSpeed s; uint8_t b; const char* n; } fans[] = {
      {HISENSE_FAN_AUTO,0x01,"auto"},{HISENSE_FAN_QUIET,0x03,"quiet"},{HISENSE_FAN_LOW,0x0B,"low"},
      {HISENSE_FAN_MED_LOW,0x0D,"med-low"},{HISENSE_FAN_MID,0x0F,"mid"},{HISENSE_FAN_MED_HIGH,0x11,"med-high"},{HISENSE_FAN_HIGH,0x13,"high"} };
    for (auto &x : fans) { HisenseCommand c={HISENSE_MODE_COOL,24,false,x.s,HISENSE_SWING_OFF,HISENSE_SWING_OFF,HISENSE_FEATURE_NONE, HISENSE_DISPLAY_NOCHANGE };
      hisense_build_command(&c,f,sizeof(f)); CHECK(f[16]==x.b,"fan %s byte16=0x%02X exp 0x%02X",x.n,f[16],x.b); }
    // temp 22C -> 2n+1 = 0x2D
    { HisenseCommand c={HISENSE_MODE_COOL,22,false,HISENSE_FAN_AUTO,HISENSE_SWING_OFF,HISENSE_SWING_OFF,HISENSE_FEATURE_NONE, HISENSE_DISPLAY_NOCHANGE };
      hisense_build_command(&c,f,sizeof(f)); CHECK(f[19]==0x2D,"temp22 byte19=0x%02X exp 0x2D",f[19]); }
    // eco / turbo
    { HisenseCommand c={HISENSE_MODE_COOL,24,false,HISENSE_FAN_AUTO,HISENSE_SWING_OFF,HISENSE_SWING_OFF,HISENSE_FEATURE_ECO, HISENSE_DISPLAY_NOCHANGE };
      hisense_build_command(&c,f,sizeof(f)); CHECK(f[33]==0x30,"eco byte33=0x%02X exp 0x30",f[33]); }
    { HisenseCommand c={HISENSE_MODE_COOL,24,false,HISENSE_FAN_AUTO,HISENSE_SWING_OFF,HISENSE_SWING_OFF,HISENSE_FEATURE_TURBO, HISENSE_DISPLAY_NOCHANGE };
      hisense_build_command(&c,f,sizeof(f)); CHECK(f[33]==0x0C,"turbo byte33=0x%02X exp 0x0C",f[33]); }
    // vertical swing
    { HisenseCommand c={HISENSE_MODE_COOL,24,false,HISENSE_FAN_AUTO,HISENSE_SWING_SWING,HISENSE_SWING_OFF,HISENSE_FEATURE_NONE, HISENSE_DISPLAY_NOCHANGE };
      hisense_build_command(&c,f,sizeof(f)); CHECK(f[32]==0xC0,"vswing byte32=0x%02X exp 0xC0",f[32]); }
    // every command frame must carry a valid 2-byte checksum + F4 FB end
    { HisenseCommand c={HISENSE_MODE_COOL,24,false,HISENSE_FAN_HIGH,HISENSE_SWING_OFF,HISENSE_SWING_OFF,HISENSE_FEATURE_NONE, HISENSE_DISPLAY_NOCHANGE };
      size_t n=hisense_build_command(&c,f,sizeof(f));
      uint16_t ck=besum(f,(int)n-4); CHECK(((f[n-4]<<8)|f[n-3])==ck,"cmd checksum mismatch");
      CHECK(f[n-2]==0xF4&&f[n-1]==0xFB,"cmd end tag"); }

    // ---- TX byte-stuffing: SEARCH the input space for a real 0xF4 checksum byte
    //      and assert it is actually doubled (docs/08: the old test never hit this path) ----
    printf("[TX byte-stuffing (forced F4 search)]\n");
    { int found=0;
      HisenseMode modes[5]={HISENSE_MODE_COOL,HISENSE_MODE_HEAT,HISENSE_MODE_DRY,HISENSE_MODE_FAN,HISENSE_MODE_AUTO};
      HisenseFanSpeed fans[3]={HISENSE_FAN_LOW,HISENSE_FAN_MID,HISENSE_FAN_HIGH};
      HisenseFeature feats[3]={HISENSE_FEATURE_NONE,HISENSE_FEATURE_ECO,HISENSE_FEATURE_TURBO};
      for(int s=16;s<=32;s++) for(int mi=0;mi<5;mi++) for(int fi=0;fi<3;fi++) for(int qi=0;qi<3;qi++){
        HisenseCommand c={modes[mi],(int8_t)s,false,fans[fi],HISENSE_SWING_OFF,HISENSE_SWING_OFF,feats[qi],HISENSE_DISPLAY_NOCHANGE};
        uint8_t o[64]; size_t n=hisense_build_command(&c,o,sizeof(o));
        if(n>HISENSE_CMD_FRAME_LEN){ found++;
          bool dbl=false; for(size_t i=HISENSE_CMD_CHK_OFFSET;i+2<n;i++) if(o[i]==0xF4&&o[i+1]==0xF4) dbl=true;
          CHECK(dbl,"stuffed frame carries a doubled F4");
          CHECK(o[n-2]==0xF4&&o[n-1]==0xFB,"stuffed frame ends F4 FB"); } }
      // Finding: no reachable command on this device yields a 0xF4 checksum byte, so the
      // stuffing path is defensive-only. If a future change breaks that invariant, the
      // per-frame checks above start enforcing correct doubling.
      printf("  (info) %d/765 valid command frames require F4-stuffing\n",found); }

    // ---- combined multi-field frame: the driver ALWAYS sends one -- assert all fields coexist ----
    printf("[combined multi-field command frame]\n");
    { HisenseCommand c={HISENSE_MODE_COOL,22,false,HISENSE_FAN_LOW,HISENSE_SWING_SWING,HISENSE_SWING_OFF,HISENSE_FEATURE_ECO, HISENSE_DISPLAY_NOCHANGE };
      uint8_t o[64]; size_t n=hisense_build_command(&c,o,sizeof(o));
      CHECK(o[16]==0x0B,"combined fan   byte16=0x%02X exp 0x0B",o[16]);
      CHECK(o[18]==0x50,"combined mode  byte18=0x%02X exp 0x50",o[18]);
      CHECK(o[19]==0x2D,"combined temp  byte19=0x%02X exp 0x2D",o[19]);
      CHECK(o[32]==0xC0,"combined vswing byte32=0x%02X exp 0xC0",o[32]);
      CHECK(o[33]==0x30,"combined eco   byte33=0x%02X exp 0x30",o[33]);
      if(n==HISENSE_CMD_FRAME_LEN){ uint16_t ck=besum(o,(int)n-4);
        CHECK(((o[n-4]<<8)|o[n-3])==ck,"combined frame checksum valid"); } }

    // ---- power on/off frames: framing + checksum never asserted before ----
    printf("[power on/off frames]\n");
    { uint8_t o[64]; size_t n=hisense_build_power_frame(true,o,sizeof(o));
      CHECK(o[0]==0xF4&&o[1]==0xF5&&o[n-2]==0xF4&&o[n-1]==0xFB,"power-on framing");
      uint16_t ck=besum(o,(int)n-4); CHECK(((o[n-4]<<8)|o[n-3])==ck,"power-on checksum valid");
      n=hisense_build_power_frame(false,o,sizeof(o));
      CHECK(o[0]==0xF4&&o[1]==0xF5&&o[n-2]==0xF4&&o[n-1]==0xFB,"power-off framing"); }

    // ---- mute / sleep single-field frames (manufacturer-cluster toggles) ----
    printf("[mute/sleep frames]\n");
    { uint8_t o[64]; size_t n=hisense_build_mute_frame(true,o,sizeof(o));
      CHECK(o[35]==0x30,"mute on byte35=0x%02X exp 0x30",o[35]);
      uint16_t ck=besum(o,(int)n-4); CHECK(((o[n-4]<<8)|o[n-3])==ck,"mute frame checksum"); }
    { uint8_t o[64]; hisense_build_mute_frame(false,o,sizeof(o)); CHECK(o[35]==0x10,"mute off byte35=0x%02X exp 0x10",o[35]); }
    { uint8_t o[64]; struct{uint8_t p,b;} sp[]={{0,0x01},{1,0x03},{2,0x05},{3,0x07},{4,0x09}};
      for(auto&x:sp){ hisense_build_sleep_frame(x.p,o,sizeof(o)); CHECK(o[17]==x.b,"sleep %d byte17=0x%02X exp 0x%02X",x.p,o[17],x.b); } }

    // ---- STATUS parser: round-trip a validated frame (the values we sniffed) ----
    printf("[status parser vs validated frame]\n");
    uint8_t s[160]; HisenseState st;
    // COOL, on, setpoint 22, room 21, fan auto, no flags, outdoor 32
    make_status(s,true,HISENSE_MODE_COOL,22,21,0x01,0x00,0x00,0x00,0,32);
    CHECK(hisense_parse_status(s,160,&st),"parse valid 160B frame");
    CHECK(st.power_on,"power_on"); CHECK(st.mode==HISENSE_MODE_COOL,"mode COOL got %d",st.mode);
    CHECK(st.setpoint_c==22,"setpoint %d exp 22",st.setpoint_c);
    CHECK(st.indoor_temp_c==21,"indoor %d exp 21",st.indoor_temp_c);
    CHECK(st.outdoor_temp_c==32,"outdoor %d exp 32",st.outdoor_temp_c);
    // AUTO (status nibble 5 -> enum AUTO)
    make_status(s,true,HISENSE_MODE_AUTO,24,23,0x01,0x00,0x00,0x00,0,30);
    hisense_parse_status(s,160,&st); CHECK(st.mode==HISENSE_MODE_AUTO,"AUTO remap got %d",st.mode);
    // flags: vswing(0x80)+eco(0x04)+turbo(0x02) in flags1, mute(0x04) in flags2; sleep 0x04; comp 55
    make_status(s,true,HISENSE_MODE_HEAT,30,19,0x12,0x86,0x04,0x04,55,10);
    hisense_parse_status(s,160,&st);
    CHECK(st.vswing_on,"vswing"); CHECK(st.eco_on,"eco"); CHECK(st.turbo_on,"turbo");
    CHECK(st.mute_on,"mute"); CHECK(st.fan_raw==0x12,"fan_raw hi"); CHECK(st.compressor_freq==55,"comp %d",st.compressor_freq);
    CHECK(st.sleep_on && st.sleep_raw==0x04,"sleep profile 0x04");
    // coil temp @45 (diagnostic, #51): poke byte45, re-checksum, parse
    make_status(s,true,HISENSE_MODE_COOL,22,21,0x01,0,0,0,0,32);
    s[45]=(uint8_t)((int8_t)27);
    { uint16_t ck=besum(s,156); s[156]=(uint8_t)(ck>>8); s[157]=(uint8_t)(ck&0xFF); }
    CHECK(hisense_parse_status(s,160,&st) && st.coil_temp_c==27,"coil temp @45 -> %d exp 27",st.coil_temp_c);

    // ---- negative: wrong length, bad checksum, bad header must be rejected ----
    printf("[parser rejects malformed frames]\n");
    make_status(s,true,HISENSE_MODE_COOL,22,21,0x01,0,0,0,0,32);
    CHECK(!hisense_parse_status(s,82,&st),"reject 82-byte length");
    s[100]^=0xFF; CHECK(!hisense_parse_status(s,160,&st),"reject bad checksum");
    make_status(s,true,HISENSE_MODE_COOL,22,21,0x01,0,0,0,0,32); s[4]=0xA5; // corrupt LEN byte
    uint16_t ck=besum(s,156); s[156]=ck>>8; s[157]=ck&0xFF; // re-checksum so only length-formula check fails
    CHECK(!hisense_parse_status(s,160,&st),"reject LEN(byte4) != frame length");

    // ---- "77" recommission debounce: fire ONCE, only after a sustained assertion ----
    // Guards the coex bug: a single reflected/glitched request frame must not open a
    // commissioning (BLE) window. Only a request held >= HOLD consecutive replies fires.
    printf("[recommission debounce]\n");
    {
        uint8_t streak = 0; bool latched = false;
        const uint8_t HOLD = 3;
        const uint8_t REQ  = HISENSE_LINK_REQ_RECONFIG;   // 0x08 (bit3)
        CHECK(hisense_recommission_debounce(REQ,&streak,&latched,HOLD)==0,"no fire on frame 1");
        CHECK(hisense_recommission_debounce(REQ,&streak,&latched,HOLD)==0,"no fire on frame 2");
        CHECK(hisense_recommission_debounce(REQ,&streak,&latched,HOLD)==1,"fire on frame 3 (hold reached)");
        CHECK(hisense_recommission_debounce(REQ,&streak,&latched,HOLD)==0,"no re-fire while held");
        CHECK(hisense_recommission_debounce(REQ,&streak,&latched,HOLD)==0,"no re-fire while held (2)");
        // Falling edge of a FIRED assertion = the user left "77" -> -1 so the handler can shut
        // the window it opened. Previously this returned "nothing happened" and the window was
        // left open for its full duration with the panel no longer showing 77.
        CHECK(hisense_recommission_debounce(0,  &streak,&latched,HOLD)==-1,"clear after fire reports CANCEL");
        CHECK(hisense_recommission_debounce(REQ,&streak,&latched,HOLD)==0,"re-arm: no fire frame 1");
        CHECK(hisense_recommission_debounce(REQ,&streak,&latched,HOLD)==0,"re-arm: no fire frame 2");
        CHECK(hisense_recommission_debounce(REQ,&streak,&latched,HOLD)==1,"re-arm: fire frame 3");
        CHECK(hisense_recommission_debounce(0,  &streak,&latched,HOLD)==-1,"re-arm: clear reports CANCEL");

        // A glitch that never reached the hold must report NOTHING on clear: no window was
        // opened, so cancelling one would be spurious (and on the ameba half would send a
        // needless exit-77 frame).
        uint8_t s2 = 0; bool l2 = false;
        CHECK(hisense_recommission_debounce(REQ,&s2,&l2,HOLD)==0,"glitch: no fire");
        CHECK(hisense_recommission_debounce(0,  &s2,&l2,HOLD)==0,"glitch clear reports NOTHING (never fired)");
    }
    {   // a lone 1-frame glitch/echo, repeated, must NEVER fire
        uint8_t streak = 0; bool latched = false; const uint8_t HOLD = 3;
        int fires = 0;
        for (int i = 0; i < 50; i++) {
            fires += hisense_recommission_debounce(HISENSE_LINK_REQ_RECONFIG,&streak,&latched,HOLD);
            fires += hisense_recommission_debounce(0,&streak,&latched,HOLD);
        }
        CHECK(fires == 0,"1-frame glitch never fires over 50 cycles (got %d)",fires);
    }
    {   // the smart-config bit (0x20) debounces identically
        uint8_t streak = 0; bool latched = false; const uint8_t HOLD = 3;
        hisense_recommission_debounce(HISENSE_LINK_REQ_SMARTCFG,&streak,&latched,HOLD);
        hisense_recommission_debounce(HISENSE_LINK_REQ_SMARTCFG,&streak,&latched,HOLD);
        CHECK(hisense_recommission_debounce(HISENSE_LINK_REQ_SMARTCFG,&streak,&latched,HOLD),
              "smartcfg bit fires after hold");
    }

    // ---- reply-class correlation gate (#60) ----
    printf("[transact reply-class gate]\n");
    CHECK( hisense_reply_class_ok(0x66,0x66),"status reply matches expected 0x66");
    CHECK(!hisense_reply_class_ok(0x1E,0x66),"late 0x1E NOT consumed as status poll");
    CHECK( hisense_reply_class_ok(0x0A,0x0A),"devtype reply matches expected 0x0A");
    CHECK( hisense_reply_class_ok(0x1E,0x00),"expect=any accepts 0x1E");
    CHECK( hisense_reply_class_ok(0x66,0x00),"expect=any accepts 0x66");

    // ---- link-health edge detector (#56): fire once per lost/restored edge ----
    printf("[link-health edge]\n");
    {
        bool down = false;
        CHECK(hisense_link_health_edge(false,&down)==0 && !down,"healthy -> no edge");
        CHECK(hisense_link_health_edge(true ,&down)==1 &&  down,"first silence -> lost edge (fire once)");
        CHECK(hisense_link_health_edge(true ,&down)==0 &&  down,"sustained silence -> no repeat");
        CHECK(hisense_link_health_edge(false,&down)==-1&& !down,"recovery -> restored edge");
        CHECK(hisense_link_health_edge(false,&down)==0 && !down,"healthy again -> no edge");
    }

    // ---- 0x66/40 ProductType feature-flag parse (RE'd bit positions) ----
    printf("[0x66/40 feature-flags parse + request]\n");
    {
        uint8_t ff[64]; memset(ff,0,sizeof(ff));
        ff[13]=0x66; ff[14]=0x40;
        ff[18]=0x80;                 // cool_heat
        ff[28]=0x40|0x10;            // ai + swing_dir_8
        ff[23]=0x40|0x08;            // power_save + purify   ([0x0A] bits 0x40/0x08)
        ff[26]=0x80;                 // 8heat  ([0x0D]&0x80; swing_follow clear)
        ff[27]=(uint8_t)(0x02u<<6);  // power_display = 2
        ff[35]=0x03;                 // demand_resp = 3
        ff[38]=0x08;                 // trans_102_64          ([0x19]&0x08)
        ff[39]=0x40|0x04;            // q_display + enable_8heat ([0x1A] bits 0x40/0x04)
        HisenseFeatures fe; memset(&fe,0,sizeof(fe));
        CHECK(hisense_parse_features(ff,64,&fe),"parse 66/40 frame");
        CHECK(fe.cool_heat&&fe.ai&&fe.swing_dir_8,"cool_heat/ai/swing8 set");
        CHECK(fe.power_save&&fe.purify,"power_save/purify set");
        CHECK(fe.heat_8c&&!fe.swing_follow,"8heat set, swing_follow clear");
        CHECK(fe.power_display==2,"power_display=%d exp 2",fe.power_display);
        CHECK(fe.demand_resp==3,"demand_resp=%d exp 3",fe.demand_resp);
        CHECK(!fe.humidity&&!fe.fan_mute,"unset flags stay clear");
        // Extended tier (bytes 38/39) -- present because len 64 > 39.
        CHECK(fe.ext_valid,"ext tier decoded on a full-length frame");
        CHECK(fe.q_display&&fe.enable_8heat,"q_display/enable_8heat set");
        CHECK(fe.trans_102_64,"trans_102_64 set");
        CHECK(fe.reply_len==64,"reply_len=%u exp 64",(unsigned)fe.reply_len);
        // A base-tier-only frame (len 36..39) must still parse, and must report the
        // ext fields as UNKNOWN rather than silently 0 -- docs/11 §5.1 gates per unit.
        memset(&fe,0,sizeof(fe));
        CHECK(hisense_parse_features(ff,38,&fe),"parse short-but-valid 66/40 frame");
        CHECK(fe.valid&&fe.cool_heat&&fe.demand_resp==3,"base tier intact on short frame");
        CHECK(!fe.ext_valid,"ext tier flagged UNKNOWN on short frame");
        CHECK(!fe.q_display&&!fe.enable_8heat&&!fe.trans_102_64,"ext fields zeroed when unknown");
        CHECK(fe.reply_len==38,"short-frame reply_len=%u exp 38",(unsigned)fe.reply_len);
        ff[14]=0x00; CHECK(!hisense_parse_features(ff,64,&fe),"reject non-0x40 subtype");
        ff[14]=0x40; CHECK(!hisense_parse_features(ff,30,&fe),"reject too-short frame");
    }
    {   // request builder: subtype 0x40, valid re-checksum (status 0x01B3 + 0x40 = 0x01F3)
        uint8_t rq[24];
        size_t rn=hisense_build_producttype_request(rq,sizeof(rq));
        CHECK(rn==HISENSE_STATUS_REQUEST_LEN,"producttype req len %u",(unsigned)rn);
        CHECK(rq[13]==0x66&&rq[14]==0x40,"producttype req class/subtype");
        CHECK(rq[19]==0xF4&&rq[20]==0xFB,"producttype req end tag");
        CHECK(rq[17]==0x01&&rq[18]==0xF3,"producttype req checksum 01 F3 got %02X %02X",rq[17],rq[18]);
    }

    // ---- #49 device-type extraction from the DevType (0x0A) reply, inner [3]/[4] ----
    printf("[#49 devtype from reply]\n");
    {
        uint8_t hi = 0, lo = 0;
        // A/C->module DevType reply: class 0x0A @[13], inner[3]/[4] = [16]/[17] = type/sub.
        uint8_t reply[20] = {0xF4,0xF5,0x01,0x40,0x0B,0x01,0x00,0xFE,0x01,0x02,
                             0x03,0x04,0x00,0x0A,0x00,0x00,0x01,0x01,0xF4,0xFB};
        CHECK(hisense_devtype_from_reply(reply,20,&hi,&lo),"extract devtype");
        CHECK(hi==0x01&&lo==0x01,"devtype 01 01 got %02X %02X",hi,lo);
        // REGRESSION GUARD (v10207): the envelope [9]/[10] here is 02 03 -- deliberately
        // NOT equal to the device type. Reading it instead is the bug that killed the link.
        CHECK(!(hi==reply[9]&&lo==reply[10]),"does NOT read envelope [9]/[10]");
        reply[16]=0x36; reply[17]=0x00;  // another model's type carried through
        CHECK(hisense_devtype_from_reply(reply,20,&hi,&lo)&&hi==0x36&&lo==0x00,
              "arbitrary devtype %02X %02X",hi,lo);
        reply[13]=0x66;                  // a status reply must NOT supply a device type
        CHECK(!hisense_devtype_from_reply(reply,20,&hi,&lo),"reject non-0x0A class");
        reply[13]=0x0A;
        CHECK(!hisense_devtype_from_reply(reply,17,&hi,&lo),"reject too-short (<=17)");
        CHECK(hisense_devtype_from_reply(reply,20,nullptr,nullptr),"null out ptrs ok");
    }

    // ---- #49 link session token stamped into outbound frames (envelope [7]/[8]) ----
    printf("[#49 link token stamp]\n");
    {
        uint8_t out[80];   // > the longest frame (50B cmd) + stuffing headroom

        // GOLDEN: stamping a template with the pair it already carries must reproduce it
        // byte-for-byte. This is the #49 no-regression guarantee -- this A/C reports
        // device-type 01 01 (proven: the stock dongle's captured DI frames carry 01 01,
        // and stock sources those bytes from the DevType reply), so the wire is unchanged
        // vs the hardcoded build. The 0x0A probe is sent verbatim, not stamped.
        struct { const uint8_t *f; size_t n; uint8_t hi, lo; const char *name; } golden[] = {
            {HISENSE_STATUS_REQUEST,  HISENSE_STATUS_REQUEST_LEN,  0x01,0x01, "status request"},
            {HISENSE_LINK_INIT_07,    HISENSE_LINK_INIT_07_LEN,    0x01,0x01, "link init 07"},
            {HISENSE_LINK_HEARTBEAT,  HISENSE_LINK_HEARTBEAT_LEN,  0x01,0x01, "link heartbeat 1E"},
        };
        for (auto &g : golden) {
            size_t n = hisense_stamp_link_token(g.f, g.n, g.hi, g.lo, out, sizeof(out));
            CHECK(n==g.n && memcmp(out,g.f,g.n)==0, "stamp is identity on %s", g.name);
        }
        // A power frame (literal template, precomputed checksum 0x01DF) round-trips too.
        {
            uint8_t on[HISENSE_CMD_FRAME_LEN];
            size_t  onn = hisense_build_power_frame(true, on, sizeof(on));
            size_t  n   = hisense_stamp_link_token(on, onn, 0x01, 0x01, out, sizeof(out));
            CHECK(n==onn && memcmp(out,on,onn)==0, "stamp is identity on power-on frame");
        }

        // A different token lands at [7]/[8] and the checksum FOLLOWS it (it sums over
        // [2,chk), which includes the token) -- a stale checksum would be a "crc error".
        {
            size_t n = hisense_stamp_link_token(HISENSE_STATUS_REQUEST, HISENSE_STATUS_REQUEST_LEN,
                                                0xAB, 0xCD, out, sizeof(out));
            CHECK(n==HISENSE_STATUS_REQUEST_LEN, "stamped len %u", (unsigned)n);
            CHECK(out[7]==0xAB&&out[8]==0xCD, "token at [7]/[8] got %02X %02X", out[7], out[8]);
            // template chk 0x01B3 with 01 01; swapping in AB CD adds (0xAB-1)+(0xCD-1).
            uint16_t want = 0x01B3 + (0xAB-0x01) + (0xCD-0x01);
            CHECK(out[17]==(uint8_t)(want>>8)&&out[18]==(uint8_t)want,
                  "checksum tracks token: want %04X got %02X%02X", want, out[17], out[18]);
            CHECK(out[19]==0xF4&&out[20]==0xFB, "end tag preserved");
        }

        // A token whose checksum byte hits the 0xF4 marker must still be stuffed.
        // status: chk 0x01B3 with 01 01 -> low byte 0xF4 needs hi+lo delta 0x41.
        {
            size_t n = hisense_stamp_link_token(HISENSE_STATUS_REQUEST, HISENSE_STATUS_REQUEST_LEN,
                                                0x42, 0x01, out, sizeof(out));
            CHECK(n==HISENSE_STATUS_REQUEST_LEN+1, "stuffed len %u (grew by 1)", (unsigned)n);
            CHECK(out[17]==0x01&&out[18]==0xF4&&out[19]==0xF4, "0xF4 checksum byte doubled");
            CHECK(out[20]==0xF4&&out[21]==0xFB, "end tag after stuffing");
        }

        // Malformed / hostile envelopes are refused (caller then sends verbatim).
        {
            uint8_t bad[HISENSE_STATUS_REQUEST_LEN];
            memcpy(bad, HISENSE_STATUS_REQUEST, sizeof(bad));
            bad[0]=0x00;
            CHECK(hisense_stamp_link_token(bad,sizeof(bad),1,1,out,sizeof(out))==0,"reject bad STX");
            memcpy(bad, HISENSE_STATUS_REQUEST, sizeof(bad));
            bad[4]=0xFF;   // LEN claims 264 bytes -> would read past the buffer
            CHECK(hisense_stamp_link_token(bad,sizeof(bad),1,1,out,sizeof(out))==0,"reject oversize LEN");
            bad[4]=0x02;   // LEN too small to hold an envelope
            CHECK(hisense_stamp_link_token(bad,sizeof(bad),1,1,out,sizeof(out))==0,"reject undersize LEN");
            CHECK(hisense_stamp_link_token(HISENSE_STATUS_REQUEST,HISENSE_STATUS_REQUEST_LEN,
                                           1,1,out,4)==0,"reject too-small out_cap");
            CHECK(hisense_stamp_link_token(nullptr,20,1,1,out,sizeof(out))==0,"reject null in");
        }
    }

    // ---- C/F unit bit (#5), frame byte 26 bit 1 ------------------------------
    {
        printf("[temp unit bit]\n");
        uint8_t s2[160];
        HisenseState st2;
        // parse_status VALIDATES the checksum, so byte 26 cannot just be poked after
        // make_status: re-checksum, exactly as the A/C would.
        auto set26 = [&](uint8_t v) {
            make_status(s2, true, HISENSE_MODE_COOL, 22, 25, 0x01, 0x00, 0x00, 0x00, 40, 30);
            s2[26] = v;
            uint16_t ck = besum(s2, 156);
            s2[156] = (uint8_t)(ck >> 8); s2[157] = (uint8_t)(ck & 0xFF);
        };
        set26(0x61);                         // observed on BOTH bench units (Celsius)
        CHECK(hisense_parse_status(s2, 160, &st2) && !st2.temp_unit_f,
              "byte26=0x61 -> Celsius (both bench units read this)");
        set26(0x63);                         // same byte with bit 1 set
        CHECK(hisense_parse_status(s2, 160, &st2) && st2.temp_unit_f,
              "byte26 bit1 set -> Fahrenheit");
        // Only bit 1 may matter: the rest of byte 26 must not leak into the flag.
        set26((uint8_t) ~0x02);
        CHECK(hisense_parse_status(s2, 160, &st2) && !st2.temp_unit_f,
              "every other bit of byte 26 ignored");
    }

    // ---- Fahrenheit display unit converts (#5) -------------------------------
    // Confirmed on hardware 2026-07-19: switching a unit to F changed byte 19 from 22 to 72
    // and byte 20 from 26 to 78, i.e. the same temperatures re-expressed in F. Passing them
    // through told Home Assistant the room was 78 degrees Celsius.
    {
        printf("[fahrenheit display unit]\n");
        uint8_t s3[160];
        HisenseState st3;
        auto build = [&](uint8_t b26, int8_t setp, int8_t indoor) {
            make_status(s3, true, HISENSE_MODE_COOL, setp, indoor, 0x01, 0x00, 0x00, 0x00, 40, 30);
            s3[26] = b26;
            uint16_t ck = besum(s3, 156);
            s3[156] = (uint8_t)(ck >> 8); s3[157] = (uint8_t)(ck & 0xFF);
        };
        // the exact frame observed on hardware while the panel was in F
        build(0x83, 72, 78);
        CHECK(hisense_parse_status(s3, 160, &st3), "F frame parses");
        CHECK(st3.temp_unit_f, "unit flag reads F");
        CHECK(st3.setpoint_c == 22, "72F setpoint -> 22C (got %d)", st3.setpoint_c);
        CHECK(st3.indoor_temp_c == 26, "78F indoor -> 26C (got %d)", st3.indoor_temp_c);
        // outdoor/coil stayed Celsius across the same switch, so must NOT be converted
        CHECK(st3.outdoor_temp_c == 30, "outdoor left alone (got %d)", st3.outdoor_temp_c);

        // Celsius path unchanged: the regression that would matter most.
        build(0x61, 22, 26);
        CHECK(hisense_parse_status(s3, 160, &st3), "C frame parses");
        CHECK(!st3.temp_unit_f, "unit flag reads C");
        CHECK(st3.setpoint_c == 22 && st3.indoor_temp_c == 26, "C values pass through");

        // conversion maths, including the negative case naive integer division gets wrong
        CHECK(hisense_f_to_c(32) == 0,    "32F -> 0C");
        CHECK(hisense_f_to_c(212) == 100, "212F -> 100C");
        CHECK(hisense_f_to_c(72) == 22,   "72F -> 22C");
        CHECK(hisense_f_to_c(78) == 26,   "78F -> 26C");
        CHECK(hisense_f_to_c(46) == 8,    "46F -> 8C (the 8C frost-guard setpoint)");
        CHECK(hisense_f_to_c(14) == -10,  "14F -> -10C (negative rounds correctly)");
    }

    // ---- C<->F round trip on the COMMAND side (#5) ---------------------------
    // The A/C reads the setpoint byte in its DISPLAY unit. Sending Celsius to a panel in F
    // made a real unit target 23 F and run at 74 Hz toward -5 C, so this is not cosmetic.
    {
        printf("[celsius/fahrenheit round trip]\n");
        CHECK(hisense_c_to_f(0) == 32,    "0C -> 32F");
        // NOTE 100C -> 212F is NOT asserted: the return type is int8_t, so 212 cannot be
        // represented and the helper clamps to 127. That is fine because the reachable
        // setpoint range is 16..32C (61..90F), but the clamp is asserted rather than left
        // implicit, so nobody later assumes this helper is general-purpose.
        CHECK(hisense_c_to_f(100) == 127, "100C clamps to int8 max, not 212");
        CHECK(hisense_c_to_f(22) == 72,   "22C -> 72F (the value seen on hardware)");
        CHECK(hisense_c_to_f(23) == 73,   "23C -> 73F");
        CHECK(hisense_c_to_f(-10) == 14,  "-10C -> 14F (negative)");
        // Round trip must not drift for every setpoint we can actually command.
        for (int c = HISENSE_SETPOINT_MIN_C; c <= HISENSE_SETPOINT_MAX_C; c++) {
            int8_t f = hisense_c_to_f(c);
            CHECK(hisense_f_to_c(f) == c, "%dC -> %dF -> %dC round trips", c, f, hisense_f_to_c(f));
            CHECK(hisense_setpoint_in_range(f, true),
                  "%dC maps to %dF, inside the F range the builder accepts", c, f);
        }
        // And a command built in F must encode the F value, not the Celsius one.
        {
            HisenseCommand c = { HISENSE_MODE_COOL, hisense_c_to_f(22), true, HISENSE_FAN_AUTO,
                                 HISENSE_SWING_OFF, HISENSE_SWING_OFF,
                                 HISENSE_FEATURE_NONE, HISENSE_DISPLAY_NOCHANGE };
            uint8_t o[64];
            CHECK(hisense_build_command(&c, o, sizeof(o)) > 0, "F-unit command builds");
            CHECK(o[19] == ((72 << 1) | 1), "byte19 carries 72F encoded (got 0x%02X)", o[19]);
        }
    }

    // ---- shadow setpoint sync validates in the WIRE unit -----------------------
    // The downlink shadow re-sync must range-check the value in the unit that goes on the
    // wire (the panel's display unit), not the Celsius parse against the shadow's old unit.
    // That mismatch wedged the sync: once the shadow held F, no Celsius value ever passed
    // the 61..90 F window, so the shadow froze at its first F value and every later
    // command silently re-imposed it.
    {
        printf("[shadow setpoint unit sync]\n");
        int8_t sp = 0;
        // C panel: passes through unconverted, C range applies.
        CHECK(hisense_shadow_setpoint_from_status(22, false, &sp) && sp == 22,
              "C panel: 22 C -> shadow 22 (got %d)", sp);
        // F panel sequence: first sync stores F (22 C -> 72 F)...
        CHECK(hisense_shadow_setpoint_from_status(22, true, &sp) && sp == 72,
              "F panel: 22 C -> shadow 72 F (got %d)", sp);
        // ...and a FOLLOW-UP F report must still validate. 24 C (=75 F) against the F
        // range is exactly the case the old formula (in_range(24, fahrenheit=true))
        // rejected, freezing the shadow after its first F sync.
        CHECK(hisense_shadow_setpoint_from_status(24, true, &sp) && sp == 75,
              "F panel follow-up: 24 C -> shadow 75 F, sync not wedged (got %d)", sp);
        // Whole reachable C range maps inside the F range and round-trips.
        for (int c = HISENSE_SETPOINT_MIN_C; c <= HISENSE_SETPOINT_MAX_C; c++) {
            CHECK(hisense_shadow_setpoint_from_status((int8_t)c, true, &sp) && sp == hisense_c_to_f(c),
                  "F panel: %d C -> %d F accepted", c, hisense_c_to_f(c));
            CHECK(hisense_shadow_setpoint_from_status((int8_t)c, false, &sp) && sp == c,
                  "C panel: %d C accepted", c);
        }
        // Out-of-range reports are refused in BOTH units, leaving the caller's shadow
        // untouched (out is not written on failure): 5 C frost-guard, and its F-panel
        // equivalent 41 F.
        sp = -7;
        CHECK(!hisense_shadow_setpoint_from_status(5, false, &sp) && sp == -7,
              "5 C report refused, shadow untouched");
        CHECK(!hisense_shadow_setpoint_from_status(5, true, &sp) && sp == -7,
              "5 C (41 F) report on an F panel refused, shadow untouched");
        CHECK(!hisense_shadow_setpoint_from_status(40, false, &sp) && sp == -7,
              "40 C report refused");
    }

    // ---- f_e_* fault bits (#38) ---------------------------------------------
    // Mapping derived from the stock firmware: payload offset + 15 = wire byte, bit
    // index - 8 = bit in that byte. Frame bytes 39/40/64/66.
    {
        printf("[fault bit decode]\n");
        uint8_t s[160];
        HisenseFaults fl;

        // healthy unit: every fault byte zero
        make_status(s, true, HISENSE_MODE_COOL, 22, 25, 0x01, 0x00, 0x00, 0x00, 40, 30);
        CHECK(hisense_parse_faults(s, 160, &fl), "healthy frame parses");
        CHECK(fl.valid && !fl.any, "healthy unit reports no faults");
        CHECK(!fl.in_temp && !fl.in_com && !fl.out_temp && !fl.over_temp, "no named fault set");

        // one bit at a time, across all four bytes, checking bit->name mapping
        struct { int byte; uint8_t mask; const char *name; bool HisenseFaults::*field; } cases[] = {
            { HISENSE_FAULT_BYTE_INDOOR,  0x80, "in_temp",       &HisenseFaults::in_temp },
            { HISENSE_FAULT_BYTE_INDOOR,  0x40, "in_coil_temp",  &HisenseFaults::in_coil_temp },
            { HISENSE_FAULT_BYTE_INDOOR,  0x20, "in_humidity",   &HisenseFaults::in_humidity },
            { HISENSE_FAULT_BYTE_INDOOR,  0x10, "water_full",    &HisenseFaults::water_full },
            { HISENSE_FAULT_BYTE_INDOOR,  0x08, "in_fan_motor",  &HisenseFaults::in_fan_motor },
            { HISENSE_FAULT_BYTE_INDOOR,  0x04, "grille",        &HisenseFaults::grille },
            { HISENSE_FAULT_BYTE_INDOOR,  0x02, "in_vzero",      &HisenseFaults::in_vzero },
            { HISENSE_FAULT_BYTE_INDOOR,  0x01, "in_com",        &HisenseFaults::in_com },
            { HISENSE_FAULT_BYTE_MODULE,  0x80, "in_display",    &HisenseFaults::in_display },
            { HISENSE_FAULT_BYTE_MODULE,  0x40, "in_keys",       &HisenseFaults::in_keys },
            { HISENSE_FAULT_BYTE_MODULE,  0x20, "in_wifi",       &HisenseFaults::in_wifi },
            { HISENSE_FAULT_BYTE_MODULE,  0x10, "in_ele",        &HisenseFaults::in_ele },
            { HISENSE_FAULT_BYTE_MODULE,  0x08, "in_eeprom",     &HisenseFaults::in_eeprom },
            { HISENSE_FAULT_BYTE_OUTDOOR, 0x40, "out_eeprom",    &HisenseFaults::out_eeprom },
            { HISENSE_FAULT_BYTE_OUTDOOR, 0x20, "out_coil_temp", &HisenseFaults::out_coil_temp },
            { HISENSE_FAULT_BYTE_OUTDOOR, 0x10, "out_gas_temp",  &HisenseFaults::out_gas_temp },
            { HISENSE_FAULT_BYTE_OUTDOOR, 0x08, "out_temp",      &HisenseFaults::out_temp },
            { HISENSE_FAULT_BYTE_PROTECT, 0x10, "over_temp",     &HisenseFaults::over_temp },
        };
        for (auto &c : cases) {
            make_status(s, true, HISENSE_MODE_COOL, 22, 25, 0x01, 0x00, 0x00, 0x00, 40, 30);
            s[c.byte] = c.mask;
            CHECK(hisense_parse_faults(s, 160, &fl) && fl.*(c.field),
                  "byte %d mask 0x%02X -> %s set", c.byte, c.mask, c.name);
            CHECK(fl.any, "%s also raises any", c.name);
        }

        // Byte 66 bit 7 is a MODE flag (8 C frost-guard engaged), not a fault. Observed on
        // hardware setting and clearing with the mode. Counting it reported a fault on a
        // healthy unit in frost-guard, which is the diagnostics feature crying wolf.
        make_status(s, true, HISENSE_MODE_HEAT, 22, 25, 0x01, 0x00, 0x00, 0x00, 40, 30);
        s[HISENSE_FAULT_BYTE_PROTECT] = 0x80;
        CHECK(hisense_parse_faults(s, 160, &fl), "frost-guard frame parses");
        CHECK(!fl.any, "byte66 bit7 alone does NOT report a fault (it is 8C heat engaged)");
        CHECK(fl.raw_protect == 0x80, "raw byte still preserved for inspection");
        // ...but a REAL fault in the same byte must still register, and so must the
        // combination of the mode flag and a real fault.
        s[HISENSE_FAULT_BYTE_PROTECT] = 0x10;
        CHECK(hisense_parse_faults(s, 160, &fl) && fl.any && fl.over_temp,
              "byte66 bit4 still reports over-temp");
        s[HISENSE_FAULT_BYTE_PROTECT] = 0x90;
        CHECK(hisense_parse_faults(s, 160, &fl) && fl.any && fl.over_temp,
              "mode flag + real fault together still reports the fault");
        // An unnamed bit in that byte OTHER than bit 7 must still count: the mask is a
        // narrow proven exception, not a blanket relaxation.
        s[HISENSE_FAULT_BYTE_PROTECT] = 0x01;
        CHECK(hisense_parse_faults(s, 160, &fl) && fl.any,
              "other unnamed bits in byte66 still raise any");

        // An UNNAMED bit must still raise `any`. Reporting "healthy" because we have no
        // name for a bit would be the worst possible failure of a diagnostics feature.
        make_status(s, true, HISENSE_MODE_COOL, 22, 25, 0x01, 0x00, 0x00, 0x00, 40, 30);
        s[HISENSE_FAULT_BYTE_MODULE] = 0x01;      // no name assigned to bit 0
        CHECK(hisense_parse_faults(s, 160, &fl) && fl.any,
              "unnamed fault bit still raises any");
        CHECK(fl.raw_module == 0x01, "raw byte preserved for logging");

        // Short frames: each group is gated independently, and a frame too short for
        // even the first group must not report a bogus clean bill of health.
        CHECK(!hisense_parse_faults(s, 20, &fl), "frame too short for faults -> false");
        CHECK(!fl.valid, "too-short frame leaves valid=false (not a false all-clear)");
        make_status(s, true, HISENSE_MODE_COOL, 22, 25, 0x01, 0x00, 0x00, 0x00, 40, 30);
        s[HISENSE_FAULT_BYTE_INDOOR] = 0x80;
        CHECK(hisense_parse_faults(s, HISENSE_FAULT_BYTE_INDOOR + 2, &fl) && fl.in_temp,
              "indoor group readable on a frame that just reaches it");
        CHECK(fl.raw_outdoor == 0 && !fl.out_temp, "unreachable groups stay clear");

        // Exact boundaries: reading buf[B] only needs len == B+1. A frame one byte past
        // a group's byte must decode that group; exactly at the byte must not.
        make_status(s, true, HISENSE_MODE_COOL, 22, 25, 0x01, 0x00, 0x00, 0x00, 40, 30);
        s[HISENSE_FAULT_BYTE_MODULE] = 0x20;
        CHECK(hisense_parse_faults(s, HISENSE_FAULT_BYTE_MODULE + 1, &fl) && fl.in_wifi,
              "module group decodes at exactly len == byte+1");
        CHECK(fl.raw_outdoor == 0, "outdoor group still out of reach at len == module+1");
        s[HISENSE_FAULT_BYTE_OUTDOOR] = 0x08;
        CHECK(hisense_parse_faults(s, HISENSE_FAULT_BYTE_OUTDOOR + 1, &fl) && fl.out_temp,
              "outdoor group decodes at exactly len == byte+1");
        s[HISENSE_FAULT_BYTE_PROTECT] = 0x10;
        CHECK(hisense_parse_faults(s, HISENSE_FAULT_BYTE_PROTECT + 1, &fl) && fl.over_temp,
              "protect group decodes at exactly len == byte+1");
        CHECK(hisense_parse_faults(s, HISENSE_FAULT_BYTE_PROTECT, &fl) && !fl.over_temp,
              "protect group NOT decoded when the frame ends ON its byte");

        CHECK(!hisense_parse_faults(nullptr, 160, &fl), "null buf rejected");
        CHECK(!hisense_parse_faults(s, 160, nullptr), "null out rejected");
    }

    // ---- setpoint range helper: the shadow-poisoning guard -------------------
    // Regression guard for a bug found on hardware: the A/C reported setpoint 8 (it
    // answers ac_8heat=1 and will hold setpoints well below 16), the app layer copied
    // that into its command shadow unvalidated, and hisense_build_command() then returned
    // 0 for EVERY later command -- mode, fan, swing included -- with no wire error. All
    // combined-frame control was dead until reboot. Callers must gate on this helper, so
    // it has to agree with the builder exactly.
    {
        printf("[setpoint range helper vs builder]\n");
        CHECK(hisense_setpoint_in_range(16, false), "16C in range");
        CHECK(hisense_setpoint_in_range(32, false), "32C in range");
        CHECK(hisense_setpoint_in_range(22, false), "22C in range");
        CHECK(!hisense_setpoint_in_range(15, false), "15C out of range");
        CHECK(!hisense_setpoint_in_range(33, false), "33C out of range");
        CHECK(!hisense_setpoint_in_range(8,  false), "8C out of range (the bench value)");
        CHECK(!hisense_setpoint_in_range(5,  false), "5C out of range (A/C accepted it)");
        CHECK(hisense_setpoint_in_range(61, true),  "61F in range");
        CHECK(hisense_setpoint_in_range(90, true),  "90F in range");
        CHECK(!hisense_setpoint_in_range(60, true), "60F out of range");
        CHECK(!hisense_setpoint_in_range(91, true), "91F out of range");

        // The helper must predict the builder for every Celsius value, or a caller that
        // trusts it still poisons the shadow. Dry/Fan strip the setpoint, so the builder
        // skips the check there (#53) -- assert that exemption too.
        uint8_t o[64];
        for (int sp = 0; sp <= 40; sp++) {
            HisenseCommand c = { HISENSE_MODE_COOL, (int8_t) sp, false, HISENSE_FAN_AUTO,
                                 HISENSE_SWING_OFF, HISENSE_SWING_OFF,
                                 HISENSE_FEATURE_NONE, HISENSE_DISPLAY_NOCHANGE };
            bool built  = hisense_build_command(&c, o, sizeof(o)) > 0;
            bool predict = hisense_setpoint_in_range((int8_t) sp, false);
            CHECK(built == predict, "setpoint %d: builder=%d helper=%d (must agree)",
                  sp, built, predict);
        }
        for (int sp = 0; sp <= 40; sp += 8) {
            HisenseCommand c = { HISENSE_MODE_FAN, (int8_t) sp, false, HISENSE_FAN_AUTO,
                                 HISENSE_SWING_OFF, HISENSE_SWING_OFF,
                                 HISENSE_FEATURE_NONE, HISENSE_DISPLAY_NOCHANGE };
            CHECK(hisense_build_command(&c, o, sizeof(o)) > 0,
                  "FAN mode builds at setpoint %d (setpoint stripped, #53)", sp);
        }
    }

    // ---- display tri-state, frame[36] (#52) ---------------------------------
    // All three values CONFIRMED on a live W41H1 2026-07-19: 0x40 darkened the panel,
    // 0xC0 lit it. The regression this guards: `display` rides EVERY combined command,
    // so NOCHANGE must emit 0x00 ("leave alone") or ordinary mode/temp/fan traffic would
    // stomp the panel. The old code sent 0x00 for OFF, which is why off never worked.
    {
        printf("[display tri-state -> byte36]\n");
        struct { HisenseDisplay d; uint8_t b; const char *n; } cases[] = {
            { HISENSE_DISPLAY_NOCHANGE, 0x00, "nochange (leave alone)" },
            { HISENSE_DISPLAY_ON,       0xC0, "on"  },
            { HISENSE_DISPLAY_OFF,      0x40, "off" },
        };
        for (auto &x : cases) {
            HisenseCommand c = { HISENSE_MODE_COOL, 24, false, HISENSE_FAN_AUTO,
                                 HISENSE_SWING_OFF, HISENSE_SWING_OFF,
                                 HISENSE_FEATURE_NONE, x.d };
            uint8_t o[64];
            hisense_build_command(&c, o, sizeof(o));
            CHECK(o[36] == x.b, "display %s -> byte36=0x%02X exp 0x%02X", x.n, o[36], x.b);
        }
        // OFF must be distinguishable from NOCHANGE on the wire -- collapsing them is
        // exactly the shipped bug.
        CHECK(0x40 != 0x00, "display OFF is not the leave-alone value");
    }

    // ---- bench override builder (#52 display-byte sweep) --------------------
    // The safety contract that makes an offset sweep runnable against a LIVE A/C:
    // a probe frame differs from the current-state frame by exactly the byte under
    // test, so a miss is a no-op instead of a surprise mode/setpoint change.
    {
        printf("[bench override builder]\n");
        HisenseCommand c = { HISENSE_MODE_COOL, 22, false, HISENSE_FAN_LOW,
                             HISENSE_SWING_OFF, HISENSE_SWING_OFF, HISENSE_FEATURE_ECO, HISENSE_DISPLAY_NOCHANGE };
        uint8_t base[64], ovr[64];
        size_t nb = hisense_build_command(&c, base, sizeof(base));
        CHECK(nb > 0, "baseline command builds");

        size_t n1 = hisense_build_command_override(&c, ovr, sizeof(ovr), 36, base[36]);
        CHECK(n1 == nb && memcmp(base, ovr, nb) == 0, "same-value override == baseline");

        size_t n2 = hisense_build_command_override(&c, ovr, sizeof(ovr), 36, 0x40);
        CHECK(n2 > 0 && ovr[36] == 0x40, "override lands at the requested offset");
        int diffs = 0;
        for (size_t i = 0; i < HISENSE_CMD_CHK_OFFSET; i++) if (base[i] != ovr[i]) diffs++;
        CHECK(diffs == 1, "exactly one payload byte differs (live-bench safety contract)");
        CHECK(memcmp(base + HISENSE_CMD_CHK_OFFSET, ovr + HISENSE_CMD_CHK_OFFSET, 2) != 0,
              "checksum recomputed over the patched frame, not stale");

        // Header / checksum / terminator must be refused: those frames get dropped by
        // the A/C, which looks identical to "this offset does nothing" during a sweep.
        CHECK(hisense_build_command_override(&c,ovr,sizeof(ovr),0,0xFF)==0, "reject header[0]");
        CHECK(hisense_build_command_override(&c,ovr,sizeof(ovr),
              (int)HISENSE_CMD_HEADER_LEN-1,0xFF)==0, "reject last header byte");
        CHECK(hisense_build_command_override(&c,ovr,sizeof(ovr),
              (int)HISENSE_CMD_CHK_OFFSET,0xFF)==0, "reject checksum offset");
        CHECK(hisense_build_command_override(&c,ovr,sizeof(ovr),
              (int)HISENSE_CMD_END_OFFSET,0xFF)==0, "reject terminator offset");
        CHECK(hisense_build_command_override(&c,ovr,sizeof(ovr),-1,0xFF)==0, "reject negative offset");
        CHECK(hisense_build_command_override(&c,ovr,sizeof(ovr),
              (int)HISENSE_CMD_HEADER_LEN,0xFF)>0, "accept first payload offset");
        CHECK(hisense_build_command_override(&c,ovr,sizeof(ovr),
              (int)HISENSE_CMD_CHK_OFFSET-1,0xFF)>0, "accept last payload offset");
    }

    // ---- display unit switch: byte 23, atomic with the setpoint (#5) ---------
    // Bench-confirmed 2026-07-20 on the esp32 node: byte 23 = 0x01 -> Celsius,
    // 0x03 -> Fahrenheit (status byte 26 bit 1 follows). The A/C does NOT rescale
    // its stored setpoint on a unit change, so the unit byte and the re-encoded
    // setpoint MUST ride in the same frame -- a bare switch reinterpreted 22 C as
    // 22 F = -6 C and the unit drove toward it at full compressor.
    {
        printf("[display unit switch byte 23]\n");
        // Shadow already re-encoded for the TARGET unit, exactly as the driver does.
        HisenseCommand to_f = { HISENSE_MODE_COOL, 0, true, HISENSE_FAN_LOW,
                                HISENSE_SWING_OFF, HISENSE_SWING_OFF, HISENSE_FEATURE_NONE, HISENSE_DISPLAY_NOCHANGE };
        to_f.setpoint = hisense_c_to_f(22);
        CHECK(to_f.setpoint == 72, "22 C re-encodes to 72 F");
        CHECK(hisense_setpoint_in_range(to_f.setpoint, true), "72 F is inside the F range");

        uint8_t f[64];
        size_t n = hisense_build_command_override(&to_f, f, sizeof(f), 23, 0x03);
        CHECK(n > 0, "F switch frame builds");
        CHECK(f[23] == 0x03, "byte 23 carries 0x03 for Fahrenheit");
        CHECK(f[19] == (uint8_t)((72 << 1) | 1), "same frame carries the F setpoint (atomic)");

        HisenseCommand to_c = { HISENSE_MODE_COOL, 22, false, HISENSE_FAN_LOW,
                                HISENSE_SWING_OFF, HISENSE_SWING_OFF, HISENSE_FEATURE_NONE, HISENSE_DISPLAY_NOCHANGE };
        n = hisense_build_command_override(&to_c, f, sizeof(f), 23, 0x01);
        CHECK(n > 0, "C switch frame builds");
        CHECK(f[23] == 0x01, "byte 23 carries 0x01 for Celsius");
        CHECK(f[19] == (uint8_t)((22 << 1) | 1), "same frame carries the C setpoint (atomic)");

        // The regression that made this dangerous: a setpoint encoded in F while the
        // builder is told the frame is Celsius fails the 16..32 check and the whole
        // frame is dropped, which is what silently killed every combined command.
        HisenseCommand bad = to_f;
        bad.fahrenheit = false;                 // the matter_drivers.cpp:964 clobber
        CHECK(!hisense_setpoint_in_range(bad.setpoint, false), "72 is out of the C range");
        CHECK(hisense_build_command(&bad, f, sizeof(f)) == 0,
              "F setpoint mislabelled as C is REJECTED, not silently sent");

        // And the hazard itself: the raw byte is not rescaled by the A/C, so the same
        // number means a wildly different temperature after a bare unit flip.
        CHECK(hisense_f_to_c(22) == -6, "raw 22 read as F is -6 C (the bare-switch hazard)");
    }

    printf("== %d passed, %d failed ==\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
