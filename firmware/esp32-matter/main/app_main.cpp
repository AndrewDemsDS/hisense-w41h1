// esp-matter Room A/C node bridging Home Assistant (Matter) <-> the Hisense RS-485
// bus. This is the ESP32 analogue of the AmebaZ2 firmware/src/sdk-edits/matter_drivers.cpp:
// same shadow-command + echo-suppression logic, same mappings (matter_aircon_map.h),
// but wired to esp-matter's cluster APIs instead of CHIP-on-Ameba.
//
// Endpoints (mirrors the AmebaZ2 .zap 1:1 -> node parity in HA):
//   ep1 Room Air Conditioner : OnOff + Thermostat (mode/setpoint/local-temp/running-state)
//                              + FanControl (mode/percent) + ElectricalPowerMeasurement
//                              (watts/volts/amps) + Hisense mfg cluster 0xFFF1FC00
//   ep2 TemperatureMeasurement: outdoor temp        ep6 ModeSelect     : Sleep profile
//   ep3 On/Off plug-in unit   : Eco                 ep7 Contact Sensor : aux/PTC heat relay
//   ep4 On/Off plug-in unit   : Quiet/Mute          ep8 TemperatureMeasurement: coil temp
//   ep5 On/Off plug-in unit   : Turbo
// Every endpoint carries a UserLabel "ha_entitylabel" (via an in-RAM DeviceInfoProvider) so HA
// names the entities. 0x66/40 feature flags + bus-link-health (#56) wire to the driver callbacks.
//
// The RS-485 driver (../src/rs485-driver) is SHARED and reused UNCHANGED: all protocol
// fixes + special-mode/telemetry decode already live there; this file only surfaces them
// through esp-matter clusters (the AmebaZ2 matter_drivers.cpp does the same via CHIP).
#include <esp_err.h>
#include <esp_system.h>   // esp_reset_reason (#12 brownout diagnosis)
#include <esp_log.h>
#include <esp_timer.h>   // esp_timer_get_time() for the "77" settling grace
#include <nvs_flash.h>
#include <string.h>
#include <esp_wifi.h>        // TX-power throttle during OTA (#12 brownout mitigation)
#include <esp_https_ota.h>     // manual HTTPS-OTA backup path (fallback for a failed Matter OTA)
#include <esp_http_client.h>

#include <esp_matter.h>
#include <esp_matter_endpoint.h>

#include <app/server/Server.h>                      // Server / FabricTable / commissioning window (F1 "77")
#include <app/server/CommissioningWindowManager.h>
#include <credentials/FabricTable.h>
#include <platform/CHIPDeviceLayer.h>               // PlatformMgr / ConnectivityMgr / SystemLayer accessors
#include <system/SystemClock.h>                     // Clock::Seconds32
#include <app/clusters/mode-select-server/supported-modes-manager.h>   // Sleep-profile ModeSelect
#include <esp_matter_providers.h>                    // set_custom_device_info_provider
#include <platform/DeviceInfoProvider.h>             // UserLabel "ha_entitylabel" -> HA entity names
#include <lib/support/CHIPMemString.h>               // Platform::CopyString
#include <map>
#include <vector>

#include "hisense_rs485.h"
#include "matter_aircon_map.h"
#include "power_estimate.h"
#ifdef CONFIG_HISENSE_DEBUG_BUILD
#include "diag_console.h"   // embedded :2323 diagnostic console (DEBUG flavour only, see Kconfig)
#endif
#include <ElectricalPowerMeasurementDelegate.h>     // reused from firmware/src/sdk-edits (CHIP EPM delegate)
#include <app/clusters/temperature-measurement-server/TemperatureMeasurementCluster.h>  // registered-cluster SetMeasuredValue
#include <app/clusters/boolean-state-server/BooleanStateCluster.h>                      // registered-cluster SetStateValue (same migration)
#include <data_model_provider/esp_matter_data_model_provider.h>                           // provider registry
#include <app/ConcreteClusterPath.h>

using namespace esp_matter;
using namespace chip::app::Clusters;

// EPM (0x0090) is served through a CHIP delegate, not ember RAM: esp-matter builds the
// ElectricalPowerMeasurement::Instance from this delegate and reads route through its Get*().
// Must outlive the stack (the Instance holds a reference). Fed via SetActivePower/... each poll.
using chip::app::Clusters::ElectricalPowerMeasurement::ElectricalPowerMeasurementDelegate;
static ElectricalPowerMeasurementDelegate s_epm_delegate;

static const char *TAG = "hisense_ac";

// Endpoint ids (assigned by esp-matter in creation order -> 1..8, matching the AmebaZ2 .zap).
static uint16_t s_ep_id      = 0;  // ep1 Room A/C
static uint16_t s_ep_outdoor = 0;  // ep2 TemperatureMeasurement outdoor
static uint16_t s_ep_eco     = 0;  // ep3 OnOff Eco
static uint16_t s_ep_mute    = 0;  // ep4 OnOff Quiet/Mute
static uint16_t s_ep_turbo   = 0;  // ep5 OnOff Turbo
static uint16_t s_ep_sleep   = 0;  // ep6 ModeSelect Sleep profile
static uint16_t s_ep_aux     = 0;  // ep7 BooleanState aux/PTC heat relay
static uint16_t s_ep_coil    = 0;  // ep8 TemperatureMeasurement coil
static uint16_t s_ep_display = 0;  // ep9 OnOff panel display (#19 cheap win)
static uint16_t s_ep_fault   = 0;  // ep10 BooleanState -> any A/C fault (#38)

// Hisense manufacturer cluster (ember-only on Ameba; HA has no schema for it, but it exists
// for node parity). 4 attrs: 0x00 Eco / 0x01 Turbo / 0x02 Mute (bool), 0x03 SleepProfile (u8).
static constexpr uint32_t kMfgClusterId = 0xFFF1FC00;

// Shadow command (mirrors matter_drivers.cpp). MUST start at a builder-valid state:
// zero-init would make mode=HISENSE_MODE_FAN(0)+setpoint=0, so the first single-field
// write (setpoint/fan) before any SystemMode write would either drop the frame (setpoint
// <16 range check) or force the A/C into Fan-only mode. Init to COOL/24 like the AmebaZ2
// reference so the first combined frame is always valid.
static HisenseCommand s_cmd   = { HISENSE_MODE_COOL, 24, false,
                                  HISENSE_FAN_AUTO, HISENSE_SWING_OFF,
                                  HISENSE_SWING_OFF, HISENSE_FEATURE_NONE,
                                  HISENSE_DISPLAY_NOCHANGE };
static volatile bool s_from_bus = false;// true while pushing status->attrs: suppress
                                        // the resulting PRE/POST_UPDATE from re-sending a cmd

// Latest parsed A/C status, snapshotted in on_status so the uplink handler can read the
// current state for its OFF / mode-appropriate / echo guards. Both on_status (bus task)
// and a genuine client-write update callback (Matter task) run under the CHIP stack lock, so
// this plain struct is serialized without a separate critical section. valid=false until the
// first good frame, so the guards no-op before then.
static HisenseState  s_status = {};

// Last Matter SystemMode the user commanded (0=off/unset, 1=Auto, 3=Cool, 4=Heat, 7=Fan, 8=Dry).
// The Hisense A/C in AUTO reports its ACTIVE sub-mode (Cool/Heat) in status, so mapping status
// straight back would flip HA out of Auto; keep reporting the user's chosen mode instead (Auto
// stays Auto, with ThermostatRunningState showing the active heating/cooling).
static uint8_t       s_user_matter_mode = 0;

// Post-command settle window: after we send a frame, skip resyncing s_cmd from status for
// this long, so a stale pre-command status poll can't revert an in-flight command (#61).
#define HISENSE_SYNC_HOLD_MS 3000
static chip::System::Clock::Timestamp s_sync_hold_until = chip::System::Clock::kZero;

// ---------------------------------------------------------------------------
// Sleep-profile ModeSelect (ep6): 5 fixed modes. esp-matter installs this as the global
// SupportedModesManager when passed as the mode_select config delegate.
// ---------------------------------------------------------------------------
namespace {
using ModeOpt = chip::app::Clusters::ModeSelect::Structs::ModeOptionStruct::Type;
const ModeOpt kSleepModes[] = {
    { chip::CharSpan::fromCharString("Off"),     0, {} },
    { chip::CharSpan::fromCharString("General"), 1, {} },
    { chip::CharSpan::fromCharString("Old"),     2, {} },
    { chip::CharSpan::fromCharString("Young"),   3, {} },
    { chip::CharSpan::fromCharString("Kids"),    4, {} },
};
}
class SleepModesMgr : public chip::app::Clusters::ModeSelect::SupportedModesManager
{
public:
    ModeOptionsProvider getModeOptionsProvider(chip::EndpointId) const override
    {
        return ModeOptionsProvider(&kSleepModes[0], &kSleepModes[5]);
    }
    chip::Protocols::InteractionModel::Status
    getModeOptionByMode(chip::EndpointId, uint8_t mode, const ModeOpt **out) const override
    {
        for (auto &m : kSleepModes)
            if (m.mode == mode) { *out = &m; return chip::Protocols::InteractionModel::Status::Success; }
        return chip::Protocols::InteractionModel::Status::InvalidCommand;
    }
};
static SleepModesMgr s_sleep_mgr;

// ---------------------------------------------------------------------------
// Downlink: Matter attribute write (HA) -> RS-485 command frame.
// ---------------------------------------------------------------------------
static void arm_sync_hold()
{
    s_sync_hold_until = chip::System::SystemClock().GetMonotonicTimestamp()
                      + chip::System::Clock::Milliseconds32(HISENSE_SYNC_HOLD_MS);
}
static void flush_cmd()
{
    uint8_t f[HISENSE_CMD_FRAME_LEN + 2];
    size_t n = hisense_build_command(&s_cmd, f, sizeof(f));
    // A build rejection used to return silently, which is how an out-of-range shadow
    // setpoint could disable ALL combined-frame control with nothing in the log to show
    // for it. The shadow is guarded above now, so this should be unreachable; if it ever
    // fires, the shadow is invalid and every command is being dropped -- say so loudly.
    if (!n) {
        ESP_LOGE(TAG, "combined command REJECTED by the builder (mode=%d setpoint=%d "
                      "fahrenheit=%d) -- frame NOT sent; all combined control is dead "
                      "until the shadow is valid again",
                 (int) s_cmd.mode, (int) s_cmd.setpoint, (int) s_cmd.fahrenheit);
        return;
    }
    if (hisense_send_frame(f, n)) arm_sync_hold();          // arm the settle only if it enqueued
    else                          ESP_LOGW(TAG, "cmd dropped (TX queue full)");
}
/* Bench bridge for the diag console's `tx` (#52 display-byte hunt). Lives here, not
 * in diag_console.cpp, so s_cmd and arm_sync_hold stay private: the probe frame is
 * built from the CURRENT command state, so it differs from what the A/C is already
 * running by exactly the byte under test. Arms the settle window like any other
 * send, otherwise the next status frame resyncs s_cmd mid-probe.
 * Returns 0 sent, -1 offset rejected, -2 TX queue full, -3 the SHADOW is invalid so the
 * builder refused (distinct from -1: nothing is wrong with the offset). Conflating -1 and
 * -3 cost real bench time -- `tx` reported "offset 19 is outside the payload [16,46)" while
 * 19 was plainly inside it, and the actual cause was an out-of-range shadow setpoint
 * silently killing every build. */
