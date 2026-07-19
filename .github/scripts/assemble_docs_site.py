#!/usr/bin/env python3
"""Assemble the Jekyll site from docs that already live in the repo and the wiki.

The point is discoverability. A page only ranks, and only gets quoted by an AI crawler, if it has
its own <title> and meta description; a wall of untitled markdown does not. So every copied file
gets front matter derived from its own first heading and first real paragraph, which is what
jekyll-seo-tag turns into <title>, <meta name="description">, Open Graph tags and JSON-LD.

Nothing is duplicated in git: sources stay where they are and this runs at build time.
"""

import os
import re
import shutil

SITE = "_site_src"

# (source dir, destination subdir, label used only in log output)
SOURCES = [
    ("firmware/docs", "firmware", "firmware docs"),
    ("reverse-engineering/docs", "internals", "reverse engineering"),
    ("wiki-src", "guide", "wiki guide"),
]

# Wiki pages that are navigation fragments, plus its own linter. Not content.
SKIP = {"_Sidebar.md", "_Header.md", "_Footer.md", "lint.py", "Home.md"}

FENCE = re.compile(r"^\s*```")
HEADING = re.compile(r"^#{1,6}\s+(.+?)\s*#*$")
# Strip inline markdown so the description reads as prose, not syntax.
INLINE = re.compile(r"(\*\*|__|\*|`|_)")
LINK = re.compile(r"\[([^\]]*)\]\([^)]*\)")
BADGE = re.compile(r"^\s*[!\[]")


def first_heading_and_para(lines):
    """Return (title, description) taken from the document itself."""
    title, desc, in_fence = None, None, False
    for raw in lines:
        line = raw.rstrip("\n")
        if FENCE.match(line):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        stripped = line.strip()
        if not stripped:
            continue
        m = HEADING.match(stripped)
        if m:
            if title is None:
                title = m.group(1)
            continue
        # First prose PARAGRAPH after the title becomes the description. Sources are
        # hard-wrapped, so taking a single line truncates mid-sentence, and that fragment is
        # exactly what a search result would show. Skip block quotes, tables, badges and list
        # markers: none of them read well as a snippet.
        if title and desc is None:
            if stripped[0] in "|>-*+" or BADGE.match(stripped):
                continue
            para = [stripped]
            for nxt in lines[lines.index(raw) + 1:]:
                n = nxt.strip()
                if not n or FENCE.match(n) or n[0] in "|>-*+#" or HEADING.match(n):
                    break
                para.append(n)
            desc = " ".join(para)
    return title, desc


def clean(text, limit=300):
    if not text:
        return None
    text = LINK.sub(r"\1", text)
    text = INLINE.sub("", text).strip()
    text = " ".join(text.split())
    if len(text) > limit:
        cut = text[:limit].rsplit(" ", 1)[0]
        text = cut + "…"
    return text or None


def yaml_quote(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def convert(src, dst):
    with open(src, encoding="utf-8") as fh:
        lines = fh.readlines()

    # Respect front matter that is already there rather than nesting a second block.
    if lines and lines[0].strip() == "---":
        shutil.copyfile(src, dst)
        return False

    title, desc = first_heading_and_para(lines)
    title = clean(title, 120) or os.path.splitext(os.path.basename(src))[0].replace(
        "-", " "
    )
    desc = clean(desc)

    fm = ["---", f"title: {yaml_quote(title)}"]
    if desc:
        fm.append(f"description: {yaml_quote(desc)}")
    fm += ["---", ""]

    with open(dst, "w", encoding="utf-8") as fh:
        fh.write("\n".join(fm))
        fh.writelines(lines)
    return True


def main():
    if os.path.isdir(SITE):
        shutil.rmtree(SITE)
    shutil.copytree("docs-site", SITE)

    total = 0
    for src_dir, sub, label in SOURCES:
        if not os.path.isdir(src_dir):
            print(f"  skip {label}: {src_dir} absent")
            continue
        out_dir = os.path.join(SITE, sub)
        os.makedirs(out_dir, exist_ok=True)
        n = 0
        for name in sorted(os.listdir(src_dir)):
            if name in SKIP or not name.endswith(".md"):
                continue
            convert(os.path.join(src_dir, name), os.path.join(out_dir, name))
            n += 1
        print(f"  {label}: {n} page(s) -> /{sub}/")
        total += n

    # Explicit robots.txt. jekyll-sitemap writes /sitemap.xml; pointing at it is what makes a
    # crawler pick up the whole site from one fetch instead of relying on link discovery.
    with open(os.path.join(SITE, "robots.txt"), "w", encoding="utf-8") as fh:
        fh.write(
            "User-agent: *\nAllow: /\n\n"
            "Sitemap: https://andrewdemsds.github.io/hisense-w41h1/sitemap.xml\n"
        )

    print(f"assembled {total} page(s) into {SITE}/")


if __name__ == "__main__":
    main()
