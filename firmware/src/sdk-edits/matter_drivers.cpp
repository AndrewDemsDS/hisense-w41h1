/*
 * matter_drivers.cpp -- Hisense W41H1 RS-485 integration for the Matter
 * room_air_conditioner example.
 *
 * Replaces the SDK example's DHT11 + PWM-fan stubs with the real RS-485 driver
 * (hisense_rs485.{h,cpp}, sniff-validated against the W41H1 A/C bus).
 *
 *   Uplink   (Matter attribute write -> A/C): a HisenseCommand "shadow" is
 *            updated per write and flushed as one frame via hisense_send_frame();
 *            power on/off and mode-off use the literal power frames.
 *   Downlink (A/C status -> Matter): hisense_on_status() (bus-task context)
 *            snapshots the parsed HisenseState and posts ONE downlink AppEvent;
 *            the handler (Matter-task context, under the CHIP stack lock) pushes
 *            LocalTemperature / SystemMode / fan into the attribute store.
 */
#include <matter_drivers.h>
#include <matter_interaction.h>
#include <gpio_api.h>
#include <room_aircon_driver.h>
#include <temp_hum_sensor_driver.h>

#include "hisense_rs485.h"
#include "matter_aircon_map.h"
#include <clusters/HisenseAircon/ClusterId.h>   // mfg cluster + attribute id constants (synced to zzz_generated)
#include "power_estimate.h"
#include "hisense_diag_console.h"   // #23 debug-flavour :2323 console (no-op in release)

// #61: defined further down this TU (it needs hisense_trigger_https_ota, which is defined
// below), but called from the init path above it -- so it needs a forward declaration.
static void hisense_breakglass_start(void);
#include "ElectricalPowerMeasurementDelegate.h"
#include <mode_select/ameba_mode_select_manager.h>   // Sleep profile ModeSelect (ep6)
#include <memory>

#include <app-common/zap-generated/attribute-type.h>
#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/util/attribute-table.h>
#include <protocols/interaction_model/StatusCode.h>
#include <app/server/Server.h>                 // Server / fabric table / commissioning window (F1 "77")
#include <app/server/CommissioningWindowManager.h>
#include <platform/ConnectivityManager.h>      // SetBLEAdvertisingEnabled (spec-compliant on-network re-pair)
#include <credentials/FabricTable.h>
#include <platform/DeviceInfoProvider.h>       // UserLabel "ha_entitylabel" -> HA entity names
#include <system/SystemLayer.h>
#include <system/SystemClock.h>                 // monotonic clock for the time-based sync hold-off (#61)

#include <FreeRTOS.h>                            // taskENTER_CRITICAL for the s_status snapshot (#57)
#include <task.h>

using namespace ::chip::app;
using chip::Protocols::InteractionModel::Status;

// Manufacturer-cluster server hooks required by the generated cluster-callbacks
// (ember-only cluster -> no dedicated server dir, so define no-op stubs here).
void emberAfHisenseAirconClusterInitCallback(chip::EndpointId) {}
void emberAfHisenseAirconClusterShutdownCallback(chip::EndpointId) {}
void MatterHisenseAirconClusterServerShutdownCallback(chip::EndpointId) {}
// Plugin server-init hook emitted into util.cpp's MATTER_PLUGINS_INIT list once the
// Hisense cluster is actually endpoint-attached (custom cluster -> no generated impl).
void MatterHisenseAirconPluginServerInitCallback() {}


namespace ThermAttr    = chip::app::Clusters::Thermostat::Attributes;
namespace FanAttr      = chip::app::Clusters::FanControl::Attributes;
namespace TempMeasAttr = chip::app::Clusters::TemperatureMeasurement::Attributes;
namespace BoolAttr     = chip::app::Clusters::BooleanState::Attributes;
namespace HisenseCl    = chip::app::Clusters::HisenseAircon;
namespace HisenseAttr  = chip::app::Clusters::HisenseAircon::Attributes;
using chip::app::Clusters::Thermostat::SystemModeEnum;

// Ember attribute types for the raw manufacturer-cluster read-back writes (custom
// cluster -> no generated accessors). ZCL: boolean=0x10, int8u=0x20, int8s=0x28.
static constexpr EmberAfAttributeType kHisenseTypeBool  = (EmberAfAttributeType)0x10;
static constexpr EmberAfAttributeType kHisenseTypeInt8u = (EmberAfAttributeType)0x20;
static constexpr EmberAfAttributeType kHisenseTypeInt8s = (EmberAfAttributeType)0x28;

MatterRoomAirCon aircon;

static const chip::EndpointId kAirconEp      = 1;
static const chip::EndpointId kOutdoorTempEp = 2;  // extra TemperatureMeasurement ep (was 3; humidity ep2 removed)

/* Special-mode switch endpoints (MA-onoffpluginunit, deviceType 266). Each is a
 * plain On/Off cluster HA renders as a switch entity; the glue routes its OnOff
 * write to the corresponding Hisense frame and syncs it back from status. These
 * mirror the manufacturer-cluster attrs (0xFFF1FC00 0x00-0x03) but, unlike raw mfg
 * attributes, are actually controllable from Home Assistant. */
static const chip::EndpointId kEcoEp   = 3;
static const chip::EndpointId kMuteEp  = 4;  // "Quiet"
static const chip::EndpointId kTurboEp = 5;
static const chip::EndpointId kSleepEp = 6;  // OnOff -> sleep profile (on=General, off=0)
static const chip::EndpointId kHeatRelayEp = 7;  // BooleanState (Contact Sensor) -> aux/PTC electric-heat relay (#51)
static const chip::EndpointId kCoilTempEp  = 8;  // TemperatureMeasurement -> outdoor/condenser coil temp (#51)
static const chip::EndpointId kDisplayEp   = 9;  // OnOff -> panel display (#19 parity; on=0xC0/off=0x40 @20)

/* --------------------------------------------------------------------------
 * Command shadow: incrementally updated by each Matter write, flushed as one
 * combined frame. (Per hisense_rs485.h: multi-field frames are protocol-legal;
 * if the bench shows the A/C ignores combined writes, switch to one flush per
 * changed field.) Power on/off is handled separately via the literal frames.
 * ------------------------------------------------------------------------ */
static HisenseCommand s_cmd = { HISENSE_MODE_COOL, 24, false,
                                HISENSE_FAN_AUTO, HISENSE_SWING_OFF,
                                HISENSE_SWING_OFF, HISENSE_FEATURE_NONE, HISENSE_DISPLAY_NOCHANGE };

/* Latest parsed A/C status, written by the bus task, read by the downlink
 * handler. Poll cadence is seconds apart so a plain snapshot copy is adequate. */
static volatile bool  s_status_fresh = false;
static HisenseState   s_status;

/* Bus-link liveness (#56): set when the RS-485 status poll has been silent for
 * HISENSE_LINK_LOST_POLLS cycles. While lost, the downlink handler nulls the
 * liveness attributes (HA shows the entities unavailable) instead of holding stale
 * values; the next good poll repopulates them. */
static volatile bool  s_link_lost = false;

/* Downlink shadow-sync hold-off: after any uplink command we skip syncing the
 * command shadow from status until a fixed wall-clock settle time elapses, so a
 * stale pre-command status frame can't revert the shadow before the A/C has applied
 * the command. Time-based rather than a poll count (#61): the poll cadence stretches
 * during a link flap, which made the old "3 polls" an unreliable duration. Both s_cmd
 * and this timestamp are only touched under the CHIP stack lock. */
#define HISENSE_SYNC_HOLD_MS 3000
static chip::System::Clock::Timestamp s_sync_hold_until = chip::System::Clock::kZero;

static void hisense_flush_command(void)
{
    uint8_t frame[HISENSE_CMD_FRAME_LEN + 2];
    size_t len = hisense_build_command(&s_cmd, frame, sizeof(frame));
    // Only arm the resync hold if the frame actually enqueued. Otherwise a dropped
    // command (TX queue full) would suppress status-resync for HISENSE_SYNC_HOLD_MS,
    // so HA shows a state that was never applied, then silently reverts.
    if (len > 0 && hisense_send_frame(frame, len)) {
        // let this command settle before trusting status (wall-clock, not poll count)
        s_sync_hold_until = chip::System::SystemClock().GetMonotonicTimestamp()
                          + chip::System::Clock::Milliseconds32(HISENSE_SYNC_HOLD_MS);
    } else if (len > 0) {
        ChipLogError(DeviceLayer, "hisense_flush_command: TX enqueue dropped (queue full)");
    } else {
        // len == 0: the builder refused the shadow. This used to be silent, which is how an
        // out-of-range shadow setpoint could disable ALL combined-frame control with nothing
        // in the log. The shadow is guarded at the resync now, so this should be unreachable;
        // if it fires, every command is being dropped -- say so loudly.
        ChipLogError(DeviceLayer,
                     "hisense_flush_command: builder REJECTED the shadow (mode=%d setpoint=%d "
                     "fahrenheit=%d) -- frame NOT sent; all combined control is dead until "
                     "the shadow is valid again",
                     (int) s_cmd.mode, (int) s_cmd.setpoint, (int) s_cmd.fahrenheit);
    }
}