extern "C" int diag_tx_override(int off, uint8_t val)
{
    if (off < (int) HISENSE_CMD_HEADER_LEN || off >= (int) HISENSE_CMD_CHK_OFFSET) {
        return -1;
    }
    uint8_t f[HISENSE_CMD_FRAME_LEN + 2];
    size_t n = hisense_build_command_override(&s_cmd, f, sizeof(f), off, val);
    if (!n) return -3;
    if (!hisense_send_frame(f, n)) return -2;
    arm_sync_hold();
    return 0;
}

/* Current shadow, for the console to explain a -3 without guessing. */
extern "C" void diag_get_cmd_state(int *mode, int *setpoint, int *fahrenheit)
{
    if (mode)       *mode       = (int) s_cmd.mode;
    if (setpoint)   *setpoint   = (int) s_cmd.setpoint;
    if (fahrenheit) *fahrenheit = (int) s_cmd.fahrenheit;
}

static void send_power(bool on)
{
    uint8_t f[HISENSE_CMD_FRAME_LEN];
    size_t n = hisense_build_power_frame(on, f, sizeof(f));
    if (!n) return;
    if (hisense_send_frame(f, n)) arm_sync_hold();
    else                          ESP_LOGW(TAG, "power dropped (TX queue full)");
}

// Special-mode frame builders (ported from matter_drivers.cpp hisense_apply_*).
static void apply_eco(bool on)
{
    if (on) {
        s_cmd.feature = HISENSE_FEATURE_ECO;
        flush_cmd();
    } else {
        // Clearing eco needs the explicit eco-off byte (ECO_OFF), NOT FEATURE_NONE
        // (which is turbo-clear). Send once, then return the shadow to neutral.
        s_cmd.feature = HISENSE_FEATURE_ECO_OFF;
        flush_cmd();
        s_cmd.feature = HISENSE_FEATURE_NONE;
    }
}
static void apply_turbo(bool on)
{
    s_cmd.feature = on ? HISENSE_FEATURE_TURBO : HISENSE_FEATURE_NONE;
    flush_cmd();
}
static void apply_mute(bool on)
{
    uint8_t f[HISENSE_CMD_FRAME_LEN + 2];
    size_t n = hisense_build_mute_frame(on, f, sizeof(f));
    if (n && hisense_send_frame(f, n)) arm_sync_hold();
}
static void apply_sleep(uint8_t profile)
{
    uint8_t f[HISENSE_CMD_FRAME_LEN + 2];
    size_t n = hisense_build_sleep_frame(profile, f, sizeof(f));
    if (n && hisense_send_frame(f, n)) arm_sync_hold();
}
// #19 cheap win: panel display on/off. `display` rides the combined command frame (@20:
// 0xC0 on / 0x40 off / 0x00 leave-alone); flush_cmd() rebuilds + sends it. No status feedback
// (the A/C doesn't report display state), so the OnOff attr is optimistic — reflects the last
// command.
//
// ONE-SHOT (#52): reset to NOCHANGE after the frame goes out. `display` is packed into EVERY
// combined command, so leaving ON/OFF latched would re-assert the panel state on every later
// mode/setpoint/fan change and fight the user's remote. Leave-alone is the only correct resting
// value.
static void apply_display(bool on)
{
    s_cmd.display = on ? HISENSE_DISPLAY_ON : HISENSE_DISPLAY_OFF;
    flush_cmd();
    s_cmd.display = HISENSE_DISPLAY_NOCHANGE;
}

static void on_recommission(uint8_t reason);   // fwd decl (defined in the "77" section below)
static void trigger_https_ota(void);           // fwd decl (manual HTTPS-OTA backup, defined below)

// Manual HTTPS-OTA backup URL: a plain-HTTP file server reachable from the device, used only as
// a fallback when the Matter (BDX) OTA won't complete. The device and the server must be on the
// same L2/subnet if there is no cross-VLAN routing, so an IPv6 literal is usually what you want:
//
//   idf.py -DHISENSE_OTA_URL="http://[<server-ipv6>]:8070/esp32-ota.bin" build
//
// The default below is a deliberately non-resolvable placeholder: this is a public repo, and a
// real address here would publish network topology (and, with SLAAC EUI-64, the server's MAC).
#ifndef HISENSE_OTA_URL
#define HISENSE_OTA_URL "http://[fd00::1]:8070/esp32-ota.bin"   // placeholder, override at build time
#endif

