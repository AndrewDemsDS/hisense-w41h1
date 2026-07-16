import asyncio, json
import ms_ws


async def main():
    nid = ms_ws.node_id(9)
    # wait for node available
    for attempt in range(20):
        try:
            async with ms_ws.connect(heartbeat=30, hello_timeout=15) as (ws, _):
                r = await ms_ws.call(ws, "get_node", {"node_id": nid}, "0", timeout=20)
                if r.get("result", {}).get("available"):
                    print(f"node{nid} available")
                    r = await ms_ws.call(
                        ws, "check_node_update", {"node_id": nid}, "1", timeout=60
                    )
                    print("check_node_update:", json.dumps(r.get("result"))[:250])
                    if r.get("result"):
                        print(">>> update_node -> v2 (OTA transfer starts) ...")
                        r = await ms_ws.call(
                            ws,
                            "update_node",
                            {"node_id": nid, "software_version": 12},
                            "2",
                            timeout=300,
                        )
                        print("update_node result:", json.dumps(r)[:400])
                    return
        except Exception as e:
            pass
        await asyncio.sleep(10)
    print(f"node{nid} never became available")


asyncio.run(main())
