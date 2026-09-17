// test_matter_map.cpp -- Matter-side QA (no chip): asserts the Matter<->Hisense
// mapping AND ties it end-to-end to the validated wire bytes. This is the "does
// a Matter attribute write produce the right A/C command?" test -- the host
// equivalent of a chip-tool write, without needing a commissioned device.

#include "hal_stub.h"
#include "matter_aircon_map.h"
#include "power_estimate.h"
#include "hisense_rs485.h"
#include "test_common.h"
#include <cstdio>
#include <initializer_list>

// build a command from a shadow and return byte @off
static uint8_t cmd_byte(HisenseCommand c, int off) {
    uint8_t f[64]; hisense_build_command(&c, f, sizeof(f)); return f[off];
}

int main() {
    printf("== Matter <-> Hisense mapping tests ==\n");
    HisenseMode hm; bool v, h;
    HisenseCommand base = { HISENSE_MODE_COOL, 24, false, HISENSE_FAN_AUTO,
                            HISENSE_SWING_OFF, HISENSE_SWING_OFF, HISENSE_FEATURE_NONE, HISENSE_DISPLAY_NOCHANGE };

    // ---- mode mapping (both directions + round-trip) ----
    printf("[mode]\n");
    CHECK(matter_mode_to_hisense(3,&hm)&&hm==HISENSE_MODE_COOL,"Cool(3)->COOL");
    CHECK(matter_mode_to_hisense(4,&hm)&&hm==HISENSE_MODE_HEAT,"Heat(4)->HEAT");
    CHECK(matter_mode_to_hisense(7,&hm)&&hm==HISENSE_MODE_FAN, "FanOnly(7)->FAN");
    CHECK(matter_mode_to_hisense(8,&hm)&&hm==HISENSE_MODE_DRY, "Dry(8)->DRY");
    CHECK(matter_mode_to_hisense(1,&hm)&&hm==HISENSE_MODE_AUTO,"Auto(1)->AUTO");
    CHECK(!matter_mode_to_hisense(0,&hm),"Off(0)->false (power frame)");
    CHECK(hisense_mode_to_matter(HISENSE_MODE_COOL)==3,"COOL->Cool(3)");
    CHECK(hisense_mode_to_matter(HISENSE_MODE_AUTO)==1,"AUTO->Auto(1)");
    for (uint8_t mm : {1,3,4,7,8}) { HisenseMode x; matter_mode_to_hisense(mm,&x);
        CHECK(hisense_mode_to_matter(x)==mm,"round-trip mode %d",mm); }

    // ---- mode -> wire byte (ties to validated codec) ----
    printf("[mode -> command byte18]\n");
    { HisenseCommand c=base; matter_mode_to_hisense(3,&c.mode); CHECK(cmd_byte(c,18)==0x50,"Cool -> byte18 0x%02X exp 0x50",cmd_byte(c,18)); }
    { HisenseCommand c=base; matter_mode_to_hisense(1,&c.mode); CHECK(cmd_byte(c,18)==0x90,"Auto -> byte18 0x%02X exp 0x90",cmd_byte(c,18)); }
    { HisenseCommand c=base; matter_mode_to_hisense(4,&c.mode); CHECK(cmd_byte(c,18)==0x30,"Heat -> byte18 0x%02X exp 0x30",cmd_byte(c,18)); }

    // ---- fan: PercentSetting + SpeedSetting -> speed -> wire byte16 ----
    printf("[fan percent/speed -> command byte16]\n");
    CHECK(percent_to_hisense_fan(0)==HISENSE_FAN_AUTO,"0%%->auto");
    CHECK(percent_to_hisense_fan(100)==HISENSE_FAN_HIGH,"100%%->high");
    CHECK(percent_to_hisense_fan(25)==HISENSE_FAN_LOW,"25%%->low");
    struct { uint8_t sp; uint8_t b; } speeds[] = {{1,0x03},{2,0x0B},{3,0x0D},{4,0x0F},{5,0x11},{6,0x13}};
    for (auto &s : speeds) { HisenseCommand c=base; c.fan=speed_to_hisense_fan(s.sp);
        CHECK(cmd_byte(c,16)==s.b,"SpeedSetting %d -> byte16 0x%02X exp 0x%02X",s.sp,cmd_byte(c,16),s.b); }

    // ---- fan: FanMode preset -> speed -> wire byte16 (v17: HA preset writes FanMode) ----
    printf("[fan FanMode preset -> command byte16]\n");
    struct { uint8_t fm; uint8_t b; const char* n; } fanmodes[] = {
        {1,0x0B,"Low"},{2,0x0F,"Medium"},{3,0x13,"High"},{4,0x13,"On"},{5,0x01,"Auto"},{0,0x01,"Off"}};
    for (auto &m : fanmodes) { HisenseCommand c=base; c.fan=fanmode_to_hisense_fan(m.fm);
        CHECK(cmd_byte(c,16)==m.b,"FanMode %s(%d) -> byte16 0x%02X exp 0x%02X",m.n,m.fm,cmd_byte(c,16),m.b); }

    // ---- fan status -> SpeedCurrent / PercentCurrent ----
    printf("[fan status -> Speed/PercentCurrent]\n");
    CHECK(hisense_fan_raw_to_speed(0x0A)==2,"low raw->speed 2");
    CHECK(hisense_fan_raw_to_speed(0x12)==6,"high raw->speed 6");
    CHECK(hisense_fan_raw_to_percent(0x12)==100,"high raw->100%%");
    // AUTO (0x01) must read as a non-zero percent so HA shows Auto/on, not off (#58);
    // unknown raws stay 0% so they remain distinct from AUTO.
    CHECK(hisense_fan_raw_to_percent(0x01)>0,"auto raw -> nonzero PercentCurrent (not off)");
    CHECK(hisense_fan_raw_to_percent(0x01)==HISENSE_FAN_AUTO_PERCENT,"auto raw -> HISENSE_FAN_AUTO_PERCENT");
    CHECK(hisense_fan_raw_to_percent(0xFF)==0,"unknown raw -> 0%% (distinct from auto)");

    // ---- fan status raw -> command-shadow enum (shadow-sync, docs/07 Tier-1 #2) ----
    printf("[fan status raw -> command enum]\n");
    CHECK(hisense_fan_raw_to_cmd(0x01)==HISENSE_FAN_AUTO,    "raw 0x01 -> AUTO");
    CHECK(hisense_fan_raw_to_cmd(0x02)==HISENSE_FAN_QUIET,   "raw 0x02 -> QUIET");
    CHECK(hisense_fan_raw_to_cmd(0x0A)==HISENSE_FAN_LOW,     "raw 0x0A -> LOW");
    CHECK(hisense_fan_raw_to_cmd(0x0C)==HISENSE_FAN_MED_LOW, "raw 0x0C -> MED_LOW");
    CHECK(hisense_fan_raw_to_cmd(0x0E)==HISENSE_FAN_MID,     "raw 0x0E -> MID");
    CHECK(hisense_fan_raw_to_cmd(0x10)==HISENSE_FAN_MED_HIGH,"raw 0x10 -> MED_HIGH");
    CHECK(hisense_fan_raw_to_cmd(0x12)==HISENSE_FAN_HIGH,    "raw 0x12 -> HIGH");
    CHECK(hisense_fan_raw_to_cmd(0xFF)==HISENSE_FAN_NOCHANGE,"raw unknown -> NOCHANGE (no clobber, #59)");
    // a synced fan must rebuild to the golden command byte16 (0x0A->0x0B, 0x12->0x13)
    { HisenseCommand c=base; c.fan=hisense_fan_raw_to_cmd(0x0A); CHECK(cmd_byte(c,16)==0x0B,"raw 0x0A synced -> byte16 0x%02X exp 0x0B",cmd_byte(c,16)); }
    { HisenseCommand c=base; c.fan=hisense_fan_raw_to_cmd(0x12); CHECK(cmd_byte(c,16)==0x13,"raw 0x12 synced -> byte16 0x%02X exp 0x13",cmd_byte(c,16)); }
    // the NOCHANGE sentinel must never reach the wire -- it packs the 0x01 filler (#59)
    { HisenseCommand c=base; c.fan=HISENSE_FAN_NOCHANGE; CHECK(cmd_byte(c,16)==0x01,"NOCHANGE fan -> byte16 0x%02X exp 0x01 (filler)",cmd_byte(c,16)); }

    // ---- swing: RockSetting <-> v/h swing -> wire byte32 ----
    printf("[swing rock <-> command byte32]\n");
    rock_to_swing(HISENSE_ROCK_UPDOWN,&v,&h);   CHECK(v&&!h,"rock updown -> vswing");
    rock_to_swing(HISENSE_ROCK_LEFTRIGHT,&v,&h); CHECK(!v&&h,"rock leftright -> hswing");
    rock_to_swing(0x03,&v,&h);                   CHECK(v&&h,"rock both -> v+h");
    CHECK(swing_to_rock(true,false)==HISENSE_ROCK_UPDOWN,"vswing -> rock 0x02");
    { HisenseCommand c=base; c.vswing=HISENSE_SWING_SWING; CHECK(cmd_byte(c,32)==0xC0,"vswing -> byte32 0x%02X exp 0xC0",cmd_byte(c,32)); }

    // ---- setpoint: Matter hundredths-C -> whole C -> wire byte19 ----
    printf("[setpoint hundredths -> command byte19]\n");
    CHECK(matter_setpoint_to_c(2200)==22,"2200->22C");
    CHECK(matter_setpoint_to_c(2449)==24,"2449->24C (round down)");
    CHECK(matter_setpoint_to_c(2450)==25,"2450->25C (round up)");
    CHECK(matter_setpoint_to_c(1000)==16,"1000->clamp 16");
    CHECK(matter_setpoint_to_c(4000)==32,"4000->clamp 32");
    { HisenseCommand c=base; c.setpoint=matter_setpoint_to_c(2200); CHECK(cmd_byte(c,19)==0x2D,"setpt 22 -> byte19 0x%02X exp 0x2D",cmd_byte(c,19)); }

    // ---- Dry/Fan-only lockout: strip setpoint (both), strip fan (Dry only) (#53) ----
    printf("[dry/fan-only frame lockout]\n");
    { HisenseCommand c=base; c.mode=HISENSE_MODE_DRY; c.setpoint=24; c.fan=HISENSE_FAN_HIGH;
      CHECK(cmd_byte(c,19)==0x00,"DRY strips setpoint byte19=0x%02X exp 0x00",cmd_byte(c,19));
      CHECK(cmd_byte(c,16)==0x01,"DRY strips fan byte16=0x%02X exp 0x01",cmd_byte(c,16)); }
    { HisenseCommand c=base; c.mode=HISENSE_MODE_FAN; c.setpoint=24; c.fan=HISENSE_FAN_HIGH;
      CHECK(cmd_byte(c,19)==0x00,"FAN-only strips setpoint byte19=0x%02X exp 0x00",cmd_byte(c,19));
      CHECK(cmd_byte(c,16)==0x13,"FAN-only keeps fan byte16=0x%02X exp 0x13",cmd_byte(c,16)); }
    // an out-of-range setpoint must NOT drop the whole Dry/Fan frame (range check gated)
    { HisenseCommand c=base; c.mode=HISENSE_MODE_FAN; c.setpoint=99; uint8_t f[64];
      CHECK(hisense_build_command(&c,f,sizeof(f))>0,"FAN-only builds despite out-of-range setpoint"); }

    // ---- FanMode preset (HA fan on/off + preset reads this, docs/08) ----
    printf("[fan raw -> FanMode preset]\n");
    CHECK(hisense_fan_raw_to_fanmode(0x12,false)==0,"powered off -> Off(0)");
    CHECK(hisense_fan_raw_to_fanmode(0x01,true )==5,"auto  -> Auto(5)");
    CHECK(hisense_fan_raw_to_fanmode(0x02,true )==1,"quiet -> Low(1)");
    CHECK(hisense_fan_raw_to_fanmode(0x0A,true )==1,"low   -> Low(1)");
    CHECK(hisense_fan_raw_to_fanmode(0x0E,true )==2,"mid   -> Medium(2)");
    CHECK(hisense_fan_raw_to_fanmode(0x12,true )==3,"high  -> High(3)");

    // ---- ThermostatRunningState -> HA hvac_action (docs/08) ----
    printf("[running state -> hvac_action bits]\n");
    CHECK(hisense_to_running_state(false,HISENSE_MODE_COOL,50)==0,"off -> 0");
    CHECK(hisense_to_running_state(true, HISENSE_MODE_COOL,50)==2,"cool+comp -> Cool(2)");
    CHECK(hisense_to_running_state(true, HISENSE_MODE_HEAT,50)==1,"heat+comp -> Heat(1)");
    CHECK(hisense_to_running_state(true, HISENSE_MODE_COOL, 0)==0,"cool idle(comp0) -> 0");
    CHECK(hisense_to_running_state(true, HISENSE_MODE_FAN,  0)==4,"fan-only -> Fan(4)");

    // ---- fixpoint / idempotence: the properties that guard the downlink->uplink
    //      feedback-loop class (a genuine echo always equals the A/C's reported value).
    printf("[fixpoint: mode / setpoint / rock round-trips]\n");
    for (HisenseMode m : {HISENSE_MODE_COOL,HISENSE_MODE_HEAT,HISENSE_MODE_DRY,HISENSE_MODE_FAN,HISENSE_MODE_AUTO}) {
        HisenseMode back; bool ok = matter_mode_to_hisense(hisense_mode_to_matter(m), &back);
        CHECK(ok && back==m, "mode round-trip %d", (int)m);
    }
    for (int c16=16; c16<=32; c16++)
        CHECK(matter_setpoint_to_c((int16_t)(c16*100))==c16, "setpoint round-trip %dC", c16);
    for (int r=0; r<4; r++) {
        bool vv=(r&2)!=0, hh=(r&1)!=0, v2,h2; rock_to_swing(swing_to_rock(vv,hh),&v2,&h2);
        CHECK(v2==vv && h2==hh, "rock round-trip v=%d h=%d", vv, hh);
    }
    // raw-fan tables must agree on which raw byte is which speed
    for (unsigned raw : {0x01u,0x02u,0x0Au,0x0Cu,0x0Eu,0x10u,0x12u})
        CHECK(hisense_fan_raw_to_speed(raw)==0 || hisense_fan_raw_to_percent(raw)>0,
              "raw 0x%02X: speed/percent tables consistent", raw);

    // ---- power estimate (calibrated 2026-07-07, docs/09 §4b) ----
    printf("[power: P = 4.15 * @55^2 W, calibrated 3-point]\n");
    CHECK(hisense_active_power_mw(0)  == 0,        "@55=0  -> 0 W");
    // meter points: @55=8 -> 268W(+-3%), @55=15 -> 934W(+-3%)
    { int64_t w = hisense_active_power_mw(8)  / 1000; CHECK(w >= 255 && w <= 280, "@55=8  -> %lldW exp ~268", (long long)w); }
    { int64_t w = hisense_active_power_mw(15) / 1000; CHECK(w >= 905 && w <= 965, "@55=15 -> %lldW exp ~934", (long long)w); }
    CHECK(hisense_active_power_mw(16) > hisense_active_power_mw(15), "monotonic in @55");
    // glitchy/reserved @55 byte must be clamped, not report ~270 kW into HA's dashboard
    CHECK(hisense_active_power_mw(255) == HISENSE_POWER_MW_MAX, "@55=255 clamped to %lldmW", (long long)HISENSE_POWER_MW_MAX);
    CHECK(hisense_active_power_mw(255) <= 50000000LL, "@55=255 <= 50kW ceiling");
    // derived current vs meter: @55=15 ~4.47A, @55=8 ~1.27A (meter 4.2/1.2)
    { int64_t ma = hisense_active_current_ma(15, 220); CHECK(ma >= 4200 && ma <= 4700, "@55=15,220V -> %lldmA exp ~4470", (long long)ma); }
    CHECK(hisense_active_current_ma(15, 0) == 0, "unknown voltage -> 0 mA");
    CHECK(hisense_voltage_mv(220) == 220000,     "@50=220 -> 220000 mV");
    // energy: 1000 W for 3600 s (3.6e6 ms) = 1000 mWh... 1000W=1e6mW; 1e6mW*3.6e6ms=3.6e12 mW-ms /3.6e6 = 1e6 mWh = 1 kWh
    { uint64_t acc = 0; hisense_energy_add(&acc, 1000000, 3600000); CHECK(hisense_energy_mwh(acc) == 1000000, "1kW*1h -> 1e6 mWh (1 kWh)"); }
    { uint64_t acc = 0; hisense_energy_add(&acc, -5, 3600000);      CHECK(acc == 0, "negative power clamped (import-only)"); }

    // ---- #72 runtime capability gating: pure gate predicates -------------------
    // The single source of truth for "expose this surface on THIS unit?". Wiring
    // (endpoint disable / FeatureMap Set / command rejection) is HIL-only and NOT
    // exercised here; these pin the DECISION logic that both platforms consume.
    printf("[#72 capability gating]\n");
    {
        HisenseFeatures f = {};   // zero-init: valid=false, every flag 0

        // valid==false (reply not yet parsed / bus silent) => PERMISSIVE: keep all.
        // "unknown" must never be read as "unsupported" (docs/11 5.1).
        CHECK(matter_gate_eco(&f),     "eco: valid=false -> permissive (exposed)");
        CHECK(matter_gate_quiet(&f),   "quiet: valid=false -> permissive (exposed)");
        CHECK(matter_gate_display(&f), "display: valid=false -> permissive (exposed)");
        CHECK(matter_thermostat_featuremap(&f)==MATTER_THERMOSTAT_FEATUREMAP_FULL,
              "featuremap: valid=false -> full 35 (permissive)");
        CHECK(matter_gate_eco(NULL) && matter_gate_quiet(NULL) && matter_gate_display(NULL),
              "NULL features -> permissive (exposed)");
        CHECK(matter_thermostat_featuremap(NULL)==MATTER_THERMOSTAT_FEATUREMAP_FULL,
              "featuremap: NULL -> full 35 (permissive)");

        // valid==true, all gated flags absent => HIDE each surface.
        f.valid = true;
        CHECK(!matter_gate_eco(&f),     "eco: valid,power_save=0 -> hidden");
        CHECK(!matter_gate_quiet(&f),   "quiet: valid,fan_mute=0 -> hidden");
        CHECK(!matter_gate_display(&f), "display: valid,power_display=0 -> hidden");
        CHECK(matter_thermostat_featuremap(&f)==MATTER_THERMOSTAT_FEATUREMAP_COOL,
              "featuremap: valid,cool_heat=0 -> cooling-only 2 (no ghost Heat/Auto)");

        // each flag present => expose exactly that surface.
        f.power_save = true;    CHECK(matter_gate_eco(&f),     "eco: power_save=1 -> exposed");
        f.fan_mute = true;      CHECK(matter_gate_quiet(&f),   "quiet: fan_mute=1 -> exposed");
        f.power_display = 1;    CHECK(matter_gate_display(&f), "display: power_display!=0 -> exposed");
        f.cool_heat = true;     CHECK(matter_thermostat_featuremap(&f)==MATTER_THERMOSTAT_FEATUREMAP_FULL,
                                      "featuremap: cool_heat=1 -> full 35");

        // gates read the VALID tier, NOT ext_valid: a basic (valid, !ext_valid) reply
        // must still gate correctly -- guards against a mis-wire to bit30.
        HisenseFeatures b = {}; b.valid = true; b.ext_valid = false;
        b.power_save = true;   // valid-tier flag set, ext tier absent
        CHECK(matter_gate_eco(&b),    "ext_valid=false: eco gates on valid-tier power_save");
        CHECK(!matter_gate_quiet(&b), "ext_valid=false: quiet still hidden (fan_mute=0)");
        CHECK(matter_thermostat_featuremap(&b)==MATTER_THERMOSTAT_FEATUREMAP_COOL,
              "ext_valid=false: featuremap cooling-only on cool_heat=0");

        // feature GAINED at runtime (0->1, e.g. head-unit swap behind the module):
        // the predicate flips to exposed with no persisted state.
        HisenseFeatures g = {}; g.valid = true;
        CHECK(!matter_gate_eco(&g), "gained: before, eco hidden");
        g.power_save = true;
        CHECK(matter_gate_eco(&g),  "gained: after 0->1, eco exposed");
    }

    // ---- #102 persist-at-boot: features <-> bitmap roundtrip + gate consistency ----------
    // The last-seen features are persisted as the compact bitmap and reconstructed at boot to
    // gate BEFORE commissioning. The reconstruction must preserve every gate decision.
    printf("[#102 features bitmap roundtrip]\n");
    {
        HisenseFeatures samples[3] = {};
        samples[0].valid = true; samples[0].power_save = true; samples[0].cool_heat = true;
                                 samples[0].power_display = 2;                 // eco+display, heat-pump
        samples[1].valid = true;                                              // cooling-only, no eco/quiet/display
        samples[2].valid = true; samples[2].ext_valid = true; samples[2].fan_mute = true;
                                 samples[2].q_display = true; samples[2].demand_resp = 3; // quiet, ext tier
        for (int i = 0; i < 3; i++) {
            uint32_t bm = hisense_features_to_bitmap32(&samples[i]);
            HisenseFeatures rt = {}; hisense_features_from_bitmap32(bm, &rt);
            CHECK(hisense_features_to_bitmap32(&rt) == bm, "sample %d: to->from->to bitmap stable", i);
            CHECK(matter_gate_eco(&rt)     == matter_gate_eco(&samples[i]),     "sample %d: eco gate survives roundtrip", i);
            CHECK(matter_gate_quiet(&rt)   == matter_gate_quiet(&samples[i]),   "sample %d: quiet gate survives roundtrip", i);
            CHECK(matter_gate_display(&rt) == matter_gate_display(&samples[i]), "sample %d: display gate survives roundtrip", i);
            CHECK(matter_thermostat_featuremap(&rt) == matter_thermostat_featuremap(&samples[i]), "sample %d: featuremap survives roundtrip", i);
        }
        // first boot: nothing persisted -> word 0 -> valid=false -> gates permissive (show all)
        HisenseFeatures z = {}; hisense_features_from_bitmap32(0, &z);
        CHECK(!z.valid, "from_bitmap32(0) -> valid=false (first boot, permissive)");
        CHECK(matter_gate_eco(&z) && matter_gate_quiet(&z) && matter_gate_display(&z), "first-boot word 0 -> all shown");
        hisense_features_from_bitmap32(0, NULL);   // NULL must not crash
    }

    printf("== %d passed, %d failed ==\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
