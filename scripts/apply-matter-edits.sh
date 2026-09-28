#!/usr/bin/env bash
# apply-matter-edits.sh -- apply our in-place edits to the Realtek AmebaZ2 *Matter component*.
# That component is an untracked overlay (a separate Realtek Matter SDK layer), so its edits
# can't be shipped as a git patch -- this reproduces them. Idempotent: safe to re-run.
# Requires: AMEBA_SDK=/path/to/ameba-rtos-z2
set -euo pipefail
: "${AMEBA_SDK:?set AMEBA_SDK to your ameba-rtos-z2 checkout}"
M="$AMEBA_SDK/component/common/application/matter"
[ -d "$M" ] || { echo "ERROR: no Matter component at $M -- install the Realtek AmebaZ2 Matter SDK layer"; exit 1; }
edited=0

# 1) Select the room_air_conditioner example (the SDK ships defaulting to chiptest).
PO="$M/common/include/platform_opts_matter.h"
if [ -f "$PO" ]; then
  sed -i -E 's/(CONFIG_EXAMPLE_MATTER_CHIPTEST[[:space:]]+)1/\10/;
             s/(CONFIG_EXAMPLE_MATTER_ROOM_AIR_CONDITIONER[[:space:]]+)0/\11/' "$PO"
  grep -qE 'ROOM_AIR_CONDITIONER[[:space:]]+1' "$PO" && { echo "  [ok] example = room_air_conditioner"; edited=1; } \
    || echo "  [!!] verify example selection in $PO"
else echo "  [!!] not found: $PO (verify the flag names for your SDK)"; fi

# 2) Add the downlink status AppEvent type used by the RS-485 driver.
EV="$M/core/matter_events.h"
if [ -f "$EV" ] && ! grep -q 'kEventType_Downlink_Aircon_Status' "$EV"; then
  # Anchor on the FIRST kEventType_Uplink* enumerator. Use [^,]* (not [A-Za-z_]*) so it also
  # matches the explicit-value form `kEventType_Uplink = 0,` that newer ameba-rtos-matter tips
  # emit -- otherwise the sed silently no-ops and the enum is never added (build fails later on
  # 'kEventType_Downlink_Aircon_Status is not a member of AppEvent').
  sed -i -E 's/(kEventType_Uplink[^,]*,)/\1\n    kEventType_Downlink_Aircon_Status,/' "$EV"
  grep -q 'kEventType_Downlink_Aircon_Status' "$EV" && { echo "  [ok] matter_events.h event added"; edited=1; } \
    || echo "  [!!] add 'kEventType_Downlink_Aircon_Status' to the AppEvent enum in $EV manually"
else echo "  [ok] matter_events.h event present (or file absent -- verify)"; fi

# 3) Add our sources to the example's main-lib makefile (the amebaz2/ path is the one that builds).
MK="$M/project/amebaz2/make/room_air_conditioner/lib_chip_room_air_conditioner_main.mk"
if [ -f "$MK" ]; then
  grep -q 'hisense_rs485.cpp' "$MK" || \
    sed -i '\#matter_drivers.cpp#a SRC_CPP += $(MATTER_EXAMPLE_DIR)/$(DEVICE_TYPE)/hisense_rs485.cpp' "$MK"
  grep -q 'ameba_mode_select_manager.cpp' "$MK" || \
    sed -i '\#matter_drivers.cpp#a SRC_CPP += $(MATTER_DRIVER_DIR)/matter_drivers/mode_select/ameba_mode_select_manager.cpp' "$MK"
  echo "  [ok] main.mk SRC_CPP (hisense_rs485 + mode-select manager)"; edited=1
else echo "  [!!] not found: $MK (verify the make path for your SDK version)"; fi