static void hisense_send_power(bool on)
{
    uint8_t frame[HISENSE_CMD_FRAME_LEN];
    size_t len = hisense_build_power_frame(on, frame, sizeof(frame));
    if (len > 0 && hisense_send_frame(frame, len)) {
        s_sync_hold_until = chip::System::SystemClock().GetMonotonicTimestamp()
                          + chip::System::Clock::Milliseconds32(HISENSE_SYNC_HOLD_MS);
    } else if (len > 0) {
        ChipLogError(DeviceLayer, "hisense_send_power: TX enqueue dropped (queue full)");
    }
}

/* --------------------------------------------------------------------------
 * Mappings live in matter_aircon_map.h (pure, host-unit-tested by
 * firmware/test/test_matter_map.cpp). This file does only the CHIP attribute
 * I/O around them.
 * ------------------------------------------------------------------------ */

/* --------------------------------------------------------------------------
 * Downlink: A/C status -> Matter. hisense_on_status runs in the bus task, so
 * it only snapshots + posts an event; the handler does the CHIP Sets.
 * ------------------------------------------------------------------------ */
static void hisense_on_status(const HisenseState *state)
{
    if (state == NULL || !state->valid) {
        return;
    }
    // Bus-task write vs the CHIP-task snapshot reads (uplink echo-guard / downlink):
    // guard the multi-word store so a reader can't observe a torn HisenseState (#57).
    hisense_diag_on_status(state);   // #23: snapshot for the debug console (no-op in release)
    taskENTER_CRITICAL();
    s_status = *state;
    s_status_fresh = true;
    taskEXIT_CRITICAL();

    AppEvent ev;
    ev.Type = AppEvent::kEventType_Downlink_Aircon_Status;
    ev.mHandler = matter_driver_downlink_update_handler;
    PostDownlinkEvent(&ev);
}

/* --------------------------------------------------------------------------
 * Init: replace DHT11 + PWM fan with the RS-485 driver. The driver's own bus
 * task (started inside hisense_init) is the reactive transactional master --
 * it does the DevType/0x1E link handshake, the 1Hz keepalive, and the status
 * poll itself (see hisense_rs485.cpp / reverse-engineering/docs/09). No
 * separate poll task here: a free-running poller is exactly what the A/C
 * ignored.
 * ------------------------------------------------------------------------ */
/* --------------------------------------------------------------------------
 * ElectricalPowerMeasurement (0x0090) on ep1 -> HA-native Watts sensor. This is
 * a Delegate/Instance-based cluster (values served through the delegate, not
 * ember RAM), fed from the RS-485 current/voltage bytes each status poll via
 * power_estimate.h. Energy (kWh) is derived in HA by Riemann-summing the power
 * sensor. Feature/optional-attr set matches the SDK energy example (the delegate
 * returns null for the attrs we don't feed). (docs/09)
 * ------------------------------------------------------------------------ */
namespace EPM = chip::app::Clusters::ElectricalPowerMeasurement;
static std::unique_ptr<EPM::ElectricalPowerMeasurementDelegate> s_epm_delegate;
static std::unique_ptr<EPM::ElectricalPowerMeasurementInstance> s_epm_instance;

static void matter_power_meter_init(chip::EndpointId ep)
{
    s_epm_delegate = std::make_unique<EPM::ElectricalPowerMeasurementDelegate>();
    if (!s_epm_delegate) return;
    s_epm_instance = std::make_unique<EPM::ElectricalPowerMeasurementInstance>(
        ep, *s_epm_delegate,
        // I11: advertise only what the A/C actually measures. The delegate feeds
        // ActivePower (mandatory), Voltage and ActiveCurrent — nothing else. Dropping
        // kHarmonics/kPowerQuality also drops their feature-mandatory attributes
        // (HarmonicCurrents, PowerFactor, ...) which we can't populate, so HA no longer
        // shows a wall of "Unknown" diagnostic entities. Single-phase AC only.
        chip::BitMask<EPM::Feature, uint32_t>(EPM::Feature::kAlternatingCurrent),
        chip::BitMask<EPM::OptionalAttributes, uint32_t>(
            EPM::OptionalAttributes::kOptionalAttributeRanges,
            EPM::OptionalAttributes::kOptionalAttributeVoltage,
            EPM::OptionalAttributes::kOptionalAttributeActiveCurrent));
    if (!s_epm_instance) { s_epm_delegate.reset(); return; }
    if (s_epm_instance->Init() != CHIP_NO_ERROR) {
        s_epm_instance.reset();
        s_epm_delegate.reset();
        ChipLogError(DeviceLayer, "EPM instance Init failed");
    }
}

/* Feed the power/voltage/current from a fresh status frame (called on downlink). */
static void matter_power_meter_update(const HisenseState &st)
{
    if (!s_epm_delegate) return;
    int64_t p_mw = st.power_on ? hisense_active_power_mw(st.current_raw) : 0;
    int64_t i_ma = st.power_on ? hisense_active_current_ma(st.current_raw, st.voltage_raw) : 0;
    s_epm_delegate->SetActivePower(chip::app::DataModel::MakeNullable(p_mw));
    s_epm_delegate->SetVoltage(chip::app::DataModel::MakeNullable(hisense_voltage_mv(st.voltage_raw)));
    s_epm_delegate->SetActiveCurrent(chip::app::DataModel::MakeNullable(i_ma));
}

/* --------------------------------------------------------------------------
 * Recommission ("77"): the A/C asked us to re-provision (bus-task callback; see
 * hisense_rs485.h + reverse-engineering/docs/02). Rather than wipe immediately,
 * we OPEN a commissioning window and keep the current fabric so Home Assistant
 * stays connected while a new controller pairs -- and only swap once it does:
 *   - "77"             -> snapshot current fabric(s), OpenBasicCommissioningWindow,
 *                         arm an expiry timer.
 *   - a new fabric joins -> delete the snapshotted old fabric(s) (swap completed).
 *   - window expires     -> revert (keep the old fabric) + tell the A/C to leave "77".
 * The window/timer/fabric-table calls must run in Matter context, so the bus-task
 * callback defers via PlatformMgr().ScheduleWork.
 * ------------------------------------------------------------------------ */
static const uint32_t     kRecommissionWindowSec = 180;
static bool               s_recommission_pending = false;
static chip::FabricIndex  s_old_fabrics[16];
static uint8_t            s_old_fabric_count     = 0;

static void recommission_timeout(chip::System::Layer *, void *);

// FabricTable delegate: a NEW fabric committing while our window is open means the
// re-pair succeeded -> drop the old fabric(s) and stand down.
class RecommissionFabricDelegate : public chip::FabricTable::Delegate
{
public:
    void OnFabricCommitted(const chip::FabricTable &, chip::FabricIndex newIndex) override
    {
        if (!s_recommission_pending) return;
        for (uint8_t i = 0; i < s_old_fabric_count; i++) {
            if (s_old_fabrics[i] == newIndex) return;   // not a newly-added fabric
        }
        ChipLogProgress(DeviceLayer, "recommission: new fabric %u joined -> deleting %u old fabric(s)",
                        newIndex, s_old_fabric_count);
        s_recommission_pending = false;
        chip::DeviceLayer::SystemLayer().CancelTimer(recommission_timeout, nullptr);
        for (uint8_t i = 0; i < s_old_fabric_count; i++) {
            chip::Server::GetInstance().GetFabricTable().Delete(s_old_fabrics[i]);
        }
        s_old_fabric_count = 0;
        hisense_set_provisioning(false);   // paired on the new fabric -> clear "77"
    }
};
static RecommissionFabricDelegate s_recommission_delegate;

