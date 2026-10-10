#!/usr/bin/env python3
"""Host test for firmware/scripts/zap_edit.py and for the data-model contract the two Matter
targets share (beeper endpoint, manufacturer-cluster diagnostic ids). No SDK, no ZAP: it checks
the edit functions and the committed files against each other. The generation itself is
`zap_edit.py check`, which needs the SDK and runs before a build, not here.
"""
import copy
import importlib.util
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
spec = importlib.util.spec_from_file_location("zap_edit", REPO / "firmware/scripts/zap_edit.py")
ze = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ze)

fails = 0


def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print(f"  FAIL  {what}")


def raises(fn, what):
    try:
        fn()
    except ze.EditError:
        return
    check(False, f"{what}: expected a refusal")


MFG = 0xFFF1FC00
raw = ze.ZAP.read_text()
doc = ze.load_zap(ze.ZAP)

# ---- the file is in ZAP's own layout, so an edit here diffs like a GUI save -------------------
check(ze.dump_zap(doc) == raw, ".zap round-trips byte for byte through load/dump")
ids = sorted(e["endpointId"] for e in doc["endpoints"])
check(ids == list(range(len(ids))), f"endpoints contiguous: {ids}")

# ---- clone-endpoint ---------------------------------------------------------------------------
d = copy.deepcopy(doc)
n_types = len(d["endpointTypes"])
new = ze.clone_endpoint(d, 9)
check(new == max(ids) + 1, "clone appends the next endpoint id")
check(len(d["endpointTypes"]) == n_types + 1 and d["endpoints"][-1]["endpointTypeIndex"] == n_types,
      "clone gets its own endpoint type, appended")
check(d["endpointTypes"][:n_types] == doc["endpointTypes"] and d["endpoints"][:-1] == doc["endpoints"],
      "clone leaves every existing endpoint and type untouched")
raises(lambda: ze.clone_endpoint(copy.deepcopy(doc), 9, new + 5), "clone with a gap")
raises(lambda: ze.clone_endpoint(copy.deepcopy(doc), 99), "clone of a missing endpoint")
# The clone is independent: changing it does not reach the source endpoint.
ze.set_attribute(d, new, "On/Off", "OnOff", default=0, storage="NVM")
check(ze.find_attribute(ze.find_cluster(ze.endpoint_type(d, 9), "On/Off"), "OnOff") ==
      ze.find_attribute(ze.find_cluster(ze.endpoint_type(doc, 9), "On/Off"), "OnOff"),
      "editing the clone does not change the endpoint it was copied from")

# ---- add-attribute / set-attribute (metadata from the ZCL XML, here the repo's own cluster) ----
d = copy.deepcopy(doc)
cl = ze.find_cluster(ze.endpoint_type(d, 1), "Hisense Aircon")
cl["attributes"] = [a for a in cl["attributes"] if a["code"] != 0x0014]
a = ze.add_attribute(d, 1, "Hisense Aircon", "ChecksumErrors", ze.LOCAL_XML)
check((a["code"], a["type"], a["defaultValue"], a["storageOption"], a["included"]) == (0x0014, "int32u", "0", "RAM", 1),
      "add-attribute takes code, type and default from the XML")
codes = [x["code"] for x in cl["attributes"]]
check(codes == sorted(codes), "attributes stay in code order, globals last")
check(d == doc, "re-adding a removed attribute reproduces the committed .zap exactly")
before = len(cl["attributes"])
ze.add_attribute(d, 1, "Hisense Aircon", "0x0014", ze.LOCAL_XML, default=7)
check(len(cl["attributes"]) == before and ze.find_attribute(cl, 0x0014)["defaultValue"] == "7",
      "add-attribute on an existing attribute updates it, by code as well as by name")
raises(lambda: ze.add_attribute(d, 1, "Hisense Aircon", "NoSuchThing", ze.LOCAL_XML), "unknown attribute")
raises(lambda: ze.set_attribute(d, 1, "Thermostat", "NoSuchThing", default=1), "set-attribute on a missing one")
raises(lambda: ze.set_attribute(d, 1, "No Such Cluster", "X", default=1), "unknown cluster")
raises(lambda: ze.set_attribute(d, 1, "Thermostat", "SystemMode", storage="DISK"), "unknown storage option")
d2 = copy.deepcopy(doc)
d2["endpoints"][3]["endpointTypeIndex"] = d2["endpoints"][4]["endpointTypeIndex"]
raises(lambda: ze.set_attribute(d2, d2["endpoints"][3]["endpointId"], "On/Off", "OnOff", default=1),
       "an endpoint type shared by two endpoints")

# ---- check helpers ----------------------------------------------------------------------------
log = ("  - ⚠ Check Device Type Compliance on endpoint: 9, device type: X, cluster: Groups server needs to be enabled\n"
       "  - ⚠ Check Device Type Compliance on endpoint: 11, device type: X, cluster: Groups server needs to be enabled\n"
       "  - ⚠ Check Cluster Compliance on endpoint: 0, cluster: Basic Information, mandatory attribute: A\n"
       "unrelated line\n")
