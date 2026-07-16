# Set the SR2 QE (quad-enable) bit so the module will boot. Transport: ch341common.
import usb.util
from ch341common import CH341
c=CH341()
sr2=c.rdsr2()
print("SR2 before: 0x%02X (QE=%d)"%(sr2,(sr2>>1)&1))
qe=c.set_qe()
sr2b=c.rdsr2()
print("SR2 after:  0x%02X (QE=%d)"%(sr2b,(sr2b>>1)&1))
print("SUCCESS - QE now set, module should boot" if qe else "FAILED - QE still clear")
usb.util.release_interface(c.d,0)
