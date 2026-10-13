// Wi-Fi setup mode: the state machine and its credential rules
// (firmware/esphome/components/wifi_setup_mode/setup_mode_logic.h).
//
// The component does what the returned actions say and nothing else, so these checks are the
// behaviour of setup mode as far as a host can show it. What they cannot show is what the Wi-Fi
// component does when told: whether the hotspot opens after disable() and enable() on each chip
// has to be seen on a unit (firmware/esphome/README.md, "Wi-Fi setup mode and the 77 gesture").
//
// Three parts: every state against every event and every combination of the inputs (invariants
// and a table), the cases wave 1 found by reading node.yaml and the ESPHome sources (named D3, D4
// and W7 after its report), and every deadline across the 32 bit millisecond clock's wrap.

#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include "setup_mode_logic.h"

using namespace esphome::wifi_setup_mode;

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(cond, ...) \
  do { \
    g_checks++; \
    if (!(cond)) { \
      g_fail++; \
      printf("  FAIL %s:%d ", __FILE__, __LINE__); \
      printf(__VA_ARGS__); \
      printf("\n"); \
    } \
  } while (0)

static const Phase PHASES[] = {Phase::IDLE, Phase::ENTERING, Phase::OPEN, Phase::ATTENDED, Phase::LEAVING};
static const Event EVENTS[] = {
    Event::NONE,           Event::GESTURE,           Event::START,        Event::STOP,    Event::HOTSPOT_UP,
    Event::HOTSPOT_FAILED, Event::STATION_CONNECTED, Event::PORTAL_SAVED, Event::TIMEOUT, Event::SHUTDOWN};
static const char *const PHASE_NAMES[] = {"IDLE", "ENTERING", "OPEN", "ATTENDED", "LEAVING"};
static const char *const EVENT_NAMES[] = {
    "NONE",           "GESTURE",           "START",        "STOP",    "HOTSPOT_UP",
    "HOTSPOT_FAILED", "STATION_CONNECTED", "PORTAL_SAVED", "TIMEOUT", "SHUTDOWN"};

static const char *name(Phase p) { return PHASE_NAMES[static_cast<int>(p)]; }
static const char *name(Event e) { return EVENT_NAMES[static_cast<int>(e)]; }

static const uint32_t FP_PREVIOUS = 0x11111111;
static const uint32_t FP_PARKED = 0x22222222;
static const uint32_t FP_PORTAL = 0x33333333;
static const uint32_t FP_PORTAL_2 = 0x44444444;

// A node as the component sees it: the state, the build's settings, and what Wi-Fi reports. The
// test moves the clock and the Wi-Fi inputs, the functions under test decide.
struct Node {
  State state;
  Config cfg;
  Inputs in;
  uint16_t actions{0};  // everything asked for since clear()
  Note note{Note::NONE};
  int restarts{0};
  int restores{0};
  bool indicator{false};
  bool journal{false};

  explicit Node(uint32_t start_ms = 1000) {
    in.now = start_ms;
    in.parked = FP_PARKED;
    in.previous = FP_PREVIOUS;
  }
  void apply(const Result &r) {
    state = r.state;
    actions |= r.actions;
    if (r.note != Note::NONE)
      note = r.note;
    // The order the component runs them in.
    if (r.actions & ACT_ARM)
      journal = true;
    if (r.actions & ACT_INDICATOR_ON)
      indicator = true;
    if (r.actions & ACT_PARK) {
      in.connected = false;
      in.station = in.parked;
    }
    if (r.actions & ACT_INDICATOR_OFF)
      indicator = false;
    if (r.actions & ACT_RESTORE) {
      restores++;
      in.station = in.previous;
    }
    if (r.actions & ACT_DISARM)
      journal = false;
    if (r.actions & ACT_RESTART)
      restarts++;
  }
  void clear() {
    actions = 0;
    note = Note::NONE;
  }
  Event look() {
    const Event e = observe(state, in);
    apply(step(state, e, in, cfg));
    return e;
  }
  void request(Event e) { apply(step(state, e, in, cfg)); }
  // Looks every 250 ms, as the component does, until `ms` have passed or the phase changes.
  void run(uint32_t ms) {
    const Phase at_start = state.phase;
    for (uint32_t done = 0; done < ms && state.phase == at_start; done += 250) {
      in.now += 250;
      look();
    }
  }
  // The usual way in: connected, a request, the hotspot comes up a second later.
  void enter_open() {
    in.connected = true;
    in.station = in.previous;
    look();
    request(Event::GESTURE);
    look();
    in.now += 1000;
    in.ap_active = true;
    look();
    clear();
  }
  // The portal saved a network: the station now holds it.
  void portal_saves(uint32_t fingerprint) {
    in.station = fingerprint;
    look();
  }
};

// --- Part 1: every state, every event, every input -------------------------------------------------

