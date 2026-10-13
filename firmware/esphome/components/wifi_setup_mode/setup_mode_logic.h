// Wi-Fi setup mode: every decision, as pure functions.
//
// Plain integers only, no ESPHome and no Wi-Fi, so the host tests compile this file as it is
// (firmware/test/test_esphome_setup_mode.cpp). The calls it decides on (the Wi-Fi component, the
// A/C panel's "77", the restart, the journal in flash) are in wifi_setup_mode.cpp, which does
// what the returned actions say and nothing else.
//
// The mode in one paragraph. A request (the remote's gesture, or the button) opens the node's
// hotspot with the captive portal while the configured network is still reachable. ESPHome has no
// call for that, so the station is pointed at a network that does not exist and the fallback
// hotspot follows. The mode ends in a restart: when a network entered in the portal has
// connected, on a second request, or at the deadline. A network that was entered and did not
// connect is replaced by the one the node had before.
#pragma once

#include <cstdint>

namespace esphome::wifi_setup_mode {

// How long the hotspot may take to open after the station was parked. ESPHome opens it from
// loop() once no network has been connected for `ap_timeout`, which the mode sets to 1 ms, so it
// is a matter of milliseconds when it opens at all. It does not open a second time in one boot
// (`ap_setup_` is set once, esphome/components/wifi/wifi_component.cpp:981, and never cleared).
static constexpr uint32_t HOTSPOT_WAIT_MS = 30000;

// How long a network saved in the portal gets to connect before the deadline may end the mode.
// One connect attempt takes up to 46 s (WIFI_CONNECT_TIMEOUT_MS, wifi_component.cpp:402) and the
// pause between attempts with the hotspot up is 30 s (WIFI_COOLDOWN_WITH_AP_ACTIVE_MS, :387).
// 90 s covers one attempt, the pause, and the start of a second.
static constexpr uint32_t CONNECT_GRACE_MS = 90000;

// The longest timeout the configuration may give. Deadlines are compared as a signed difference
// of two 32 bit millisecond clocks, which is right for spans below 2^31 ms (24.8 days).
static constexpr uint32_t MAX_TIMEOUT_MS = 24UL * 60UL * 60UL * 1000UL;

enum class Phase : uint8_t {
  IDLE,      // not in setup mode
  ENTERING,  // station parked, waiting for the hotspot
  OPEN,      // hotspot open on demand, station parked
  ATTENDED,  // the hotspot had opened by itself (network out of reach) and a request lit "77"
  LEAVING,   // restart asked for, or shutting down
};

enum class Event : uint8_t {
  NONE,               // a look at the inputs that found nothing new
  GESTURE,            // the remote's "77" gesture: the way in and the way out
  START,              // the button, or the start action
  STOP,               // the stop action
  HOTSPOT_UP,         // the hotspot opened while ENTERING
  HOTSPOT_FAILED,     // it did not open within HOTSPOT_WAIT_MS
  STATION_CONNECTED,  // the station went from not connected to connected
  PORTAL_SAVED,       // the station holds credentials the node did not put there
  TIMEOUT,            // the deadline of OPEN or ATTENDED passed
  SHUTDOWN,           // the node is restarting for a reason of its own (on_shutdown)
};

// What the component does for a step. Several may be set. They run in the order of their values.
enum ActionBit : uint16_t {
  ACT_REMEMBER = 1 << 0,       // the connected network is the one to go back to from now on
  ACT_ARM = 1 << 1,            // write the previous credentials to the journal in flash
  ACT_INDICATOR_ON = 1 << 2,   // "77" on the panel
  ACT_PARK = 1 << 3,           // leave the network: restart Wi-Fi on the placeholder network
  ACT_INDICATOR_OFF = 1 << 4,  // "77" off
  ACT_RESTORE = 1 << 5,        // write the previous credentials back over what the portal saved
  ACT_DISARM = 1 << 6,         // clear the journal
  ACT_RESTART = 1 << 7,        // restart the node
};

// What happened, for the log. The component holds the wording.
enum class Note : uint8_t {
  NONE,
  ENTERING,              // leaving the network to open the hotspot
  ATTENDING,             // the hotspot was already open: "77" lit, nothing else changed
  UNAVAILABLE,           // this configuration cannot open the hotspot on demand
  ALREADY_ACTIVE,        // a start request while the mode is on
  HOTSPOT_OPEN,          // the portal can be reached
  HOTSPOT_FAILED,        // it did not open: restart, ask again
  PORTAL_SAVED,          // a network was entered, waiting for it to connect
  CONNECTED,             // the network entered has connected: restart onto it
  CONNECTED_NO_RESTART,  // ATTENDED: a network connected, the mode is over
  UNEXPECTED_CONNECT,    // connected while no network had been entered: restart
  STOPPED,               // ended by request, nothing to undo
  STOPPED_RESTORING,     // ended by request, the network entered had not connected
  TIMED_OUT,             // deadline, nothing to undo
  TIMED_OUT_RESTORING,   // deadline, the network entered had not connected
  SHUTDOWN_RESTORING,    // restart from elsewhere, the network entered had not connected
};

struct State {
  Phase phase{Phase::IDLE};
  bool connected{false};  // the station at the previous step, for the connect edge
  bool armed{false};      // the journal holds the previous credentials
  bool dirty{false};      // the portal saved a network that has not connected
  uint32_t deadline{0};   // clock value at which ENTERING, OPEN or ATTENDED gives up
  uint32_t baseline{0};   // what the station holds while the portal has saved nothing. 0: cannot tell
  uint32_t seen{0};       // the network the portal saved last
};

// What the component reads before each step. A fingerprint is credentials_fingerprint() of an
// SSID and its password: 0 means "none".
struct Inputs {
  uint32_t now{0};        // millisecond clock, wraps
  bool ap_active{false};  // the hotspot is up
  bool connected{false};  // the station is connected
  uint32_t station{0};    // fingerprint of the station's current credentials
  uint32_t parked{0};     // fingerprint of the placeholder network
  uint32_t previous{0};   // fingerprint of the credentials a session started now would restore
};

// Fixed at build time.
struct Config {
  uint32_t timeout_ms{600000};
  // False for a configuration whose station reconnects by itself while it is being parked:
  // `fast_connect`, or a first network marked `hidden` (both connect from start() before the
  // placeholder is in place, wifi_component.cpp:588-599 and :698-720), and for an enterprise
  // login, which cannot be written back as an SSID and a password.
  bool on_demand{true};
  // False when the build names several networks. Then the station holding something else than
  // the previous network is normal, and ATTENDED cannot tell a portal entry from it.
  bool single_network{true};
};

struct Result {
  State state;
  uint16_t actions{0};
  Note note{Note::NONE};
};

// FNV-1a over the SSID, a zero byte and the password. Never 0 for a non-empty SSID. Used to tell
// "the same credentials" from "other credentials" without keeping or logging either.
inline uint32_t credentials_fingerprint(const char *ssid, const char *password) {
  if (ssid == nullptr || ssid[0] == '\0')
    return 0;
  uint32_t hash = 2166136261UL;
  for (const char *p = ssid; *p != '\0'; p++)
    hash = (hash ^ static_cast<uint8_t>(*p)) * 16777619UL;
  hash = (hash ^ 0U) * 16777619UL;
  if (password != nullptr) {
    for (const char *p = password; *p != '\0'; p++)
      hash = (hash ^ static_cast<uint8_t>(*p)) * 16777619UL;
  }
  return hash == 0 ? 1 : hash;
}

// True once `now` has reached `deadline`, for deadlines less than 2^31 ms away.
inline bool deadline_reached(uint32_t now, uint32_t deadline) { return static_cast<int32_t>(now - deadline) >= 0; }

inline uint32_t clamp_timeout(uint32_t timeout_ms) { return timeout_ms > MAX_TIMEOUT_MS ? MAX_TIMEOUT_MS : timeout_ms; }

// The later of two deadlines that are both less than 2^31 ms from now.
inline uint32_t later_deadline(uint32_t a, uint32_t b) { return static_cast<int32_t>(b - a) > 0 ? b : a; }

// Setup mode proper: "77" is on the panel.
inline bool mode_active(Phase phase) {
  return phase == Phase::ENTERING || phase == Phase::OPEN || phase == Phase::ATTENDED;
}

// --- Which credentials are "the previous ones" -----------------------------------------------------

// The network a session restores when a portal entry fails. `remembered` is the network the node
// was last connected to in this boot, or the one loaded from flash at boot. The station's
// current entry is not asked: it is empty while Wi-Fi scans (get_sta() returns an empty WiFiAP,
// wifi_component.cpp:1077-1080 with selected_sta_index_ at -1), and after an earlier portal entry
// outside setup mode it holds that entry, connected or not.
enum class PreviousSource : uint8_t { NONE, REMEMBERED, FROM_BUILD };

inline PreviousSource previous_source(bool have_remembered, bool have_configured) {
  if (have_remembered)
    return PreviousSource::REMEMBERED;
  return have_configured ? PreviousSource::FROM_BUILD : PreviousSource::NONE;
}

// What to do at boot with the journal a session left behind, which means the node went down
// without the mode ending (a power cut, a crash, a watchdog reset).
//   journal_armed   the journal holds previous credentials
//   same_build      it was written by this build. ESPHome keys saved Wi-Fi credentials by the
//                   build's configuration hash, so another build does not read what the portal
//                   saved and has nothing to undo
//   previous        fingerprint of the credentials in the journal
//   boot_station    fingerprint of what Wi-Fi loaded from flash at boot, 0 when it loaded nothing
enum class BootRepair : uint8_t { NOTHING, DISARM, RESTORE };

inline BootRepair boot_repair(bool journal_armed, bool same_build, uint32_t previous, uint32_t boot_station) {
  if (!journal_armed)
    return BootRepair::NOTHING;
  if (!same_build || previous == 0)
    return BootRepair::DISARM;
  // Nothing loaded: the portal saved nothing. The same credentials: nothing to undo either.
  if (boot_station == 0 || boot_station == previous)
    return BootRepair::DISARM;
  return BootRepair::RESTORE;
}

// --- The state machine ---------------------------------------------------------------------------

// The station holds credentials the node did not put there and has not reported yet.
inline bool portal_saved(const State &state, const Inputs &in) {
  return state.baseline != 0 && in.station != 0 && in.station != state.baseline && in.station != state.seen;
}

// What a look at the inputs amounts to. At most one event per look. The order is the priority: a
// connection outranks everything, because it is what ends the mode for good.
inline Event observe(const State &state, const Inputs &in) {
  if (state.phase == Phase::LEAVING)
    return Event::NONE;
  if (in.connected && !state.connected)
    return Event::STATION_CONNECTED;
  switch (state.phase) {
    case Phase::ENTERING:
      if (in.ap_active)
        return Event::HOTSPOT_UP;
      if (deadline_reached(in.now, state.deadline))
        return Event::HOTSPOT_FAILED;
      return Event::NONE;
    case Phase::OPEN:
    case Phase::ATTENDED:
      if (portal_saved(state, in))
        return Event::PORTAL_SAVED;
      if (deadline_reached(in.now, state.deadline))
        return Event::TIMEOUT;
      return Event::NONE;
    default:
      return Event::NONE;
  }
}

namespace detail {

inline Result enter(State state, const Inputs &in, const Config &cfg) {
  Result r;
  if (!in.ap_active && !cfg.on_demand) {
    r.state = state;
    r.note = Note::UNAVAILABLE;
    return r;
  }
  state.dirty = false;
  state.seen = 0;
  if (in.ap_active) {
    // The network is out of reach and ESPHome opened the hotspot. Nothing is parked, so the node
    // keeps trying its network and no restart is needed to go back to it.
    state.phase = Phase::ATTENDED;
    state.deadline = in.now + clamp_timeout(cfg.timeout_ms);
    state.baseline = cfg.single_network ? in.previous : 0;
    state.armed = state.baseline != 0;
    r.note = Note::ATTENDING;
    r.actions = ACT_INDICATOR_ON;
  } else {
    state.phase = Phase::ENTERING;
    state.deadline = in.now + HOTSPOT_WAIT_MS;
    state.baseline = in.parked;
    state.armed = in.previous != 0;
    r.note = Note::ENTERING;
    r.actions = ACT_INDICATOR_ON | ACT_PARK;
  }
  if (state.armed)
    r.actions |= ACT_ARM;
  r.state = state;
  return r;
}

// The actions that undo a session: put the previous network back if the portal's did not
// connect, and clear the journal.
inline uint16_t undo(const State &state) {
  uint16_t actions = 0;
  if (state.armed && state.dirty)
    actions |= ACT_RESTORE;
  if (state.armed)
    actions |= ACT_DISARM;
  return actions;
}

// End the mode without a connection. ENTERING and OPEN have parked the station and end in a
// restart. ATTENDED changed nothing and goes back to IDLE.
inline Result leave(State state, Note plain, Note restoring) {
  Result r;
  const bool restore = state.armed && state.dirty;
  r.note = restore ? restoring : plain;
  r.actions = ACT_INDICATOR_OFF | undo(state);
  if (state.phase == Phase::ATTENDED) {
    state.phase = Phase::IDLE;
  } else {
    state.phase = Phase::LEAVING;
    r.actions |= ACT_RESTART;
  }
  state.armed = false;
  state.dirty = false;
  state.baseline = 0;
  state.seen = 0;
  r.state = state;
  return r;
}

// A network connected: whatever is in flash now is good, so nothing is restored. `entered` says
// whether the portal saved a network in this session.
inline Result commit(State state, bool entered) {
  Result r;
  r.actions = ACT_INDICATOR_OFF;
  if (state.armed)
    r.actions |= ACT_DISARM;
  if (state.phase == Phase::ATTENDED) {
    state.phase = Phase::IDLE;
    r.actions |= ACT_REMEMBER;
    r.note = Note::CONNECTED_NO_RESTART;
  } else {
    // OPEN with a portal entry is the expected case. Without one (or while ENTERING) the
    // configured network came back although the station was parked, which leaves Wi-Fi in a
    // state only a restart cleans up.
    r.note = entered ? Note::CONNECTED : Note::UNEXPECTED_CONNECT;
    state.phase = Phase::LEAVING;
    r.actions |= ACT_RESTART;
  }
  state.armed = false;
  state.dirty = false;
  state.baseline = 0;
  state.seen = 0;
  r.state = state;
  return r;
}

}  // namespace detail

// One step. `event` is a request, SHUTDOWN, or what observe() returned for the same inputs.
// Requests do not touch the connect edge, so a connection that arrives in the same look as a
// request is still reported by the next observe().
inline Result step(const State &state, Event event, const Inputs &in, const Config &cfg) {
  Result r;
  r.state = state;
  const bool observed =
      event != Event::GESTURE && event != Event::START && event != Event::STOP && event != Event::SHUTDOWN;
  if (observed)
    r.state.connected = in.connected;

  if (state.phase == Phase::LEAVING)
    return r;

  if (state.phase == Phase::IDLE) {
    switch (event) {
      case Event::GESTURE:
      case Event::START:
        return detail::enter(r.state, in, cfg);
      case Event::STATION_CONNECTED:
        r.actions = ACT_REMEMBER;
        return r;
      default:
        return r;
    }
  }

  // ENTERING, OPEN or ATTENDED.
  switch (event) {
    case Event::GESTURE:
    case Event::STOP:
      return detail::leave(r.state, Note::STOPPED, Note::STOPPED_RESTORING);
    case Event::START:
      r.note = Note::ALREADY_ACTIVE;
      return r;
    case Event::HOTSPOT_UP:
      if (state.phase == Phase::ENTERING) {
        r.state.phase = Phase::OPEN;
        r.state.deadline = in.now + clamp_timeout(cfg.timeout_ms);
        r.note = Note::HOTSPOT_OPEN;
      }
      return r;
    case Event::HOTSPOT_FAILED:
      if (state.phase == Phase::ENTERING)
        return detail::leave(r.state, Note::HOTSPOT_FAILED, Note::HOTSPOT_FAILED);
      return r;
    case Event::STATION_CONNECTED:
      // The entry and its connection can show up in one look.
      return detail::commit(r.state, state.dirty || portal_saved(state, in));
    case Event::PORTAL_SAVED:
      if (in.station == 0)
        return r;
      r.state.dirty = true;
      r.state.seen = in.station;
      // A network entered late still gets its attempt. ENTERING keeps its own, shorter deadline:
      // the hotspot has to come up first.
      if (state.phase != Phase::ENTERING)
        r.state.deadline = later_deadline(state.deadline, in.now + CONNECT_GRACE_MS);
      r.note = Note::PORTAL_SAVED;
      return r;
    case Event::TIMEOUT:
      if (state.phase == Phase::ENTERING)
        return r;
      return detail::leave(r.state, Note::TIMED_OUT, Note::TIMED_OUT_RESTORING);
    case Event::SHUTDOWN: {
      // A restart nobody asked this component for (the API's reboot timeout, an update, the
      // restart button). The node must not come back on a network that never connected.
      r.actions = ACT_INDICATOR_OFF | detail::undo(state);
      if (state.armed && state.dirty)
        r.note = Note::SHUTDOWN_RESTORING;
      r.state.phase = Phase::LEAVING;
      r.state.armed = false;
      r.state.dirty = false;
      return r;
    }
    default:
      return r;
  }
}

// --- What the status LED and the status pixel show -------------------------------------------------

// One rule for both packages, so they cannot disagree.
enum class Display : uint8_t {
  DARK,     // healthy
  SETUP,    // the hotspot is open, or setup mode is anywhere between the request and the restart
  NO_WIFI,  // no Wi-Fi for DISPLAY_AFTER_S, hotspot not open (yet)
  NO_BUS,   // Wi-Fi up, the A/C not answering for DISPLAY_AFTER_S
};

// The delay keeps a normal boot dark.
static constexpr uint8_t DISPLAY_AFTER_S = 15;

struct DownSeconds {
  uint8_t wifi{0};
  uint8_t bus{0};
};

// Call once a second. The counts stop at 255.
inline void count_down_seconds(DownSeconds *down, bool wifi_connected, bool bus_up) {
  down->wifi = wifi_connected ? 0 : (down->wifi < 255 ? down->wifi + 1 : 255);
  down->bus = bus_up ? 0 : (down->bus < 255 ? down->bus + 1 : 255);
}

inline bool setup_showing(Phase phase, bool ap_active) { return ap_active || phase != Phase::IDLE; }

inline Display status_display(Phase phase, bool ap_active, const DownSeconds &down) {
  if (setup_showing(phase, ap_active))
    return Display::SETUP;
  if (down.wifi >= DISPLAY_AFTER_S)
    return Display::NO_WIFI;
  if (down.bus >= DISPLAY_AFTER_S)
    return Display::NO_BUS;
  return Display::DARK;
}

}  // namespace esphome::wifi_setup_mode
