# Renode spike: running the driver on an emulated RTL8710C (Layer 3)

> ⚠️ **NOT RUNNABLE AS-IS.** This is a scaffold only, `hisense_qa.resc` references a
> `driver_test.elf` that isn't built/committed. To use it you must install Renode and
> build a bare-metal driver-test ELF first (see "the honest path" below). For a turnkey
> no-hardware run use Layers 1–2 (`firmware/test/run_tests.sh`) and the OTA sim
> (`firmware/test/sim_ota_convert.sh`) instead.

Renode runs *unmodified* Cortex-M firmware against emulated cores + peripherals,
deterministically, in CI, the way to exercise the **real** driver code (bus
task, IRQ RX, TX queue) with no physical chip. This directory is a **spike**: a
platform scaffold + run script + the honest path to make it useful.

## What's here

- `rtl8710c.repl`: platform: Cortex-M33 + the memory map the firmware links
  against (`rtl8710c_ram_matter.ld`) + UART0 @ `0x40003000`.
- `hisense_qa.resc`: loads a firmware ELF, bridges UART0 to TCP `:3456`, so the
  Python virtual A/C can be the other end of the bus.

## The honest scope (read before sinking time in)

RTL8710C is **not** a built-in Renode platform. Two realistic targets:

1. **Minimal bare-metal driver test (recommended first).** Build a tiny ELF that
   links `hisense_rs485.cpp` + a Cortex-M startup + a thin UART shim mapping
   `serial_*`/`serial_irq_*` onto the modeled UART0 registers, and a `main()`
   that sends a status-request and a few commands. This exercises the *actual*
   RX reassembly / un-stuffing / TX stuffing / bus-task logic against the
   virtual A/C, the part `hal_stub.h` (Layer 1) can't cover, with only ~one
   peripheral to model. **Highest value per effort.**

2. **Full stock/Matter image boot (hard).** Booting `flash_is.bin` also needs the
   ROM bootloader, the XIP-flash controller, clock/PMU, pinmux and the BLE/WiFi
   blocks modeled. That's a multi-day platform-bring-up, disproportionate to the
   QA goal. Only worth it if you want full-stack (Matter↔driver) emulation.

The UART model is the crux: the RTL8710C block is DesignWare-APB/16550-style.
`UART.NS16550` is the starting guess in the `.repl`; if the driver's `serial_putc`/
`serial_getc`/`serial_readable`/`RxIrq` don't behave, replace it with a small
Renode **Python peripheral** modeling just THR/RBR + LSR (data-ready/THR-empty) +
the RX-IRQ line, that's the entire surface `hisense_rs485.cpp` uses.

## Run (once Renode is installed)

```bash
# 1) point $bin in hisense_qa.resc at your driver-test ELF, then:
renode firmware/test/renode/hisense_qa.resc
# 2) attach the virtual A/C to UART0 (native TCP, no socat needed):
python3 firmware/test/virtual_ac.py --connect 127.0.0.1:3456
# 3) in the Renode monitor:
(w41h1) start
```

You should see the driver poll → the virtual A/C answer with a 160-byte status →
the driver parse it, and commands mutate the simulated A/C's state, all on the
emulated core, in CI, no hardware.

## Where this fits

Layers 1–2 (`../run_tests.sh`) already cover the codec + protocol round-trip and
need nothing but a PC. Renode (Layer 3) is the increment that runs the *firmware
code* on the *real core*; it's the piece that turns "no chip" from a gap into a
CI job. See `firmware/docs/04-qa-strategy.md` for the whole picture.