static esp_err_t on_attribute_update(attribute::callback_type_t type, uint16_t endpoint_id,
                                     uint32_t cluster_id, uint32_t attribute_id,
                                     esp_matter_attr_val_t *val, void *priv)
{
    // Act on the committed value of a client write; never on our own status echo. (esp-matter
    // fires PRE then POST synchronously inside attribute::update; s_from_bus is held across the
    // whole downlink push so both are suppressed. OnOff/ModeSelect arrive as a command applied
    // internally then surfaced as POST_UPDATE, so POST is the one type that catches everything.)
    if (type != attribute::POST_UPDATE || s_from_bus) return ESP_OK;

    // Manual "77" recommission trigger: writing Identify.IdentifyTime = 77 on ep1 fires the same
    // recommission flow the A/C's "77" request would (opens a commissioning window etc.). Lets us
    // exercise + field-recover the flow without the A/C initiating it. (77 = the mode's mnemonic.)
    if (endpoint_id == s_ep_id && cluster_id == Identify::Id &&
        attribute_id == Identify::Attributes::IdentifyTime::Id && val->val.u16 == 77) {
        ESP_LOGW(TAG, "manual recommission trigger (Identify=77)");
        on_recommission(0x77);
        return ESP_OK;
    }
    // Manual HTTPS-OTA backup: writing Identify.IdentifyTime = 88 on ep1 pulls firmware over
    // HTTP (TCP) from the Pi file server -- the break-glass path when the Matter BDX OTA won't
    // complete (e.g. marginal Wi-Fi). Bypasses the Matter OTA provider entirely.
    if (endpoint_id == s_ep_id && cluster_id == Identify::Id &&
        attribute_id == Identify::Attributes::IdentifyTime::Id && val->val.u16 == 88) {
        ESP_LOGW(TAG, "manual HTTPS-OTA trigger (Identify=88)");
        trigger_https_ota();
        return ESP_OK;
    }

    // Current A/C status for the guards below (snapshotted by on_status under the CHIP stack
    // lock, which is held while a client-write callback is dispatched).
    const HisenseState st = s_status;

    // ---- Special-mode switch endpoints (ep3/4/5 OnOff, ep6 ModeSelect) ------------------
    if (endpoint_id == s_ep_eco && cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
        if (!(st.valid && val->val.b == st.eco_on)) apply_eco(val->val.b);      // echo guard
        return ESP_OK;
    }
    if (endpoint_id == s_ep_turbo && cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
        if (!(st.valid && val->val.b == st.turbo_on)) apply_turbo(val->val.b);
        return ESP_OK;
    }
    if (endpoint_id == s_ep_mute && cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
        if (!(st.valid && val->val.b == st.mute_on)) apply_mute(val->val.b);
        return ESP_OK;
    }
    // #19: panel display switch (ep9). No status feedback -> no echo guard; always command.
    if (endpoint_id == s_ep_display && cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
        apply_display(val->val.b);
        return ESP_OK;
    }
    if (endpoint_id == s_ep_sleep && cluster_id == ModeSelect::Id &&
        attribute_id == ModeSelect::Attributes::CurrentMode::Id) {
        if (!(st.valid && val->val.u8 == (uint8_t)(st.sleep_raw / 2))) apply_sleep(val->val.u8);
        return ESP_OK;
    }

    // ---- Room A/C endpoint (ep1) ---------------------------------------------------------
    if (endpoint_id != s_ep_id) return ESP_OK;

    if (cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
        if (!(st.valid && val->val.b == st.power_on)) send_power(val->val.b);   // echo guard

    } else if (cluster_id == Thermostat::Id) {
        if (attribute_id == Thermostat::Attributes::SystemMode::Id) {
            // #5: Off must actively power the unit DOWN; picking a real mode must ensure it's
            // ON (a mode frame alone won't wake an off unit). Mirrors matter_drivers.cpp:526-534.
            uint8_t matter_mode = val->val.u8;
            if (st.valid) {   // echo guard vs what we currently report (Auto preserved, see below)
                uint8_t cur = st.power_on ? (s_user_matter_mode == 1 ? 1 : hisense_mode_to_matter(st.mode)) : 0;
                if (matter_mode == cur) return ESP_OK;
            }
            if (matter_mode == 0) {                        // Off
                send_power(false);
                s_user_matter_mode = 0;
            } else {
                HisenseMode hm;
                if (matter_mode_to_hisense(matter_mode, &hm)) {
                    s_cmd.mode = hm;
                    s_user_matter_mode = matter_mode;      // remember the choice (esp. Auto=1)
                    send_power(true);                      // ensure the unit is on
                    flush_cmd();
                }
            }
        } else if (attribute_id == Thermostat::Attributes::OccupiedCoolingSetpoint::Id ||
                   attribute_id == Thermostat::Attributes::OccupiedHeatingSetpoint::Id) {
            // #4: no setpoint changes while the A/C is OFF (a temp write on an off unit is a
            // confusing no-op -- select a mode to turn it on first).
            if (st.valid && !st.power_on) return ESP_OK;
            // #3: the A/C has a SINGLE setpoint; which Matter attr is authoritative depends on
            // mode (HEAT -> heating, else cooling). Ignore the inactive one so HA's deadband
            // adjusting the *other* setpoint can't command a wrong temperature (the HIL-v6
            // "18C over our 20C" bug). Mirrors matter_drivers.cpp:548-550.
            bool attr_is_heat = (attribute_id == Thermostat::Attributes::OccupiedHeatingSetpoint::Id);
            bool mode_is_heat = (st.valid && st.mode == HISENSE_MODE_HEAT);
            if (st.valid && attr_is_heat != mode_is_heat) return ESP_OK;
            // Round on the full int, clamp last (never narrow to int8 before the clamp).
            int whole_c = matter_round_setpoint_c(val->val.i16);
            if (st.valid && whole_c == st.setpoint_c) return ESP_OK;   // echo guard (pre-clamp)
            /* The A/C reads the setpoint byte in ITS OWN display unit, so a command built
             * while the panel is in Fahrenheit must carry Fahrenheit. Matter is always
             * Celsius, so convert here and tell the builder which unit it is holding (that
             * also selects the right range check: 61..90 rather than 16..32).
             *
             * Getting this wrong is not a silent no-op. On hardware, sending Celsius 23 to
             * a panel in F made the A/C target 23 F and run at 74 Hz toward -5 C. */
            int8_t want_c = (int8_t) matter_clamp_setpoint_c(whole_c);
            bool   unit_f = st.valid && st.temp_unit_f;
            s_cmd.fahrenheit = unit_f;
            s_cmd.setpoint   = unit_f ? hisense_c_to_f(want_c) : want_c;
            flush_cmd();
        }

    } else if (cluster_id == ThermostatUserInterfaceConfiguration::Id) {
        /* #5: panel display unit (C/F). Bench-confirmed on THIS node 2026-07-20 with the `tx`
         * probe: command byte 23 = 0x01 selects Celsius, 0x03 selects Fahrenheit, and status
         * byte 26 bit 1 follows in the next frame. Both directions verified.
         *
         * The switch MUST be atomic with a setpoint rewrite. The A/C stores its setpoint as a
         * raw byte in whatever unit the panel shows and does NOT rescale it on a unit change,
         * so a bare switch reinterprets the same number. Both failure modes were reproduced on
         * this unit: 22 (as C) became 22 F = -6 C and it drove toward that at full compressor;
         * and when the reinterpreted value falls out of range instead, the builder rejects the
         * shadow and the A/C drops EVERY combined command -- including the one that would undo
         * it, which needed a Matter setpoint write to clear.
         *
         * hisense_build_command_override patches one pre-checksum byte while carrying the rest
         * of the shadow, so setting the shadow for the TARGET unit and overriding byte 23 emits
         * a single frame with no window in between. Same approach as the ameba half. */
        if (attribute_id ==
            ThermostatUserInterfaceConfiguration::Attributes::TemperatureDisplayMode::Id) {
            // 0 = Celsius, 1 = Fahrenheit. Raw values rather than the enum type: esp-matter's
            // CHIP revision does not expose the ::Enums:: namespace here, and the read path
            // above already writes this attribute as esp_matter_enum8(temp_unit_f ? 1 : 0).
            const bool want_f = (val->val.u8 == 1);
            if (!st.valid) return ESP_OK;
            if (st.temp_unit_f == want_f) return ESP_OK;    // echo guard

            /* st.setpoint_c is always Celsius (the decoder normalises it) -- the only correct
             * source for the re-encode. */
            const int8_t keep_c = (int8_t) matter_clamp_setpoint_c(st.setpoint_c);
            s_cmd.fahrenheit = want_f;
            s_cmd.setpoint   = want_f ? hisense_c_to_f(keep_c) : keep_c;

            uint8_t f[HISENSE_CMD_FRAME_LEN + 2];
            size_t  n = hisense_build_command_override(&s_cmd, f, sizeof(f),
                                                       23, want_f ? 0x03 : 0x01);
            if (n > 0 && hisense_send_frame(f, n)) {
                arm_sync_hold();
                ESP_LOGI(TAG, "display unit -> %s (setpoint re-encoded %d C -> %d)",
                         want_f ? "F" : "C", (int) keep_c, (int) s_cmd.setpoint);
            } else {
                /* Restore the shadow so a later flush cannot ship a setpoint encoded for a unit
                 * the A/C never adopted. */
                s_cmd.fahrenheit = st.temp_unit_f;
                s_cmd.setpoint   = st.temp_unit_f ? hisense_c_to_f(keep_c) : keep_c;
                ESP_LOGE(TAG, "display unit change REJECTED (n=%u) -- shadow restored",
                         (unsigned) n);
            }
        }

    } else if (cluster_id == FanControl::Id) {
        // #4: no fan changes while the A/C is OFF (fan is meaningless with the unit off).
        if (st.valid && !st.power_on) return ESP_OK;
        if (attribute_id == FanControl::Attributes::FanMode::Id) {
            HisenseFanSpeed nf = fanmode_to_hisense_fan(val->val.u8);
            if (nf != s_cmd.fan) { s_cmd.fan = nf; flush_cmd(); }
        } else if (attribute_id == FanControl::Attributes::PercentSetting::Id) {
            HisenseFanSpeed nf = percent_to_hisense_fan(val->val.u8);
            if (nf != s_cmd.fan) { s_cmd.fan = nf; flush_cmd(); }
        } else if (attribute_id == FanControl::Attributes::RockSetting::Id) {
            // #19: vertical swing. RockUpDown (0x02) -> vswing on. Echo-guard vs status.
            bool sw = (val->val.u8 & 0x02) != 0;
            if (sw != (st.valid && st.vswing_on)) {
                s_cmd.vswing = sw ? HISENSE_SWING_SWING : HISENSE_SWING_OFF;
                flush_cmd();
            }
        }
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Uplink: parsed RS-485 status -> Matter attributes. Runs in the driver's bus
// task, so take the CHIP stack lock and guard against the echo loop.
// ---------------------------------------------------------------------------
static void set_attr(uint16_t ep, uint32_t cluster, uint32_t attr, esp_matter_attr_val_t val)
{
    attribute::update(ep, cluster, attr, &val);
}

// TemperatureMeasurement MeasuredValue must be set through the REGISTERED cluster object, not
// attribute::update: esp-matter is mid-migration and reads for 0x0402 come from the new
// TemperatureMeasurementCluster's own member, while attribute::update only writes the legacy
// shadow store -> reads would return null. (LocalTemperature has no such registry integration,
// so it works via attribute::update.) SetMeasuredValue also emits the change report for HA.
static void set_temp_measured(uint16_t ep, chip::app::DataModel::Nullable<int16_t> v)
{
    using namespace chip::app;
    auto *iface = esp_matter::data_model::provider::get_instance().registry()
                    .Get(ConcreteClusterPath(ep, Clusters::TemperatureMeasurement::Id));
    if (!iface) return;
    static_cast<Clusters::TemperatureMeasurementCluster *>(iface)->SetMeasuredValue(v);
}

/* BooleanState has the SAME migration problem as TemperatureMeasurement above, and getting it
 * wrong took node 28 off the network for hours. attribute::update() returns
 * ESP_ERR_NOT_SUPPORTED (262) for this cluster, and the driver republishes it once per status
 * frame -- so the failure logged at ERROR level thousands of times a second, saturating the
 * 115200 console (measured ~25 KB/s against an 11.5 KB/s ceiling). esp_log BLOCKS the calling
 * task when the TX buffer fills, so the whole application starved: ICMP still answered while
 * TCP :2323/:2324 was refused and every Matter CASE handshake failed. It looked exactly like a
 * dead device and was in fact a logging deadlock.
 *
 * Route through the registered cluster object instead, which is the authoritative store and
 * also emits the change report for HA. */
static void set_bool_state(uint16_t ep, bool v)
{
    using namespace chip::app;
    auto *iface = esp_matter::data_model::provider::get_instance().registry()
                    .Get(ConcreteClusterPath(ep, Clusters::BooleanState::Id));
    if (!iface) return;
    static_cast<Clusters::BooleanStateCluster *>(iface)->SetStateValue(v);
}

/* Remote activity is ignored until this deadline after the window opens. The "77" entry gesture
 * ("Horizon Airflow x6") is itself a burst of remote presses, so without a settling window the
 * mode cancels itself the instant it opens -- measured, every attempt. */
static const int64_t     kRecommissionGraceUs    = 6 * 1000 * 1000;   // 6 s
static int64_t           s_recommission_grace_us = 0;

// Forward decls: the "77" machinery is defined further down, but on_status (above it) needs to
// know whether a window is open so remote activity can close it.
static bool recommission_window_is_open(void);
static void recommission_user_cancel(intptr_t);

static void on_status(const HisenseState *st)
{
    if (!st || !st->valid) return;

    // esp-matter's lock API is RAII: construct to take the CHIP stack lock, released at scope exit.
    lock::ScopedChipStackLock lk(portMAX_DELAY);
    s_from_bus = true;

    /* Stock behaviour: ANY remote button exits "77", not just pressing the pattern again. We
     * cannot see IR, but every such press lands on the bus as a change to a user-settable field,
     * so treat that as the user having moved on and shut the window.
     *
     * Deliberately only the fields a HUMAN sets. Temperatures, compressor frequency, current and
     * coil readings drift on their own every frame and would cancel the window instantly. Our own
     * Matter-originated writes also land here, but during a "77" window nothing should be driving
     * the unit from Matter -- and if something is, the user is plainly not mid-pairing. */
    if (recommission_window_is_open() && s_status.valid &&
        esp_timer_get_time() > s_recommission_grace_us) {
        /* Swing IS included, and the grace period above is what makes that safe.
         *
         * History worth keeping: the entry gesture is "Horizon Airflow x6" -- the swing button --
         * so swing changes arrive for a beat after the window opens and briefly self-cancelled it
         * (open 95424 ms, killed 95674 ms). The first fix excluded swing AND added the grace. The
         * grace alone covers the settle; excluding swing on top of it removed the exit route that
         * was actually working, because pressing the pattern again is a SWING press and this A/C
         * appears to emit its 0x20 pulse only on ENTRY. Result: nothing exited "77" but expiry.
         *
         * So: keep swing, rely on the grace. Stock exits on any button, and swing is a button. */
        const bool user_touched =
            st->power_on   != s_status.power_on   || st->mode      != s_status.mode      ||
            st->setpoint_c != s_status.setpoint_c || st->fan_raw   != s_status.fan_raw   ||
            st->eco_on     != s_status.eco_on     || st->turbo_on  != s_status.turbo_on  ||
            st->mute_on    != s_status.mute_on    || st->sleep_raw != s_status.sleep_raw ||
            st->vswing_on  != s_status.vswing_on  || st->hswing_on != s_status.hswing_on;
        if (user_touched) {
            ESP_LOGW(TAG, "A/C driven from the remote during the \"77\" window -> treating as EXIT");
            chip::DeviceLayer::PlatformMgr().ScheduleWork(recommission_user_cancel, 0);
        }
    }

    // Snapshot for the uplink guards (read under this same lock in on_attribute_update).
    s_status = *st;
#ifdef CONFIG_HISENSE_DEBUG_BUILD
    diag_on_status(st);   // snapshot-only (no I/O) -> safe under the CHIP stack lock
#endif

    // #2: keep the uplink command shadow in sync with the A/C's ACTUAL state, so a later
    // single-field write rebuilds the combined frame from reality instead of a stale shadow
    // (which would clobber an out-of-band IR-remote / turbo change, or force COOL/24 after a
    // reboot). Held off for a settle window after our own commands so an in-flight command
    // isn't reverted by a pre-command status frame (#61). Mirrors matter_drivers.cpp:732-749.
    if (chip::System::SystemClock().GetMonotonicTimestamp() >= s_sync_hold_until) {
        s_cmd.mode     = st->mode;
        // NEVER copy an out-of-range setpoint into the shadow. The A/C legitimately reports
        // them (this unit answers ac_8heat=1, and the bench saw it accept and hold 5 C), and
        // an out-of-range shadow makes hisense_build_command() return 0 for EVERY later
        // command -- mode, fan and swing included -- silently killing all combined-frame
        // control until a reboot. Keeping the last good value degrades gracefully instead:
        // the shadow is only a base for the next command, so a stale setpoint is far cheaper
        // than a dead control path.
        /* Track the A/C's display unit, and hold the shadow setpoint in THAT unit, because
         * that is what goes on the wire. st->setpoint_c is always Celsius (the parser
         * converts), so the helper converts back and validates against the WIRE unit's
         * range -- validating the Celsius number against the shadow's old unit is what
         * wedged the AmebaZ2 sync in F mode. On an out-of-range report BOTH fields keep
         * their last good values: flipping only the unit would reinterpret the stale
         * Celsius number as Fahrenheit on the next command. */
        int8_t shadow_sp;
        if (hisense_shadow_setpoint_from_status(st->setpoint_c, st->temp_unit_f, &shadow_sp)) {
            s_cmd.fahrenheit = st->temp_unit_f;
            s_cmd.setpoint   = shadow_sp;
        } else {
            ESP_LOGW(TAG, "status setpoint %d C (%s) out of range -- keeping shadow at %d "
                          "(copying it would drop every later command)",
                     (int) st->setpoint_c, st->temp_unit_f ? "F panel" : "C panel",
                     (int) s_cmd.setpoint);
        }
        HisenseFanSpeed sf = hisense_fan_raw_to_cmd(st->fan_raw);
        if (sf != HISENSE_FAN_NOCHANGE) s_cmd.fan = sf;    // keep previous fan on an unknown raw (#59)
        s_cmd.vswing  = st->vswing_on ? HISENSE_SWING_SWING : HISENSE_SWING_OFF;
        s_cmd.hswing  = HISENSE_SWING_OFF;                 // no H-swing motor on this unit
        s_cmd.feature = st->eco_on   ? HISENSE_FEATURE_ECO
                      : st->turbo_on ? HISENSE_FEATURE_TURBO
                                     : HISENSE_FEATURE_NONE;
    }

    // --- Room A/C ep1 ---------------------------------------------------------------------
    // OnOff power
    set_attr(s_ep_id, OnOff::Id, OnOff::Attributes::OnOff::Id, esp_matter_bool(st->power_on));
    // Thermostat: mode (Off when the unit reports powered down / #6), local temp (0.01C),
    // the mode-appropriate setpoint, running state
    set_attr(s_ep_id, Thermostat::Id, Thermostat::Attributes::SystemMode::Id,
             esp_matter_enum8(st->power_on ? (s_user_matter_mode == 1 ? 1 : hisense_mode_to_matter(st->mode)) : 0));
    set_attr(s_ep_id, Thermostat::Id, Thermostat::Attributes::LocalTemperature::Id,
             esp_matter_nullable_int16(nullable<int16_t>((int16_t)(st->indoor_temp_c * 100))));
    int16_t sp = (int16_t)(st->setpoint_c * 100);
    if (st->mode == HISENSE_MODE_HEAT)
        set_attr(s_ep_id, Thermostat::Id, Thermostat::Attributes::OccupiedHeatingSetpoint::Id, esp_matter_int16(sp));
    else
        set_attr(s_ep_id, Thermostat::Id, Thermostat::Attributes::OccupiedCoolingSetpoint::Id, esp_matter_int16(sp));
    set_attr(s_ep_id, Thermostat::Id, Thermostat::Attributes::ThermostatRunningState::Id,
             esp_matter_bitmap16(hisense_to_running_state(st->power_on, st->mode, st->compressor_freq)));
    // FanControl: mode + current percent
    set_attr(s_ep_id, FanControl::Id, FanControl::Attributes::FanMode::Id,
             esp_matter_enum8(hisense_fan_raw_to_fanmode(st->fan_raw, st->power_on)));
    set_attr(s_ep_id, FanControl::Id, FanControl::Attributes::PercentCurrent::Id,
             esp_matter_uint8(hisense_fan_raw_to_percent(st->fan_raw)));
    // #19: vertical swing -> RockSetting (RockUpDown 0x02). Re-enters the uplink handler; the
    // echo guard there stops it re-commanding its own readback.
    set_attr(s_ep_id, FanControl::Id, FanControl::Attributes::RockSetting::Id,
             esp_matter_bitmap8(st->vswing_on ? 0x02 : 0x00));

    // Outdoor + condenser-coil temperatures -> their own TemperatureMeasurement endpoints
    // (0.01 C). Via the registered cluster (see set_temp_measured).
    set_temp_measured(s_ep_outdoor, chip::app::DataModel::MakeNullable<int16_t>((int16_t)(st->outdoor_temp_c * 100)));
    set_temp_measured(s_ep_coil,    chip::app::DataModel::MakeNullable<int16_t>((int16_t)(st->coil_temp_c * 100)));

    // Special-mode switch endpoints (ep3/4/5) -> HA-controllable OnOff mirror. Each re-enters
    // the uplink handler; the per-endpoint echo guards there stop it re-commanding its readback.
    set_attr(s_ep_eco,   OnOff::Id, OnOff::Attributes::OnOff::Id, esp_matter_bool(st->eco_on));
    set_attr(s_ep_mute,  OnOff::Id, OnOff::Attributes::OnOff::Id, esp_matter_bool(st->mute_on));
    set_attr(s_ep_turbo, OnOff::Id, OnOff::Attributes::OnOff::Id, esp_matter_bool(st->turbo_on));
    // Sleep-profile ModeSelect (ep6) tracks the actual profile (0=off..4=Kids).
    set_attr(s_ep_sleep, ModeSelect::Id, ModeSelect::Attributes::CurrentMode::Id,
             esp_matter_uint8((uint8_t)(st->sleep_raw / 2)));
    // Aux/PTC electric-heat relay -> BooleanState contact sensor (ep7).
    // #5: report the A/C's display unit. Writes are now accepted too (see the TUIC handler in
    // the attribute-update path); this is the read-back that keeps the attribute honest when
    // the unit is changed from the IR remote or the panel.
    set_attr(s_ep_id, ThermostatUserInterfaceConfiguration::Id,
             ThermostatUserInterfaceConfiguration::Attributes::TemperatureDisplayMode::Id,
             esp_matter_enum8(st->temp_unit_f ? 1 : 0));
    /* #38: aggregate fault -> BooleanState (ep10) as a NORMALLY-CLOSED loop: true = closed =
     * healthy, false = open = fault. Inverted deliberately. Matter contact-sensor semantics are
     * "true = closed" and Home Assistant inverts on read (binary_sensor.py
     * `device_to_ha=lambda x: not x`), so publishing fl.any directly rendered a HEALTHY unit as
     * "Problem" in HA. A normally-closed alarm loop is the standard convention for this, so the
     * value now reads correctly in HA and in any other controller. Mirrors the ameba half.
     *
     * HisenseFaults.any already ORs the raw fault bytes (minus the one bit proven to be a mode
     * flag), so do not re-derive it from the named bools. */
    {
        HisenseFaults fl;
        if (hisense_get_faults(&fl)) {
            set_bool_state(s_ep_fault, !fl.any);
        }
    }
    // Inverted to match the fault endpoint and the ameba half: normally-closed, so HA (which
    // flips BooleanState on read) shows "on"/detected exactly when the relay is engaged.
    set_bool_state(s_ep_aux, !st->heat_relay_on);

    // ElectricalPowerMeasurement (ep1) -> HA-native Watts/Volts/Amps. Matter base units are
    // mW / mV / mA; power_estimate.h returns exactly those from the calibrated proxies. Fed
    // through the delegate (NOT attribute::update -- these attrs are MANAGED_INTERNALLY and
    // read via the delegate's Get*()). Setters take chip DataModel::Nullable, not esp_matter's.
    {
        int64_t p_mw = st->power_on ? hisense_active_power_mw(st->current_raw) : 0;
        int64_t i_ma = st->power_on ? hisense_active_current_ma(st->current_raw, st->voltage_raw) : 0;
        int64_t v_mv = hisense_voltage_mv(st->voltage_raw);
        s_epm_delegate.SetActivePower(chip::app::DataModel::MakeNullable(p_mw));
        s_epm_delegate.SetVoltage(chip::app::DataModel::MakeNullable(v_mv));
        s_epm_delegate.SetActiveCurrent(chip::app::DataModel::MakeNullable(i_ma));
    }

    // Hisense manufacturer cluster (0xFFF1FC00) read-back on ep1 (HA has no schema; parity only).
    set_attr(s_ep_id, kMfgClusterId, 0x0000, esp_matter_bool(st->eco_on));
    set_attr(s_ep_id, kMfgClusterId, 0x0001, esp_matter_bool(st->turbo_on));
    set_attr(s_ep_id, kMfgClusterId, 0x0002, esp_matter_bool(st->mute_on));
    set_attr(s_ep_id, kMfgClusterId, 0x0003, esp_matter_uint8((uint8_t)(st->sleep_raw / 2)));

    s_from_bus = false;
    // lk (ScopedChipStackLock) releases the CHIP stack lock here at scope exit.
}

// 0x66/40 ProductType feature-flags (bus-task context) -> log. Bit positions RE'd from the
// stock firmware; decoded in the shared driver, not surfaced to HA (capability flags are static).
static void on_features(const HisenseFeatures *f)
{
    ESP_LOGI(TAG, "A/C features (0x66/40): ai=%d display=%d swing8=%d eco=%d mute=%d purify=%d",
             f->ai, f->power_display, f->swing_dir_8, f->power_save, f->fan_mute, f->purify);
}

// Bus link lost/restored (#56). On loss, null every liveness attribute (LocalTemperature +
// outdoor + coil temps) so HA marks the entities unavailable instead of holding stale values;
// the next good status repopulates them. Mirrors matter_drivers.cpp:703-713 (no EPM null here
// -- HA reads ActivePower as steady 0 when off, not a liveness signal).
static void on_link(bool up)
{
    ESP_LOGW(TAG, "A/C RS-485 link %s", up ? "restored" : "lost (bus silent)");
    if (!up) {
        lock::ScopedChipStackLock lk(portMAX_DELAY);
        esp_matter_attr_val_t nullv = esp_matter_nullable_int16(nullable<int16_t>());
        attribute::update(s_ep_id, Thermostat::Id, Thermostat::Attributes::LocalTemperature::Id, &nullv);
        set_temp_measured(s_ep_outdoor, chip::app::DataModel::Nullable<int16_t>());   // null = unavailable
        set_temp_measured(s_ep_coil,    chip::app::DataModel::Nullable<int16_t>());
    }
}

// ---------------------------------------------------------------------------
// "77" recommission (F1): the A/C asked us to re-provision (shared-driver bus-task
// callback). Rather than wipe the fabric, OPEN a commissioning window keeping the
// current fabric so HA stays connected while a new controller pairs, and only swap
// once it does. Ported 1:1 from the AmebaZ2 matter_drivers.cpp reference (same CHIP APIs).
//   "77"             -> snapshot fabric(s), OpenBasicCommissioningWindow, arm expiry.
//   new fabric joins -> delete the snapshotted old fabric(s) (swap done).
//   window expires   -> keep old fabric + tell the A/C to leave "77".
// Window/fabric/timer calls must run in Matter context, so the bus-task callback
// defers via PlatformMgr().ScheduleWork.
// ---------------------------------------------------------------------------
static const uint32_t    kRecommissionWindowSec = 180;
/* Held open while the device has NO fabric. Deliberately long: this is the only way back in
 * after a failed or partial commissioning, and 180 s is not enough for a human to notice the
 * device is unjoinable, find a controller and complete pairing. The window is harmless here --
 * with zero fabrics there is nothing to protect, and it closes the moment one is added. */
static const uint32_t    kUncommissionedWindowSec = 900;
static bool              s_recommission_pending  = false;
static chip::FabricIndex s_old_fabrics[16];
static uint8_t           s_old_fabric_count      = 0;

static void recommission_timeout(chip::System::Layer *, void *);
static void recommission_finish(bool paired, const char *why);

// A NEW fabric committing while our window is open means the re-pair succeeded ->
// drop the old fabric(s) and stand down.
class RecommissionFabricDelegate : public chip::FabricTable::Delegate
{
public:
    void OnFabricCommitted(const chip::FabricTable &, chip::FabricIndex newIndex) override
    {
        if (!s_recommission_pending) return;
        for (uint8_t i = 0; i < s_old_fabric_count; i++)
            if (s_old_fabrics[i] == newIndex) return;   // not a newly-added fabric
        ESP_LOGI(TAG, "recommission: new fabric %u joined -> deleting %u old fabric(s)",
                 newIndex, s_old_fabric_count);
        // Copy the snapshot first: recommission_finish() clears the count, and deleting a
        // fabric can re-enter this delegate.
        chip::FabricIndex doomed[16];
        uint8_t n = s_old_fabric_count;
        for (uint8_t i = 0; i < n; i++) doomed[i] = s_old_fabrics[i];
        hisense_set_provisioning(false);          // paired on the new fabric -> clear "77"
        recommission_finish(true, "new fabric committed");
        for (uint8_t i = 0; i < n; i++)
            chip::Server::GetInstance().GetFabricTable().Delete(doomed[i]);
    }
};
static RecommissionFabricDelegate s_recommission_delegate;

/* Single teardown for EVERY exit from "77", so the device can never be left half-in it.
 *
 * Three ways out, all landing here: the window expired, the user took the A/C out of "77" from
 * the panel/remote, or the re-pair succeeded. In the first two the device must return EXACTLY to
 * its previous state -- old fabric intact, commissioning window shut, BLE advert back off (it is
 * suppressed on a commissioned node), and the A/C told to drop "77". Previously the timeout path
 * only flipped flags and sent exit_77: it left the commissioning window OPEN and BLE advertising
 * indefinitely, so a lapsed window stayed joinable long after the A/C had stopped showing "77".
 *
 * `paired` distinguishes success (new fabric committed; old ones already deleted by the delegate
 * and the A/C cleared) from abort (revert). */
static void recommission_finish(bool paired, const char *why)
{
    if (!s_recommission_pending) return;
    s_recommission_pending = false;
    s_old_fabric_count     = 0;
    chip::DeviceLayer::SystemLayer().CancelTimer(recommission_timeout, nullptr);

    // Close the window explicitly -- expiry of OUR timer does not itself shut the CHIP window.
    auto &cwm = chip::Server::GetInstance().GetCommissioningWindowManager();
    if (cwm.IsCommissioningWindowOpen()) cwm.CloseCommissioningWindow();

    // Restore the BLE advert to what a commissioned node should be doing: nothing. If the
    // re-pair FAILED we still hold a fabric, so this is the correct resting state either way.
    CHIP_ERROR berr = chip::DeviceLayer::ConnectivityMgr().SetBLEAdvertisingEnabled(false);
    if (berr != CHIP_NO_ERROR)
        ESP_LOGE(TAG, "recommission: SetBLEAdvertisingEnabled(false) failed: %" CHIP_ERROR_FORMAT, berr.Format());

    if (!paired) hisense_send_exit_77();   // on success the delegate already cleared it
    ESP_LOGI(TAG, "recommission: %s (%s) -> window closed, BLE advert off",
             paired ? "paired" : "reverted", why);
}

// Window expired with no new pairing -> keep the old fabric, tell the A/C to exit "77".
static void recommission_timeout(chip::System::Layer *, void *)
{
    recommission_finish(false, "window expired");
}

/* The user took the A/C out of "77" themselves (panel/remote): abandon the window immediately
 * rather than leaving the device joinable for the rest of kRecommissionWindowSec. Called from
 * Matter context via ScheduleWork. */
static bool recommission_window_is_open(void)
{
    return s_recommission_pending;
}

static void recommission_user_cancel(intptr_t)
{
    recommission_finish(false, "A/C left 77");
}

// Matter-context entry (via ScheduleWork): snapshot fabrics + open the window + arm the timer.
static void recommission_open_window(intptr_t)
{
    /* TOGGLE, matching the stock dongle: pressing the sequence again while a window is open
     * EXITS "77" rather than being ignored. The A/C pulses 0x20 for one frame per press, so an
     * enter-press and an exit-press look identical on the wire -- the only thing distinguishing
     * them is whether we already have a window open. Without this, the user's documented way out
     * ("press the pattern again") did nothing and the device stayed joinable for the full 180 s. */
    if (s_recommission_pending) {
        ESP_LOGW(TAG, "\"77\" pressed again while the window is open -> treating as EXIT");
        recommission_finish(false, "user pressed 77 again");
        return;
    }
    s_old_fabric_count = 0;
    for (auto it = chip::Server::GetInstance().GetFabricTable().begin();
         it != chip::Server::GetInstance().GetFabricTable().end(); ++it) {
        if (s_old_fabric_count < (uint8_t)(sizeof(s_old_fabrics) / sizeof(s_old_fabrics[0])))
            s_old_fabrics[s_old_fabric_count++] = it->GetFabricIndex();
    }
    CHIP_ERROR err = chip::Server::GetInstance().GetCommissioningWindowManager()
                         .OpenBasicCommissioningWindow(chip::System::Clock::Seconds32(kRecommissionWindowSec));
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "recommission: OpenBasicCommissioningWindow failed: %" CHIP_ERROR_FORMAT, err.Format());
        s_old_fabric_count = 0;
        return;
    }
    /* Advertise over BLE for the whole window, ALWAYS -- including when Wi-Fi is up.
     *
     * This previously suppressed BLE whenever Wi-Fi was connected, on the reasoning that a
     * commissioned node re-pairs over IP (_matterc._udp). Spec-wise that is right; in practice
     * it made "77" useless. Measured 2026-07-20: with the device sitting healthily on Wi-Fi,
     * matter-server's discover_commissionable_nodes returned NOTHING, and commissioning over IP
     * failed with "Discovery timed out" every time. So the window opened, the A/C lit "77", and
     * no controller could see the device -- which is exactly the "77 does not work" symptom.
     *
     * BLE is also what phone commissioners actually use. Keeping it on costs a radio advert for
     * at most kRecommissionWindowSec and makes the window reachable by BOTH transports, which is
     * the entire point of a recovery path: it must work when the normal one does not. */
    err = chip::DeviceLayer::ConnectivityMgr().SetBLEAdvertisingEnabled(true);
    if (err != CHIP_NO_ERROR)
        ESP_LOGE(TAG, "recommission: SetBLEAdvertisingEnabled(true) failed: %" CHIP_ERROR_FORMAT, err.Format());
    s_recommission_pending = true;
    chip::DeviceLayer::SystemLayer().StartTimer(chip::System::Clock::Seconds32(kRecommissionWindowSec),
                                                recommission_timeout, nullptr);
    hisense_set_provisioning(true);   // report prov=1 -> A/C lights "77" while the window is open
    s_recommission_grace_us = esp_timer_get_time() + kRecommissionGraceUs;
    ESP_LOGI(TAG, "recommission: window open %us, snapshot %u old fabric(s)",
             (unsigned) kRecommissionWindowSec, s_old_fabric_count);
}

