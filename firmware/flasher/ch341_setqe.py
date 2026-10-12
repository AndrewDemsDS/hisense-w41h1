#!/usr/bin/env python3
"""Set the SR2 QE (quad-enable) bit so the module will boot. Writes the status register and
nothing else, and only after the chip answered with the GD25Q32's id. Transport: ch341common."""
import ch341common as ch


def main():
    c = ch.open_chip()
    sr2 = c.rdsr2()
    print("SR2 before: 0x%02X (QE=%d)" % (sr2, (sr2 >> 1) & 1))
    try:
        qe = c.set_qe()
    except ch.FlashTimeout as e:
        raise SystemExit("FAILED - the chip was still busy after the %s" % e)
    sr2b = c.rdsr2()
    print("SR2 after:  0x%02X (QE=%d)" % (sr2b, (sr2b >> 1) & 1))
    print("SUCCESS - QE now set, module should boot" if qe else "FAILED - QE still clear")
    c.close()
    return 0 if qe else 1


if __name__ == "__main__":
    raise SystemExit(main())