static void test_exhaustive() {
  printf("== every phase x every event x every input: invariants ==\n");
  static const uint32_t STATIONS[] = {0, FP_PREVIOUS, FP_PARKED, FP_PORTAL};
  static const uint32_t PREVIOUS[] = {0, FP_PREVIOUS};
  long combos = 0;
  for (Phase phase : PHASES) {
    for (Event event : EVENTS) {
      for (unsigned bits = 0; bits < 128; bits++) {
        for (uint32_t station : STATIONS) {
          for (uint32_t previous : PREVIOUS) {
            State s;
            s.phase = phase;
            s.connected = (bits & 1) != 0;
            s.armed = (bits & 2) != 0;
            s.dirty = (bits & 4) != 0;
            s.deadline = 5000;
            s.baseline = phase == Phase::ATTENDED ? FP_PREVIOUS : FP_PARKED;
            Inputs in;
            in.now = 4000;
            in.ap_active = (bits & 8) != 0;
            in.connected = (bits & 16) != 0;
            in.station = station;
            in.parked = FP_PARKED;
            in.previous = previous;
            Config cfg;
            cfg.on_demand = (bits & 32) != 0;
            cfg.single_network = (bits & 64) != 0;
            const Result r = step(s, event, in, cfg);
            combos++;
            const char *p = name(phase);
            const char *e = name(event);
            const bool was_active = mode_active(phase);
            const bool is_active = mode_active(r.state.phase);

            if (phase == Phase::LEAVING) {
              CHECK(r.actions == 0 && r.state.phase == Phase::LEAVING, "%s/%s: LEAVING takes no event", p, e);
            }
            // "77" follows the mode, and only its edges switch it.
            CHECK(((r.actions & ACT_INDICATOR_ON) != 0) == (!was_active && is_active), "%s/%s: indicator on", p, e);
            CHECK(((r.actions & ACT_INDICATOR_OFF) != 0) == (was_active && !is_active), "%s/%s: indicator off", p, e);
            // The station is parked on the way into ENTERING and nowhere else.
            CHECK(((r.actions & ACT_PARK) != 0) == (phase == Phase::IDLE && r.state.phase == Phase::ENTERING),
                  "%s/%s: park", p, e);
            if (r.actions & ACT_PARK) {
              CHECK(cfg.on_demand && !in.ap_active, "%s/%s: parked although the hotspot cannot follow", p, e);
            }
            // A restart ends ENTERING and OPEN, and only those.
            if (r.actions & ACT_RESTART) {
              CHECK(phase == Phase::ENTERING || phase == Phase::OPEN, "%s/%s: restart from here", p, e);
              CHECK(r.state.phase == Phase::LEAVING, "%s/%s: restart without LEAVING", p, e);
            }
            if (was_active && !is_active && phase != Phase::ATTENDED && event != Event::SHUTDOWN) {
              CHECK((r.actions & ACT_RESTART) != 0, "%s/%s: parked session ended without a restart", p, e);
            }
            if (phase == Phase::ATTENDED) {
              CHECK((r.actions & ACT_RESTART) == 0, "%s/%s: ATTENDED never restarts the node", p, e);
            }
            // Credentials: written back only when the journal has them and the portal's entry did
            // not connect, never after a connection, and the journal is cleared with it.
            if (r.actions & ACT_RESTORE) {
              CHECK(s.armed && s.dirty, "%s/%s: restore without a failed entry", p, e);
              CHECK(event != Event::STATION_CONNECTED, "%s/%s: restore over a network that connected", p, e);
              CHECK((r.actions & ACT_DISARM) != 0, "%s/%s: restore leaves the journal armed", p, e);
            }
            if (was_active && !is_active) {
              CHECK(((r.actions & ACT_DISARM) != 0) == s.armed, "%s/%s: journal cleared iff armed", p, e);
              CHECK(!r.state.armed && !r.state.dirty, "%s/%s: session left flags behind", p, e);
              if (event != Event::STATION_CONNECTED) {
                CHECK(((r.actions & ACT_RESTORE) != 0) == (s.armed && s.dirty), "%s/%s: failed entry not undone", p, e);
              }
            }
            CHECK(((r.actions & ACT_ARM) != 0) == (!was_active && is_active && r.state.armed), "%s/%s: arm", p, e);
            if (r.actions & ACT_ARM) {
              CHECK(in.previous != 0, "%s/%s: armed with nothing to restore", p, e);
            }
            // The button never ends a session. Nothing but a request starts one.
            if (event == Event::START && was_active) {
              CHECK(r.state.phase == phase && r.actions == 0, "%s/%s: START ended a session", p, e);
            }
            if (phase == Phase::IDLE && event != Event::GESTURE && event != Event::START) {
              CHECK(r.state.phase == Phase::IDLE, "%s/%s: left IDLE without a request", p, e);
              CHECK((r.actions & ~ACT_REMEMBER) == 0, "%s/%s: acted in IDLE", p, e);
            }
            // Only looks move the connect edge. Requests leave it alone.
            const bool request =
                event == Event::GESTURE || event == Event::START || event == Event::STOP || event == Event::SHUTDOWN;
            CHECK(r.state.connected == (request ? s.connected : in.connected), "%s/%s: connect edge", p, e);
            // observe() never reports what step() would have to ignore.
            const Event seen = observe(s, in);
            if (phase == Phase::LEAVING) {
              CHECK(seen == Event::NONE, "%s: observe in LEAVING", p);
            }
            if (seen == Event::HOTSPOT_UP || seen == Event::HOTSPOT_FAILED) {
              CHECK(phase == Phase::ENTERING, "%s: hotspot event outside ENTERING", p);
            }
            if (seen == Event::TIMEOUT || seen == Event::PORTAL_SAVED) {
              CHECK(phase == Phase::OPEN || phase == Phase::ATTENDED, "%s: %s outside the open phases", p, name(seen));
            }
            if (in.connected && !s.connected && phase != Phase::LEAVING) {
              CHECK(seen == Event::STATION_CONNECTED, "%s: a connection outranks everything", p);
            }
          }
        }
      }
    }
  }
  printf("  %ld combinations\n", combos);
}

struct Row {
  Phase phase;
  Event event;
  Phase next;
  uint16_t actions;
};