// Driver "77" callback (bus-task context) -> defer the real work to Matter context.
static void on_recommission(uint8_t reason)
{
    ESP_LOGW(TAG, "A/C requested recommission (\"77\") payload[4]=0x%02x", reason);
    chip::DeviceLayer::PlatformMgr().ScheduleWork(recommission_open_window, 0);
}

/* Log EVERY 0x1E LINK reply to the serial console, with a sequence number and uptime.
 *
 * These replies are infrequent and irregular, so polling the console snapshot cannot catch the
 * one that changes when the user presses the remote's recommission sequence -- seven presses
 * produced no visible change that way, which was ambiguous between "the A/C never asks" and
 * "we polled at the wrong moment". Pushing every reply to serial removes the ambiguity: the
 * bench captures continuously and diffs offline. Cheap -- these arrive far too rarely to spam. */
static void on_link_frame(const uint8_t *f, uint8_t n)
{
    static uint32_t seq = 0;
    char hex[3 * 40 + 1];
    int  o = 0;
    for (uint8_t i = 0; i < n && o < (int) sizeof(hex) - 3; i++)
        o += snprintf(hex + o, sizeof(hex) - o, "%02x ", f[i]);
    ESP_LOGW(TAG, "LINK#%u len=%u b17=0x%02x | %s",
             (unsigned) ++seq, (unsigned) n, n > 17 ? f[17] : 0, hex);
}

