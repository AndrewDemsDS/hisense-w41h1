#!/usr/bin/env python3
"""Gitea-wiki linter for the hisense-w41h1 wiki.

Catches the mistakes that generic markdown linters miss on a Gitea *wiki*:
  - heading-level skips (H1->H3) that break the auto Table of Contents
  - broken inter-wiki links  [text](Page-Name)  -> Page-Name.md must exist
  - broken image embeds       ![alt](images/x)  -> file must exist
  - unbalanced ```mermaid fences (silently drop a diagram)
  - repo-path links that won't resolve on the wiki (should be backticked)
  - pages missing from _Sidebar.md
  - public-ready leaks: absolute home paths, hard-coded node numbers, private IPs, HA hostname
  - stop-slop: em dashes (error) + throat-clearing / filler / X-not-Y (warn)

Stdlib only. Run from the wiki repo root:  python3 lint.py
Exit non-zero if any ERROR is found (WARN does not fail).

Complements (not replaces) generic tooling if you want it:
  markdownlint-cli2 '**/*.md'      # MD001 heading-increment, style
  lychee --offline .               # link/asset checking
"""

import os, re, sys, glob

ROOT = os.path.dirname(os.path.abspath(__file__))
errors, warns = [], []


def err(f, line, msg):
    errors.append(f"{f}:{line}: ERROR {msg}")


def warn(f, line, msg):
    warns.append(f"{f}:{line}: warn  {msg}")


# public-ready leak denylist (regex, message)
LEAKS = [
    # Generic class-based checks (catch a kind of leak, not a specific value), so this is
    # useful to any maintainer/fork without hard-coding one person's hostnames or SSID.
    (
        r"/home/(?!you\b|user\b)[A-Za-z0-9_.-]+/",
        "absolute home path (use ~ or a placeholder)",
    ),
    (r"\bnode (?:[0-9]|1[0-9])\b", "hard-coded node number (use a placeholder)"),
    (
        r"\b192\.168\.(?!1\.)\d+\.\d+\b|\b10\.\d+\.\d+\.\d+\b",
        "private IP (use the 192.168.1.x placeholder)",
    ),
    (r"homeassistant\.local", "hard-coded HA hostname (use your-ha-host.local)"),
]

# stop-slop: AI-writing tells. Em dash is an ERROR (house style bans it);
# the rest are WARN so they surface without blocking.
SLOP_ERR = [
    ("—", "em dash (recast as a period, comma, colon, or parens)"),
]
SLOP_WARN = [
    (r"(?i)\bhere'?s (what|how|why|the|this|that)\b", "throat-clearing opener"),
    (
        r"(?i)\b(it'?s worth noting|the (truth|reality) is|at its core|at the end of the day"
        r"|when it comes to|in a world where|let me be clear|the uncomfortable truth)\b",
        "throat-clearing filler",
    ),
    (
        r"(?i)\b(simply|basically|essentially|literally|of course|really|actually|genuinely"
        r"|honestly|truly|deeply|fundamentally|inherently|inevitably|interestingly"
        r"|importantly|crucially|obviously|clearly)\b",
        "filler adverb",
    ),
    (
        r"(?i)\bnot (just |only )?\w+,? (it'?s|but)\b",
        "X-not-Y contrast; state it directly",
    ),
    (
        r"(?i)\b(full stop\.|let that sink in|make no mistake|this matters because"
        r"|here'?s why that matters)",
        "emphasis crutch; delete it",
    ),
    (
        r"(?i)\b(let me walk you through|in this section, we|as we'?ll see|the rest of this"
        r"|plot twist|spoiler:)",
        "meta-commentary; let the page move",
    ),
    (
        r"(?i)\b(navigate|unpack|lean into|deep dive|double down|circle back|game-changer"
        r"|moving forward|on the same page)\b",
        "business jargon; use plain language",
    ),
    (r"(?i)\b(think about it:|what if )", "rhetorical setup; make the point"),
    (
        r"(?i)\bthe (reasons|implications|stakes|consequences) are\b",
        "vague declarative; name the specific thing",
    ),
]
# Deliberately NOT flagged: the lazy extremes (never / every / always). This is a
# technical wiki, where "never commit dumps/" and "always retry" are precise
# instructions rather than false authority. Flagging them would bury the real hits.


