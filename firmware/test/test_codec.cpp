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
    { HisenseCommand c={HISENSE_MODE_AUTO,24,false,HISENSE_FAN_AUTO,HISENSE_SWING_OFF,HISENSE_SWING_OFF,HISENSE_FEATURE_NONE,false};
      hisense_build_command(&c,f,sizeof(f)); CHECK(f[18]==0x90,"AUTO byte18=0x%02X exp 0x90",f[18]); }
    { HisenseCommand c={HISENSE_MODE_COOL,24,false,HISENSE_FAN_AUTO,HISENSE_SWING_OFF,HISENSE_SWING_OFF,HISENSE_FEATURE_NONE,false};
      hisense_build_command(&c,f,sizeof(f)); CHECK(f[18]==0x50,"COOL byte18=0x%02X exp 0x50",f[18]); }
    { HisenseCommand c={HISENSE_MODE_HEAT,24,false,HISENSE_FAN_AUTO,HISENSE_SWING_OFF,HISENSE_SWING_OFF,HISENSE_FEATURE_NONE,false};
      hisense_build_command(&c,f,sizeof(f)); CHECK(f[18]==0x30,"HEAT byte18=0x%02X exp 0x30",f[18]); }
    { HisenseCommand c={HISENSE_MODE_DRY,24,false,HISENSE_FAN_AUTO,HISENSE_SWING_OFF,HISENSE_SWING_OFF,HISENSE_FEATURE_NONE,false};
      hisense_build_command(&c,f,sizeof(f)); CHECK(f[18]==0x70,"DRY byte18=0x%02X exp 0x70",f[18]); }
    { HisenseCommand c={HISENSE_MODE_FAN,24,false,HISENSE_FAN_AUTO,HISENSE_SWING_OFF,HISENSE_SWING_OFF,HISENSE_FEATURE_NONE,false};
      hisense_build_command(&c,f,sizeof(f)); CHECK(f[18]==0x10,"FANmode byte18=0x%02X exp 0x10",f[18]); }
    // six fan speeds
    struct { HisenseFanSpeed s; uint8_t b; const char* n; } fans[] = {
      {HISENSE_FAN_AUTO,0x01,"auto"},{HISENSE_FAN_QUIET,0x03,"quiet"},{HISENSE_FAN_LOW,0x0B,"low"},
      {HISENSE_FAN_MED_LOW,0x0D,"med-low"},{HISENSE_FAN_MID,0x0F,"mid"},{HISENSE_FAN_MED_HIGH,0x11,"med-high"},{HISENSE_FAN_HIGH,0x13,"high"} };
    for (auto &x : fans) { HisenseCommand c={HISENSE_MODE_COOL,24,false,x.s,HISENSE_SWING_OFF,HISENSE_SWING_OFF,HISENSE_FEATURE_NONE,false};
      hisense_build_command(&c,f,sizeof(f)); CHECK(f[16]==x.b,"fan %s byte16=0x%02X exp 0x%02X",x.n,f[16],x.b); }
    // temp 22C -> 2n+1 = 0x2D
    { HisenseCommand c={HISENSE_MODE_COOL,22,false,HISENSE_FAN_AUTO,HISENSE_SWING_OFF,HISENSE_SWING_OFF,HISENSE_FEATURE_NONE,false};
      hisense_build_command(&c,f,sizeof(f)); CHECK(f[19]==0x2D,"temp22 byte19=0x%02X exp 0x2D",f[19]); }
    // eco / turbo
    { HisenseCommand c={HISENSE_MODE_COOL,24,false,HISENSE_FAN_AUTO,HISENSE_SWING_OFF,HISENSE_SWING_OFF,HISENSE_FEATURE_ECO,false};
      hisense_build_command(&c,f,sizeof(f)); CHECK(f[33]==0x30,"eco byte33=0x%02X exp 0x30",f[33]); }
    { HisenseCommand c={HISENSE_MODE_COOL,24,false,HISENSE_FAN_AUTO,HISENSE_SWING_OFF,HISENSE_SWING_OFF,HISENSE_FEATURE_TURBO,false};
      hisense_build_command(&c,f,sizeof(f)); CHECK(f[33]==0x0C,"turbo byte33=0x%02X exp 0x0C",f[33]); }
    // vertical swing
    { HisenseCommand c={HISENSE_MODE_COOL,24,false,HISENSE_FAN_AUTO,HISENSE_SWING_SWING,HISENSE_SWING_OFF,HISENSE_FEATURE_NONE,false};
      hisense_build_command(&c,f,sizeof(f)); CHECK(f[32]==0xC0,"vswing byte32=0x%02X exp 0xC0",f[32]); }
    // every command frame must carry a valid 2-byte checksum + F4 FB end
    { HisenseCommand c={HISENSE_MODE_COOL,24,false,HISENSE_FAN_HIGH,HISENSE_SWING_OFF,HISENSE_SWING_OFF,HISENSE_FEATURE_NONE,false};
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
        HisenseCommand c={modes[mi],(int8_t)s,false,fans[fi],HISENSE_SWING_OFF,HISENSE_SWING_OFF,feats[qi],false};
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
    { HisenseCommand c={HISENSE_MODE_COOL,22,false,HISENSE_FAN_LOW,HISENSE_SWING_SWING,HISENSE_SWING_OFF,HISENSE_FEATURE_ECO,false};
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
        CHECK(!hisense_recommission_debounce(REQ,&streak,&latched,HOLD),"no fire on frame 1");
        CHECK(!hisense_recommission_debounce(REQ,&streak,&latched,HOLD),"no fire on frame 2");
        CHECK( hisense_recommission_debounce(REQ,&streak,&latched,HOLD),"fire on frame 3 (hold reached)");
        CHECK(!hisense_recommission_debounce(REQ,&streak,&latched,HOLD),"no re-fire while held");
        CHECK(!hisense_recommission_debounce(REQ,&streak,&latched,HOLD),"no re-fire while held (2)");
        CHECK(!hisense_recommission_debounce(0,  &streak,&latched,HOLD),"clear re-arms (no fire)");
        CHECK(!hisense_recommission_debounce(REQ,&streak,&latched,HOLD),"re-arm: no fire frame 1");
        CHECK(!hisense_recommission_debounce(REQ,&streak,&latched,HOLD),"re-arm: no fire frame 2");
        CHECK( hisense_recommission_debounce(REQ,&streak,&latched,HOLD),"re-arm: fire frame 3");
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

    printf("== %d passed, %d failed ==\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