// The A/C dropped the "77" request: the user backed out from the panel/remote. Shut the window
// we opened rather than leaving the device joinable with nothing on the panel to indicate it.
static void on_recommission_cancel(void)
{
    ESP_LOGW(TAG, "A/C left \"77\" -> closing the commissioning window");
    chip::DeviceLayer::PlatformMgr().ScheduleWork(recommission_user_cancel, 0);
}

// ---------------------------------------------------------------------------
// Manual HTTPS-OTA backup (Identify=88). Fetches a full firmware image over HTTP (TCP) from
// the Pi file server and applies it via esp_https_ota (writes the idle OTA slot, verifies,
// reboots). TCP's window/retransmit is far more robust than Matter BDX on a lossy link -- this
// is the break-glass path when the standard Matter OTA fails. Runs off the Matter task.
// ---------------------------------------------------------------------------
/* Brownout mitigation for the OTA path (#12).
 *
 * This module is powered from the A/C's 5 V rail, which is marginal. An OTA is the
 * highest-current thing the chip ever does: flash writes spike 300-500 mA and Wi-Fi RX/TX
 * runs concurrently to pull the image. On this hardware that combination browned the module
 * out, and a brownout DURING a flash write can corrupt the image, which is why recovery
 * needed a USB reflash rather than a power cycle (see espressif/arduino-esp32#10445).
 *
 * The real fix is a 470-1000 uF low-ESR cap across VCC near the chip. Until that exists,
 * two software levers cut the peak:
 *
 *   1. Drop Wi-Fi TX power for the duration. TX bursts are the other half of the peak, and
 *      the OTA source is a server on the LAN, so we can afford much less power. Restored
 *      afterwards so normal Matter operation is unaffected.
 *   2. Pace the download. Using the advanced begin/perform/finish API instead of the
 *      one-shot esp_https_ota() lets us yield between chunks, which spreads the flash
 *      writes out instead of issuing them back to back.
 *
 * Neither is a substitute for the capacitor. They reduce the probability of a brownout,
 * they do not eliminate it, and the honest mitigation for a marginal supply is hardware. */
