#!/usr/bin/env python3
"""Builds the installer's page (installer/SPEC.md, "Page"): installer/page/index.html with the wiring diagram
docs/images/installer-wiring.svg inlined byte for byte in place of its marker comment, every picture named as
src="@docs/images/..." inlined as a data URI, gzipped.

Usage: make_page.py PAGE SVG OUT_GZ
"""

import base64
import gzip
import os
import re
import sys

MARKER = b"<!-- wiring diagram: docs/images/installer-wiring.svg, inlined by installer/tools/make_page.py -->\n"
PICTURE = re.compile(rb'src="@(docs/images/[A-Za-z0-9_./-]+\.png)"')


def build(page, svg, root):
    if page.count(MARKER) != 1:
        raise SystemExit("make_page: the page needs the wiring marker exactly once")

    def inline(m):
        with open(os.path.join(root, m.group(1).decode()), "rb") as f:
            return b'src="data:image/png;base64,' + base64.b64encode(f.read()) + b'"'

    return PICTURE.sub(inline, page.replace(MARKER, svg))


def main():
    page_path, svg_path, out_path = sys.argv[1:4]
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(svg_path))))
    with open(page_path, "rb") as f:
        page = f.read()
    with open(svg_path, "rb") as f:
        svg = f.read()
    html = build(page, svg, root)
    if b'src="@' in html:
        raise SystemExit("make_page: a picture reference was not inlined")
    with open(out_path, "wb") as f:
        f.write(gzip.compress(html, compresslevel=9, mtime=0))


if __name__ == "__main__":
    main()