check(len(ze.warning_kinds(log)) == 2, "the same warning on another endpoint is one kind, not a new one")
matter = ("cluster OnOff = 6 {\n}\ncluster HisenseAircon = 4294048768 {\n}\n"
          "endpoint 1 {\n  server cluster OnOff {\n    persist  attribute onOff default = 1;\n"
          "    callback attribute attributeList;\n    ram      attribute featureMap default = 2;\n"
          "    handle command Off;\n  }\n}\n")
check(ze.matter_counts(matter) == {(1, "OnOff"): 3}, "generated .matter attribute lines are counted per endpoint cluster")
check(ze.matter_cluster_names(matter) == {6: "OnOff", MFG: "HisenseAircon"}, "cluster codes map to .matter names")

# ---- contract: beeper endpoint, the same on both Matter targets -------------------------------
map_h = (REPO / "firmware/src/rs485-driver/matter_aircon_map.h").read_text()


def define(name):
    m = re.search(rf"^#define {name}\s+(\S+)", map_h, re.M)
    check(m is not None, f"{name} is defined in matter_aircon_map.h")
    return m.group(1) if m else "0"


beeper_ep = int(define("MATTER_EP_BEEPER"), 0)
check(beeper_ep == max(ids), f"the beeper is the last endpoint ({max(ids)}), so nothing before it moved")
bt = ze.endpoint_type(doc, beeper_ep)
check(bt["deviceTypeCode"] == 266, "beeper endpoint is an On/Off plug-in unit, like Eco/Quiet/Turbo/Display")
oo = ze.find_cluster(bt, "On/Off")
onoff = ze.find_attribute(oo, "OnOff")
check(onoff["defaultValue"] == "1", "beeper defaults ON (the stock module beeps)")
check(onoff["storageOption"] == "NVM", "beeper OnOff is persisted by the data model")
check(ze.find_attribute(oo, "FeatureMap")["defaultValue"] == "2",
      "no Lighting feature: without StartUpOnOff the server leaves the restored value alone at boot")
check(ze.find_cluster(bt, "User Label")["enabled"] == 1, "beeper endpoint carries UserLabel (the HA name)")
for src in ("firmware/src/sdk-edits/matter_drivers.cpp", "firmware/esp32-matter/main/app_main.cpp"):
    text = (REPO / src).read_text()
    check("MATTER_LABEL_BEEPER" in text and "MATTER_EP_BEEPER" in text, f"{src} takes the endpoint and label from the map")
check(define("MATTER_LABEL_BEEPER") == '"Beeper"', 'the Home Assistant label contains "Beeper"')

# ---- contract: Thermostat dead band -----------------------------------------------------------
db = ze.find_attribute(ze.find_cluster(ze.endpoint_type(doc, 1), "Thermostat"), "MinSetpointDeadBand")
check(db is not None and db["included"] == 1 and db["defaultValue"] == "0" and db["type"] == "int8s",
      "Thermostat MinSetpointDeadBand is enabled with default 0")

# ---- contract: manufacturer-cluster ids agree in the XML, the id header, the map and the .zap --
xml = {}
for at in ET.parse(ze.LOCAL_XML[0]).getroot().iter("attribute"):
    xml[int(at.get("code"), 0)] = ((at.text or "").strip(), at.get("type"))
hdr = (REPO / "firmware/src/sdk-edits/HisenseAircon-ClusterId.h").read_text()
hdr_ids = {n: int(v, 0) for n, v in re.findall(r"namespace (\w+)\s*\{ inline constexpr AttributeId Id = (\w+); \}", hdr)}
zap_mfg = {a["code"]: a for a in ze.find_cluster(ze.endpoint_type(doc, 1), MFG)["attributes"] if a["code"] < 0xFFF0}
for name, macro in (("ChecksumErrors", "CHECKSUM_ERRORS"), ("ReplyTimeouts", "REPLY_TIMEOUTS"),
                    ("UnansweredCommands", "UNANSWERED_COMMANDS"), ("LinkLosses", "LINK_LOSSES"),
                    ("LinkToken", "LINK_TOKEN"), ("BusLink", "BUS_LINK")):
    code = int(define(f"MATTER_HISENSE_ATTR_{macro}"), 0)
    check(hdr_ids.get(name) == code, f"{name}: id header says {hdr_ids.get(name)}, map says {code}")
    check(xml.get(code, ("", ""))[0] == name, f"{name}: cluster XML has it at 0x{code:04X}")
    z = zap_mfg.get(code)
    check(z is not None and z["included"] == 1 and z["name"] == name and z["type"] == xml.get(code, ("", ""))[1],
          f"{name}: enabled in the .zap with the XML's type")
for code, z in zap_mfg.items():
    check(code in xml and xml[code][0] == z["name"], f".zap attribute 0x{code:04X} {z['name']} exists in the cluster XML")
    check(hdr_ids.get(z["name"]) == code, f".zap attribute {z['name']} has its id in HisenseAircon-ClusterId.h")

print("== ZAP EDIT + DATA-MODEL CONTRACT " + ("OK ==" if not fails else f"FAILED ({fails}) =="))
sys.exit(1 if fails else 0)