#define HISENSE_OTA_TX_POWER_QDBM  40   /* 10 dBm, quarter-dBm units. Plenty for a LAN hop. */
#define HISENSE_OTA_CHUNK_YIELD_MS 8    /* breathing room between flash writes */

static void https_ota_task(void *arg)
{
    ESP_LOGW(TAG, "HTTPS-OTA: fetching %s", HISENSE_OTA_URL);

    int8_t saved_tx = 0;
    bool   tx_saved = (esp_wifi_get_max_tx_power(&saved_tx) == ESP_OK);
    if (tx_saved) {
        esp_wifi_set_max_tx_power(HISENSE_OTA_TX_POWER_QDBM);
        ESP_LOGW(TAG, "HTTPS-OTA: Wi-Fi TX power %d -> %d (quarter-dBm) to cut the current peak",
                 (int) saved_tx, HISENSE_OTA_TX_POWER_QDBM);
    }

    esp_http_client_config_t http_cfg = {};
    http_cfg.url               = HISENSE_OTA_URL;
    http_cfg.timeout_ms        = 30000;
    http_cfg.keep_alive_enable = true;
    esp_https_ota_config_t ota_cfg = {};
    ota_cfg.http_config = &http_cfg;

    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &h);
    if (err != ESP_OK || h == NULL) {
        ESP_LOGE(TAG, "HTTPS-OTA: begin failed: %s", esp_err_to_name(err));
        if (tx_saved) esp_wifi_set_max_tx_power(saved_tx);
        vTaskDelete(NULL);
        return;
    }

    // Paced download: one chunk per iteration, then yield. Keeps flash writes from being
    // issued back-to-back at full rate.
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        vTaskDelay(pdMS_TO_TICKS(HISENSE_OTA_CHUNK_YIELD_MS));
    }

    bool complete = esp_https_ota_is_complete_data_received(h);
    if (err == ESP_OK && complete) {
        err = esp_https_ota_finish(h);
        if (tx_saved) esp_wifi_set_max_tx_power(saved_tx);
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "HTTPS-OTA: success -> rebooting into the new image");
            esp_restart();
        }
        ESP_LOGE(TAG, "HTTPS-OTA: finish failed: %s", esp_err_to_name(err));
    } else {
        // Truncated transfer: abort rather than finish, so a partial image is never marked
        // bootable. This is the case that previously left the module needing USB recovery.
        ESP_LOGE(TAG, "HTTPS-OTA: incomplete (err=%s complete=%d) -- aborting, NOT booting it",
                 esp_err_to_name(err), (int) complete);
        esp_https_ota_abort(h);
        if (tx_saved) esp_wifi_set_max_tx_power(saved_tx);
    }
    vTaskDelete(NULL);
}

static void trigger_https_ota(void)
{
    // 8 KB stack: esp_https_ota + the HTTP client + TLS-off buffers fit comfortably.
    xTaskCreate(https_ota_task, "https_ota", 8192, NULL, 5, NULL);
}

/* #61: break-glass OTA trigger that does NOT ride the Matter layer.
 *
 * Identify=88 is unreachable exactly when it is needed: a controller that has marked the node
 * unavailable refuses the write before it reaches the device ("Node <id> is not (yet)
 * available"). That is what happened to the AmebaZ2 node after an OTA broke its subscriptions --
 * healthy on the network, answering its console, but un-reflashable over the air.
 *
 * Deliberately NOT a diag-console command: that console is debug-flavour only, so a trigger
 * there would be missing from precisely the images most likely to need it. This is compiled
 * unconditionally, and therefore authenticated and fail-closed -- with no token configured the
 * socket is never opened. No default token: a default in a public repo is not authentication.
 */
#ifdef HISENSE_BREAKGLASS_TOKEN

#ifndef HISENSE_BREAKGLASS_PORT
#define HISENSE_BREAKGLASS_PORT 2324
#endif

// Non-short-circuiting compare: an early-return memcmp leaks the matched prefix across repeated
// attempts. Cheap to avoid even though the attacker must already be on the LAN.
static bool breakglass_token_ok(const char *got, size_t got_len)
{
    const char  *want     = HISENSE_BREAKGLASS_TOKEN;
    const size_t want_len = sizeof(HISENSE_BREAKGLASS_TOKEN) - 1;
    unsigned char diff = 0;

    if (got_len != want_len) return false;
    for (size_t i = 0; i < want_len; i++) diff |= (unsigned char) (got[i] ^ want[i]);
    return diff == 0;
}

static void breakglass_task(void *arg)
{
    int srv = socket(AF_INET6, SOCK_STREAM, 0);
    struct sockaddr_in6 a = {};
    int one = 1;

    (void) arg;
    if (srv < 0) { ESP_LOGE(TAG, "break-glass: socket failed"); vTaskDelete(NULL); return; }
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    a.sin6_family = AF_INET6;
    a.sin6_port   = htons(HISENSE_BREAKGLASS_PORT);
    if (bind(srv, (struct sockaddr *) &a, sizeof(a)) != 0 || listen(srv, 1) != 0) {
        ESP_LOGE(TAG, "break-glass: bind/listen failed on %d", HISENSE_BREAKGLASS_PORT);
        close(srv);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "break-glass listener up on :%d (#61)", HISENSE_BREAKGLASS_PORT);

    for (;;) {
        int cs = accept(srv, NULL, NULL);
        char buf[96];
        if (cs < 0) continue;

        int n = recv(cs, buf, sizeof(buf) - 1, 0);
        if (n <= 0) { close(cs); continue; }
        buf[n] = 0;

        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n' || buf[len - 1] == ' ')) {
            buf[--len] = 0;
        }

        if (breakglass_token_ok(buf, len)) {
            static const char kOk[] =
                "ok: fetching; device reboots into the idle slot.\r\n"
                "verify the RUNNING version afterwards -- a failed apply\r\n"
                "looks like success until you check.\r\n";
            ESP_LOGW(TAG, "break-glass: token accepted, starting OTA fetch");
            send(cs, kOk, sizeof(kOk) - 1, 0);
            close(cs);
            trigger_https_ota();
        } else {
            // No detail: do not reveal whether the length or the bytes were wrong.
            ESP_LOGW(TAG, "break-glass: rejected (bad token)");
            send(cs, "no\r\n", 4, 0);
            close(cs);
        }
    }
}

static void breakglass_start(void)
{
    xTaskCreate(breakglass_task, "breakglass", 4096, NULL, 5, NULL);
}

#else  /* fail closed, and say so at boot rather than silently */
static void breakglass_start(void)
{
    ESP_LOGE(TAG, "break-glass DISABLED: no HISENSE_BREAKGLASS_TOKEN at build time (#61). "
                  "Recovery depends on the Matter layer being healthy.");
}
#endif /* HISENSE_BREAKGLASS_TOKEN */

