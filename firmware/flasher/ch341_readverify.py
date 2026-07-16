# Spot-check that the chip content matches a built image at key offsets, + report QE.
# Read-only (no erase/write). Transport: ch341common.
import sys, usb.util
from ch341common import CH341
c=CH341()
jid=c.jedec().hex()
print("JEDEC:", jid, "(c84016=GD25Q32)" if jid=="c84016" else "*** WRONG CHIP ***")
img=open(sys.argv[1],"rb").read()
offsets=[0x0, 0x1000, 0xC000, 0x80000, 0x100000, 0x137000, 0x2FF000, 0x3FF000, 0x3FFF00]
allok=True
for off in offsets:
    got=c.read(off,64); want=img[off:off+64]
    ok = got==want; allok = allok and ok
    print("0x%07X: %s  chip=%s"%(off, "OK " if ok else "DIFF", got[:16].hex()))
    if not ok: print("            want=%s"%want[:16].hex())
print("\n=== SPOT-CHECK %s ==="%("PASS - flash content matches the custom image" if allok else "FAIL - content differs, reflash needed"))
sr2=c.rdsr2()
print("QE bit currently:", (sr2>>1)&1, "(0=needs setting before it will boot)")
usb.util.release_interface(c.d,0)