# 4) Sleep-profile ModeSelect: the supported-modes manager is a Realtek file (NOT vendored here).
#    Point it at endpoint 6 with our profiles -- exact block in firmware/src/sdk-edits/README.md.
MSM="$M/drivers/matter_drivers/mode_select/ameba_mode_select_manager.cpp"
if grep -q 'coffeeOptions' "$MSM" 2>/dev/null && ! grep -q '"General"' "$MSM" 2>/dev/null; then
  echo "  [MANUAL] $MSM : replace coffeeOptions[] with Off/General/Old/Young/Kids and"
  echo "           EndpointSpanPair(6, ...) -- see firmware/src/sdk-edits/README.md (Sleep ModeSelect)."
fi

# 5) IPv6 SLAAC on the station netif. lwIP creates every netif with ip6_autoconfig_enabled = 0
#    (netif.c: "not enabled by default") and nothing in the SDK turns it on; the Matter port only
#    starts DHCPv6. On a router that hands out its prefix by RA (UniFi does) the node therefore
#    stays link-local only, reachable solely by a controller on the same VLAN. Enable SLAAC after
#    the DHCPv6 start (DHCP6_START clears address slots 1..n, so doing it first would race) and
#    solicit an RA at once instead of waiting for the next periodic one.
LW="$M/common/port/matter_lwip.c"
if [ -f "$LW" ] && ! grep -q 'HISENSE_SLAAC' "$LW"; then
  python3 - "$LW" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
inc_anchor = "#include <chip_porting.h>\n"
call = "    LwIP_DHCP6(0, DHCP6_START);\n"
if s.count(inc_anchor) != 1 or s.count(call) != 1:
    sys.exit("anchor not unique")
s = s.replace(inc_anchor, inc_anchor + "#include <lwip/nd6.h>\n#include <lwip/tcpip.h>\n")
s = s.replace(call, call +
    "    /* HISENSE_SLAAC: see scripts/apply-matter-edits.sh step 5. */\n"
    "    netif_set_ip6_autoconfig_enabled(&xnetif[0], 1);\n"
    "    tcpip_callback((tcpip_callback_fn) nd6_restart_netif, &xnetif[0]);\n")
open(p, "w").write(s)
PY
fi
grep -q 'HISENSE_SLAAC' "$LW" 2>/dev/null && { echo "  [ok] matter_lwip.c: IPv6 SLAAC enabled on the station netif"; edited=1; } \
  || echo "  [!!] add the HISENSE_SLAAC block to $LW manually (firmware/src/sdk-edits/README.md)"

# 6) Dual-stack Matter (IPv4 on). The Realtek overlay builds connectedhomeip with
#    INET_CONFIG_ENABLE_IPV4=0, so minimal mDNS listens on ff02::fb only. An AP that converts
#    multicast to unicast from IGMP membership (UniFi Multicast Enhancement) delivers no IPv6 mDNS
#    at all, so the node never hears a query: it announces, but a restarted controller cannot
#    resolve it and CASE fails. ESP32 esp-matter builds dual-stack and is unaffected. The same
#    Makefile line feeds the GN arg chip_inet_config_enable_ipv4 (chip_core_sources.mk greps it).
MK="$M/project/amebaz2/Makefile.include.matter"
if [ -f "$MK" ] && grep -q -- '-DINET_CONFIG_ENABLE_IPV4=0' "$MK"; then
  sed -i 's/-DINET_CONFIG_ENABLE_IPV4=0/-DINET_CONFIG_ENABLE_IPV4=1/' "$MK"
fi
grep -q -- '-DINET_CONFIG_ENABLE_IPV4=1' "$MK" 2>/dev/null && { echo "  [ok] Makefile.include.matter: IPv4 enabled (dual-stack mDNS)"; edited=1; } \
  || echo "  [!!] set -DINET_CONFIG_ENABLE_IPV4=1 in $MK manually (firmware/src/sdk-edits/README.md)"

echo "== apply-matter-edits: done (edited=$edited). Re-run safely; verify any [!!]/[MANUAL] above. =="
