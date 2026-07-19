#!/usr/bin/env python3
"""Push the site's URLs to IndexNow after a deploy.

Crawlers otherwise find changes whenever they next wander by, which for a new site on a shared
host can be weeks. IndexNow inverts that: you POST the URLs and participating engines fetch them
within minutes. Bing, Yandex, Seznam, Naver and Yep participate; Google has trialled it but never
committed, so Search Console still matters separately.

Authentication is the key FILE, not a header: the engine fetches
https://<host>/<baseurl>/<key>.txt and checks it contains the same key it was given. That is why
the key lives in docs-site/ and is committed. It is not a secret, and publishing it is the design.

URLs come from the sitemap the build already generates, so there is one source of truth for what
the site contains. Failure here must never fail the deploy: a submission endpoint being down says
nothing about whether the site published correctly.
"""

import json
import os
import sys
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET

HOST = "andrewdemsds.github.io"
SITEMAP = "https://andrewdemsds.github.io/hisense-w41h1/sitemap.xml"
ENDPOINT = "https://api.indexnow.org/IndexNow"
NS = "{http://www.sitemaps.org/schemas/sitemap/0.9}"
TIMEOUT = 30


def sitemap_urls(url):
    with urllib.request.urlopen(url, timeout=TIMEOUT) as r:
        root = ET.fromstring(r.read())
    return [e.text.strip() for e in root.iter(NS + "loc") if e.text]


def main():
    key = os.environ.get("INDEXNOW_KEY", "").strip()
    if not key:
        print("INDEXNOW_KEY unset -- skipping", file=sys.stderr)
        return 0

    try:
        urls = sitemap_urls(SITEMAP)
    except Exception as exc:  # noqa: BLE001 - never fail the deploy
        print(f"could not read sitemap ({exc}) -- skipping", file=sys.stderr)
        return 0

    # The key file must be reachable before submitting: engines verify it, and a submission whose
    # key 404s is rejected wholesale. Checking here turns a silent rejection into a visible skip.
    key_url = f"https://{HOST}/hisense-w41h1/{key}.txt"
    try:
        with urllib.request.urlopen(key_url, timeout=TIMEOUT) as r:
            served = r.read().decode().strip()
        if served != key:
            print(
                f"key file at {key_url} does not match INDEXNOW_KEY -- skipping",
                file=sys.stderr,
            )
            return 0
    except Exception as exc:  # noqa: BLE001
        print(
            f"key file not reachable at {key_url} ({exc}) -- skipping", file=sys.stderr
        )
        return 0

    payload = {
        "host": HOST,
        "key": key,
        "keyLocation": key_url,
        "urlList": urls,
    }
    req = urllib.request.Request(
        ENDPOINT,
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json; charset=utf-8"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
            print(f"IndexNow accepted {len(urls)} URL(s): HTTP {r.status}")
    except urllib.error.HTTPError as exc:
        # 202 is success-with-processing; 4xx means our payload is wrong and is worth reading.
        body = exc.read().decode(errors="replace")[:300]
        print(f"IndexNow returned HTTP {exc.code}: {body}", file=sys.stderr)
    except Exception as exc:  # noqa: BLE001
        print(f"IndexNow submission failed ({exc}) -- ignoring", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
