import asyncio, json, os, sys
import ms_ws

# Wi-Fi the module joins during commissioning. Override via env for your network.
SSID = os.environ.get("MATTER_SSID", "your-ssid")
PW = os.environ.get("MATTER_PSK", "your-password")
CODE = os.environ.get(
    "MATTER_CODE", "34970112332"
)  # manual pairing code (CHIP test default)


async def main(action):
    async with ms_ws.connect(heartbeat=30, hello_timeout=30) as (ws, hello):
        print("server_info:", json.loads(hello.data).get("fabric_id", "?"))
        r = await ms_ws.call(
            ws,
            "set_wifi_credentials",
            {"ssid": SSID, "credentials": PW},
            "1",
            timeout=180,
        )
        print("set_wifi_credentials:", json.dumps(r)[:200])
        if action == "commission":
            print("commissioning with code %s (BLE+wifi, ~2-4 min)..." % CODE)
            r = await ms_ws.call(
                ws,
                "commission_with_code",
                {"code": CODE, "network_only": False},
                "2",
                timeout=180,
            )
            print("commission result:", json.dumps(r)[:800])


asyncio.run(main(sys.argv[1] if len(sys.argv) > 1 else "setwifi"))
