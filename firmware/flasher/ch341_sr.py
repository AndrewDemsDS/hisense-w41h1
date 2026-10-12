#!/usr/bin/env python3
"""Read JEDEC id + status registers (SR1/SR2) and report the QE bit. Read-only.
Transport: ch341common."""
import ch341common as ch


def main():
    c = ch.CH341()
    jid = c.jedec().hex(); sr1 = c.rdsr1(); sr2 = c.rdsr2()
    print("JEDEC ID:", jid, "(expect %s for GD25Q32)" % ch.GD25Q32_JEDEC)
    print("SR1: 0x%02X  SR2: 0x%02X" % (sr1, sr2))
    print("QE bit (SR2 bit1):", (sr2 >> 1) & 1, "->",
          "SET (quad enabled)" if (sr2 >> 1) & 1 else "CLEAR (quad DISABLED -> won't boot!)")
    c.close()


if __name__ == "__main__":
    main()