// Window expired with no new pairing -> keep the old fabric, tell the A/C to exit "77".
static void recommission_timeout(chip::System::Layer *, void *)
{
    if (!s_recommission_pending) return;
    s_recommission_pending = false;
    s_old_fabric_count     = 0;
    ChipLogProgress(DeviceLayer, "recommission: window expired, no new pairing -> revert + signal A/C out of 77");
    hisense_send_exit_77();
}

// Matter-context entry (via ScheduleWork): snapshot fabrics + open the window + arm the timer.
static void recommission_open_window(intptr_t)
{
    if (s_recommission_pending) return;   // a window is already open
    s_old_fabric_count = 0;
    for (auto it = chip::Server::GetInstance().GetFabricTable().begin();
         it != chip::Server::GetInstance().GetFabricTable().end(); ++it) {
        if (s_old_fabric_count < (uint8_t)(sizeof(s_old_fabrics) / sizeof(s_old_fabrics[0]))) {
            s_old_fabrics[s_old_fabric_count++] = it->GetFabricIndex();
        }
    }
    CHIP_ERROR err = chip::Server::GetInstance().GetCommissioningWindowManager()
                         .OpenBasicCommissioningWindow(chip::System::Clock::Seconds32(kRecommissionWindowSec));
    if (err != CHIP_NO_ERROR) {
        ChipLogError(DeviceLayer, "recommission: OpenBasicCommissioningWindow failed: %" CHIP_ERROR_FORMAT, err.Format());
        s_old_fabric_count = 0;
        return;
    }
    // Matter spec: a node commissioned to a fabric MUST stop BLE advertising (BLE is
    // out-of-box/uncommissioned only; operational + re-pair discovery is IP/DNS-SD,
    // _matterc._udp). This unit is already on Wi-Fi, so re-pair the on-network way and
    // suppress the CHIPoBLE advertisement the window would otherwise (re)start -- on the
    // single-radio RTL8710C that BLE window starved Wi-Fi (coex) and dropped weak-signal
    // units. See recommission-77-mode notes. The _matterc._udp advert stays up, so a
    // commissioner still pairs over IP with the original passcode.
    // Only suppress BLE when an operational IP path actually exists. If Wi-Fi is down
    // (e.g. a corrupt/rotated stored profile), BLE is the ONLY transport left that can
    // reach the device to push new creds -- killing it here would make the very "77"
    // recovery this window exists for impossible. (Audit: unconditional suppress = lockout.)
    if (chip::DeviceLayer::ConnectivityMgr().IsWiFiStationConnected()) {
        err = chip::DeviceLayer::ConnectivityMgr().SetBLEAdvertisingEnabled(false);
        if (err != CHIP_NO_ERROR) {
            ChipLogError(DeviceLayer, "recommission: SetBLEAdvertisingEnabled(false) failed: %" CHIP_ERROR_FORMAT,
                         err.Format());
        }
    } else {
        ChipLogProgress(DeviceLayer, "recommission: Wi-Fi down -> keeping BLE up so creds can be pushed");
    }
    s_recommission_pending = true;
    chip::DeviceLayer::SystemLayer().StartTimer(chip::System::Clock::Seconds32(kRecommissionWindowSec),
                                                recommission_timeout, nullptr);
    hisense_set_provisioning(true);   // report prov=1 -> A/C lights "77" while the window is open
    ChipLogProgress(DeviceLayer, "recommission: window open %lus, snapshot %u old fabric(s)",
                    (unsigned long) kRecommissionWindowSec, s_old_fabric_count);
}

// Driver "77" callback (bus-task context) -> defer the real work to Matter context.
static void matter_driver_on_recommission(uint8_t reason)
{
    ChipLogProgress(DeviceLayer, "A/C requested recommission (\"77\"), payload[4]=0x%02x", reason);
    chip::DeviceLayer::PlatformMgr().ScheduleWork(recommission_open_window, 0);
}

// Driver feature-flags callback (bus-task context): log the A/C's 0x66/40 ProductType
// capability/state flags each time they're parsed. Not surfaced to HA (capability flags
// are static per model); available in-driver via hisense_get_features().
static void matter_driver_on_features(const HisenseFeatures *f)
{
    ChipLogProgress(DeviceLayer,
        "A/C features (0x66/40): cool_heat=%d ai=%d inf_fan=%d eco=%d mute=%d swing8=%d "
        "swing_follow=%d display=%d dr=%d humidity=%d purify=%d 8heat=%d",
        f->cool_heat, f->ai, f->infinite_fan, f->power_save, f->fan_mute, f->swing_dir_8,
        f->swing_follow, f->power_display, f->demand_resp, f->humidity, f->purify, f->heat_8c);
    if (f->ext_valid) {
        ChipLogProgress(DeviceLayer,
            "A/C features (ext, reply %uB): q_display=%d enable_8heat=%d trans_102_64=%d",
            (unsigned)f->reply_len, f->q_display, f->enable_8heat, f->trans_102_64);
    } else {
        ChipLogProgress(DeviceLayer,
            "A/C features (ext): unknown -- reply %uB, need >39B", (unsigned)f->reply_len);
    }
}

// Driver link-health callback (#56, bus-task context) -> latch the state and post one
// downlink event so the CHIP-task handler nulls (on lost) or re-reads (on restored)
// the liveness attributes. Reuses the existing downlink status event/handler.
static void matter_driver_on_link(bool up)
{
    s_link_lost = !up;
    ChipLogProgress(DeviceLayer, "A/C RS-485 link %s", up ? "restored" : "lost (bus silent)");
    AppEvent ev;
    ev.Type     = AppEvent::kEventType_Downlink_Aircon_Status;
    ev.mHandler = matter_driver_downlink_update_handler;
    PostDownlinkEvent(&ev);
}

// Give each endpoint a UserLabel with key "ha_entitylabel". Home Assistant's Matter
// integration uses that label (for our VID 0xFFF1 + PID 0x8001) as the entity-name
// postfix, so the otherwise-identical Temperature/Switch entities become
// distinguishable (e.g. "<device> Coil" vs "<device> Outdoor"). Persisted in the
// DeviceInfoProvider KVS; SetUserLabelList replaces the list, so re-running is idempotent.
static void set_ha_entity_label(chip::EndpointId ep, const char *name)
{
    auto *provider = chip::DeviceLayer::GetDeviceInfoProvider();
    if (provider == nullptr) {
        return;
    }
    chip::DeviceLayer::DeviceInfoProvider::UserLabelType label;
    label.label = chip::CharSpan::fromCharString("ha_entitylabel");
    label.value = chip::CharSpan::fromCharString(name);
    chip::DeviceLayer::AttributeList<chip::DeviceLayer::DeviceInfoProvider::UserLabelType,
                                     chip::DeviceLayer::kMaxUserLabelListLength> list;
    if (list.add(label) != CHIP_NO_ERROR) {
        return;
    }
    CHIP_ERROR err = provider->SetUserLabelList(ep, list);
    if (err != CHIP_NO_ERROR) {
        ChipLogError(DeviceLayer, "SetUserLabelList ep%u failed: %" CHIP_ERROR_FORMAT, ep, err.Format());
    }
}

