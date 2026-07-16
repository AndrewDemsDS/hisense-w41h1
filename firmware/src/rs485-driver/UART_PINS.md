# A/C RS-485 UART — pin & peripheral findings (RTL8710C / AmebaZ2)

> **⚠️ CORRECTION — the "no DE/RE / auto-direction" conclusion below is WRONG (disproven on
> hardware).** There IS a software direction line: the stock firmware drives an RS-485 **DE GPIO on
> PA_17** (HIGH=TX / LOW=RX) around every transmit. Missing it cost a multi-day debug (TX reached
> the transceiver DI but never the A/B bus). See
> [`reverse-engineering/docs/10-stock-fw-init-and-comms.md` §2.1](../../../reverse-engineering/docs/10-stock-fw-init-and-comms.md#21-ac-path-pins-proven)
> (DE = PA_17) and §1.3/§4.4 for the toggle timing. Everything below is preserved as the valid
> record of the pre-hardware static-RE reasoning that reached the wrong conclusion.

Reverse-engineered from the stock Hisense firmware dump
`w41h1-matter-firmware/factory/STOCK-RECOVERY-w41h1_dump1.bin` (4 MB, plaintext).

## Summary (fill these into the driver)

| Item | Value | Raw int | Confidence |
|------|-------|---------|------------|
| Peripheral | **UART0** (`Uart0 = 0`) | 0 | High |
| TX pin | **PA_14** | `0x0E` (14) | **High** |
| RX pin | **PA_13** | `0x0D` (13) | **High** |
| Frame format | **8N1** (8 data, no parity, 1 stop) | — | High |
| Baud (boot default) | **115200** | `0x1C200` | High |
| Baud (A/C-mode select) | **9600** (used when link mode == 9) | `0x2580` | High |
| DE/RE direction GPIO | **None** — auto-direction transceiver | — | Med-High |

`serial_init(&obj, PA_14 /*tx*/, PA_13 /*rx*/)` is the call to reproduce. The UART
*index* (UART0) is derived automatically by the ROM HAL from the pin pair, so the driver
only needs the two PinNames.

## PinName encoding (confirmed from SDK)

`ameba-rtos-z2/.../rtl8710c_pin_name.h`: `PIN_NAME(port,pin) = (port<<5)|pin`, PORT_A=0,
PORT_B=1. So **PA_x = x** (0..23), **PB_x = 32+x**. Therefore raw `0x0E`→PA_14, `0x0D`→PA_13.
(`PinNames.h` mirrors this: `PA_14 = PIN_A14`, etc.)

## Primary evidence — the A/C UART setup

The A/C bus UART is configured inside **`uart_ctl_process_main`** (the "uart server" /
`uart_thread` task that drives `BC_cmd_task`, the `F4 F5 … F4 FB` frame parser, and
`dev_ota_uart_process_cmd` / `matter_uart_process`). The setup code is **not** in the
XIP image the RE notes use (base `0x9b6d0000`); it is in a **second XIP image** (base
≈ `0x9b000000`, where the mbed `serial_api` lives — the SDK build maps `serial_init` at
`0x9b013dc8`). Relative/PC-relative addressing is preserved in file space, so the region
disassembles cleanly at raw file offsets.

Disassembly (file offsets; the serial obj is the SRAM global `0x1000e87c`):

```
f0x1a207c  push  {r0,r1,r4,lr}
f0x1a2086  movs  r2, #0xd            ; rx = 13 = PA_13
f0x1a2088  movs  r1, #0xe            ; tx = 14 = PA_14
f0x1a208a  ldr   r0, =0x1000e87c     ; &serial_obj
f0x1a208c  bl    0x1c553c            ; serial_init(obj, PA_14, PA_13)
f0x1a2090  mov.w r1, #0x1c200        ; 115200
f0x1a2094  ldr   r0, =0x1000e87c
f0x1a2096  bl    0x1c55b8            ; serial_baud(obj, 115200)
f0x1a209a  movs  r3,#1 ; r2,#0 ; r1,#8
f0x1a20a2  bl    0x1c55c4            ; serial_format(obj, data=8, parity=None, stop=1)  -> 8N1
f0x1a20ae  bl    0x1c55d4            ; serial_irq_handler(obj, handler, id=0)
f0x1a20b8  bl    0x1c561c            ; serial_irq_set(obj, RxIrq, enable=1)
```

Function identification is unambiguous:
- The call fan-out `serial_init / serial_baud / serial_format / serial_irq_handler /
  serial_irq_set` matches the argument arities in
  `component/common/mbed/hal/serial_api.h` exactly.
- The offset delta **serial_baud − serial_init = 0x7c** (`0x1c55b8 − 0x1c553c`) matches
  the SDK build's `serial_baud(0x9b013e44) − serial_init(0x9b013dc8) = 0x7c` byte-for-byte,
  confirming `0x1c553c = serial_init`, `0x1c55b8 = serial_baud`.

### Baud cross-reference (9600 confirmed)

The **same** serial object (`0x1000e87c`) is re-baud'd at runtime depending on the A/C
link mode — this is the `flash_set_uart_baudrate` / `B9600`/`B115200` path from the notes:

```
f0x1a1f08  cmp   r5, #9
f0x1a1f0a  ite   eq
f0x1a1f0c  mov.w r1, #0x2580          ; 9600   (mode == 9)
f0x1a1f10  mov.w r1, #0x1c200         ; 115200 (otherwise)
f0x1a1f14  ldr   r0, =0x1000e87c      ; SAME obj as serial_init above
f0x1a1f16  bl    0x1c55b8             ; serial_baud(obj, baud)
```

So the A/C UART physically runs **9600 8N1** in the mode that matches the RE-notes protocol,
and 115200 otherwise. Same pins (PA_14/PA_13) in both cases.

## DE/RE direction GPIO — none found

**No `gpio_init` / `gpio_dir` / `gpio_write` appears in the UART bring-up path** (the two
helpers called immediately before `serial_init`, at `0x1a1b74` and `0x1c5a04`, are
link-state/reset bookkeeping, not GPIO). A scan of the whole A/C-UART code window
(`0x1a1000–0x1a6000`) found only the six `serial_*` HAL calls above and no bracketing
GPIO toggle around transmit.

Conclusion: **no software DE/RE control.** The UM3352E is almost certainly wired for
**auto-direction** (DE driven by the TX line via a transistor, or DE tied high / RE tied
low for a permanently-enabled driver — typical for these 4-pin Hisense/Aircon dongles).
Confidence Med-High: absence-of-evidence, but the evidence window (init + TX region) is the
place such a pin would have to be set up, and it is clean.

## Which UART / not the log console

`PA_14 = UART0 TX`, `PA_13 = UART0 RX` is stated verbatim in the SDK:
`project/realtek_amebaz2_v0_example/example_sources/uart_auto_flow_ctrl/src/main.c`
```
#define LOG_UART_TX  PA_14   //UART0  TX
#define LOG_UART_RX  PA_13   //UART0  RX
```
and PA_13/PA_14 is a listed valid UART TX/RX group in `atcmd_sys.c`. The **debug/log
console is a different UART** — `atcmd_sys.c:1856` puts log-UART RX/TX on **PA_16/PA_17**
(or PA_29/PA_30), so PA_14/PA_13 is a dedicated data UART, not the console. `Uart0..Uart2`
are defined in `rtl8710c_uart.h`; the ROM `hal_uart_pin_to_idx` maps the PA_14/PA_13 pair
to index 0 (UART0) and `hal_uart_init` then does `hal_pinmux_register(pin, PID_UART0+idx)`.

## Suggested driver defines

```c
// RTL8710C / AmebaZ2 — Hisense A/C RS-485 bus (UM3352E transceiver)
#define AC_UART_TX     PA_14   // raw 0x0E ; UART0 TX
#define AC_UART_RX     PA_13   // raw 0x0D ; UART0 RX
#define AC_UART_BAUD   9600    // 8N1 (stock also boots 115200, switches to 9600 for the A/C link)
// No DE/RE GPIO: transceiver is auto-direction (leave direction control unused).
```

## What remains uncertain

- **DE/RE**: "none" is inferred from a clean init + TX window, not from a schematic. If the
  board turns out to need it, the pin would be a GPIO configured near the UART bring-up —
  none is there. Verify on the wire / PCB if a driver-enable line matters (Med-High).
- **Second-image base**: taken as `0x9b000000` because the mbed `serial_api` offsets line up
  with the SDK map. Pin *values* (movs immediates) are base-independent, so the PA_14/PA_13
  result does not depend on this assumption (High).
- **Operational baud**: the firmware supports both 9600 and 115200 on these pins; the notes'
  9600 is confirmed as the A/C-link mode. Confirm the exact mode your unit negotiates by
  sniffing if the driver must match dynamically.

_Method: `capstone` (Thumb) over the raw dump; PinName/serial_api/uart HAL cross-checked
against `ameba-dev/ameba-rtos-z2` (disassembly done with throwaway helper scripts in a scratch dir)._