// ---------------------------------------------------------------------------
// UserLabel entity naming (cluster 0x0041). esp-matter's UserLabel cluster VerifyOrDie's
// without a DeviceInfoProvider, and ESP32DeviceInfoProvider isn't linkable without the
// factory-data path (which would break test-cred commissioning). So provide a minimal in-RAM
// DeviceInfoProvider that only serves UserLabels; installed via set_custom_device_info_provider
// BEFORE start() (runs before Server::Init -> the cluster init sees a non-null provider). Labels
// are re-applied each boot; HA caches the names after interview. (ports matter_drivers.cpp labels)
// ---------------------------------------------------------------------------
namespace {
using UserLabelType = chip::DeviceLayer::DeviceInfoProvider::UserLabelType;
class AppDeviceInfoProvider : public chip::DeviceLayer::DeviceInfoProvider
{
public:
    static AppDeviceInfoProvider &Instance() { static AppDeviceInfoProvider i; return i; }
    FixedLabelIterator *IterateFixedLabel(chip::EndpointId) override { return nullptr; }
    SupportedLocalesIterator *IterateSupportedLocales() override { return nullptr; }
    SupportedCalendarTypesIterator *IterateSupportedCalendarTypes() override { return nullptr; }
    UserLabelIterator *IterateUserLabel(chip::EndpointId ep) override
    {
        return chip::Platform::New<Iter>(mStore[ep]);
    }
protected:
    struct Entry {
        char name[chip::DeviceLayer::kMaxLabelNameLength + 1];
        char value[chip::DeviceLayer::kMaxLabelValueLength + 1];
    };
    CHIP_ERROR GetUserLabelLength(chip::EndpointId ep, size_t &val) override
    {
        auto it = mStore.find(ep); val = (it == mStore.end()) ? 0 : it->second.size();
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR SetUserLabelLength(chip::EndpointId ep, size_t val) override
    {
        mStore[ep].resize(val); return CHIP_NO_ERROR;
    }
    CHIP_ERROR SetUserLabelAt(chip::EndpointId ep, size_t idx, const UserLabelType &l) override
    {
        auto &v = mStore[ep];
        if (idx >= v.size()) v.resize(idx + 1);
        chip::Platform::CopyString(v[idx].name,  sizeof(v[idx].name),  l.label);
        chip::Platform::CopyString(v[idx].value, sizeof(v[idx].value), l.value);
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR DeleteUserLabelAt(chip::EndpointId ep, size_t idx) override
    {
        auto &v = mStore[ep];
        if (idx < v.size()) v.erase(v.begin() + idx);
        return CHIP_NO_ERROR;
    }
private:
    class Iter : public UserLabelIterator
    {
    public:
        explicit Iter(std::vector<Entry> &v) : mVec(v) {}
        size_t Count() override { return mVec.size(); }
        bool Next(UserLabelType &out) override
        {
            if (mIndex >= mVec.size()) return false;
            out.label = chip::CharSpan::fromCharString(mVec[mIndex].name);
            out.value = chip::CharSpan::fromCharString(mVec[mIndex].value);
            ++mIndex; return true;
        }
        void Release() override { chip::Platform::Delete(this); }
    private:
        std::vector<Entry> &mVec; size_t mIndex = 0;
    };
    std::map<chip::EndpointId, std::vector<Entry>> mStore;
};

static void set_entity_label(chip::EndpointId ep, const char *name)
{
    UserLabelType l;
    l.label = chip::CharSpan::fromCharString("ha_entitylabel");
    l.value = chip::CharSpan::fromCharString(name);
    AppDeviceInfoProvider::Instance().SetUserLabelList(ep, chip::Span<const UserLabelType>(&l, 1));
}
} // namespace

// ---------------------------------------------------------------------------
// Endpoint construction helpers.
// ---------------------------------------------------------------------------
// Minimal On/Off plug-in unit MATCHING the AmebaZ2 .zap switch endpoints exactly: device type
// 0x010A with only Identify + OnOff (DeadFront feature -> FeatureMap=2, like node 14) + UserLabel.
// esp-matter's endpoint::on_off_plug_in_unit::create() would ALSO add Groups (0x04),
// ScenesManagement (0x62), and the OnOff Lighting feature (StartUpOnOff = HA's "power-on
// behavior") -- none of which the kitchen unit has -- so build the endpoint by hand instead.
static uint16_t make_onoff_switch(node_t *node)
{
    endpoint_t *ep = endpoint::create(node, ENDPOINT_FLAG_NONE, NULL);
    endpoint::add_device_type(ep, endpoint::on_off_plug_in_unit::get_device_type_id(),
                              endpoint::on_off_plug_in_unit::get_device_type_version());

    // Raw endpoint::create doesn't add the mandatory Descriptor (0x1d); the device-type
    // helpers add it via common::create. Add it by hand (matches every other endpoint).
    cluster::descriptor::config_t descriptor_cfg;
    cluster::descriptor::create(ep, &descriptor_cfg, CLUSTER_FLAG_SERVER);

    cluster::identify::config_t identify_cfg;
    cluster_t *idc = cluster::identify::create(ep, &identify_cfg, CLUSTER_FLAG_SERVER);
    cluster::identify::command::create_trigger_effect(idc);

    cluster::on_off::config_t onoff_cfg;
    // `on_off` defaults false. Callers that model something the A/C reports back (eco,
    // turbo, mute) want that, because status resync corrects it within a second. The
    // Display switch does NOT: the A/C reports no display state, so nothing can ever
    // correct a wrong default. See make_display_switch below.
    cluster_t *ooc = cluster::on_off::create(ep, &onoff_cfg, CLUSTER_FLAG_SERVER);
    cluster::on_off::feature::dead_front_behavior::add(ooc);   // FeatureMap=2, matches node 14 (no Lighting/StartUpOnOff)
    cluster::on_off::command::create_on(ooc);
    cluster::on_off::command::create_toggle(ooc);

    cluster::user_label::create(ep, NULL, CLUSTER_FLAG_SERVER);
    return endpoint::get_id(ep);
}

/* ep9 Display (#33). Same switch, but the OnOff attribute starts TRUE.
 *
 * The panel is lit by default on this hardware, and the A/C reports no display state, so
 * an attribute that starts false is wrong from boot and can never self-correct. That is
 * not just cosmetic: Matter skips both attribute-change callbacks when a write does not
 * change the value (emAfWriteAttribute returns early on !valueChanging), so our handler
 * never runs and NO frame reaches the A/C. A user seeing "off" while the panel is lit and
 * pressing off therefore got nothing at all, and had to toggle on-then-off.
 *
 * Starting true makes the common case a real transition, so the first press works.
 *
 * This does NOT make the attribute truthful in general: change the display from the IR
 * remote and it drifts again, and no read-back exists to fix it. A stateless control would
 * model this honestly, but OnOff is what Home Assistant renders as a switch. */
static uint16_t make_display_switch(node_t *node)
{
    uint16_t id = make_onoff_switch(node);
    esp_matter_attr_val_t on = esp_matter_bool(true);
    attribute::update(id, OnOff::Id, OnOff::Attributes::OnOff::Id, &on);
    return id;
}

/* Why the last boot happened (#12). Captured once at startup, before anything can
 * overwrite it, and surfaced on the console.
 *
 * This exists because a module went unresponsive after an OTA and had to be recovered by
 * powering it from USB, and we had NO evidence why. The suspicion is the A/C's 5 V rail
 * sagging: flash writes during an OTA plus Wi-Fi TX peaks draw far more than steady state,
 * and a marginal supply can hang the chip mid-write. That is a guess until a reset reason
 * says ESP_RST_BROWNOUT, which is exactly the point of recording it.
 *
 * Note the brownout detector is at the LOWEST threshold (CONFIG_ESP_BROWNOUT_DET_LVL_SEL_0,
 * about 2.43 V). That is the worst setting for this failure: the rail can sag far enough to
 * upset the flash chip or the PHY while the CPU keeps running, so you get a HANG rather
 * than a clean reset-and-recover. Raising it would trade a hang for an automatic reboot.
 * Not changed here: it should be an evidence-driven decision, and this is the evidence. */
static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_EXT:      return "external pin";
    case ESP_RST_SW:       return "software restart (expected after our OTA)";
    case ESP_RST_PANIC:    return "PANIC / exception";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "other watchdog";
    case ESP_RST_DEEPSLEEP:return "deep sleep wake";
    case ESP_RST_BROWNOUT: return "BROWNOUT (supply sagged)";
    case ESP_RST_SDIO:     return "SDIO";
    default:               return "unknown";
    }
}

static esp_reset_reason_t s_boot_reason = ESP_RST_UNKNOWN;

extern "C" void diag_get_boot_reason(int *code, const char **text)
{
    if (code) *code = (int) s_boot_reason;
    if (text) *text = reset_reason_str(s_boot_reason);
}

/* Make TemperatureDisplayMode genuinely READ-ONLY at the protocol layer (#5).
 *
 * Home Assistant renders this attribute as a WRITABLE select. We can read the A/C's display
 * unit correctly, but the WRITE path is unverified: RE docs/10 7.4b predicts command byte 23
 * (0x03 = F, 0x01 = C) and nobody has tested it. Guessing wrong here is not cosmetic; sending
 * the wrong unit previously made a unit target 23 F and run at 74 Hz toward -5 C.
 *
 * A comment saying "do not wire this yet" is not a gate. This repo already shipped that
 * failure once: the ep9 Display switch appeared in HA and silently did nothing (#33). So the
 * rejection is enforced in code.
 *
 * emberAfAttributeWriteAccessCallback is a weak symbol in the SDK (generic-callback-stubs.cpp)
 * checked by attribute-storage.cpp BEFORE the value reaches storage. Returning false yields
 * Status::UnsupportedAccess, so a HA write fails VISIBLY rather than appearing to succeed and
 * silently reverting on the next status poll.
 *
 * TemperatureDisplayMode is now WRITABLE: the bench check passed on this node 2026-07-20
 * (tx 23 0x01 -> Celsius, tx 23 0x03 -> Fahrenheit, status byte 26 bit 1 followed both times)
 * and the TUIC handler above changes the unit and the setpoint in one frame.
 *
 * KeypadLockout stays rejected. It exists because the cluster requires it, but no bus command
 * for it has been identified, so a write would silently do nothing -- exactly the ep9-Display
 * failure (#33) this hook exists to prevent. */
// NOTE: C++ linkage, NOT extern "C". The SDK declares this weak hook as a C++ symbol, so an
// extern "C" definition here does not override it (and fails to compile against the decl).
bool emberAfAttributeWriteAccessCallback(chip::EndpointId endpoint,
                                         chip::ClusterId clusterId,
                                         chip::AttributeId attributeId)
{
    if (clusterId == ThermostatUserInterfaceConfiguration::Id &&
        attributeId ==
            ThermostatUserInterfaceConfiguration::Attributes::KeypadLockout::Id) {
        return false;   // no bus command identified for keypad lock
    }
    (void) endpoint;
    return true;
}

