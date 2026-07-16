import asyncio
import ms_ws


async def main():
    nid = ms_ws.node_id(9)
    async with ms_ws.connect(heartbeat=30, hello_timeout=20) as (ws, _):
        seq = [
            ("SystemMode=Off", "1/513/28", 0),
            ("SystemMode=Cool", "1/513/28", 3),
            ("CoolSetpoint=18C", "1/513/17", 1800),
            ("SystemMode=Heat", "1/513/28", 4),
            ("HeatSetpoint=28C", "1/513/18", 2800),
        ]
        for name, path, val in seq:
            r = await ms_ws.call(
                ws,
                "write_attribute",
                {"node_id": nid, "attribute_path": path, "value": val},
                name,
                timeout=30,
            )
            st = (
                r.get("result", [{}])[0].get("Status", "?")
                if isinstance(r.get("result"), list)
                else r
            )
            print("%-18s -> Status %s" % (name, st))
            await asyncio.sleep(4)


asyncio.run(main())
