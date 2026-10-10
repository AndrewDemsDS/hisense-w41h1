#!/usr/bin/env python3
"""zap_edit.py: change the AmebaZ2 data model (.zap) without the ZAP GUI, and check the result.

The .zap is JSON that ZAP itself writes with a fixed layout (2-space indent, no trailing newline),
so an edit made here is byte-identical to a GUI save for everything it does not touch. Attribute
metadata (name, type) is read from the same ZCL XML that ZAP loads, never typed by hand.

  zap_edit.py list
  zap_edit.py add-attribute  --endpoint N --cluster C --attribute A [--default V] [--storage S]
  zap_edit.py set-attribute  --endpoint N --cluster C --attribute A [--default V] [--storage S]
  zap_edit.py clone-endpoint --from N [--id M]
  zap_edit.py check [--baseline REF] [--keep DIR]

C and A take a name ("Thermostat", "MinSetpointDeadBand") or a code (0x0201, 25). S is one of
RAM, NVM, External. `clone-endpoint` appends endpoint M (default: the next free id) with its own
copy of endpoint N's endpoint type, so endpoints stay contiguous and nothing is renumbered.

`check` runs what the build runs (the GENERATE_ZAP steps of the SDK Makefile) on a scratch copy
and fails when:
  * a generation step exits non-zero,
  * ZAP prints a warning of a kind the baseline .zap (git REF, default HEAD) does not print,
  * the generated endpoint array is not contiguous or does not match the .zap,
  * an attribute enabled in the .zap is missing from the generated .matter.
The last one is the trap a hand-edited cluster block falls into: valid JSON that codegen ignores.

The SDK is found through --sdk, $SDK_ROOT, or the `sdk` symlink at the repo root. Only `check`
and the lookup of standard-cluster attributes need it. Never run zap_regen_all.py instead.
"""
import argparse
import copy
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
ZAP = REPO / "firmware/src/sdk-edits/room-air-conditioner-app.zap"
LOCAL_XML = [REPO / "firmware/src/sdk-edits/hisense-aircon-cluster.xml"]
STORAGE = ("RAM", "NVM", "External")
EXAMPLES_REL = "ameba-rtos-z2/component/common/application/matter/examples"
EXPECTED_REL = "ameba-rtos-z2/component/common/application/matter/tools/codegen_helpers/expected.outputs"
TEMPLATES = ("src/app/zap-templates/matter-idl-server.json", "src/app/zap-templates/app-templates.json")


class EditError(Exception):
    pass


# ---- .zap file ------------------------------------------------------------------------------
def load_zap(path):
    return json.loads(Path(path).read_text())


def dump_zap(doc):
    """ZAP's own serialisation: 2-space indent, non-ASCII escaped, no trailing newline."""
    return json.dumps(doc, indent=2)


def num(text):
    """'0x19', '25' -> int; anything else -> None."""
    try:
        return int(str(text), 0)
    except ValueError:
        return None


def endpoint_type(doc, endpoint_id):
    """The endpoint type behind an endpoint. Refuses a type two endpoints share: an edit meant for
    one would silently change the other."""
    for ep in doc["endpoints"]:
        if ep["endpointId"] == endpoint_id:
            idx = ep["endpointTypeIndex"]
            users = [e["endpointId"] for e in doc["endpoints"] if e["endpointTypeIndex"] == idx]
            if len(users) > 1:
                raise EditError(f"endpoints {users} share one endpoint type; edit would change all of them")
            return doc["endpointTypes"][idx]
    raise EditError(f"endpoint {endpoint_id} is not in the .zap")


def find_cluster(etype, cluster, side="server"):
    code = num(cluster)
    for c in etype["clusters"]:
        if c["side"] == side and (c["code"] == code if code is not None else c["name"] == cluster):
            return c
    raise EditError(f"no {side} cluster {cluster!r} on that endpoint")


def find_attribute(cl, attribute):
    code = num(attribute)
    for a in cl.get("attributes", []):
        if a["code"] == code if code is not None else a["name"] == attribute:
            return a
    return None


# ---- ZCL metadata ---------------------------------------------------------------------------
def zcl_xml_files(sdk):
    """The XML files ZAP loads, in zcl.json order, plus the repo's own manufacturer cluster."""
    out = list(LOCAL_XML)
    if sdk:
        zcl = Path(sdk) / "connectedhomeip/src/app/zap-templates/zcl/zcl.json"
        if zcl.is_file():
            cfg = json.loads(zcl.read_text())
            for name in cfg["xmlFile"]:
                for root in cfg["xmlRoot"]:
                    p = zcl.parent / root / name
                    if p.is_file():
                        out.append(p)
                        break
    return out