CHIP_ERROR matter_driver_room_aircon_init(void)
{
    CHIP_ERROR err = CHIP_NO_ERROR;

    aircon.SetEp(kAirconEp);

    if (hisense_init(hisense_on_status) != pdPASS) {
        ChipLogError(DeviceLayer, "hisense_init failed!");
        return CHIP_ERROR_INTERNAL;
    }
    chip::Server::GetInstance().GetFabricTable().AddFabricDelegate(&s_recommission_delegate);  // F1: swap fabric on new pairing
    hisense_set_recommission_cb(matter_driver_on_recommission);   // F1: remote "77" -> open window
    hisense_set_link_cb(matter_driver_on_link);                   // #56: bus silence -> mark unavailable
    hisense_set_features_cb(matter_driver_on_features);           // 0x66/40 feature flags -> device log
    hisense_diag_console_start();                                 // #23: :2323 console, DEBUG flavour only
    hisense_breakglass_start();                                   // #61: OTA trigger off the Matter path (BOTH flavours)

    // HA entity labels (UserLabel key "ha_entitylabel") so the same-type entities are
    // distinguishable in Home Assistant. Requires the UserLabel cluster (0x0041) enabled
    // on each endpoint in the .zap.
    set_ha_entity_label(kAirconEp,      "Climate");
    set_ha_entity_label(kOutdoorTempEp, "Outdoor");
    set_ha_entity_label(kEcoEp,         "Eco");
    set_ha_entity_label(kMuteEp,        "Quiet");
    set_ha_entity_label(kTurboEp,       "Turbo");
    set_ha_entity_label(kSleepEp,       "Sleep");
    set_ha_entity_label(kHeatRelayEp,   "Aux Heat");
    set_ha_entity_label(kCoilTempEp,    "Coil");
    set_ha_entity_label(kDisplayEp,     "Display");

    /* ep9 Display starts ON (#33). The panel is lit by default and the A/C reports no
     * display state, so a switch that boots "off" is wrong from the first second and
     * nothing can ever correct it. Worse, Matter skips both attribute-change callbacks
     * when a write does not change the value (emAfWriteAttribute returns early on
     * !valueChanging), so pressing "off" on an already-false attribute ran no handler and
     * put NO frame on the bus: the user saw a lit panel, pressed off, and nothing happened.
     * Seeding true makes that first press a real transition.
     *
     * Not a general fix: change the display from the IR remote and the attribute drifts
     * again, with no read-back to recover. Set here rather than in the .zap so the data
     * model does not need a GUI round-trip for a one-bit default. */
    {
        bool display_default = true;
        emberAfWriteAttribute(kDisplayEp, chip::app::Clusters::OnOff::Id,
                              chip::app::Clusters::OnOff::Attributes::OnOff::Id,
                              (uint8_t *) &display_default, kHisenseTypeBool);
    }

    matter_power_meter_init(kAirconEp);

    // Register the Sleep-profile ModeSelect supported-modes (Off/General/Old/Young/Kids on ep6).
    static chip::app::Clusters::ModeSelect::AmebaSupportedModesManager s_sleep_modes_mgr;
    chip::app::Clusters::ModeSelect::setSupportedModesManager(&s_sleep_modes_mgr);

    ChipLogProgress(DeviceLayer, "Hisense RS-485 aircon driver initialized on ep%d", kAirconEp);
    return err;
}

/* --------------------------------------------------------------------------
 * Identify (unchanged)
 * ------------------------------------------------------------------------ */
void matter_driver_on_identify_start(Identify * identify)   { ChipLogProgress(Zcl, "OnIdentifyStart"); }
void matter_driver_on_identify_stop(Identify * identify)    { ChipLogProgress(Zcl, "OnIdentifyStop"); }

void matter_driver_on_trigger_effect(Identify * identify)
{
    switch (identify->mCurrentEffectIdentifier)
    {
    case Clusters::Identify::EffectIdentifierEnum::kBlink:
    case Clusters::Identify::EffectIdentifierEnum::kBreathe:
    case Clusters::Identify::EffectIdentifierEnum::kOkay:
    case Clusters::Identify::EffectIdentifierEnum::kChannelChange:
        ChipLogProgress(Zcl, "Identify effect");
        break;
    default:
        ChipLogProgress(Zcl, "No identifier effect");
        return;
    }
}

/* --------------------------------------------------------------------------
 * Shared special-mode command builders. Eco/Turbo/Mute are each driven from
 * two places -- the OnOff-cluster special-mode-switch endpoints (ep4/5/6) and
 * the raw 0xFFF1FC00 manufacturer-cluster attrs (0x0000/0x0001/0x0002) -- and
 * both build the identical Hisense frame from a plain on/off bool. Factored
 * here so the two switch statements stop duplicating the frame logic; each
 * call site keeps its own echo-guard (it needs to `break` its own case).
 * ------------------------------------------------------------------------ */
static void hisense_apply_eco(bool on)
{
    if (on) {
        s_cmd.feature = HISENSE_FEATURE_ECO;
        hisense_flush_command();
    } else {
        // Clearing eco needs the explicit eco-off byte (0x10), NOT
        // FEATURE_NONE (0x04, which is turbo-clear). Send once, then
        // return the shadow to neutral so later combined frames don't
        // keep re-asserting eco-off.  (docs/07 Tier-1 #1 / docs/05 P3c)
        s_cmd.feature = HISENSE_FEATURE_ECO_OFF; // 0x10, NOT NONE (0x04=turbo-clear)
        hisense_flush_command();
        s_cmd.feature = HISENSE_FEATURE_NONE;
    }
}

static void hisense_apply_turbo(bool on)
{
    s_cmd.feature = on ? HISENSE_FEATURE_TURBO : HISENSE_FEATURE_NONE;
    hisense_flush_command();
}

static void hisense_apply_mute(bool on)
{
    uint8_t f[HISENSE_CMD_FRAME_LEN + 2];
    size_t n = hisense_build_mute_frame(on, f, sizeof(f));
    if (n) hisense_send_frame(f, n);
}

/* Panel display on/off (#19 parity with the esp32 build). `display` rides the
 * combined command frame (@20: 0xC0 on / 0x40 off / 0x00 leave-alone); flush rebuilds
 * + sends it. Write-only -- the A/C reports no display state, so there's no status echo.
 *
 * ONE-SHOT (#52): reset to NOCHANGE afterwards. The field is packed into EVERY combined
 * command, so a latched ON/OFF would re-assert the panel on every later mode/setpoint/fan
 * change and fight the user's remote. */
static void hisense_apply_display(bool on)
{
    s_cmd.display = on ? HISENSE_DISPLAY_ON : HISENSE_DISPLAY_OFF;
    hisense_flush_command();
    s_cmd.display = HISENSE_DISPLAY_NOCHANGE;
}

/* Sleep profile (0=off, 1..4 = General/Old/Young/Kids). Driven from two places --
 * the raw 0xFFF1FC00 SleepProfile attr (0x0003) and the ep6 ModeSelect CurrentMode
 * -- and both build the identical frame, so it lives here; each call site keeps its
 * own echo guard. */
static void hisense_apply_sleep(uint8_t profile)
{
    uint8_t f[HISENSE_CMD_FRAME_LEN + 2];
    size_t n = hisense_build_sleep_frame(profile, f, sizeof(f));
    if (n) hisense_send_frame(f, n);
}

/* --------------------------------------------------------------------------
 * #78: manual HTTPS-OTA backup path (break-glass). On a magic Identify write
 * (the Identify case in the uplink handler) the device fetches a firmware image
 * over plain HTTP (TCP) from the Pi file server and applies it to the idle A/B
 * OTA slot, then reboots. TCP's sliding window / fast retransmit is far more
 * robust than Matter's stop-and-wait BDX (one ~1KB block per UDP+MRP round-trip)
 * on a marginal link, so this completes where the standard Matter OTA stalls
 * (error 11, see #76). Matter OTA stays primary; this is a manual escape hatch.
 *
 * ROOT-CAUSE FIX (#78): the earlier version called the SDK's generic
 * http_update_ota(), which is the LEGACY AmebaZ2 OTA writer -- it reserves a
 * 32-byte signature and writes the slot incompatibly with THIS Matter build's
 * OTA scheme (matter_ota.c reserves a 40-byte header and signs via
 * update_ota_signature(matter_ota_header,...)). Feeding firmware_is.bin to
 * http_update_ota wrote a slot the bootloader rejects (download OK, no boot --
 * confirmed on hardware, node 14). This path instead streams the HTTP body
 * through the SAME matter_ota_* functions the working Matter (BDX) OTA uses
 * (mirror of connectedhomeip AmebaOTAImageProcessor::{PrepareDownload,Process
 * Block,Finalize,Apply}) -- identical slot/header/signature handling, just
 * sourced from a TCP socket instead of BDX.
 *
 * Serve the raw build output firmware_is.bin at HISENSE_OTA_RESOURCE (NOT the
 * .ota wrapper -- matter_ota_* expects the ameba payload, and the Matter/CSA TLV
 * envelope is not present here). Its FWHS serial = SERIAL_BASE + softwareVersion
 * is set by `ota-release.sh build`, so the A/B bootloader picks the higher-serial
 * slot (docs/10 §11). On the Pi: `cp .../firmware_is.bin <docroot>/rac-ota.bin`.
 * ------------------------------------------------------------------------ */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