// The table for the plain case: one network configured, the hotspot can be opened on demand, the
// journal armed, nothing entered in the portal. `ap` says whether the hotspot is up at the step.
static void test_table() {
  printf("== transition table (armed, nothing entered) ==\n");
  const uint16_t leave = ACT_INDICATOR_OFF | ACT_DISARM | ACT_RESTART;
  const uint16_t back = ACT_INDICATOR_OFF | ACT_DISARM;
  static const Row ROWS[] = {
      {Phase::IDLE, Event::NONE, Phase::IDLE, 0},
      {Phase::IDLE, Event::GESTURE, Phase::ENTERING, ACT_ARM | ACT_INDICATOR_ON | ACT_PARK},
      {Phase::IDLE, Event::START, Phase::ENTERING, ACT_ARM | ACT_INDICATOR_ON | ACT_PARK},
      {Phase::IDLE, Event::STOP, Phase::IDLE, 0},
      {Phase::IDLE, Event::HOTSPOT_UP, Phase::IDLE, 0},
      {Phase::IDLE, Event::HOTSPOT_FAILED, Phase::IDLE, 0},
      {Phase::IDLE, Event::STATION_CONNECTED, Phase::IDLE, ACT_REMEMBER},
      {Phase::IDLE, Event::PORTAL_SAVED, Phase::IDLE, 0},
      {Phase::IDLE, Event::TIMEOUT, Phase::IDLE, 0},
      {Phase::IDLE, Event::SHUTDOWN, Phase::IDLE, 0},

      {Phase::ENTERING, Event::NONE, Phase::ENTERING, 0},
      {Phase::ENTERING, Event::GESTURE, Phase::LEAVING, leave},
      {Phase::ENTERING, Event::START, Phase::ENTERING, 0},
      {Phase::ENTERING, Event::STOP, Phase::LEAVING, leave},
      {Phase::ENTERING, Event::HOTSPOT_UP, Phase::OPEN, 0},
      {Phase::ENTERING, Event::HOTSPOT_FAILED, Phase::LEAVING, leave},
      {Phase::ENTERING, Event::STATION_CONNECTED, Phase::LEAVING, leave},
      {Phase::ENTERING, Event::PORTAL_SAVED, Phase::ENTERING, 0},
      {Phase::ENTERING, Event::TIMEOUT, Phase::ENTERING, 0},
      {Phase::ENTERING, Event::SHUTDOWN, Phase::LEAVING, back},

      {Phase::OPEN, Event::NONE, Phase::OPEN, 0},
      {Phase::OPEN, Event::GESTURE, Phase::LEAVING, leave},
      {Phase::OPEN, Event::START, Phase::OPEN, 0},
      {Phase::OPEN, Event::STOP, Phase::LEAVING, leave},
      {Phase::OPEN, Event::HOTSPOT_UP, Phase::OPEN, 0},
      {Phase::OPEN, Event::HOTSPOT_FAILED, Phase::OPEN, 0},
      {Phase::OPEN, Event::STATION_CONNECTED, Phase::LEAVING, leave},
      {Phase::OPEN, Event::PORTAL_SAVED, Phase::OPEN, 0},
      {Phase::OPEN, Event::TIMEOUT, Phase::LEAVING, leave},
      {Phase::OPEN, Event::SHUTDOWN, Phase::LEAVING, back},

      {Phase::ATTENDED, Event::NONE, Phase::ATTENDED, 0},
      {Phase::ATTENDED, Event::GESTURE, Phase::IDLE, back},
      {Phase::ATTENDED, Event::START, Phase::ATTENDED, 0},
      {Phase::ATTENDED, Event::STOP, Phase::IDLE, back},
      {Phase::ATTENDED, Event::HOTSPOT_UP, Phase::ATTENDED, 0},
      {Phase::ATTENDED, Event::HOTSPOT_FAILED, Phase::ATTENDED, 0},
      {Phase::ATTENDED, Event::STATION_CONNECTED, Phase::IDLE, back | ACT_REMEMBER},
      {Phase::ATTENDED, Event::PORTAL_SAVED, Phase::ATTENDED, 0},
      {Phase::ATTENDED, Event::TIMEOUT, Phase::IDLE, back},
      {Phase::ATTENDED, Event::SHUTDOWN, Phase::LEAVING, back},

      {Phase::LEAVING, Event::NONE, Phase::LEAVING, 0},
      {Phase::LEAVING, Event::GESTURE, Phase::LEAVING, 0},
      {Phase::LEAVING, Event::START, Phase::LEAVING, 0},
      {Phase::LEAVING, Event::STOP, Phase::LEAVING, 0},
      {Phase::LEAVING, Event::HOTSPOT_UP, Phase::LEAVING, 0},
      {Phase::LEAVING, Event::HOTSPOT_FAILED, Phase::LEAVING, 0},
      {Phase::LEAVING, Event::STATION_CONNECTED, Phase::LEAVING, 0},
      {Phase::LEAVING, Event::PORTAL_SAVED, Phase::LEAVING, 0},
      {Phase::LEAVING, Event::TIMEOUT, Phase::LEAVING, 0},
      {Phase::LEAVING, Event::SHUTDOWN, Phase::LEAVING, 0},
  };
  CHECK(sizeof(ROWS) / sizeof(ROWS[0]) == 50, "the table has every phase x every event");
  bool covered[5][10] = {};
  for (const Row &row : ROWS) {
    covered[static_cast<int>(row.phase)][static_cast<int>(row.event)] = true;
    State s;
    s.phase = row.phase;
    s.armed = mode_active(row.phase);
    s.deadline = 100000;
    s.baseline = row.phase == Phase::ATTENDED ? FP_PREVIOUS : FP_PARKED;
    Inputs in;
    in.now = 5000;
    in.parked = FP_PARKED;
    in.previous = FP_PREVIOUS;
    in.station = 0;  // PORTAL_SAVED with nothing in the station is a stray event: ignored
    in.connected = row.event == Event::STATION_CONNECTED;
    const Result r = step(s, row.event, in, Config());
    CHECK(r.state.phase == row.next, "%s + %s: next is %s, expected %s", name(row.phase), name(row.event),
          name(r.state.phase), name(row.next));
    CHECK(r.actions == row.actions, "%s + %s: actions 0x%02x, expected 0x%02x", name(row.phase), name(row.event),
          r.actions, row.actions);
  }
  for (int p = 0; p < 5; p++) {
    for (int e = 0; e < 10; e++)
      CHECK(covered[p][e], "no row for %s + %s", PHASE_NAMES[p], EVENT_NAMES[e]);
  }
}