def zcl_attribute(xml_files, cluster_code, attribute):
    """(name, code, type, default) of a server attribute as the ZCL XML defines it."""
    want = num(attribute)
    for path in xml_files:
        try:
            root = ET.parse(path).getroot()
        except ET.ParseError:
            continue
        for cl in root.iter("cluster"):
            if num((cl.findtext("code") or "").strip()) != cluster_code:
                continue
            for a in cl.iter("attribute"):
                if a.get("side") != "server":
                    continue
                name = a.get("name") or (a.text or "").strip()
                code = num(a.get("code"))
                if code == want if want is not None else name == attribute:
                    return name, code, a.get("type"), a.get("default")
    raise EditError(f"attribute {attribute!r} of cluster 0x{cluster_code:04X} is not in the ZCL XML "
                    "(is the SDK reachable? see --sdk)")


# ---- edits (pure: doc in, doc changed in place) ---------------------------------------------
def set_attribute(doc, endpoint_id, cluster, attribute, default=None, storage=None):
    cl = find_cluster(endpoint_type(doc, endpoint_id), cluster)
    a = find_attribute(cl, attribute)
    if a is None:
        raise EditError(f"{attribute!r} is not in cluster {cl['name']} on endpoint {endpoint_id}; use add-attribute")
    _apply(a, default, storage)
    a["included"] = 1
    return a


def add_attribute(doc, endpoint_id, cluster, attribute, xml_files, default=None, storage=None):
    cl = find_cluster(endpoint_type(doc, endpoint_id), cluster)
    name, code, typ, zcl_default = zcl_attribute(xml_files, cl["code"], attribute)
    a = find_attribute(cl, code)
    if a is None:
        a = {"name": name, "code": code, "mfgCode": None, "side": "server", "type": typ, "included": 1,
             "storageOption": "RAM", "singleton": 0, "bounded": 0, "defaultValue": zcl_default,
             "reportable": 1, "minInterval": 1, "maxInterval": 65534, "reportableChange": 0}
        attrs = cl.setdefault("attributes", [])
        attrs.append(a)
        attrs.sort(key=lambda x: x["code"])   # ZAP keeps them in code order, globals last
    a["included"] = 1
    _apply(a, default, storage)
    return a


def _apply(a, default, storage):
    if storage is not None:
        if storage not in STORAGE:
            raise EditError(f"storage must be one of {', '.join(STORAGE)}")
        a["storageOption"] = storage
    if default is not None:
        a["defaultValue"] = str(default)


def clone_endpoint(doc, from_id, new_id=None):
    ids = [e["endpointId"] for e in doc["endpoints"]]
    nxt = max(ids) + 1
    new_id = nxt if new_id is None else new_id
    if new_id != nxt:
        raise EditError(f"new endpoint must be {nxt} (append only: endpoints stay contiguous)")
    src = next((e for e in doc["endpoints"] if e["endpointId"] == from_id), None)
    if src is None:
        raise EditError(f"endpoint {from_id} is not in the .zap")
    etype = copy.deepcopy(doc["endpointTypes"][src["endpointTypeIndex"]])
    etype["id"] = max(t["id"] for t in doc["endpointTypes"]) + 1
    doc["endpointTypes"].append(etype)
    ep = copy.deepcopy(src)
    ep["endpointTypeIndex"] = len(doc["endpointTypes"]) - 1
    ep["endpointId"] = new_id
    doc["endpoints"].append(ep)
    return new_id


# ---- check: the build's generation, on a scratch copy ---------------------------------------
WARN = re.compile(r"^\s*-\s*⚠\s*(.*)$")


def warning_kinds(log):
    """ZAP's compliance warnings with the endpoint number taken out, so a warning an existing
    endpoint of the same shape already produces is not counted as new on a cloned endpoint."""
    out = set()
    for line in log.splitlines():
        m = WARN.match(line)
        if m:
            out.add(re.sub(r"endpoint: \d*", "endpoint: N", m.group(1).strip()))
    return out


def included_counts(doc):
    """{(endpointId, cluster code): enabled server attribute count} from the .zap."""
    out = {}
    for ep in doc["endpoints"]:
        for c in doc["endpointTypes"][ep["endpointTypeIndex"]]["clusters"]:
            if c["side"] == "server" and c["enabled"]:
                out[(ep["endpointId"], c["code"])] = sum(1 for a in c.get("attributes", []) if a["included"])
    return out


def matter_counts(text):
    """{(endpointId, cluster name): attribute count} from a generated .matter file."""
    out, ep, cl = {}, None, None
    for line in text.splitlines():
        m = re.match(r"^endpoint (\d+) \{", line)
        if m:
            ep, cl = int(m.group(1)), None
            continue
        m = re.match(r"^  server cluster (\w+) \{", line)
        if m and ep is not None:
            cl = m.group(1)
            out[(ep, cl)] = 0
            continue
        if ep is not None and cl and re.match(r"^    (ram|callback|persist)\s+attribute ", line):
            out[(ep, cl)] += 1
    return out