extern "C" {
    // The Matter OTA image writer (component/.../matter/common/port/matter_ota.c; declared in
    // matter_ota.h, transitively in scope). Types MUST match matter_ota.h exactly (uint32_t is
    // 'unsigned long' on this target). All return OTA_SUCCESS == 1 (see AmebaUtils::MapFlashError).
    void    matter_ota_prepare_partition(void);
    uint8_t matter_ota_get_total_header_size(void);
    uint8_t matter_ota_get_current_header_size(void);
    int8_t  matter_ota_store_header(uint8_t *data, uint32_t size);
    int8_t  matter_ota_flash_burst_write(uint8_t *data, uint32_t size);
    int8_t  matter_ota_flush_last(void);
    int8_t  matter_ota_update_signature(void);
    void    matter_ota_platform_reset(void);         // commit + reboot into the new slot
    // Realtek SDK socket connect helper (ota_8710c.c) + lwIP BSD socket I/O.
    int  update_ota_http_connect_server(int server_socket, char *host, int port);
    int  lwip_read(int s, void *mem, size_t len);
    int  lwip_write(int s, const void *dataptr, size_t size);
    int  lwip_close(int s);
}
#define HISENSE_OTA_OK 1   // matter_ota.h: OTA_SUCCESS = 1

// Deployment-specific server address. This is a PUBLIC repo, so the real address must never be
// committed here: `ota-release.sh build` generates hisense_ota_config.h into the SDK example dir
// from OTA_HTTP_* in ota-release.env (gitignored), and it is picked up below when present. A build
// without it falls back to the placeholder, which is inert rather than someone else's host.
// Overriding by -DHISENSE_OTA_HOST=... still works.
#if defined(__has_include)
#  if __has_include("hisense_ota_config.h")
#    include "hisense_ota_config.h"
#  endif
#endif
#ifndef HISENSE_OTA_HOST
#define HISENSE_OTA_HOST     "192.168.1.50"
#endif
#ifndef HISENSE_OTA_PORT
#define HISENSE_OTA_PORT     8070
#endif
#ifndef HISENSE_OTA_RESOURCE
#define HISENSE_OTA_RESOURCE "/rac-ota.bin"          // = the build's firmware_is.bin (correct FWHS serial)
#endif

// Feed one HTTP body chunk to the Matter OTA writer, splitting off the 40-byte
// ameba header first (mirror of AmebaOTAImageProcessor::ProcessBlock). Returns 0 / -1.
static int hisense_ota_feed(unsigned char *data, unsigned int len)
{
    unsigned int off = 0;
    unsigned char remain = (unsigned char)(matter_ota_get_total_header_size() -
                                           matter_ota_get_current_header_size());
    if (remain > 0) {
        unsigned int take = (len >= remain) ? remain : len;
        if (matter_ota_store_header(data, take) != HISENSE_OTA_OK) return -1;
        off += take;
    }
    if (off < len) {
        if (matter_ota_flash_burst_write(data + off, len - off) != HISENSE_OTA_OK) return -1;
    }
    return 0;
}

static void hisense_https_ota_task(void *arg)
{
    (void) arg;
    ChipLogProgress(DeviceLayer, "HTTPS-OTA: connecting %s:%d%s (matter_ota writer; break-glass)",
                    HISENSE_OTA_HOST, HISENSE_OTA_PORT, HISENSE_OTA_RESOURCE);
    int sock = update_ota_http_connect_server(-1, (char *) HISENSE_OTA_HOST, HISENSE_OTA_PORT);
    if (sock < 0) { ChipLogError(DeviceLayer, "HTTPS-OTA: connect failed"); vTaskDelete(NULL); return; }

    char req[192];
    int rn = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                      HISENSE_OTA_RESOURCE, HISENSE_OTA_HOST);
    if (lwip_write(sock, req, (unsigned int) rn) < 0) {
        ChipLogError(DeviceLayer, "HTTPS-OTA: send request failed");
        lwip_close(sock); vTaskDelete(NULL); return;
    }

    // Parse response headers line-by-line; capture Content-Length; stop at the blank line.
    // buf/line are static (NOT on the task stack): a ~1.5KB stack buffer overflowed the 8KB task
    // stack (the old http_update_ota heap-allocated its buffers) and the task died before connect.
    long content_length = -1;
    static char line[160];
    bool bad = false;
    while (!bad) {
        int li = 0; char c;
        while (1) {
            if (lwip_read(sock, &c, 1) != 1) { bad = true; break; }
            if (c == '\n') break;
            if (c != '\r' && li < (int) sizeof(line) - 1) line[li++] = c;
        }
        if (bad) break;
        line[li] = 0;
        if (li == 0) break;                                    // blank line -> end of headers
        if (strncmp(line, "Content-Length:", 15) == 0 || strncmp(line, "content-length:", 15) == 0)
            content_length = strtol(line + 15, NULL, 10);
    }
    if (bad) { ChipLogError(DeviceLayer, "HTTPS-OTA: bad HTTP response");
               lwip_close(sock); vTaskDelete(NULL); return; }

    // Stream the body through the Matter OTA writer (chunks < one 4KB sector).
    matter_ota_prepare_partition();
    static unsigned char buf[1460];   // static: keep off the 8KB task stack (see note above)
    long received = 0; int failed = 0;
    while (1) {
        unsigned int want = sizeof(buf);
        if (content_length >= 0) {
            long rem = content_length - received;
            if (rem <= 0) break;
            if ((long) want > rem) want = (unsigned int) rem;
        }
        int n = lwip_read(sock, buf, want);
        if (n <= 0) break;                                     // EOF (Connection: close) or error
        if (hisense_ota_feed(buf, (unsigned int) n) != 0) { failed = 1; break; }
        received += n;
    }
    lwip_close(sock);

    if (failed) {
        ChipLogError(DeviceLayer, "HTTPS-OTA: flash write failed at %ld bytes", received);
        vTaskDelete(NULL); return;
    }
    if (content_length >= 0 && received != content_length) {   // truncated -> do NOT commit the slot
        ChipLogError(DeviceLayer, "HTTPS-OTA: truncated (%ld/%ld) -- not committing", received, content_length);
        vTaskDelete(NULL); return;
    }
    if (matter_ota_flush_last() != HISENSE_OTA_OK) {
        ChipLogError(DeviceLayer, "HTTPS-OTA: flush_last failed"); vTaskDelete(NULL); return;
    }
    if (matter_ota_update_signature() != HISENSE_OTA_OK) {
        ChipLogError(DeviceLayer, "HTTPS-OTA: sign failed"); vTaskDelete(NULL); return;
    }
    ChipLogProgress(DeviceLayer, "HTTPS-OTA: image written+signed (%ld bytes) -> rebooting", received);
    matter_ota_platform_reset();                               // reboots into the new slot
    vTaskDelete(NULL);
}

