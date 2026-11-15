#!/usr/bin/env python3
"""Checks that the assembled Pages site links to itself correctly.

    scripts/check-site-links.py site [--allow-missing track_annotator.html]

For every .html page in the site: each relative href/src must resolve to a
file in the site. For the landing page: every track variant it can pick must
have its tracks/<id>.json and laps/<id>.json. External links are listed but
not fetched, so the check does not depend on the network. Exits non-zero on
the first run that finds a problem.
"""

import os
import re
import sys
from html.parser import HTMLParser


class Links(HTMLParser):
    def __init__(self):
        super().__init__()
        self.found = []

    def handle_starttag(self, tag, attrs):
        for key, value in attrs:
            if key in ("href", "src") and value:
                self.found.append(value)


def main(site, allow_missing):
    problems, external = [], set()
    pages = sorted(f for f in os.listdir(site) if f.endswith(".html"))
    if "index.html" not in pages:
        problems.append("index.html is missing")
    for page in pages:
        parser = Links()
        parser.feed(open(os.path.join(site, page), encoding="utf-8", errors="replace").read())
        for link in parser.found:
            if re.match(r"^[a-z][a-z0-9+.-]*:", link, re.I) or link.startswith("//"):
                external.add(link)
                continue
            target = link.split("#")[0].split("?")[0]
            if not target:
                continue
            path = os.path.normpath(os.path.join(site, target))
            if os.path.isdir(path):
                path = os.path.join(path, "index.html")
            if not os.path.isfile(path) and target not in allow_missing:
                problems.append(f"{page}: '{link}' does not exist in the site")

    index = os.path.join(site, "index.html")
    if os.path.isfile(index):
        text = open(index, encoding="utf-8").read()
        block = re.search(r"const VARIANTS = \[(.*?)\];", text, re.S)
        ids = re.findall(r"id:\s*'([^']+)'", block.group(1)) if block else []
        if not ids:
            problems.append("index.html: no track variants found (is it the landing page?)")
        for track in ids:
            for needed in (f"tracks/{track}.json", f"laps/{track}.json"):
                if not os.path.isfile(os.path.join(site, needed)):
                    problems.append(f"index.html: variant '{track}' needs {needed}")

    print(f"checked {len(pages)} page(s): {', '.join(pages)}")
    for link in sorted(external):
        print(f"  external, not fetched: {link}")
    for problem in problems:
        print(f"BROKEN  {problem}", file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    args = sys.argv[1:]
    allowed = set()
    while "--allow-missing" in args:
        i = args.index("--allow-missing")
        allowed.add(args[i + 1])
        del args[i:i + 2]
    if len(args) != 1:
        sys.exit(__doc__)
    sys.exit(main(args[0], allowed))