def matter_cluster_names(text):
    """{cluster code: name} from the .matter cluster definitions."""
    return {int(c, 0): n for n, c in re.findall(r"^(?:provisional |internal )*cluster (\w+) = (\w+) \{", text, re.M)}


def chip_env(sdk):
    act = f"{sdk}/connectedhomeip/scripts/activate.sh"
    r = subprocess.run(["bash", "-c", 'source "$1" >/dev/null 2>&1 || exit 97; env -0', "bash", act],
                       stdout=subprocess.PIPE, cwd=f"{sdk}/connectedhomeip")
    if r.returncode != 0:
        raise EditError(f"could not source {act}")
    return dict(kv.split("=", 1) for kv in r.stdout.decode(errors="replace").split("\0") if "=" in kv)


def generate(sdk, env, zap_text, out):
    """The SDK Makefile's GENERATE_ZAP on a scratch copy. Returns (ok, log, .matter text, config)."""
    chip = Path(sdk) / "connectedhomeip"
    # Next to the real example dir, so the .zap's relative package paths resolve as in the build.
    # No ".zap" in the directory name: generate.py derives the .matter path with a plain
    # str.replace(".zap", ".matter") over the whole path.
    work = Path(tempfile.mkdtemp(prefix="datamodel-check.", dir=Path(sdk) / EXAMPLES_REL))
    log, ok = "", True
    try:
        zap = work / "room-air-conditioner-app.zap"
        zap.write_text(zap_text)
        gen = Path(out) / "zap-generated"
        gen.mkdir(parents=True, exist_ok=True)
        steps = [["python3", "scripts/tools/zap/generate.py", "--no-prettify-output", "--templates", t,
                  "-z", "src/app/zap-templates/zcl/zcl.json", "--output-dir", str(gen), str(zap)] for t in TEMPLATES]
        steps.append(["python3", "scripts/codegen.py", "--generator", "cpp-app", "--output-dir", str(out),
                      "--expected-outputs", str(Path(sdk) / EXPECTED_REL), str(zap.with_suffix(".matter"))])
        steps.append(["python3", "src/app/zap_cluster_list.py", "--zap_file", str(zap)])
        for cmd in steps:
            r = subprocess.run(cmd, cwd=chip, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, errors="replace")
            log += f"$ {' '.join(cmd[:3])}\n{r.stdout}\n"
            if r.returncode != 0:
                ok = False
                log += f"exit status {r.returncode}\n"
                break
        matter = zap.with_suffix(".matter")
        cfg = gen / "endpoint_config.h"
        return ok, log, matter.read_text() if matter.is_file() else "", cfg.read_text() if cfg.is_file() else ""
    finally:
        shutil.rmtree(work, ignore_errors=True)


