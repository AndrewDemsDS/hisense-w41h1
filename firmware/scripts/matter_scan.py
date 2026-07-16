import asyncio
from bleak import BleakScanner
MATTER_UUID = "0000fff6-0000-1000-8000-00805f9b34fb"
found = {}
def cb(dev, adv):
    sd = adv.service_data or {}
    if MATTER_UUID in sd or any("fff6" in u.lower() for u in (adv.service_uuids or [])):
        data = sd.get(MATTER_UUID, b"")
        disc = None
        if len(data) >= 3:
            disc = ((data[2] & 0x0F) << 8) | data[1]  # 12-bit discriminator
        if dev.address not in found:
            found[dev.address] = True
            print(f"*** MATTER BEACON: {dev.address} rssi={adv.rssi} "
                  f"discriminator={disc} (0x{disc:03x}) sdata={data.hex()}")
            print("    => IT BOOTED. Secure boot is OFF. Custom firmware viable.")
async def main():
    print("scanning 40s for Matter commissioning beacon (0xFFF6)...")
    s = BleakScanner(detection_callback=cb)
    await s.start(); await asyncio.sleep(40); await s.stop()
    if not found:
        print("no Matter beacon seen. Either not powered yet, needs more time, "
              "or image didn't boot (possible secure boot). Re-run / check power.")
asyncio.run(main())