// ---------------------------------------------------------------------------
extern "C" void app_main()
{
    s_boot_reason = esp_reset_reason();
    ESP_LOGW(TAG, "boot reason: %s (%d)", reset_reason_str(s_boot_reason), (int) s_boot_reason);

    /* Silence esp_matter's per-attribute INFO logging. It prints a banner line for EVERY
     * attribute write, and this driver republishes ~19 attributes per status frame. Measured on
     * node 28: 12.7 KB/s of UART output against a 11.5 KB/s ceiling at 115200 8N1 -- i.e. the
     * console is saturated CONTINUOUSLY.
     *
     * That is not cosmetic. esp_log writes block the calling task once the TX buffer fills, so
     * the whole application starves: the device answered ICMP (handled low in the stack) while
     * REFUSING TCP on :2323/:2324 and failing every Matter CASE handshake, which reads exactly
     * like a dead node from the network side. It looked like a crash and was not one.
     *
     * Targeted rather than lowering CONFIG_LOG_DEFAULT_LEVEL: everything else stays at INFO, so
     * boot, Wi-Fi, OTA and our own hisense_ac logs are unaffected. */
    esp_log_level_set("esp_matter_attribute", ESP_LOG_WARN);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase(); nvs_flash_init();
    }

    node::config_t node_cfg;
    node_t *node = node::create(&node_cfg, on_attribute_update, NULL);

    // ep1: Room Air Conditioner. The device type creates ONLY identify + on_off +
    // thermostat(COOLING ONLY); add FanControl, Thermostat RunningState + the Heating & AutoMode
    // features, ElectricalPowerMeasurement, the Hisense mfg cluster, and UserLabel to reach
    // parity with the AmebaZ2 .zap.
    endpoint::room_air_conditioner::config_t rac_cfg;
    endpoint_t *ep = endpoint::room_air_conditioner::create(node, &rac_cfg, ENDPOINT_FLAG_NONE, NULL);
    s_ep_id = endpoint::get_id(ep);

    cluster_t *th = cluster::get(ep, Thermostat::Id);
    cluster::thermostat::attribute::create_thermostat_running_state(th, 0);
    // The RAC device type enables ONLY the Cooling feature, so HA offers just Cool/Off. Add
    // Heating + AutoMode to reach FeatureMap 35 (Heat+Cool+Auto) like the kitchen (node 14),
    // so Heat/Auto appear; the driver also maps Dry->SystemMode Dry and Fan->FanOnly.
    // (AutoMode mandates Heating+Cooling, which are both present.)
    cluster::thermostat::feature::heating::config_t heat_feat;
    cluster::thermostat::feature::heating::add(th, &heat_feat);
    cluster::thermostat::feature::auto_mode::config_t auto_feat;
    cluster::thermostat::feature::auto_mode::add(th, &auto_feat);

    cluster::fan_control::config_t fc_cfg;              // FanMode=0(Off), seq=2, PercentCurrent=0
    cluster_t *fc = cluster::fan_control::create(ep, &fc_cfg, CLUSTER_FLAG_SERVER);
    // #19 cheap win: vertical swing via the Rocking feature (RockSupport/RockSetting). The A/C
    // has a vertical louvre motor only -> advertise RockUpDown (0x02); HA gets a swing toggle.
    cluster::fan_control::feature::rocking::config_t rock_cfg;
    rock_cfg.rock_support = 0x02;   // RockUpDown supported
    rock_cfg.rock_setting = 0x00;
    cluster::fan_control::feature::rocking::add(fc, &rock_cfg);

    // ElectricalPowerMeasurement: create() auto-adds PowerMode/NumMeasTypes/Accuracy/ActivePower;
    // add Voltage + ActiveCurrent (the AC feature only sets the feature-map bit).
    cluster::electrical_power_measurement::config_t epm_cfg;
    epm_cfg.feature_flags = cluster::electrical_power_measurement::feature::alternating_current::get_id();
    epm_cfg.delegate      = &s_epm_delegate;   // reads route through the delegate's Get*()
    cluster_t *epm = cluster::electrical_power_measurement::create(ep, &epm_cfg, CLUSTER_FLAG_SERVER);
    cluster::electrical_power_measurement::attribute::create_voltage(epm, nullable<int64_t>());
    cluster::electrical_power_measurement::attribute::create_active_current(epm, nullable<int64_t>());

    /* C/F display unit (#5) via the STANDARD ThermostatUserInterfaceConfiguration cluster
     * (0x0204) on ep1, so Home Assistant renders it natively. No new endpoint.
     *
     * KeypadLockout is created whether we want it or not: esp-matter's create() makes both
     * attributes unconditionally, and the cluster spec marks only ScheduleProgrammingVisibility
     * optional. Seeded to NoLockout(0) and never fed; the A/C has no keypad-lockout concept.
     * Harmless in practice because HA gates its child-lock switch on that attribute to Eve's
     * vendor id, and we report 0xFFF1, so no stray entity appears.
     *
     * TemperatureDisplayMode is READ-ONLY here on purpose: see the write-access override
     * below. */
    cluster::thermostat_user_interface_configuration::config_t tuic_cfg;
    cluster::thermostat_user_interface_configuration::create(ep, &tuic_cfg, CLUSTER_FLAG_SERVER);

    // Hisense manufacturer cluster 0xFFF1FC00 (4 attrs, non-volatile read-back).
    cluster_t *mfg = cluster::create(ep, kMfgClusterId, CLUSTER_FLAG_SERVER);
    attribute::create(mfg, 0x0000, ATTRIBUTE_FLAG_NONVOLATILE, esp_matter_bool(false));
    attribute::create(mfg, 0x0001, ATTRIBUTE_FLAG_NONVOLATILE, esp_matter_bool(false));
    attribute::create(mfg, 0x0002, ATTRIBUTE_FLAG_NONVOLATILE, esp_matter_bool(false));
    attribute::create(mfg, 0x0003, ATTRIBUTE_FLAG_NONVOLATILE, esp_matter_uint8(0));

    cluster::user_label::create(ep, NULL, CLUSTER_FLAG_SERVER);

    // ep2: outdoor temperature.
    endpoint::temperature_sensor::config_t outdoor_cfg;
    endpoint_t *ep_out = endpoint::temperature_sensor::create(node, &outdoor_cfg, ENDPOINT_FLAG_NONE, NULL);
    cluster::user_label::create(ep_out, NULL, CLUSTER_FLAG_SERVER);
    s_ep_outdoor = endpoint::get_id(ep_out);

    // ep3/4/5: Eco / Quiet / Turbo On-Off switches.
    s_ep_eco   = make_onoff_switch(node);
    s_ep_mute  = make_onoff_switch(node);
    s_ep_turbo = make_onoff_switch(node);

    // ep6: Sleep-profile ModeSelect (Off/General/Old/Young/Kids).
    endpoint::mode_select::config_t sleep_cfg;
    strncpy(sleep_cfg.mode_select.description, "Sleep", sizeof(sleep_cfg.mode_select.description) - 1);
    sleep_cfg.mode_select.current_mode = 0;
    sleep_cfg.mode_select.delegate     = &s_sleep_mgr;   // installed as the global SupportedModesManager
    endpoint_t *ep_sleep = endpoint::mode_select::create(node, &sleep_cfg, ENDPOINT_FLAG_NONE, NULL);
    cluster::user_label::create(ep_sleep, NULL, CLUSTER_FLAG_SERVER);
    s_ep_sleep = endpoint::get_id(ep_sleep);

    // ep7: aux/PTC heat relay -> Contact Sensor (BooleanState).
    endpoint::contact_sensor::config_t aux_cfg;
    endpoint_t *ep_aux = endpoint::contact_sensor::create(node, &aux_cfg, ENDPOINT_FLAG_NONE, NULL);
    cluster::user_label::create(ep_aux, NULL, CLUSTER_FLAG_SERVER);
    s_ep_aux = endpoint::get_id(ep_aux);

    // ep8: coil temperature.
    endpoint::temperature_sensor::config_t coil_cfg;
    endpoint_t *ep_coil = endpoint::temperature_sensor::create(node, &coil_cfg, ENDPOINT_FLAG_NONE, NULL);
    cluster::user_label::create(ep_coil, NULL, CLUSTER_FLAG_SERVER);
    s_ep_coil = endpoint::get_id(ep_coil);

    // ep9: panel display -> OnOff switch (#19 cheap win). Created after coil so ep1..8
    // keep their IDs (renumbering would break the HA entity map + AmebaZ2 .zap parity;
    // endpoints stay contiguous).
    s_ep_display = make_display_switch(node);   // #33: starts TRUE, panel is lit by default

    // ep10: aggregate A/C fault -> Contact Sensor (BooleanState), a structural clone of
    // ep7 above. ONE aggregate rather than 18 per-fault endpoints: only f_e_incom has a
    // confirmed trigger/clear cycle on real hardware, and 17 entities reading false forever
    // is clutter, not diagnostics. Promote an individual fault to its own endpoint later if
    // one earns it. Created LAST: esp-matter assigns IDs in creation order, and the docs +
    // the AmebaZ2 .zap number this endpoint 10 (a gap hard-faults AmebaZ2 on boot).
    endpoint::contact_sensor::config_t fault_cfg;
    endpoint_t *ep_fault = endpoint::contact_sensor::create(node, &fault_cfg, ENDPOINT_FLAG_NONE, NULL);
    cluster::user_label::create(ep_fault, NULL, CLUSTER_FLAG_SERVER);
    s_ep_fault = endpoint::get_id(ep_fault);

    ESP_LOGI(TAG, "endpoints: aircon=%u outdoor=%u eco=%u mute=%u turbo=%u sleep=%u aux=%u coil=%u display=%u fault=%u",
             s_ep_id, s_ep_outdoor, s_ep_eco, s_ep_mute, s_ep_turbo, s_ep_sleep, s_ep_aux, s_ep_coil, s_ep_display, s_ep_fault);

    // Install the in-RAM DeviceInfoProvider BEFORE start() so the UserLabel cluster init
    // (during Server::Init) sees a non-null provider (else it VerifyOrDie's).
    esp_matter::set_custom_device_info_provider(&AppDeviceInfoProvider::Instance());

    esp_matter::start(NULL);   // brings up Wi-Fi/Matter + commissioning

    // HA entity labels (UserLabel key "ha_entitylabel") -> distinguishable same-type entities.
    // start() has returned (Server::Init done, endpoints exist); take the stack lock since we
    // run on a different task than the Matter event loop. Read live by the UserLabel cluster.
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    set_entity_label(s_ep_id,      "Climate");
    set_entity_label(s_ep_outdoor, "Outdoor");
    set_entity_label(s_ep_eco,     "Eco");
    set_entity_label(s_ep_mute,    "Quiet");
    set_entity_label(s_ep_turbo,   "Turbo");
    set_entity_label(s_ep_sleep,   "Sleep");
    set_entity_label(s_ep_aux,     "Aux Heat");
    set_entity_label(s_ep_coil,    "Coil");
    set_entity_label(s_ep_display, "Display");
    set_entity_label(s_ep_fault,   "Fault");
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    // F1: swap the fabric when a new controller pairs during a "77" window.
    chip::Server::GetInstance().GetFabricTable().AddFabricDelegate(&s_recommission_delegate);

    /* An UNCOMMISSIONED device must stay joinable. Without this it is effectively unrecoverable
     * without a serial cable, which cost most of a day.
     *
     * Default esp-matter behaviour: while no Wi-Fi credentials exist the device advertises
     * commissionable over BLE, but the moment it associates it switches to "commissioning mode 0"
     * and stops advertising entirely -- even with ZERO fabrics. Observed here: `sta ip:` at 3.2 s
     * followed immediately by `Updating services using commissioning mode 0`. The BLE window is
     * then only open for the ~3 s before association, which no human or controller can hit
     * reliably, and over IP the device is never discoverable at all
     * (`discover_commissionable_nodes` returned nothing while the device sat happily on Wi-Fi).
     *
     * That combination is the trap: credentials good enough to join Wi-Fi, no fabric, and no way
     * back in. It bites hardest after a partial commissioning, which leaves exactly that state.
     *
     * So: if there are no fabrics, hold a commissioning window open. It advertises over BOTH BLE
     * and IP, so a controller can reach it on the network without physical proximity. Re-armed
     * from the fabric delegate is unnecessary -- once a fabric exists this is moot, and the "77"
     * path handles deliberate re-pairing. */
    if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
        chip::DeviceLayer::PlatformMgr().LockChipStack();
        CHIP_ERROR werr = chip::Server::GetInstance().GetCommissioningWindowManager()
                              .OpenBasicCommissioningWindow(
                                  chip::System::Clock::Seconds32(kUncommissionedWindowSec));
        chip::DeviceLayer::PlatformMgr().UnlockChipStack();
        if (werr == CHIP_NO_ERROR) {
            ESP_LOGW(TAG, "no fabrics: commissioning window held open for %d s (BLE + IP)",
                     (int) kUncommissionedWindowSec);
        } else {
            ESP_LOGE(TAG, "no fabrics but OpenBasicCommissioningWindow failed: %" CHIP_ERROR_FORMAT,
                     werr.Format());
        }
    }

    // Start the (unchanged) RS-485 driver; wire status uplink + "77" / feature / link downlinks.
    hisense_set_recommission_cb(on_recommission);
    hisense_set_recommission_cancel_cb(on_recommission_cancel);
    hisense_set_link_frame_cb(on_link_frame);   // bench: every 0x1E reply -> serial
    hisense_set_features_cb(on_features);   // 0x66/40 ProductType flags -> log
    hisense_set_link_cb(on_link);           // #56: bus silence -> null liveness attrs
    if (hisense_init(on_status) != pdPASS) ESP_LOGE(TAG, "hisense_init failed");

#ifdef CONFIG_HISENSE_DEBUG_BUILD
    diag_console_start();   // :2323 telnet diagnostics (token/poll/watch/decode/selftest)
#endif
    // #61: OTA trigger off the Matter path. Deliberately OUTSIDE the debug gate: the
    // function is compiled unconditionally with a fail-closed stub, and a recovery
    // listener is needed most on release images, which have no diag console.
    breakglass_start();
}
