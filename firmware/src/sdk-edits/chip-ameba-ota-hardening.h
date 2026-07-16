
// ==== HISENSE_OTA_HARDENING (Gitea #76) =====================================
// Appended to connectedhomeip/src/platform/Ameba/CHIPPlatformConfig.h by
// firmware/scripts/ota-release.sh build() -> apply_ota_hardening() (idempotent; the marker
// "HISENSE_OTA_HARDENING" above is the guard). This file is the CANONICAL copy captured in the
// repo; the SDK header (gitignored, outside the tree) is a derived build target. Never hand-edit
// the SDK header -- edit this and rebuild.
//
// WHY: Matter OTA runs over stop-and-wait CHIP BDX -- the device pulls one ~1 KB block per
// UDP+MRP round-trip, so a ~1.5 MB image is minutes of chatty exchanges. On a marginal link
// (observed hard at ~-67 dBm) a deep fade outlasts the MRP retransmit budget, the exchange is
// abandoned, and matter-server returns error 11 "Target node did not process the update file".
// AmebaZ2 has NO ESP-IDF Kconfig, so CHIP falls back to the upstream defaults in
// connectedhomeip/src/messaging/ReliableMessageProtocolConfig.h -- MAX_RETRANS=4, active-retry
// 300 ms, idle-retry 500 ms -- too few retransmits and too-short intervals to ride out fades.
//
// These overrides mirror the ESP32 esp-matter hardening (firmware/esp32-matter/sdkconfig.defaults:
// CONFIG_MRP_MAX_RETRANS=8, active 500, idle 800, sender-boost 300) so both firmware paths behave
// the same on weak Wi-Fi. CHIPConfig.h includes this platform header (CHIP_PLATFORM_CONFIG_INCLUDE,
// CHIPConfig.h:71-72) before ReliableMessageProtocolConfig.h applies its own #ifndef defaults, so
// ours win. Values are macro text -- chip::System::Clock::Milliseconds32(...) is only evaluated at
// the use sites (message layer), where SystemClock.h is in scope, exactly as ESP32 does it.
//
// NOTE (#76 scope): the AmebaZ2 matter core + main already build with -Os
// (chip_core_sources.mk / chip_main_sources.mk), so the image-size lever from the ESP32 work is
// already in place here; only the MRP tuning needed porting. Delta-OTA and raising the BDX block
// size (esp-matter#900) remain follow-ups on #76.
#ifndef CHIP_CONFIG_RMP_DEFAULT_MAX_RETRANS
#define CHIP_CONFIG_RMP_DEFAULT_MAX_RETRANS (8)
#endif
#ifndef CHIP_CONFIG_MRP_LOCAL_ACTIVE_RETRY_INTERVAL
#define CHIP_CONFIG_MRP_LOCAL_ACTIVE_RETRY_INTERVAL chip::System::Clock::Milliseconds32(500)
#endif
#ifndef CHIP_CONFIG_MRP_LOCAL_IDLE_RETRY_INTERVAL
#define CHIP_CONFIG_MRP_LOCAL_IDLE_RETRY_INTERVAL chip::System::Clock::Milliseconds32(800)
#endif
#ifndef CHIP_CONFIG_MRP_RETRY_INTERVAL_SENDER_BOOST
#define CHIP_CONFIG_MRP_RETRY_INTERVAL_SENDER_BOOST chip::System::Clock::Milliseconds32(300)
#endif
// ==== end HISENSE_OTA_HARDENING =============================================