// --- Part 2: the cases ---------------------------------------------------------------------------

static void test_plain_session() {
  printf("== a session: gesture, portal, the new network connects ==\n");
  Node n;
  n.in.connected = true;
  n.in.station = FP_PREVIOUS;
  CHECK(n.look() == Event::STATION_CONNECTED && (n.actions & ACT_REMEMBER), "the first connection is remembered");
  n.clear();
  n.request(Event::GESTURE);
  CHECK(n.state.phase == Phase::ENTERING && n.note == Note::ENTERING, "gesture: entering");
  CHECK(n.actions == (ACT_ARM | ACT_INDICATOR_ON | ACT_PARK), "journal armed, 77 on, station parked");
  CHECK(n.indicator && n.journal, "77 is on and the journal holds the previous network");
  n.in.now += 300;
  CHECK(n.look() == Event::NONE && n.state.phase == Phase::ENTERING, "the station dropping is not an event");
  n.in.ap_active = true;
  CHECK(n.look() == Event::HOTSPOT_UP && n.state.phase == Phase::OPEN && n.note == Note::HOTSPOT_OPEN, "hotspot open");
  CHECK(n.state.deadline == n.in.now + 600000, "the timeout counts from the hotspot opening");
  n.in.now += 60000;
  CHECK(n.look() == Event::NONE, "the parked station is not a portal entry");
  n.in.station = 0;
  CHECK(n.look() == Event::NONE, "nor is an empty station (Wi-Fi scanning)");
  n.clear();
  n.portal_saves(FP_PORTAL);
  CHECK(n.note == Note::PORTAL_SAVED && n.state.dirty && n.actions == 0, "the portal's network is noticed");
  CHECK(n.look() == Event::NONE, "and reported once");
  n.in.now += 20000;
  n.in.connected = true;
  n.in.ap_active = false;  // ESPHome closes the hotspot on a connection
  n.clear();
  CHECK(n.look() == Event::STATION_CONNECTED, "it connects");
  CHECK(n.actions == (ACT_INDICATOR_OFF | ACT_DISARM | ACT_RESTART),
        "77 off, journal cleared, restart: nothing restored");
  CHECK(n.note == Note::CONNECTED && n.state.phase == Phase::LEAVING && n.restores == 0, "onto the new network");
  CHECK(!n.journal && !n.indicator, "nothing left behind");
  n.clear();
  n.request(Event::SHUTDOWN);
  CHECK(n.actions == 0, "the restart's own shutdown hook has nothing left to do");
}

static void test_leave_paths() {
  printf("== leaving without a connection ==\n");
  {
    Node n;
    n.enter_open();
    n.request(Event::GESTURE);
    CHECK(n.actions == (ACT_INDICATOR_OFF | ACT_DISARM | ACT_RESTART) && n.note == Note::STOPPED,
          "second gesture, nothing entered: restart, nothing to restore");
  }
  {
    Node n;
    n.enter_open();
    n.portal_saves(FP_PORTAL);
    n.clear();
    n.request(Event::STOP);
    CHECK(n.actions == (ACT_INDICATOR_OFF | ACT_RESTORE | ACT_DISARM | ACT_RESTART),
          "stop with a failed entry restores");
    CHECK(n.note == Note::STOPPED_RESTORING && n.in.station == FP_PREVIOUS, "the previous network is back");
  }
  {
    Node n;
    n.enter_open();
    n.run(700000);
    CHECK(n.note == Note::TIMED_OUT && n.restarts == 1 && n.restores == 0, "timeout, nothing entered: restart only");
    CHECK(n.in.now >= 1000 + 1000 + 600000 && n.in.now <= 1000 + 1000 + 600000 + 500, "after wifi_setup_timeout");
  }
  {
    Node n;
    n.enter_open();
    n.request(Event::START);
    CHECK(n.state.phase == Phase::OPEN && n.note == Note::ALREADY_ACTIVE && n.actions == 0,
          "the button in setup mode changes nothing");
  }
  {
    // The hotspot opens once per boot. If it opened and closed earlier, it does not come back.
    Node n;
    n.in.connected = true;
    n.look();
    n.request(Event::START);
    n.clear();
    n.run(40000);
    CHECK(n.note == Note::HOTSPOT_FAILED && n.state.phase == Phase::LEAVING, "no hotspot in 30 s: give up");
    CHECK(n.actions == (ACT_INDICATOR_OFF | ACT_DISARM | ACT_RESTART), "restart, journal cleared");
    CHECK(n.in.now >= 1000 + 30000 && n.in.now <= 1000 + 30000 + 250, "at HOTSPOT_WAIT_MS");
  }
  {
    Node n;
    n.in.connected = true;
    n.look();
    n.request(Event::GESTURE);
    n.in.now += 5000;
    n.look();
    n.clear();
    n.request(Event::GESTURE);
    CHECK(n.state.phase == Phase::LEAVING && n.restarts == 1, "a gesture during the wait for the hotspot restarts");
  }
}

