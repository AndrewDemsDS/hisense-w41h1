import asyncio
import ms_ws


async def main():
    async with ms_ws.connect(heartbeat=30, hello_timeout=30) as (ws, _):
        r = await ms_ws.call(ws, "get_nodes", {}, "1", timeout=30)
        for n in r.get("result", []):
            nid = n.get("node_id")
            avail = n.get("available")
            attrs = n.get("attributes", {})
            # endpoints from descriptor parts, thermostat cluster = 513
            eps = sorted(set(int(k.split("/")[0]) for k in attrs))
            has_thermo = any(k.startswith(f"{ep}/513") for ep in eps for k in attrs)
            print(
                f"node {nid} available={avail} endpoints={eps} thermostat_cluster={has_thermo}"
            )
            # dump thermostat local temp / mode if present
            for k in ("1/513/0", "1/513/28", "1/513/29", "1/514/0"):
                if k in attrs:
                    print(f"   {k} = {attrs[k]}")


asyncio.run(main())