def check(sdk, zap_path, baseline_ref, keep=None):
    if not sdk or not (Path(sdk) / "connectedhomeip").is_dir():
        raise EditError("check needs the SDK (--sdk, $SDK_ROOT or the `sdk` symlink)")
    env = chip_env(sdk)
    # ZAP reads the manufacturer cluster from the SDK tree. The build's mirror sync puts the repo
    # copy there; do the same, or an attribute added to the XML here is unknown to ZAP and dropped.
    zcl_dir = Path(sdk) / "connectedhomeip/src/app/zap-templates/zcl/data-model/chip"
    for x in LOCAL_XML:
        if not (zcl_dir / x.name).is_file() or (zcl_dir / x.name).read_bytes() != x.read_bytes():
            shutil.copy(x, zcl_dir / x.name)
            print(f"synced {x.name} into the SDK (as `dev.py ota amebaz2 build` does)")
    rel =Path(zap_path).resolve().relative_to(REPO)
    base = subprocess.run(["git", "show", f"{baseline_ref}:{rel.as_posix()}"], cwd=REPO,
                          stdout=subprocess.PIPE, text=True)
    if base.returncode != 0:
        raise EditError(f"no baseline: git show {baseline_ref}:{rel} failed")
    problems = []
    with tempfile.TemporaryDirectory(prefix="zapcheck-") as tmp:
        b_ok, b_log, _, _ = generate(sdk, env, base.stdout, Path(tmp) / "base")
        if not b_ok:
            raise EditError(f"the baseline itself does not generate:\n{b_log[-2000:]}")
        text = Path(zap_path).read_text()
        ok, log, matter, cfg = generate(sdk, env, text, Path(tmp) / "new")
        if keep:   # the generated files and both logs, for a closer look
            shutil.copytree(tmp, keep, dirs_exist_ok=True)
            (Path(keep) / "new.matter").write_text(matter)
            (Path(keep) / "new.log").write_text(log)
            (Path(keep) / "base.log").write_text(b_log)
    if not ok:
        problems.append("generation failed:\n" + log[-3000:])
    doc = json.loads(text)
    if text != dump_zap(doc):
        problems.append(".zap is not in ZAP's own layout (2-space JSON, no trailing newline)")
    new_kinds = warning_kinds(log) - warning_kinds(b_log)
    problems += [f"new ZAP warning: {w}" for w in sorted(new_kinds)]
    ids = sorted(e["endpointId"] for e in doc["endpoints"])
    if ids != list(range(len(ids))):
        problems.append(f"endpoints not contiguous: {ids}")
    m = re.search(r"#define FIXED_ENDPOINT_ARRAY \{([^}]*)\}", cfg)
    built = [int(x, 0) for x in m.group(1).split(",")] if m else []
    if built != ids:
        problems.append(f"generated FIXED_ENDPOINT_ARRAY {built} != .zap endpoints {ids}")
    names, got = matter_cluster_names(matter), matter_counts(matter)
    for (ep, code), want in sorted(included_counts(doc).items()):
        have = got.get((ep, names.get(code, "?")))
        if have != want:
            problems.append(f"endpoint {ep} cluster 0x{code:04X} ({names.get(code, 'not generated')}): "
                            f".zap enables {want} attributes, generated .matter has {have}")
    n_warn, n_base = len(warning_kinds(log)), len(warning_kinds(b_log))
    print(f"generation: {'ok' if ok else 'FAILED'}; endpoints {ids}; "
          f"{sum(included_counts(doc).values())} enabled server attributes, all generated: "
          f"{not any('.zap enables' in p for p in problems)}")
    lines = [sum(1 for ln in t.splitlines() if WARN.match(ln)) for t in (log, b_log)]
    print(f"ZAP warning kinds: {n_warn} (baseline {baseline_ref}: {n_base}); new: {len(new_kinds)}. "
          f"Warning lines over both passes: {lines[0]} (baseline {lines[1]})")
    return problems


# ---- CLI ------------------------------------------------------------------------------------
def default_sdk():
    for cand in (os.environ.get("SDK_ROOT"), REPO / "sdk"):
        if cand and Path(cand).is_dir():
            return str(Path(cand).resolve())
    return None


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--zap", default=str(ZAP))
    p.add_argument("--sdk", default=default_sdk())
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    for name in ("add-attribute", "set-attribute"):
        s = sub.add_parser(name)
        s.add_argument("--endpoint", type=int, required=True)
        s.add_argument("--cluster", required=True)
        s.add_argument("--attribute", required=True)
        s.add_argument("--default")
        s.add_argument("--storage", choices=STORAGE)
    s = sub.add_parser("clone-endpoint")
    s.add_argument("--from", dest="src", type=int, required=True)
    s.add_argument("--id", type=int)
    s = sub.add_parser("check")
    s.add_argument("--baseline", default="HEAD")
    s.add_argument("--keep", help="directory to keep the generated files and logs in")
    a = p.parse_args(argv)
    try:
        if a.cmd == "check":
            problems = check(a.sdk, a.zap, a.baseline, a.keep)
            for line in problems:
                print(f"FAIL: {line}")
            print("zap check: " + ("FAILED" if problems else "OK"))
            return 1 if problems else 0
        doc = load_zap(a.zap)
        if a.cmd == "list":
            for ep in sorted(doc["endpoints"], key=lambda e: e["endpointId"]):
                t = doc["endpointTypes"][ep["endpointTypeIndex"]]
                clusters = ", ".join(c["name"] for c in t["clusters"] if c["enabled"] and c["side"] == "server")
                print(f"ep{ep['endpointId']:<3} {t['deviceTypeName']:<24} {clusters}")
            return 0
        if a.cmd == "clone-endpoint":
            new = clone_endpoint(doc, a.src, a.id)
            print(f"endpoint {new} added (own copy of endpoint {a.src}'s type)")
        else:
            if a.cmd == "add-attribute":
                at = add_attribute(doc, a.endpoint, a.cluster, a.attribute, zcl_xml_files(a.sdk), a.default, a.storage)
            else:
                at = set_attribute(doc, a.endpoint, a.cluster, a.attribute, a.default, a.storage)
            print(f"endpoint {a.endpoint} {a.cluster}: {at['name']} (0x{at['code']:04X}, {at['type']}) "
                  f"default={at['defaultValue']} storage={at['storageOption']}")
        Path(a.zap).write_text(dump_zap(doc))
        return 0
    except EditError as e:
        print(f"zap_edit: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