// D3: a restart that does not go through the leave path used to keep what the portal saved.
static void test_d3_restart_from_elsewhere() {
  printf("== D3: a restart or a power cut in setup mode ==\n");
  {
    // The API component's reboot timeout fires (App.reboot(), which runs on_shutdown).
    Node n;
    n.enter_open();
    n.portal_saves(FP_PORTAL);
    n.clear();
    n.request(Event::SHUTDOWN);
    CHECK(n.actions == (ACT_INDICATOR_OFF | ACT_RESTORE | ACT_DISARM), "shutdown hook: restore, clear the journal");
    CHECK(n.note == Note::SHUTDOWN_RESTORING && n.in.station == FP_PREVIOUS && !n.journal, "previous network back");
    CHECK((n.actions & ACT_RESTART) == 0, "the restart is already under way");
    n.clear();
    n.request(Event::GESTURE);
    n.look();
    CHECK(n.actions == 0 && n.state.phase == Phase::LEAVING, "nothing runs after the shutdown hook");
  }
  {
    Node n;
    n.enter_open();
    n.request(Event::SHUTDOWN);
    CHECK(n.actions == (ACT_INDICATOR_OFF | ACT_DISARM) && n.restores == 0, "nothing entered: only the journal goes");
  }
  {
    Node n;
    n.request(Event::SHUTDOWN);
    CHECK(n.actions == 0 && n.state.phase == Phase::IDLE, "a restart outside setup mode is none of its business");
  }
  // A power cut runs no hook. The journal written on the way in is still there at the next boot.
  CHECK(boot_repair(false, true, FP_PREVIOUS, FP_PORTAL) == BootRepair::NOTHING, "no journal: an ordinary boot");
  CHECK(boot_repair(true, true, FP_PREVIOUS, FP_PORTAL) == BootRepair::RESTORE,
        "power cut after the portal saved: the previous network is written back");
  CHECK(boot_repair(true, true, FP_PREVIOUS, 0) == BootRepair::DISARM,
        "power cut before anything was saved (Wi-Fi loaded nothing): just clear the journal");
  CHECK(boot_repair(true, true, FP_PREVIOUS, FP_PREVIOUS) == BootRepair::DISARM,
        "the saved network is the previous one: nothing to undo");
  CHECK(boot_repair(true, false, FP_PREVIOUS, FP_PORTAL) == BootRepair::DISARM,
        "a journal from another build: that build's saved network is not read by this one");
  CHECK(boot_repair(true, true, 0, FP_PORTAL) == BootRepair::DISARM, "a journal without credentials restores nothing");
  CHECK(boot_repair(false, false, 0, 0) == BootRepair::NOTHING, "an empty journal");
}

// D4: the previous credentials were not captured when the hotspot was already open, or when Wi-Fi
// was scanning and get_sta() returned an empty entry.
static void test_d4_previous_credentials() {
  printf("== D4: which credentials are the previous ones ==\n");
  CHECK(previous_source(true, true) == PreviousSource::REMEMBERED, "the network last connected to, or loaded at boot");
  CHECK(previous_source(false, true) == PreviousSource::FROM_BUILD,
        "never connected in this boot and nothing saved: the configured network");
  CHECK(previous_source(true, false) == PreviousSource::REMEMBERED, "hotspot-only build with a saved network");
  CHECK(previous_source(false, false) == PreviousSource::NONE, "hotspot-only build, nothing saved: nothing to restore");
  {
    // Scanning at the request: the station entry is empty, the journal is armed all the same.
    Node n;
    n.in.connected = false;
    n.in.station = 0;
    n.request(Event::GESTURE);
    CHECK((n.actions & ACT_ARM) && n.state.armed, "armed from the remembered or configured network");
    n.in.ap_active = true;
    n.look();
    n.portal_saves(FP_PORTAL);
    n.clear();
    n.run(700000);
    CHECK((n.actions & ACT_RESTORE) && n.note == Note::TIMED_OUT_RESTORING, "and the failed entry is undone");
  }
  {
    // The hotspot was already open.
    Node n;
    n.in.ap_active = true;
    n.in.station = FP_PREVIOUS;
    n.request(Event::GESTURE);
    CHECK(n.state.phase == Phase::ATTENDED && n.actions == (ACT_ARM | ACT_INDICATOR_ON), "armed, 77 on, not parked");
    n.portal_saves(FP_PORTAL);
    CHECK(n.state.dirty, "a portal entry is noticed against the previous network");
    n.clear();
    n.run(700000);
    CHECK(n.actions == (ACT_INDICATOR_OFF | ACT_RESTORE | ACT_DISARM), "timeout: restore, no restart");
    CHECK(n.state.phase == Phase::IDLE && n.restarts == 0 && n.in.station == FP_PREVIOUS,
          "back to idle on the old network");
  }
  {
    // A hotspot-only build has no previous network: the mode works, nothing is written back.
    Node n;
    n.in.previous = 0;
    n.in.connected = false;
    n.request(Event::START);
    CHECK(n.state.phase == Phase::ENTERING && !(n.actions & ACT_ARM) && !n.journal, "no journal without credentials");
    n.in.ap_active = true;
    n.look();
    n.portal_saves(FP_PORTAL);
    n.clear();
    n.run(700000);
    CHECK(n.actions == (ACT_INDICATOR_OFF | ACT_RESTART) && n.restores == 0, "nothing to restore, restart");
  }
}

