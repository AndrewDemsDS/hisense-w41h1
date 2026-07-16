"""Shared CH341A SPI transport for the GD25Q32 flasher tools.
One CH341 class (USB open, kernel-detach, SPI stream, bit-reversal) + the verify/retry
flash_region(). All ch341_*.py entrypoints import from here so the transport lives in
one place. Use these tools, NOT flashrom (silent partial writes on this CH341A).
"""
import time, usb.core, usb.util
VID,PID=0x1a86,0x5512; EP_OUT,EP_IN=0x02,0x82
SPI=0xA8; UIO=0xAB; OUT=0x80; DIR=0x40; END=0x20; I2C=0xAA; SET=0x60
_REV=[int('{:08b}'.format(i)[::-1],2) for i in range(256)]   # LSB-first bit-reverse LUT
GD25Q32_JEDEC="c84016"

class CH341:
    def __init__(s): s._open()
    def _open(s):
        s.d=usb.core.find(idVendor=VID,idProduct=PID)
        if s.d is None: raise SystemExit("CH341A not found - replug")
        try:
            if s.d.is_kernel_driver_active(0): s.d.detach_kernel_driver(0)
        except: pass
        s.d.set_configuration(); usb.util.claim_interface(s.d,0)
        s.d.write(EP_OUT,bytes([I2C,SET|0x02,0x00]))
    def cs(s,e):
        s.d.write(EP_OUT, bytes([UIO,OUT|0x37,DIR|0x3F,OUT|0x36,END]) if e else bytes([UIO,OUT|0x37,END]))
    def xfer(s,out):
        s.cs(True); res=bytearray(); i=0
        while i<len(out):
            ch=out[i:i+31]
            s.d.write(EP_OUT,bytes([SPI])+bytes(_REV[b] for b in ch))
            got=bytearray()
            while len(got)<len(ch):
                got+=bytes(s.d.read(EP_IN,len(ch)-len(got),timeout=3000))
            res+=bytes(_REV[b] for b in got); i+=31
        s.cs(False); return bytes(res)
    def jedec(s): return s.xfer(bytes([0x9F,0,0,0]))[1:4]
    def rdsr1(s): return s.xfer(bytes([0x05,0xFF]))[1]
    def rdsr2(s): return s.xfer(bytes([0x35,0xFF]))[1]
    def wren(s): s.xfer(bytes([0x06]))
    def wip_wait(s,ms=2000):
        t=time.time()
        while (time.time()-t)*1000<ms:
            if not (s.rdsr1()&1): return True
            time.sleep(0.001)
        return False
    def read(s,addr,n):
        return s.xfer(bytes([0x03,(addr>>16)&0xFF,(addr>>8)&0xFF,addr&0xFF])+bytes(n))[4:]
    def sector_erase(s,addr):
        s.wren(); s.xfer(bytes([0x20,(addr>>16)&0xFF,(addr>>8)&0xFF,addr&0xFF])); s.wip_wait()
    def page_prog(s,addr,data):
        s.wren(); s.xfer(bytes([0x02,(addr>>16)&0xFF,(addr>>8)&0xFF,addr&0xFF])+data); s.wip_wait()
    def set_qe(s):
        sr2=s.rdsr2(); s.wren(); s.xfer(bytes([0x31,sr2|0x02])); s.wip_wait(); return (s.rdsr2()>>1)&1

def flash_region(dev_img, start, length):
    """Write [start,start+length) of dev_img, verify+retry each sector, then set QE.
    Aborts (SystemExit) on wrong chip id or a sector that won't verify -- fails safe."""
    c=CH341()
    jid=c.jedec().hex()
    print("JEDEC:",jid, "OK" if jid==GD25Q32_JEDEC else "*** WRONG ***")
    if jid!=GD25Q32_JEDEC: raise SystemExit("wrong chip id")
    img=open(dev_img,"rb").read()
    end=start+length; SEC=0x1000; nsec=(length+SEC-1)//SEC
    print("writing 0x%X..0x%X (%d sectors, verify+retry each)"%(start,end,nsec))
    done=0; a=start
    while a<end:
        want=img[a:a+SEC]
        if len(want)<SEC: want=want+b'\xff'*(SEC-len(want))
        ok=False
        for attempt in range(4):
            try:
                cur=c.read(a,SEC)
                if cur==want: ok=True; break
                c.sector_erase(a)
                for p in range(0,SEC,256):
                    c.page_prog(a+p, want[p:p+256])
                if c.read(a,SEC)==want: ok=True; break
            except usb.core.USBError:
                time.sleep(0.3)
                try: usb.util.release_interface(c.d,0)
                except: pass
                try: c._open()
                except: pass
        if not ok: raise SystemExit("SECTOR 0x%X failed after retries"%a)
        done+=1
        if done%16==0 or a+SEC>=end:
            print("  %d/%d sectors ok (0x%X)"%(done,nsec,a), flush=True)
        a+=SEC
    print("=== all sectors written+verified ===")
    qe=c.set_qe()
    print("QE bit set:", qe, "-> WILL BOOT" if qe else "-> QE FAILED")
    usb.util.release_interface(c.d,0)
