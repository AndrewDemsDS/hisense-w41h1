// Flash layout guard for the stock AEH-W41H1 module under LibreTiny.
//
// LibreTiny decides where an OTA goes, and which image to invalidate afterwards, from offsets
// fixed at build time. The module's bootloader decides which image to boot from the partition
// table in flash. When the two disagree, the first OTA writes inside the wrong region and then
// invalidates the image that is running. The unit then needs the clip.
//
// Two checks, so that cannot happen quietly:
//   1. at build time, the offsets in this image are exactly those of the layout the board file
//      names (so a layout file that was not applied fails the build), and
//   2. at run time, w41h1_layout_ok() compares them with what the bootloader's partition table
//      says, through the Realtek SDK's own slot helpers.
#pragma once
#include <stdint.h>

// sys_api.c in the Realtek SDK, linked into every LibreTiny AmebaZ2 image. They read the slot
// addresses from the partition table in flash, not from build-time offsets.
extern "C" {
uint32_t sys_update_ota_get_curr_fw_idx(void);
uint32_t sys_update_ota_get_fw1_sn(void);
uint32_t sys_update_ota_get_fw2_sn(void);
uint32_t sys_update_ota_prepare_addr(void);
}

// The board file passes -DW41H1_LAYOUT_<name>=1. Each layout is one of the files next to this one.
#if defined(W41H1_LAYOUT_factory)
// A unit converted over the air from the factory firmware: it keeps the factory partition table.
#define W41H1_LAYOUT_NAME "factory"
static_assert(FLASH_OTA1_OFFSET == 0x010000 && FLASH_OTA1_LENGTH == 0x170000, "factory layout: FW1");
static_assert(FLASH_OTA2_OFFSET == 0x190000 && FLASH_OTA2_LENGTH == 0x170000, "factory layout: FW2");
static_assert(FLASH_KVS_OFFSET == 0x3D0000, "factory layout: settings area");
#elif defined(W41H1_LAYOUT_sdk)
// A unit first written with this repo's clip image: the Realtek SDK's partition table.
#define W41H1_LAYOUT_NAME "sdk"
static_assert(FLASH_OTA1_OFFSET == 0x00C000 && FLASH_OTA1_LENGTH == 0x1AC000, "sdk layout: FW1");
static_assert(FLASH_OTA2_OFFSET == 0x1B8000 && FLASH_OTA2_LENGTH == 0x1AC000, "sdk layout: FW2");
static_assert(FLASH_KVS_OFFSET == 0x3D0000, "sdk layout: settings area");
#elif defined(W41H1_LAYOUT_native)
// A module written from scratch with LibreTiny's own partition table and bootloader.
#define W41H1_LAYOUT_NAME "native"
static_assert(FLASH_OTA1_OFFSET == 0x010000 && FLASH_OTA1_LENGTH == 0x1AC000, "native layout: FW1");
static_assert(FLASH_OTA2_OFFSET == 0x1BC000 && FLASH_OTA2_LENGTH == 0x1AC000, "native layout: FW2");
static_assert(FLASH_KVS_OFFSET == 0x3F8000, "native layout: settings area");
#else
#error "No flash layout chosen. Build one of w41h1-amebaz2-factory.yaml, -sdk.yaml or -native.yaml."
#endif

// The bootloader's address for the next update is one of the two slots this image was built for.
static inline bool w41h1_layout_ok() {
  uint32_t next = sys_update_ota_prepare_addr();
  return next == FLASH_OTA1_OFFSET || next == FLASH_OTA2_OFFSET;
}

// One line for the log and the diagnostic sensor.
static inline void w41h1_layout_report(char *buf, size_t len) {
  snprintf(buf, len, "%s %s slot=%u next=0x%06X fw1_sn=%u fw2_sn=%u", W41H1_LAYOUT_NAME,
           w41h1_layout_ok() ? "ok" : "MISMATCH", (unsigned) sys_update_ota_get_curr_fw_idx(),
           (unsigned) sys_update_ota_prepare_addr(), (unsigned) sys_update_ota_get_fw1_sn(),
           (unsigned) sys_update_ota_get_fw2_sn());
}