static void test_w7_late_entry() {
  printf("== W7: a network entered just before the timeout ==\n");
  {
    Node n;
    n.enter_open();
    const uint32_t opened = n.in.now;
    n.in.now = opened + 600000 - 10000;  // ten seconds before the deadline
    n.portal_saves(FP_PORTAL);
    CHECK(n.state.deadline == n.in.now + CONNECT_GRACE_MS, "the deadline moves out by the connect grace");
    n.in.now += 46000;  // one full connect attempt, well past the old deadline
    CHECK(n.look() == Event::NONE && n.state.phase == Phase::OPEN, "still open at the old deadline plus 36 s");
    n.in.connected = true;
    n.clear();
    n.look();
    CHECK(n.note == Note::CONNECTED && n.restores == 0, "the late network connects and is kept");
  }
  {
    Node n;
    n.enter_open();
    const uint32_t opened = n.in.now;
    n.in.now = opened + 600000 - 10000;
    n.portal_saves(FP_PORTAL);
    const uint32_t saved_at = n.in.now;
    n.clear();
    n.run(200000);
    CHECK(n.note == Note::TIMED_OUT_RESTORING && n.restores == 1, "if it never connects it is undone");
    CHECK(n.in.now - saved_at >= CONNECT_GRACE_MS && n.in.now - saved_at <= CONNECT_GRACE_MS + 250, "after the grace");
  }
  {
    // Entered early: the grace must not shorten the time left.
    Node n;
    n.enter_open();
    const uint32_t deadline = n.state.deadline;
    n.in.now += 5000;
    n.portal_saves(FP_PORTAL);
    CHECK(n.state.deadline == deadline, "an early entry leaves the deadline alone");
    // A second, different entry later is a new attempt.
    n.in.now = deadline - 1000;
    n.clear();
    n.portal_saves(FP_PORTAL_2);
    CHECK(n.note == Note::PORTAL_SAVED && n.state.deadline == n.in.now + CONNECT_GRACE_MS,
          "a corrected entry gets its own grace");
    // The same entry again is not news (the fingerprint has not changed).
    n.in.now += 60000;
    n.clear();
    n.look();
    CHECK(n.note == Note::NONE, "the same entry is reported once");
  }
  {
    // The connection and the entry are seen in one look: the connection wins, nothing is undone.
    Node n;
    n.enter_open();
    n.in.station = FP_PORTAL;
    n.in.connected = true;
    n.clear();
    CHECK(n.look() == Event::STATION_CONNECTED, "connection first");
    CHECK((n.actions & ACT_RESTORE) == 0 && (n.actions & ACT_RESTART) && n.note == Note::CONNECTED, "kept, restart");
  }
  {
    // A request in the look in which the network connects must not undo it.
    Node n;
    n.enter_open();
    n.portal_saves(FP_PORTAL);
    n.in.connected = true;
    n.clear();
    n.look();
    n.request(Event::GESTURE);
    CHECK(n.restores == 0 && n.note == Note::CONNECTED, "the look runs before the request: the connection is kept");
  }
}

static void test_w7_hotspot_already_open() {
  printf("== W7: a gesture while the fallback hotspot is open ==\n");
  {
    // The router comes back by itself.
    Node n;
    n.in.ap_active = true;
    n.in.station = FP_PREVIOUS;
    n.request(Event::GESTURE);
    CHECK(n.note == Note::ATTENDING && n.indicator, "77 lights");
    n.in.now += 120000;
    n.in.connected = true;
    n.in.ap_active = false;
    n.clear();
    n.look();
    CHECK(n.state.phase == Phase::IDLE && n.restarts == 0, "connected again: mode over, no restart");
    CHECK(n.actions == (ACT_REMEMBER | ACT_INDICATOR_OFF | ACT_DISARM) && n.note == Note::CONNECTED_NO_RESTART,
          "77 off");
  }
  {
    Node n;
    n.in.ap_active = true;
    n.request(Event::GESTURE);
    n.run(700000);
    CHECK(n.state.phase == Phase::IDLE && n.restarts == 0 && n.note == Note::TIMED_OUT, "timeout: 77 off, no restart");
  }
  {
    Node n;
    n.in.ap_active = true;
    n.request(Event::GESTURE);
    n.in.now += 15000;
    n.request(Event::GESTURE);
    CHECK(n.state.phase == Phase::IDLE && n.restarts == 0 && !n.indicator, "second gesture: 77 off, no restart");
    n.in.now += 15000;
    n.request(Event::GESTURE);
    CHECK(n.state.phase == Phase::ATTENDED && n.indicator, "and a third lights it again");
  }
  {
    // A network entered in the portal connects: stock ESPHome behaviour, kept, no restart.
    Node n;
    n.in.ap_active = true;
    n.in.station = FP_PREVIOUS;
    n.request(Event::GESTURE);
    n.portal_saves(FP_PORTAL);
    n.in.connected = true;
    n.clear();
    n.look();
    CHECK(n.restores == 0 && n.restarts == 0 && n.state.phase == Phase::IDLE, "kept, no restart");
  }
  {
    // Several networks in the build: the station moving between them is not a portal entry, so
    // nothing is watched and nothing is written.
    Node n;
    n.cfg.single_network = false;
    n.in.ap_active = true;
    n.in.station = FP_PREVIOUS;
    n.request(Event::GESTURE);
    CHECK(n.state.phase == Phase::ATTENDED && n.actions == ACT_INDICATOR_ON && !n.journal, "77 only");
    n.in.station = FP_PORTAL;  // the second configured network being tried
    CHECK(n.look() == Event::NONE && !n.state.dirty, "not taken for a portal entry");
    n.clear();
    n.run(700000);
    CHECK(n.actions == ACT_INDICATOR_OFF && n.restores == 0, "timeout: 77 off, nothing written");
  }
  {
    // Several networks, hotspot opened on demand: the placeholder still tells a portal entry.
    Node n;
    n.cfg.single_network = false;
    n.enter_open();
    n.portal_saves(FP_PORTAL);
    CHECK(n.state.dirty && n.state.armed, "on demand the entry is seen and can be undone");
  }
}

