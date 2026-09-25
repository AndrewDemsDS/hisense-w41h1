#!/usr/bin/env python3
"""C/C++ lint for every tree we own: clang-format, custom rules, clang-tidy.

Driven by firmware/scripts/cpp-lint.sh, which resolves the pinned tools and passes them in.
Usable directly too:

    cpp-lint.py check [--clang-format BIN] [--clang-tidy BIN] [--no-tidy] [PATH...]
    cpp-lint.py fix   [--clang-format BIN] [--clang-tidy BIN] [--no-tidy] [PATH...]
    cpp-lint.py files [PATH...]          list the in-scope files
    cpp-lint.py compdb OUT_DIR           write compile_commands.json for the host-compilable set

PATH narrows the run to files or directories; out-of-scope paths are skipped silently, so the
pre-commit hook can pass the whole staged list.

The custom rules are the ones from ESPHome's script/ci-custom.py that make sense outside ESPHome
(adapted, MIT licensed like the original): no integer-constant #define where constexpr works,
braces around a lone ESP_LOG body, no `byte` type, no sprintf/scanf/std::bind, inclusive
language, #pragma once, and the whitespace hygiene checks. A line containing NOLINT is exempt,
the same escape hatch ESPHome uses; say why next to it.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import fnmatch
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

CPP_EXT = (".c", ".cc", ".cpp", ".h", ".hpp", ".tcc")

# Trees we own. Everything else (SDK copies, patches, the HACS submodule) is out of scope.
INCLUDE = [
    "firmware/src/rs485-driver/*",
    "firmware/esp32-matter/main/*",
    "firmware/esp32-matter/components/hisense_hal/*",
    "firmware/esp32-matter/smoketest/*",
    "firmware/esp32-recon/*",
    "firmware/esphome/components/hisense_ac/*",
    "firmware/test/*",
]

# Never lint or reformat these, even if a path argument names them. sdk-edits holds copies of
# Realtek/CHIP example files and generated captures; reformatting them breaks every SDK rebase.
EXCLUDE = [
    "firmware/src/sdk-edits/*",
    "patches/*",
    "sdk/*",
    "dumps/*",
    "integrations/*",
    "*/build/*",
    "*/managed_components/*",
]

# Safety net for a vendored file dropped into an owned tree: a proprietary banner means hands off.
VENDOR_BANNER = re.compile(r"confidential", re.IGNORECASE)

# Integer-constant #define is allowed where a macro is the only thing that works.
NO_DEFINES_EXEMPT = {
    # C-API headers: included from C translation units (esp32-recon) and inside extern "C", and
    # test_diag_contract.py parses the HISENSE_FEAT1_/HISENSE_FAULT1_ #defines as the contract
    # with the HACS integration. constexpr is C++ only, so these stay macros.
    "firmware/src/rs485-driver/hisense_rs485.h",
    "firmware/src/rs485-driver/matter_aircon_map.h",
    "firmware/src/rs485-driver/esphome_aircon_map.h",
    "firmware/src/rs485-driver/power_estimate.h",
    # Stand-ins for SDK headers (PinNames.h, FreeRTOS.h): they must look like the real macros.
    "firmware/test/hal_stub.h",
    "firmware/esp32-matter/components/hisense_hal/include/*",
}

# Headers that are one-line shims mirroring an SDK header name; hal_stub.h carries the guard.
PRAGMA_ONCE_EXEMPT = {"firmware/test/stubinc/*"}


def _match(rel: str, patterns) -> bool:
    return any(fnmatch.fnmatch(rel, p) for p in patterns)


def tracked_files() -> list[str]:
    out = subprocess.run(
        ["git", "-C", str(ROOT), "ls-files", "-z"], capture_output=True, check=True
    ).stdout.decode()
    return [f for f in out.split("\0") if f]


def in_scope(rel: str) -> bool:
    if not rel.endswith(CPP_EXT) or not _match(rel, INCLUDE) or _match(rel, EXCLUDE):
        return False
    try:
        head = (ROOT / rel).read_text(errors="replace").splitlines()[:40]
    except OSError:
        return False
    return not VENDOR_BANNER.search("\n".join(head))


def select_files(paths: list[str]) -> list[str]:
    files = [f for f in tracked_files() if in_scope(f)]
    if not paths:
        return files
    wanted = []
    for p in paths:
        ap = Path(p)
        ap = (ap if ap.is_absolute() else Path.cwd() / ap).resolve()
        try:
            wanted.append(ap.relative_to(ROOT).as_posix())
        except ValueError:
            continue
    return [f for f in files if any(f == w or f.startswith(w.rstrip("/") + "/") for w in wanted)]


# ---------------------------------------------------------------------------------------------
# Custom rules (adapted from ESPHome script/ci-custom.py)
# ---------------------------------------------------------------------------------------------

CPP_ONLY = (".cc", ".cpp", ".h", ".hpp", ".tcc")  # constexpr needs C++; .c files keep #define

DEFINE_RE = re.compile(
    r"^#define\s+([a-zA-Z0-9_]+)\s+(0b[10]+|0x[0-9a-fA-F]+|\d+)\s*?(?://.*?)?$", re.MULTILINE
)
BYTE_RE = re.compile(r"[^\w]byte +\w+\s*=")
SPRINTF_RE = re.compile(r"[^\w](v?sprintf)\s*\(")
SCANF_RE = re.compile(r"[^\w]((?:std::)?v?[fs]?scanf)\s*\(")
BIND_RE = re.compile(r"[^\w]std\s*::\s*bind\s*\(")
INCLUSIVE_RE = re.compile(r"(whitelist|blacklist|slave)", re.IGNORECASE)
TRAILING_WS_RE = re.compile(r"[\t\r\f\v ]+$", re.MULTILINE)
# An if/else/for/while whose body is a lone log call. When the log level compiles the macro out
# the body becomes empty (-Wempty-body) and the next statement silently becomes the body.
LOG_BRACES_RE = re.compile(
    r"(?:\bif\s*\([^{};]*\)|\bwhile\s*\([^{};]*\)|\bfor\s*\((?:[^{}()]|\([^{}()]*\))*\)|\belse\b)"
    r"[ \t]*\n?[ \t]*(?:ESP_LOG[A-Z]*|esph_log_[a-z]+)\s*\(",
    re.MULTILINE,
)


def mask_comments_strings(s: str) -> str:
    """Blank comments and string/char literals (length and newlines kept), so regexes see code."""
    out = list(s)
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if c == "R" and i + 1 < n and s[i + 1] == '"':
            j = i + 2
            delim = ""
            while j < n and s[j] not in "( \t\r\n\\" and len(delim) < 16:
                delim += s[j]
                j += 1
            if j < n and s[j] == "(":
                closing = ")" + delim + '"'
                end = s.find(closing, j + 1)
                end = n if end == -1 else end + len(closing)
                for k in range(i, end):
                    if s[k] != "\n":
                        out[k] = " "
                i = end
                continue
            i += 1
        elif c == "/" and i + 1 < n and s[i + 1] == "/":
            while i < n and s[i] != "\n":
                out[i] = " "
                i += 1
        elif c == "/" and i + 1 < n and s[i + 1] == "*":
            out[i] = out[i + 1] = " "
            i += 2
            while i < n and not (s[i] == "*" and i + 1 < n and s[i + 1] == "/"):
                if s[i] != "\n":
                    out[i] = " "
                i += 1
            for k in (i, i + 1):
                if k < n:
                    out[k] = " "
            i += 2
        elif c == '"' or (c == "'" and not (i and (s[i - 1].isalnum() or s[i - 1] == "_"))):
            quote = c
            out[i] = " "
            i += 1
            while i < n:
                if s[i] == "\\":
                    out[i] = " "
                    if i + 1 < n:
                        out[i + 1] = " "
                    i += 2
                    continue
                if s[i] == quote:
                    out[i] = " "
                    i += 1
                    break
                if s[i] != "\n":
                    out[i] = " "
                i += 1
        else:
            i += 1
    return "".join(out)


def _line_of(content: str, pos: int) -> tuple[int, int]:
    line_start = content.rfind("\n", 0, pos) + 1
    return content.count("\n", 0, pos) + 1, pos - line_start + 1


def _line_text(content: str, lineno: int) -> str:
    lines = content.split("\n")
    return lines[lineno - 1] if 0 < lineno <= len(lines) else ""


def check_rules(rel: str, content: str) -> list[tuple[int, int, str]]:
    errs: list[tuple[int, int, str]] = []
    ext = os.path.splitext(rel)[1]

    def re_rule(regex, text, msg_fn):
        for m in regex.finditer(text):
            ln, col = _line_of(content, m.start())
            if "NOLINT" in _line_text(content, ln):
                continue
            errs.append((ln, col, msg_fn(m)))

    if "\t" in content:
        ln, col = _line_of(content, content.index("\t"))
        errs.append((ln, col, "tab character; indent with spaces"))
    if "\r" in content:
        ln, col = _line_of(content, content.index("\r"))
        errs.append((ln, col, "Windows newline; use LF (fixable)"))
    if content and not content.endswith("\n"):
        errs.append((content.count("\n") + 1, 1, "file does not end with a newline (fixable)"))
    re_rule(TRAILING_WS_RE, content, lambda m: "trailing whitespace (fixable)")

    masked = mask_comments_strings(content)
    if ext in CPP_ONLY and not _match(rel, NO_DEFINES_EXEMPT):
        re_rule(
            DEFINE_RE,
            content,
            lambda m: (
                f"#define for an integer constant; use `static constexpr int {m.group(1)} = "
                f"{m.group(2)};` (pick the type the literal had). Keep the macro only if it is used "
                "by #if, overridden with -D, or shared with C, and say so with // NOLINT"
            ),
        )
    re_rule(BYTE_RE, masked, lambda m: "`byte` type; use uint8_t")
    re_rule(SPRINTF_RE, masked, lambda m: f"{m.group(1)}() has no bound; use snprintf()")
    re_rule(SCANF_RE, masked, lambda m: f"{m.group(1)}() pulls in the scanf family; parse by hand")
    re_rule(BIND_RE, masked, lambda m: "std::bind(); use a lambda")
    re_rule(
        INCLUSIVE_RE,
        content,
        lambda m: f"'{m.group(1)}'; use allowlist/denylist, or responder/target for a bus device",
    )
    if ext in (".h", ".hpp") and not _match(rel, PRAGMA_ONCE_EXEMPT):
        if "#pragma once" not in content:
            errs.append((1, 1, "header has no #pragma once"))
    if "ESP_LOG" in content or "esph_log_" in content:
        for m in LOG_BRACES_RE.finditer(masked):
            ln, col = _line_of(content, m.start())
            line_start = content.rfind("\n", 0, m.start()) + 1
            if content[line_start : m.start()].lstrip().startswith("#"):
                continue
            end = content.find(";", m.end())
            stmt = content[m.start() : (end if end != -1 else m.end())]
            nl = content.find("\n", end if end != -1 else m.end())
            if "NOLINT" in content[line_start : nl if nl != -1 else len(content)] or "NOLINT" in stmt:
                continue
            errs.append((ln, col, "lone log call as an if/else/for/while body; wrap it in { }"))
    return sorted(errs)


def fix_rules(content: str) -> str:
    content = content.replace("\r\n", "\n").replace("\r", "\n")
    content = TRAILING_WS_RE.sub("", content)
    if content and not content.endswith("\n"):
        content += "\n"
    return content


# ---------------------------------------------------------------------------------------------
# clang-tidy: the host-compilable set (mirrors firmware/test/run_tests.sh)
# ---------------------------------------------------------------------------------------------

C11 = ["-std=c++11", "-Ifirmware/test/stubinc", "-Ifirmware/test", "-Ifirmware/src/rs485-driver"]
C17_PORT = ["-std=c++17", "-Ifirmware/esphome/components/hisense_ac"]
HDR = ["-x", "c++-header"]

# (file, flags). Headers are listed as their own units so each is checked under the .clang-tidy
# of its own directory, not the one of whichever test happens to include it. The ESPHome glue
# (hisense_ac.cpp, climate, select, switch, legacy) needs ESPHome's headers and is gated by
# ESPHome's own tooling instead; the esp32-matter and esp32-recon trees need ESP-IDF.
TIDY_UNITS = [
    ("firmware/src/rs485-driver/hisense_rs485.cpp", C11),
    ("firmware/src/rs485-driver/hisense_rs485.h", HDR + C11),
    ("firmware/src/rs485-driver/matter_aircon_map.h", HDR + C11),
    ("firmware/src/rs485-driver/esphome_aircon_map.h", HDR + C11),
    ("firmware/src/rs485-driver/power_estimate.h", HDR + C11),
    ("firmware/test/test_codec.cpp", C11),
    ("firmware/test/test_matter_map.cpp", C11),
    ("firmware/test/test_esphome_map.cpp", C11),
    ("firmware/test/test_esphome_codec_parity.cpp", C11[1:] + C17_PORT),
    ("firmware/test/test_esphome_bus.cpp", C17_PORT),
    ("firmware/esphome/components/hisense_ac/hisense_protocol.cpp", C17_PORT),
    ("firmware/esphome/components/hisense_ac/hisense_bus.cpp", C17_PORT),
]

# Checks whose --fix is mechanical and cannot change behaviour. `fix` applies only these, and
# only where the file's own .clang-tidy enables them; everything else is reported by `check`.
SAFE_FIX_CHECKS = [
    "readability-braces-around-statements",
    "readability-container-size-empty",
    "readability-duplicate-include",
    "readability-redundant-control-flow",
    "readability-redundant-string-init",
    "readability-static-accessed-through-instance",
    "modernize-use-override",
    "modernize-use-bool-literals",
    "modernize-redundant-void-arg",
    "modernize-deprecated-headers",
    "modernize-use-using",
    "llvm-namespace-comment",
]


def write_compdb(out_dir: Path) -> Path:
    out_dir.mkdir(parents=True, exist_ok=True)
    entries = []
    for f, flags in TIDY_UNITS:
        args = ["c++"] + [
            ("-I" + str(ROOT / a[2:])) if a.startswith("-I") else a for a in flags
        ] + ["-c", str(ROOT / f)]
        entries.append({"directory": str(ROOT), "file": str(ROOT / f), "arguments": args})
    path = out_dir / "compile_commands.json"
    path.write_text(json.dumps(entries, indent=1) + "\n")
    return path


def _enabled_checks(tidy: str, db: Path, f: str) -> set[str]:
    r = subprocess.run(
        [tidy, "-p", str(db), "--list-checks", str(ROOT / f)], capture_output=True, text=True
    )
    return {ln.strip() for ln in r.stdout.splitlines() if ln.startswith("    ")}


def run_tidy(tidy: str, files: list[str], fix: bool) -> int:
    units = [(f, fl) for f, fl in TIDY_UNITS if f in files]
    if not units:
        return 0
    with tempfile.TemporaryDirectory(prefix="cpp-lint-") as tmp:
        db = write_compdb(Path(tmp)).parent

        def one(unit):
            f = unit[0]
            own_dir = re.escape(str((ROOT / f).parent)) + r"/[^/]+$"
            cmd = [tidy, "-p", str(db), "--quiet", f"--header-filter={own_dir}", str(ROOT / f)]
            if fix:
                safe = sorted(_enabled_checks(tidy, db, f) & set(SAFE_FIX_CHECKS))
                if not safe:
                    return f, 0, ""
                cmd[1:1] = ["--checks=-*," + ",".join(safe), "--fix"]
            r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
            return f, r.returncode, r.stdout + r.stderr

        rc = 0
        seen: set[str] = set()
        workers = min(8, os.cpu_count() or 2)
        # --fix edits files in place: run serially so two units never rewrite a header at once.
        with concurrent.futures.ThreadPoolExecutor(1 if fix else workers) as ex:
            for f, code, out in ex.map(one, units):
                lines = [
                    ln
                    for ln in out.splitlines()
                    if ln.strip() and not re.match(r"^\d+ (warnings?|errors?) generated", ln)
                    and "Suppressed" not in ln and "Use -header-filter" not in ln
                ]
                text = "\n".join(lines).replace(str(ROOT) + "/", "")
                if code != 0 or re.search(r": (warning|error):", text):
                    rc = 1
                # A header shows up once per unit that includes it; print each diagnostic once.
                for diag in re.split(r"\n(?=\S+:\d+:\d+: (?:warning|error):)", text):
                    if diag and diag not in seen:
                        seen.add(diag)
                        print(diag, flush=True)
        return rc


# ---------------------------------------------------------------------------------------------
# clang-format
# ---------------------------------------------------------------------------------------------


def run_format(fmt: str, files: list[str], fix: bool) -> int:
    if not files:
        return 0
    cmd = [fmt, "-i"] if fix else [fmt, "--dry-run", "-Werror"]
    rc = 0
    for i in range(0, len(files), 50):
        chunk = [str(ROOT / f) for f in files[i : i + 50]]
        r = subprocess.run(cmd + chunk, capture_output=True, text=True)
        if r.returncode != 0:
            rc = 1
            # One line per offending file keeps CI output short; `fix` shows the whole diff.
            bad = sorted(set(re.findall(r"^(\S+?):\d+:\d+: (?:error|warning)", r.stderr, re.M)))
            for b in bad:
                print(f"{Path(b).relative_to(ROOT)}: not clang-format clean", flush=True)
            if not bad:
                print(r.stderr.strip(), flush=True)
    return rc


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("mode", choices=["check", "fix", "files", "compdb"])
    ap.add_argument("paths", nargs="*")
    ap.add_argument("--clang-format", default=os.environ.get("CLANG_FORMAT", "clang-format"))
    ap.add_argument("--clang-tidy", default=os.environ.get("CLANG_TIDY", "clang-tidy"))
    ap.add_argument("--no-tidy", action="store_true", help="skip clang-tidy (fast, pre-commit)")
    ap.add_argument("--no-format", action="store_true", help="skip clang-format")
    # Intermixed: cpp-lint.sh puts --clang-format/--clang-tidy between the mode and the paths, and
    # plain parse_args() hands the empty paths list to the mode's group, then rejects every path.
    a = ap.parse_intermixed_args()

    if a.mode == "compdb":
        out = Path(a.paths[0]) if a.paths else ROOT / "build" / "cpp-lint"
        print(write_compdb(out.resolve()))
        return 0

    files = select_files(a.paths)
    if a.mode == "files":
        print("\n".join(files))
        return 0

    fix = a.mode == "fix"
    rc = 0

    # 1. custom rules (autofix the trivial whitespace ones first so clang-format sees clean input)
    rule_errs = 0
    for f in files:
        p = ROOT / f
        content = p.read_text()
        if fix:
            fixed = fix_rules(content)
            if fixed != content:
                p.write_text(fixed)
                content = fixed
        for ln, col, msg in check_rules(f, content):
            if fix and "(fixable)" in msg:
                continue
            print(f"{f}:{ln}:{col}: {msg}", flush=True)
            rule_errs += 1
    if rule_errs:
        rc = 1

    # 2. clang-tidy before clang-format in fix mode, so format tidies whatever tidy inserted
    if not a.no_tidy:
        # In fix mode clang-tidy exits non-zero for every finding it just fixed; what is left is
        # for `check` to report, so only check mode takes its exit code.
        tidy_rc = run_tidy(a.clang_tidy, files, fix)
        rc |= 0 if fix else tidy_rc
    # 3. clang-format
    if not a.no_format:
        rc |= run_format(a.clang_format, files, fix)

    n = len(files)
    if rc:
        hint = " (run: firmware/scripts/cpp-lint.sh fix)" if not fix else " (left for a human)"
        print(f"[cpp-lint] {a.mode}: findings in {n} file(s) scanned{hint}", file=sys.stderr)
    else:
        print(f"[cpp-lint] {a.mode}: {n} file(s) clean", file=sys.stderr)
    return rc


if __name__ == "__main__":
    sys.exit(main())