def strip_code_map(lines):
    """Return list of (lineno, text, in_code, mermaid_open_count_delta).
    Marks lines inside fenced code blocks so headings/links there are ignored."""
    out, in_code, fence = [], False, ""
    mermaid_open = mermaid_bad = 0
    for i, raw in enumerate(lines, 1):
        s = raw.rstrip("\n")
        m = re.match(r"^\s*(```+|~~~+)(.*)$", s)
        if m:
            tok, info = m.group(1), m.group(2).strip().lower()
            if not in_code:
                in_code, fence = True, tok[0] * 3
                if info.startswith("mermaid"):
                    mermaid_open += 1
                out.append((i, s, True))
                continue
            elif s.strip().startswith(fence):
                in_code = False
                out.append((i, s, True))
                continue
        out.append((i, s, in_code))
    if in_code:
        mermaid_bad = 1  # an unclosed fence at EOF
    return out, mermaid_open, mermaid_bad


def pages():
    return sorted(os.path.basename(p) for p in glob.glob(os.path.join(ROOT, "*.md")))


PAGE_SET = {p[:-3] for p in pages()}  # "Home", "FAQ-Gotchas", ...
LINK_RE = re.compile(r"(!?)\[[^\]]*\]\(([^)]+)\)")
HEAD_RE = re.compile(r"^(#{1,6})\s+(\S.*)$")
IMG_EXT = (".png", ".jpg", ".jpeg", ".svg", ".gif", ".webp")


def check_file(fname):
    path = os.path.join(ROOT, fname)
    with open(path, encoding="utf-8") as fh:
        lines = fh.readlines()
    mapped, mermaid_open, unclosed = strip_code_map(lines)
    if unclosed:
        err(
            fname,
            len(lines),
            "unclosed ``` code/mermaid fence (diagram will not render)",
        )

    # headings: first must be H1, exactly one H1, no level skips.
    # Special Gitea files (_Sidebar/_Header/_Footer) are nav fragments, not pages.
    special = fname.startswith("_")
    prev = 0
    h1 = 0
    for ln, text, in_code in () if special else mapped:
        if in_code:
            continue
        m = HEAD_RE.match(text)
        if not m:
            continue
        lvl = len(m.group(1))
        if lvl == 1:
            h1 += 1
        if prev == 0 and lvl != 1:
            err(fname, ln, f"first heading is H{lvl}, must be H1")
        if prev and lvl > prev + 1:
            err(
                fname,
                ln,
                f"heading jumps H{prev}->H{lvl} (breaks the TOC); increment by one",
            )
        prev = lvl
    if not special and h1 == 0:
        err(fname, 1, "no H1 title heading")
    elif h1 > 1:
        warn(fname, 1, f"{h1} H1 headings (expected 1)")

    # links & images
    for ln, text, in_code in mapped:
        if in_code:
            continue
        for bang, target in LINK_RE.findall(text):
            t = target.strip()
            if t.startswith(("http://", "https://", "mailto:", "#")):
                continue
            t_noanchor = t.split("#", 1)[0]
            if bang == "!" or t_noanchor.lower().endswith(IMG_EXT):
                if not os.path.exists(os.path.join(ROOT, t_noanchor)):
                    err(fname, ln, f"image not found: {t}")
            elif "/" in t_noanchor:
                err(
                    fname,
                    ln,
                    f"link target '{t}' has a path; wiki links can't reach repo files "
                    f"(use a backticked path, or a full URL)",
                )
            elif t_noanchor and t_noanchor not in PAGE_SET:
                err(fname, ln, f"inter-wiki link to missing page: {t_noanchor}")

    # leaks + stop-slop (skip fenced code so commands/diagrams are exempt)
    for ln, text, in_code in mapped:
        for rx, why in LEAKS:
            if re.search(rx, text):
                err(fname, ln, f"public-ready leak ({why}): matches /{rx}/")
        if in_code:
            continue
        for token, why in SLOP_ERR:
            if token in text:
                err(fname, ln, f"stop-slop: {why}")
        for rx, why in SLOP_WARN:
            if re.search(rx, text):
                warn(fname, ln, f"stop-slop: {why}")


def check_sidebar():
    sb = os.path.join(ROOT, "_Sidebar.md")
    if not os.path.exists(sb):
        warn("_Sidebar.md", 0, "no _Sidebar.md")
        return
    txt = open(sb, encoding="utf-8").read()
    linked = set(m.group(1) for m in re.finditer(r"\]\(([A-Za-z0-9._-]+)\)", txt))
    for p in PAGE_SET:
        if p.startswith("_") or p == "Home":
            continue
        if p not in linked:
            warn("_Sidebar.md", 0, f"page not linked from sidebar: {p}")


def main():
    for f in pages():
        check_file(f)
    check_sidebar()
    for w in warns:
        print(w)
    for e in errors:
        print(e)
    n = len(errors)
    print(
        f"\n{'FAIL' if n else 'OK'}: {n} error(s), {len(warns)} warning(s) across {len(pages())} pages"
    )
    sys.exit(1 if n else 0)


if __name__ == "__main__":
    main()