static void test_w7_no_on_demand() {
  printf("== W7: fast_connect, a hidden network, an enterprise login ==\n");
  Node n;
  n.cfg.on_demand = false;
  n.in.connected = true;
  n.in.station = FP_PREVIOUS;
  n.look();
  n.clear();
  n.request(Event::GESTURE);
  CHECK(n.state.phase == Phase::IDLE && n.actions == 0 && n.note == Note::UNAVAILABLE, "refused with a message");
  n.clear();
  n.request(Event::START);
  CHECK(n.state.phase == Phase::IDLE && n.actions == 0 && n.note == Note::UNAVAILABLE, "the button too");
  CHECK(!n.indicator && n.in.connected, "the node stays on its network, 77 stays off");
  // Once the fallback hotspot is open by itself the request can still light "77".
  n.in.connected = false;
  n.in.ap_active = true;
  n.look();
  n.clear();
  n.request(Event::GESTURE);
  CHECK(n.state.phase == Phase::ATTENDED && (n.actions & ACT_PARK) == 0, "attended works without parking");
}

static void test_unexpected_connect() {
  printf("== a connection nobody entered ==\n");
  {
    Node n;
    n.enter_open();
    n.in.connected = true;
    n.look();
    CHECK(n.note == Note::UNEXPECTED_CONNECT && n.restarts == 1 && n.restores == 0, "OPEN: restart, nothing written");
  }
  {
    // A stale "connected" right after parking is ignored. Only an edge counts.
    Node n;
    n.in.connected = true;
    n.look();
    n.apply(step(n.state, Event::GESTURE, n.in, n.cfg));
    n.in.connected = true;  // stale: Wi-Fi's loop runs after this look
    n.in.station = FP_PARKED;
    CHECK(n.look() == Event::NONE && n.state.phase == Phase::ENTERING, "still connected in the next look: no edge");
    n.in.connected = false;
    n.look();
    n.in.connected = true;
    CHECK(n.look() == Event::STATION_CONNECTED && n.note == Note::UNEXPECTED_CONNECT, "a real reconnect ends it");
  }
}

static void test_status_display() {
  printf("== what the status LED and the pixel show ==\n");
  const DownSeconds healthy;
  DownSeconds no_wifi;
  no_wifi.wifi = 15;
  no_wifi.bus = 200;
  DownSeconds no_bus;
  no_bus.bus = 15;
  CHECK(status_display(Phase::IDLE, false, healthy) == Display::DARK, "healthy: dark");
  CHECK(status_display(Phase::IDLE, true, healthy) == Display::SETUP, "the fallback hotspot by itself: setup");
  // One state from the request to the restart, whatever Wi-Fi and the hotspot are doing.
  for (Phase phase : {Phase::ENTERING, Phase::OPEN, Phase::ATTENDED, Phase::LEAVING}) {
    for (bool ap : {false, true}) {
      CHECK(status_display(phase, ap, healthy) == Display::SETUP, "%s: setup", name(phase));
      CHECK(status_display(phase, ap, no_wifi) == Display::SETUP, "%s with Wi-Fi down: setup wins", name(phase));
      CHECK(setup_showing(phase, ap), "%s shows", name(phase));
    }
  }
  CHECK(!setup_showing(Phase::IDLE, false) && setup_showing(Phase::IDLE, true), "idle shows only an open hotspot");
  CHECK(status_display(Phase::IDLE, false, no_wifi) == Display::NO_WIFI, "no Wi-Fi outranks the bus");
  CHECK(status_display(Phase::IDLE, false, no_bus) == Display::NO_BUS, "bus down");
  DownSeconds d;
  for (int i = 0; i < 14; i++)
    count_down_seconds(&d, false, false);
  CHECK(status_display(Phase::IDLE, false, d) == Display::DARK, "14 s down: still dark (a normal boot)");
  count_down_seconds(&d, false, false);
  CHECK(status_display(Phase::IDLE, false, d) == Display::NO_WIFI, "15 s down: shown");
  count_down_seconds(&d, true, false);
  CHECK(d.wifi == 0 && d.bus == 16 && status_display(Phase::IDLE, false, d) == Display::NO_BUS, "Wi-Fi back, bus not");
  for (int i = 0; i < 1000; i++)
    count_down_seconds(&d, false, false);
  CHECK(d.wifi == 255 && d.bus == 255, "the counts stop at 255");
  count_down_seconds(&d, true, true);
  CHECK(d.wifi == 0 && d.bus == 0, "and clear at once");
  // The failed open, which the two packages used to show differently: ENTERING without a hotspot.
  CHECK(status_display(Phase::ENTERING, false, no_wifi) == Display::SETUP, "failed open: setup until the restart");
}

static void test_fingerprint() {
  printf("== credential fingerprints ==\n");
  CHECK(credentials_fingerprint("", "x") == 0 && credentials_fingerprint(nullptr, "x") == 0, "no SSID: none");
  CHECK(credentials_fingerprint("net", nullptr) == credentials_fingerprint("net", ""), "an open network");
  CHECK(credentials_fingerprint("net", "") != 0, "never 0 for a network");
  CHECK(credentials_fingerprint("net", "pass") == credentials_fingerprint("net", "pass"), "stable");
  CHECK(credentials_fingerprint("net", "pass") != credentials_fingerprint("net", "pasS"), "password counts");
  CHECK(credentials_fingerprint("net", "pass") != credentials_fingerprint("neT", "pass"), "SSID counts");
  CHECK(credentials_fingerprint("ab", "c") != credentials_fingerprint("a", "bc"), "the boundary counts");
  CHECK(credentials_fingerprint("abc", "") != credentials_fingerprint("ab", "c"), "an empty password is not a shift");
  // A value from an independent FNV-1a 32 of the bytes "a\0b".
  CHECK(credentials_fingerprint("a", "b") == 0x10F3ABD2UL, "FNV-1a of 61 00 62 (got 0x%08X)",
        (unsigned) credentials_fingerprint("a", "b"));
  // High-bit bytes (UTF-8 names) are hashed as unsigned.
  CHECK(credentials_fingerprint("\xC3\xA9", "") != credentials_fingerprint("\x43\x29", ""), "non-ASCII SSID");
}