static void hisense_trigger_https_ota(void)
{
    // Off the Matter task: the socket transfer + flash writes block. matter_ota buffers a full
    // 4KB sector in a file-scope static (not stack), so 2048 words (~8KB) of stack is ample.
    if (xTaskCreate(hisense_https_ota_task, "hisense_https_ota", 2048, NULL,
                    tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        ChipLogError(DeviceLayer, "HTTPS-OTA: task create failed");
    }
}

/* --------------------------------------------------------------------------
 * #61: break-glass OTA trigger that does NOT ride the Matter layer.
 *
 * The Identify=88 trigger is unreachable exactly when it is needed: a controller that has
 * marked the node unavailable refuses the write before it reaches the device ("Node <id> is
 * not (yet) available"). That happened after an OTA that broke Matter subscriptions -- the
 * device was healthy on the network and answering its console, but could not be re-flashed
 * over the air, because the only escape hatch sat on the transport that was broken.
 *
 * Deliberately NOT part of the :2323 diagnostic console: that console is DEBUG-flavour only
 * and unauthenticated by design, so a trigger living there would be absent from exactly the
 * images most likely to need it. This listener is compiled into BOTH flavours.
 *
 * Because it ships in release, it is authenticated and fails CLOSED: with no token configured
 * at build time the socket is never opened. A default token in a public repo would be
 * equivalent to no authentication at all, so there is no default.
 *
 * Set HISENSE_BREAKGLASS_TOKEN via ota-release.env (gitignored) -> -D on the build.
 * ------------------------------------------------------------------------ */
#ifdef HISENSE_BREAKGLASS_TOKEN

#ifndef HISENSE_BREAKGLASS_PORT
#define HISENSE_BREAKGLASS_PORT 2324
#endif

// Length-checked, non-short-circuiting compare. The token is low-value and the attacker is
// already on the LAN, but an early-return memcmp leaks the matched prefix over repeated tries
// and costs nothing to avoid.
static bool hisense_token_ok(const char *got, size_t got_len)
{
    const char  *want     = HISENSE_BREAKGLASS_TOKEN;
    const size_t want_len = sizeof(HISENSE_BREAKGLASS_TOKEN) - 1;
    unsigned char diff = (unsigned char) (got_len ^ want_len);
    size_t i;

    if (got_len != want_len) return false;
    for (i = 0; i < want_len; i++) diff |= (unsigned char) (got[i] ^ want[i]);
    return diff == 0;
}

static void hisense_breakglass_task(void *arg)
{
    int srv = lwip_socket(AF_INET6, SOCK_STREAM, 0);
    struct sockaddr_in6 a;
    int one = 1;

    (void) arg;
    if (srv < 0) { ChipLogError(DeviceLayer, "break-glass: socket failed"); vTaskDelete(NULL); return; }
    lwip_setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    memset(&a, 0, sizeof(a));
    a.sin6_family = AF_INET6;
    a.sin6_port   = htons(HISENSE_BREAKGLASS_PORT);
    if (lwip_bind(srv, (struct sockaddr *) &a, sizeof(a)) != 0 || lwip_listen(srv, 1) != 0) {
        ChipLogError(DeviceLayer, "break-glass: bind/listen failed on %d", HISENSE_BREAKGLASS_PORT);
        lwip_close(srv);
        vTaskDelete(NULL);
        return;
    }
    ChipLogProgress(DeviceLayer, "break-glass listener up on :%d (#61)", HISENSE_BREAKGLASS_PORT);

    for (;;) {
        int cs = lwip_accept(srv, NULL, NULL);
        char buf[96];
        int  n;
        size_t len;

        if (cs < 0) continue;
        n = lwip_read(cs, buf, sizeof(buf) - 1);
        if (n <= 0) { lwip_close(cs); continue; }
        buf[n] = 0;

        len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n' || buf[len - 1] == ' ')) {
            buf[--len] = 0;
        }

        if (hisense_token_ok(buf, len)) {
            ChipLogProgress(DeviceLayer, "break-glass: token accepted, starting OTA fetch");
            static const char kOk[] =
                "ok: fetching; device reboots into the idle slot.\r\n"
                "verify the RUNNING version afterwards -- a failed apply\r\n"
                "looks like success until you check (docs/10).\r\n";
            lwip_write(cs, kOk, sizeof(kOk) - 1);
            lwip_close(cs);
            hisense_trigger_https_ota();
        } else {
            // No detail on failure, and no hint whether the length or the bytes were wrong.
            ChipLogError(DeviceLayer, "break-glass: rejected (bad token)");
            lwip_write(cs, "no\r\n", 4);
            lwip_close(cs);
        }
    }
}

