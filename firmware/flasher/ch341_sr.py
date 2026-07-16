# Read JEDEC id + status registers (SR1/SR2) and report the QE bit. Transport: ch341common.
import usb.util
from ch341common import CH341
c=CH341()
jid=c.jedec().hex(); sr1=c.rdsr1(); sr2=c.rdsr2()
print("JEDEC ID:", jid, "(expect c84016 for GD25Q32)")
print("SR1: 0x%02X  SR2: 0x%02X"%(sr1,sr2))
print("QE bit (SR2 bit1):", (sr2>>1)&1, "->",
      "SET (quad enabled)" if (sr2>>1)&1 else "CLEAR (quad DISABLED -> won't boot!)")
usb.util.release_interface(c.d,0)