// --- Part 3: the clock's wrap ----------------------------------------------------------------------

static void test_wraparound() {
  printf("== every deadline across the millis() wrap ==\n");
  CHECK(!deadline_reached(0xFFFFFF00UL, 0x00000100UL), "a deadline past the wrap is not reached before it");
  CHECK(deadline_reached(0x00000100UL, 0x00000100UL), "reached at the deadline");
  CHECK(deadline_reached(0x00000200UL, 0xFFFFFF00UL), "a deadline before the wrap is reached after it");
  CHECK(later_deadline(0xFFFFFF00UL, 0x00000100UL) == 0x00000100UL, "later across the wrap");
  CHECK(later_deadline(0x00000100UL, 0xFFFFFF00UL) == 0x00000100UL, "either way round");
  CHECK(later_deadline(5, 5) == 5, "equal");
  CHECK(clamp_timeout(600000) == 600000 && clamp_timeout(0xFFFFFFFFUL) == MAX_TIMEOUT_MS, "timeout clamp");

  // Starts chosen so that each deadline, and each event before it, falls on either side of 2^32.
  static const uint32_t STARTS[] = {1000,
                                    0xFFFFFFFFUL - 100,
                                    0xFFFFFFFFUL - 15000,
                                    0xFFFFFFFFUL - 31000,
                                    0xFFFFFFFFUL - 80000,
                                    0xFFFFFFFFUL - 300000,
                                    0xFFFFFFFFUL - 601000,
                                    0xFFFFFFFFUL - 650000,
                                    0x7FFFFFFFUL - 300000};
  for (uint32_t start : STARTS) {
    {
      // HOTSPOT_WAIT_MS.
      Node n(start);
      n.in.connected = true;
      n.look();
      n.request(Event::GESTURE);
      n.run(29000);
      CHECK(n.state.phase == Phase::ENTERING, "start 0x%08X: still waiting for the hotspot at 29 s", (unsigned) start);
      n.run(2000);
      CHECK(n.note == Note::HOTSPOT_FAILED && n.in.now - start >= 30000 && n.in.now - start <= 30250,
            "start 0x%08X: gave up at 30 s (%u ms)", (unsigned) start, (unsigned) (n.in.now - start));
    }
    {
      // The timeout of OPEN.
      Node n(start);
      n.enter_open();
      const uint32_t opened = n.in.now;
      n.run(599000);
      CHECK(n.state.phase == Phase::OPEN, "start 0x%08X: open at 599 s", (unsigned) start);
      n.run(2000);
      CHECK(n.note == Note::TIMED_OUT && n.in.now - opened >= 600000 && n.in.now - opened <= 600250,
            "start 0x%08X: timed out at 600 s (%u ms)", (unsigned) start, (unsigned) (n.in.now - opened));
    }
    {
      // The timeout of ATTENDED.
      Node n(start);
      n.in.ap_active = true;
      n.request(Event::GESTURE);
      n.run(599000);
      CHECK(n.state.phase == Phase::ATTENDED, "start 0x%08X: attended at 599 s", (unsigned) start);
      n.run(2000);
      CHECK(n.state.phase == Phase::IDLE && n.in.now - start >= 600000 && n.in.now - start <= 600250,
            "start 0x%08X: attended timed out at 600 s", (unsigned) start);
    }
    {
      // The connect grace, entered 5 s before the timeout.
      Node n(start);
      n.enter_open();
      n.run(595000);
      n.portal_saves(FP_PORTAL);
      const uint32_t saved = n.in.now;
      n.run(89000);
      CHECK(n.state.phase == Phase::OPEN, "start 0x%08X: grace still running at 89 s", (unsigned) start);
      n.run(2000);
      CHECK(n.note == Note::TIMED_OUT_RESTORING && n.in.now - saved >= 90000 && n.in.now - saved <= 90250,
            "start 0x%08X: grace over at 90 s (%u ms)", (unsigned) start, (unsigned) (n.in.now - saved));
    }
  }
  {
    // The longest timeout the configuration allows, started just before the wrap.
    Node n(0xFFFFFFFFUL - 5000);
    n.cfg.timeout_ms = MAX_TIMEOUT_MS;
    n.enter_open();
    const uint32_t opened = n.in.now;
    n.in.now = opened + MAX_TIMEOUT_MS - 1;
    CHECK(n.look() == Event::NONE, "24 h timeout: not yet");
    n.in.now = opened + MAX_TIMEOUT_MS;
    CHECK(n.look() == Event::TIMEOUT, "24 h timeout: reached, 24 h after a start 5 s before the wrap");
  }
  {
    // A timeout beyond what the comparison can carry is clamped. It never wraps into the past.
    Node n;
    n.cfg.timeout_ms = 0xFFFFFFF0UL;
    n.enter_open();
    n.in.now += 1000;
    CHECK(n.look() == Event::NONE, "an oversized timeout does not end the mode at once");
  }
}

int main() {
  printf("== Layer 1n: Wi-Fi setup mode (state machine, credentials, status display) ==\n");
  test_exhaustive();
  test_table();
  test_plain_session();
  test_leave_paths();
  test_d3_restart_from_elsewhere();
  test_d4_previous_credentials();
  test_w7_late_entry();
  test_w7_hotspot_already_open();
  test_w7_no_on_demand();
  test_unexpected_connect();
  test_status_display();
  test_fingerprint();
  test_wraparound();
  printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail == 0 ? 0 : 1;
}