static void hisense_breakglass_start(void)
{
    if (xTaskCreate(hisense_breakglass_task, "hisense_bg", 1024, NULL,
                    tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        ChipLogError(DeviceLayer, "break-glass: task create failed");
    }
}

#else  /* no token configured -> fail closed, and say so loudly at boot */
static void hisense_breakglass_start(void)
{
    ChipLogError(DeviceLayer,
                 "break-glass DISABLED: no HISENSE_BREAKGLASS_TOKEN at build time (#61). "
                 "Recovery depends on the Matter layer being healthy.");
}
#endif /* HISENSE_BREAKGLASS_TOKEN */

/* --------------------------------------------------------------------------
 * Uplink: Matter attribute write -> A/C command frame.
 * ------------------------------------------------------------------------ */
void matter_driver_uplink_update_handler(AppEvent *aEvent)
{
    chip::app::ConcreteAttributePath path = aEvent->path;

    chip::DeviceLayer::PlatformMgr().LockChipStack();

    // One consistent snapshot of the bus-task-written status for every echo guard
    // below, so a concurrent hisense_on_status write can't tear a read (#57).
    HisenseState st;
    taskENTER_CRITICAL();
    st = s_status;
    taskEXIT_CRITICAL();

    switch (path.mClusterId)
    {
    case Clusters::Thermostat::Id:
        if (path.mAttributeId == ThermAttr::SystemMode::Id)
        {
            uint8_t matter_mode = aEvent->value._u8;
            // Echo-suppression: the downlink handler writes SystemMode back from
            // the A/C's status; that internal write re-enters here via the async
            // uplink queue. If it already matches the A/C's reported state it is
            // our own echo -- re-commanding it is what made setpoint/mode bounce
            // (docs/07 HIL regression). Only act on a genuine change.
            if (st.valid) {
                uint8_t cur = st.power_on ? hisense_mode_to_matter(st.mode) : 0;
                if (matter_mode == cur) break;
            }
            if (matter_mode == 0) {           // Off
                hisense_send_power(false);
            } else {
                HisenseMode hm;
                if (matter_mode_to_hisense(matter_mode, &hm)) {
                    s_cmd.mode = hm;
                    hisense_send_power(true);  // ensure the unit is on
                    hisense_flush_command();
                }
            }
        }
        else if (path.mAttributeId == ThermAttr::OccupiedCoolingSetpoint::Id
              || path.mAttributeId == ThermAttr::OccupiedHeatingSetpoint::Id)
        {
            // Guard: no setpoint changes while the A/C is OFF -- select a mode to turn
            // it on first (a temp write on an off unit is a confusing no-op). (user req)
            if (st.valid && !st.power_on) break;
            // The A/C has a SINGLE setpoint; which Matter attribute is authoritative
            // depends on mode (HEAT -> heating, else cooling). Ignore the inactive
            // one, else Matter's deadband adjusting the *other* setpoint fires a
            // spurious command (HIL v6: a cool-mode heating-setpoint write commanded
            // 18C over our 20C). (docs/07 / docs/08)
            bool attr_is_heat = (path.mAttributeId == ThermAttr::OccupiedHeatingSetpoint::Id);
            bool mode_is_heat = (st.valid && st.mode == HISENSE_MODE_HEAT);
            if (st.valid && attr_is_heat != mode_is_heat) break;
            int16_t hundredths = aEvent->value._i16;
            int whole_c = matter_round_setpoint_c(hundredths);   // rounded, UNCLAMPED int -- never narrow to int8 before clamp (would sign-wrap and invert direction)
            // Echo-suppression: compare the raw whole-degree value BEFORE clamping
            // so an A/C setpoint outside 16..32 can't dodge the guard.
            if (st.valid && whole_c == st.setpoint_c) break;
            /* The A/C reads the setpoint byte in ITS OWN display unit, so a command built
             * while the panel is in Fahrenheit must carry Fahrenheit. Matter is always
             * Celsius, so convert here and tell the builder which unit it holds (that also
             * selects the 61..90 range check instead of 16..32). Confirmed on the esp32
             * bench: sending Celsius 23 to a panel in F made the A/C target 23 F. */
            {
                int8_t want_c = matter_clamp_setpoint_c(whole_c);
                bool   unit_f = s_status.valid && s_status.temp_unit_f;
                s_cmd.fahrenheit = unit_f;
                s_cmd.setpoint   = unit_f ? hisense_c_to_f(want_c) : want_c;
            }
            s_cmd.fahrenheit = false;
            hisense_flush_command();
        }
        break;

    case Clusters::FanControl::Id:
        // Guard: no fan/swing changes while the A/C is OFF (fan is meaningless with the
        // unit off; select a mode to turn it on first). (user request)
        if (st.valid && !st.power_on) break;
        if (path.mAttributeId == FanAttr::PercentSetting::Id)
        {
            // The SDK FanControl server mirrors PercentSetting<->SpeedSetting, so
            // one user fan change re-enters here twice. Skip if the resolved speed
            // already matches the shadow -> one frame per change, not two. (docs/08)
            HisenseFanSpeed nf = percent_to_hisense_fan(aEvent->value._u8);
            if (nf == s_cmd.fan) break;
            s_cmd.fan = nf;
            hisense_flush_command();
        }
        else if (path.mAttributeId == FanAttr::SpeedSetting::Id)
        {
            HisenseFanSpeed nf = speed_to_hisense_fan(aEvent->value._u8); // 1..6 discrete
            if (nf == s_cmd.fan) break;
            s_cmd.fan = nf;
            hisense_flush_command();
        }
        else if (path.mAttributeId == FanAttr::FanMode::Id)
        {
            // HA's fan card writes FanMode when the user picks a Low/Medium/High/Auto
            // preset. The SDK FanControl server does NOT mirror FanMode->PercentSetting
            // here, so without this branch the preset was silently dropped. (v17 fix)
            HisenseFanSpeed nf = fanmode_to_hisense_fan(aEvent->value._u8);
            if (nf == s_cmd.fan) break;   // echo / no-op
            s_cmd.fan = nf;
            hisense_flush_command();
        }
        else if (path.mAttributeId == FanAttr::RockSetting::Id)
        {
            bool v, h;
            rock_to_swing(aEvent->value._u8, &v, &h);
            (void)h;  // no H-swing motor on this unit (RockSupport=0x02) -- vertical only
            if (st.valid && v == st.vswing_on) break;  // echo
            s_cmd.vswing = v ? HISENSE_SWING_SWING : HISENSE_SWING_OFF;
            s_cmd.hswing = HISENSE_SWING_OFF;
            hisense_flush_command();
        }
        break;

    case Clusters::OnOff::Id:
        if (path.mAttributeId == Clusters::OnOff::Attributes::OnOff::Id)
        {
            bool on = (aEvent->value._u8 == 1);
            // The downlink syncs every OnOff attr from status; that write re-enters
            // here -- each case echo-guards so it never re-commands its own readback.
            switch (path.mEndpointId)
            {
            case kAirconEp: // ep1: A/C power
                if (st.valid && on == st.power_on) break;
                hisense_send_power(on);
                break;
            case kEcoEp:    // ep3: Eco (byte33 0x30 on / explicit 0x10 off)
                if (st.valid && on == st.eco_on) break;
                hisense_apply_eco(on);
                break;
            case kTurboEp:  // ep5: Turbo (byte33 0x0C on / 0x04 off)
                if (st.valid && on == st.turbo_on) break;
                hisense_apply_turbo(on);
                break;
            case kMuteEp: { // ep4: Quiet/Mute (byte35 0x30 on / 0x10 off)
                if (st.valid && on == st.mute_on) break;
                hisense_apply_mute(on);
                break;
            }
            case kDisplayEp: // ep9: panel display (write-only; A/C reports no display state -> no echo guard)
                hisense_apply_display(on);
                break;
            // (v20) kSleepEp OnOff removed -- Sleep is now the ModeSelect "Sleep Profile"
            // on ep6 (Off/General/Old/Young/Kids), handled in the ModeSelect case below.
            default:
                break;
            }
        }
        break;

    case HisenseCl::Id: // Hisense Aircon manufacturer cluster (ember-only)
        switch (path.mAttributeId)
        {
        case HisenseAttr::Eco::Id: // Eco
            // Echo guard: the downlink handler writes these mfg attrs back from
            // status every poll; without this, a changed value would loop like the
            // setpoint did (re-send a frame every cycle). (docs/08 CRITICAL)
            if (st.valid && (bool)aEvent->value._u8 == st.eco_on) break;
            hisense_apply_eco(aEvent->value._u8);
            break;
        case HisenseAttr::Turbo::Id: // Turbo
            if (st.valid && (bool)aEvent->value._u8 == st.turbo_on) break;  // echo guard
            hisense_apply_turbo(aEvent->value._u8);
            break;
        case HisenseAttr::Mute::Id: { // Mute
            if (st.valid && (bool)aEvent->value._u8 == st.mute_on) break;  // echo guard
            hisense_apply_mute(aEvent->value._u8);
            break;
        }
        case HisenseAttr::SleepProfile::Id: { // SleepProfile (0..4)
            if (st.valid && aEvent->value._u8 == (uint8_t)(st.sleep_raw / 2)) break;  // echo guard
            hisense_apply_sleep(aEvent->value._u8);
            break;
        }
        default:
            break;
        }
        break;

    case Clusters::ModeSelect::Id:
        // Sleep-profile ModeSelect on ep6. The ModeSelect `mode` value IS the Hisense sleep
        // profile index (0=off, 1..4 = General/Old/Young/Kids); route it straight to the same
        // sleep-frame builder the mfg SleepProfile attr uses. The server sets CurrentMode from
        // a ChangeToMode command, which lands here via MatterPostAttributeChangeCallback.
        if (path.mAttributeId == Clusters::ModeSelect::Attributes::CurrentMode::Id)
        {
            if (st.valid && aEvent->value._u8 == (uint8_t)(st.sleep_raw / 2)) break;  // echo guard
            hisense_apply_sleep(aEvent->value._u8);
        }
        break;

    case Clusters::Identify::Id:
        // #78: manual HTTPS-OTA break-glass trigger. Writing Identify.IdentifyTime = 88 on ep1
        // pulls firmware over HTTP (TCP) from the Pi file server and applies it via the SDK's
        // http_update_ota (idle A/B slot) -- the escape hatch when the Matter BDX OTA won't
        // complete on a marginal link (#76/#78). Mirrors the ESP32 Identify=88 trigger. (77 is
        // the A/C-initiated recommission, surfaced via hisense_set_recommission_cb, not here.)
        if (path.mAttributeId == Clusters::Identify::Attributes::IdentifyTime::Id
            && aEvent->value._u16 == 88)
        {
            ChipLogProgress(DeviceLayer, "manual HTTPS-OTA trigger (Identify=88)");
            hisense_trigger_https_ota();
        }
        break;
    default:
        break;
    }

    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
}

/* --------------------------------------------------------------------------
 * Downlink: apply a snapshotted A/C status to the Matter attribute store.
 * ------------------------------------------------------------------------ */
void matter_driver_downlink_update_handler(AppEvent *aEvent)
{
    chip::DeviceLayer::PlatformMgr().LockChipStack();

    switch (aEvent->Type)
    {
    case AppEvent::kEventType_Downlink_Aircon_Status:
    {
        // #56: RS-485 bus silent -> null the liveness attributes so HA renders the
        // entities unavailable instead of holding stale values. Runs before the
        // fresh-status gate (the link-lost event carries no new status); the next good
        // poll repopulates everything below. LocalTemperature is HA's documented link-
        // health signal; MeasuredValue + EPM extend it to the outdoor-temp/power sensors.
        if (s_link_lost) {
            ThermAttr::LocalTemperature::SetNull(kAirconEp);
            TempMeasAttr::MeasuredValue::SetNull(kOutdoorTempEp);
            TempMeasAttr::MeasuredValue::SetNull(kCoilTempEp);
            if (s_epm_delegate) {
                s_epm_delegate->SetActivePower(chip::app::DataModel::NullNullable);
                s_epm_delegate->SetVoltage(chip::app::DataModel::NullNullable);
                s_epm_delegate->SetActiveCurrent(chip::app::DataModel::NullNullable);
            }
            break;
        }

        // Atomic snapshot of the bus-task-written status + fresh flag (#57).
        bool fresh;
        HisenseState st;
        taskENTER_CRITICAL();
        fresh          = s_status_fresh;
        st             = s_status;
        s_status_fresh = false;
        taskEXIT_CRITICAL();
        if (!fresh) break;

        // Keep the uplink command shadow in sync with the A/C's ACTUAL state,
        // so a later single-attribute write rebuilds the combined frame from
        // reality instead of a stale shadow (which would clobber an out-of-band
        // change from the IR remote / the unit's own logic, and force COOL/24
        // after a reboot). Held off for a few polls after our own commands so an
        // in-flight command isn't reverted by a pre-command status frame.
        // (docs/07 Tier-1 #2)
        if (chip::System::SystemClock().GetMonotonicTimestamp() < s_sync_hold_until) {
            // still settling after our own command -- don't trust a (possibly
            // pre-command) status frame yet (#61)
        } else {
            s_cmd.mode     = st.mode;
            // NEVER copy an out-of-range setpoint into the shadow. The A/C legitimately
            // reports them (this unit answers ac_8heat=1, and the bench saw it accept and
            // hold 5 C), and an out-of-range shadow makes hisense_build_command() return 0
            // for EVERY later command -- mode, fan and swing included -- silently killing
            // all combined-frame control until a reboot. Keeping the last good value
            // degrades gracefully: the shadow is only a base for the next command.
            if (hisense_setpoint_in_range(st.setpoint_c, s_cmd.fahrenheit)) {
                // Hold the shadow setpoint in the A/C's DISPLAY unit: that is what goes on
                // the wire. st.setpoint_c is always Celsius (the parser converts).
                s_cmd.fahrenheit = st.temp_unit_f;
                s_cmd.setpoint   = st.temp_unit_f ? hisense_c_to_f(st.setpoint_c)
                                                  : st.setpoint_c;
            } else {
                ChipLogError(DeviceLayer,
                             "status setpoint %d out of range -- keeping shadow at %d "
                             "(copying it would drop every later command)",
                             (int) st.setpoint_c, (int) s_cmd.setpoint);
            }
            // Keep the previous fan on an unknown/garbled raw instead of clobbering to
            // AUTO (#59): hisense_fan_raw_to_cmd returns NOCHANGE for an unrecognized raw.
            {
                HisenseFanSpeed sf = hisense_fan_raw_to_cmd(st.fan_raw);
                if (sf != HISENSE_FAN_NOCHANGE) s_cmd.fan = sf;
            }
            s_cmd.vswing   = st.vswing_on ? HISENSE_SWING_SWING : HISENSE_SWING_OFF;
            s_cmd.hswing   = HISENSE_SWING_OFF;  // H-swing motor absent on this unit
            s_cmd.feature  = st.eco_on   ? HISENSE_FEATURE_ECO
                           : st.turbo_on ? HISENSE_FEATURE_TURBO
                                         : HISENSE_FEATURE_NONE;
        }

        // Thermostat LocalTemperature (int16, 0.01 C)
        ThermAttr::LocalTemperature::Set(kAirconEp, (int16_t)(st.indoor_temp_c * 100));

        // Thermostat SystemMode (Off if the unit reports powered down)
        uint8_t mm = st.power_on ? hisense_mode_to_matter(st.mode) : 0;
        ThermAttr::SystemMode::Set(kAirconEp, (SystemModeEnum)mm);

        // OnOff MUST track the A/C's power state: HA's Matter climate forces
        // hvac_mode=OFF whenever the OnOff cluster reads False, *regardless* of
        // SystemMode (climate.py:417) -- so without this the entity always shows
        // "off" and the heating setpoint, even while the unit is cooling. The
        // uplink OnOff case echo-guards against this write re-commanding. (docs/08 #10)
        Clusters::OnOff::Attributes::OnOff::Set(kAirconEp, st.power_on);

        // ThermostatRunningState -> HA hvac_action (cooling/heating/fan badge). Requires
        // the attribute enabled in the .zap (0x0029); harmless no-op if absent. (docs/08)
        ThermAttr::ThermostatRunningState::Set(kAirconEp,
            hisense_to_running_state(st.power_on, st.mode, st.compressor_freq));

        // Setpoint readback (cooling/heating chosen by mode)
        int16_t sp = (int16_t)(st.setpoint_c * 100);
        if (st.mode == HISENSE_MODE_HEAT) {
            ThermAttr::OccupiedHeatingSetpoint::Set(kAirconEp, sp);
        } else {
            ThermAttr::OccupiedCoolingSetpoint::Set(kAirconEp, sp);
        }

        // FanControl current percent + discrete speed (read-only wire mirror)
        FanAttr::PercentCurrent::Set(kAirconEp, hisense_fan_raw_to_percent(st.fan_raw));
        FanAttr::SpeedCurrent::Set(kAirconEp, hisense_fan_raw_to_speed(st.fan_raw));
        // FanMode -> HA's fan entity reads THIS (not PercentCurrent) for on/off + preset,
        // so without it the fan shows off even while running. (docs/08)
        FanAttr::FanMode::Set(kAirconEp,
            (chip::app::Clusters::FanControl::FanModeEnum)hisense_fan_raw_to_fanmode(st.fan_raw, st.power_on));
        // Swing state -> RockSetting bitmap (vertical only; no H-swing motor on this unit,
        // so never report RockLeftRight even if the vestigial status bit is set)
        FanAttr::RockSetting::Set(kAirconEp, swing_to_rock(st.vswing_on, false));

        // Outdoor temp -> ep2 TemperatureMeasurement/MeasuredValue (int16, 0.01 C).
        // Standard cluster => HA-readable (the mfg attr 0x0011 below is not, since
        // matter-server can't read a custom cluster). (docs/07 Tier-2)
        TempMeasAttr::MeasuredValue::Set(kOutdoorTempEp, (int16_t)(st.outdoor_temp_c * 100));

        // Outdoor/condenser coil temp -> ep8 TemperatureMeasurement (int16, 0.01 C).
        // Standard cluster => HA-readable sensor; a diagnostic that reverses cool/heat (#51).
        TempMeasAttr::MeasuredValue::Set(kCoilTempEp, (int16_t)(st.coil_temp_c * 100));

        // aux/PTC electric-heat relay -> BooleanState (Contact Sensor ep7) so HA gets a
        // standard binary_sensor (#51). Decoded from status offset-35 bit4; normally 0,
        // asserts in cold/defrost. Standard cluster => HA-readable (unlike the mfg attrs).
        BoolAttr::StateValue::Set(kHeatRelayEp, st.heat_relay_on);

        // REVERTED (was: TUIC display-unit on ep1 + aggregate fault on ep10).
        // Both were added by a hand-edited .zap and, once booted, the device's Matter
        // SUBSCRIPTIONS failed persistently with CHIP Error 0x24 "Invalid TLV tag" while
        // reads still worked -- so matter-server marked the node unavailable and HA lost the
        // kitchen unit. The generated tables looked correct (ENUM8/size 1, min/max present,
        // FIXED_ENDPOINT_COUNT 11), so the defect is subtler than a bad attribute type.
        // Reverted here to restore service; re-land once the root cause is understood and a
        // SUBSCRIPTION smoke-test exists (a clean build + contiguity lint did NOT catch this).
        // The ESP32 half is unaffected and keeps both features.

        // Manufacturer cluster read-back (no generated accessors for a custom
        // cluster -> raw ember writes). Types: boolean=0x10, int8u=0x20, int8s=0x28.
        {
            uint8_t b;
            b = st.eco_on   ? 1 : 0; emberAfWriteAttribute(kAirconEp, HisenseCl::Id, HisenseAttr::Eco::Id,          &b, kHisenseTypeBool);
            b = st.turbo_on ? 1 : 0; emberAfWriteAttribute(kAirconEp, HisenseCl::Id, HisenseAttr::Turbo::Id,        &b, kHisenseTypeBool);
            b = st.mute_on  ? 1 : 0; emberAfWriteAttribute(kAirconEp, HisenseCl::Id, HisenseAttr::Mute::Id,         &b, kHisenseTypeBool);
            b = (uint8_t)(st.sleep_raw / 2);      emberAfWriteAttribute(kAirconEp, HisenseCl::Id, HisenseAttr::SleepProfile::Id, &b, kHisenseTypeInt8u);
            b = st.compressor_freq;               emberAfWriteAttribute(kAirconEp, HisenseCl::Id, HisenseAttr::CompressorHz::Id, &b, kHisenseTypeInt8u);
            b = (uint8_t)st.outdoor_temp_c;       emberAfWriteAttribute(kAirconEp, HisenseCl::Id, HisenseAttr::OutdoorTemp::Id,  &b, kHisenseTypeInt8s);
        }

        // Special-mode switch endpoints (ep3-6) -> HA-controllable OnOff mirror of the
        // eco/mute/turbo/sleep status. Each Set re-enters the uplink handler on change;
        // the per-endpoint echo guards above stop it re-commanding its own readback.
        Clusters::OnOff::Attributes::OnOff::Set(kEcoEp,   st.eco_on);
        Clusters::OnOff::Attributes::OnOff::Set(kMuteEp,  st.mute_on);
        Clusters::OnOff::Attributes::OnOff::Set(kTurboEp, st.turbo_on);
        // Sleep-profile ModeSelect (ep6) tracks the actual profile (0=off..4=Kids). Echo-
        // guarded in the uplink handler so this readback doesn't re-command the A/C.
        Clusters::ModeSelect::Attributes::CurrentMode::Set(kSleepEp, (uint8_t)(st.sleep_raw / 2));

        // Energy: feed ElectricalPowerMeasurement (ActivePower/Voltage/Current) from
        // the calibrated current-proxy byte. HA reads ActivePower as a Watts sensor.
        matter_power_meter_update(st);
        break;
    }
    default:
        break;
    }

    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
}

/* The SDK build won't compile a newly-added SRC_CPP file (dep-tracking bug), so the
 * reference EPM delegate is compiled here, inline in this always-rebuilt TU, instead
 * of as its own object. It is intentionally NOT in the room_air_conditioner main.mk
 * SRC_CPP (would double-define). See firmware/docs/09. */
#include "ElectricalPowerMeasurementDelegate.cpp"
