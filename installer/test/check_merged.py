#!/usr/bin/env python3
"""Checks the merged C3 image (installer/SPEC.md, Tests 5).

Usage: check_merged.py INSTALLER_BUILD_DIR STAGE_FILE INSTALL_IMAGE WIRING_SVG

- every IDF output sits at its flash_args offset in havoc-radio-installer.bin;
- the partition table has otadata, the two app slots and the stage partition as in partitions.csv;
- installer.bin, the app image for self-update, is the app in slot ota_0, fits both slots, and carries the project
  name "installer" and the version in installer/version.txt;
- the stage partition holds the stage file from tools/make_stage.py: a valid header, every region present, the
  install image's regions byte-identical;
- the app carries the gzipped page, whose wiring diagram is the SVG file byte-identical;
- the SVG parses as XML and uses no external references.
"""

import gzip
import os
import re
import struct
import sys
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "tools"))
import make_stage  # noqa: E402
from hostserver import Checks  # noqa: E402

MERGED = "havoc-radio-installer.bin"
APP = "installer.bin"
PAGE_GZ = "page.html.gz"
VERSION_TXT = os.path.join(os.path.dirname(HERE), "version.txt")
TYPE_APP, TYPE_DATA = 0x00, 0x01
SUBTYPE_OTA_DATA, SUBTYPE_OTA_0, SUBTYPE_OTA_1 = 0x00, 0x10, 0x11
# esp_app_desc_t after the image header (24 bytes) and the first segment header (8 bytes).
APP_DESC_AT = 32
APP_DESC_MAGIC = 0xABCD5432


def partitions(table):
    """{label: (type, subtype, offset, size)} of an ESP-IDF partition table binary."""
    out = {}
    for pos in range(0, len(table) - 31, 32):
        magic, ptype, sub, offset, size = struct.unpack_from("<HBBII", table, pos)
        if magic != 0x50AA:
            break
        out[table[pos + 12:pos + 28].split(b"\0")[0].decode()] = (ptype, sub, offset, size)
    return out


def check_svg(check, svg):
    try:
        root = ET.fromstring(svg)
    except ET.ParseError as e:
        check(False, "SVG parses as XML: %s" % e)
        return
    check(root.tag == "{http://www.w3.org/2000/svg}svg", "SVG root element: %s" % root.tag)
    external = []
    for el in root.iter():
        tag = el.tag.split("}")[-1]
        if tag in ("image", "script", "foreignObject", "use", "style"):
            external.append(tag)
        for name, value in el.attrib.items():
            if name.split("}")[-1] == "href" and not value.startswith("#"):
                external.append("%s %s" % (name, value))
            if re.search(r"url\(\s*['\"]?(?!#)", value):
                external.append("%s=%s" % (name, value))
    text = svg.decode()
    if re.search(r"@import|(?<!xmlns=\")https?://(?!www\.w3\.org/2000/svg\")", text):
        external.append("URL in the text")
    check(not external, "SVG has no external references: %s" % external)


def main():
    build, stage_path, image_path, svg_path = sys.argv[1:5]
    check = Checks("installer image")
    with open(os.path.join(build, MERGED), "rb") as f:
        merged = f.read()
    with open(stage_path, "rb") as f:
        stage = f.read()
    with open(image_path, "rb") as f:
        image = f.read()
    with open(svg_path, "rb") as f:
        svg = f.read()
    with open(os.path.join(build, "flash_args")) as f:
        tokens = f.read().split()
    files = [(int(tokens[i], 0), tokens[i + 1]) for i in range(len(tokens) - 1) if tokens[i].startswith("0x")]
    contents = {}
    for offset, rel in files:
        with open(os.path.join(build, rel), "rb") as f:
            data = f.read()
        contents[offset] = data
        check(merged[offset:offset + len(data)] == data, "%s at 0x%x in the merged image" % (rel, offset))

    parts = partitions(contents.get(0x8000, b""))
    check(parts.get("otadata", (None, None))[:2] == (TYPE_DATA, SUBTYPE_OTA_DATA), "otadata partition: %s" % parts)
    slots = [parts.get("ota_0"), parts.get("ota_1")]
    check([s[:2] if s else None for s in slots] == [(TYPE_APP, SUBTYPE_OTA_0), (TYPE_APP, SUBTYPE_OTA_1)],
          "two app slots: %s" % parts)
    app = slots[0]
    app_bin = contents.get(app[2], b"") if app else b""
    with open(os.path.join(build, APP), "rb") as f:
        check(bool(app_bin) and f.read() == app_bin, "%s is the app in ota_0" % APP)
    check(all(s and 0 < len(app_bin) <= s[3] for s in slots), "app fits both slots")
    desc = app_bin[APP_DESC_AT:APP_DESC_AT + 80].ljust(80, b"\0")
    magic = struct.unpack_from("<I", desc)[0]
    version = desc[16:48].split(b"\0")[0].decode(errors="replace")
    project = desc[48:80].split(b"\0")[0].decode(errors="replace")
    with open(VERSION_TXT) as f:
        want_version = f.read().strip()
    check(magic == APP_DESC_MAGIC and project == "installer", "app project name: %r" % project)
    check(version == want_version, "app version %r is version.txt's %r" % (version, want_version))
    st = parts.get("stage")
    check(st is not None and st[0] == TYPE_DATA, "stage data partition: %s" % (st,))
    check(st is not None and (st[2], st[3]) == make_stage.stage_partition(), "stage partition as in partitions.csv")
    if st is None:
        return check.report()
    off, size = st[2], st[3]
    check(len(stage) <= size, "stage contents fit the partition")
    in_flash = merged[off:off + len(stage)]
    check(in_flash == stage, "stage contents at 0x%x in the merged image" % off)
    try:
        label, total, regions = make_stage.decode_header(in_flash[:make_stage.HEADER_SIZE])
    except make_stage.StageError as e:
        check(False, "stage header: %s" % e)
        return check.report()
    check(all(r["flags"] & (make_stage.FLAG_ERASE | make_stage.FLAG_PRESENT) for r in regions),
          "every region erase-only or present")
    padded = image + b"\xff" * (-len(image) % make_stage.SECTOR)
    check(make_stage.flat_image(in_flash, regions, total) == padded, "install image regions byte-identical")

    with open(os.path.join(build, PAGE_GZ), "rb") as f:
        page_gz = f.read()
    check(app_bin.count(page_gz) == 1, "the gzipped page is in the app once")
    page = gzip.decompress(page_gz)
    check(page.count(svg) == 1, "the page holds the wiring SVG byte-identical")
    check(b"<title>Havoc Radio Installer</title>" in page, "page title")
    check(b"inlined by installer/tools/make_page.py" not in page, "the marker is replaced")
    check(b'src="@' not in page and page.count(b'src="data:image/png;base64,') == 9, "the 9 stock pictures are inlined")
    check_svg(check, svg)

    print("%s: %d B; app %s %d B of a %d B slot; page %d B gzipped (%d B); stage %d B of %d B at 0x%x: \"%s\", "
          "%d regions, image %d B" % (MERGED, len(merged), version, len(app_bin), app[3] if app else 0, len(page_gz),
                                      len(page), len(stage), size, off, label, len(regions), total))
    return check.report()


if __name__ == "__main__":
    sys.exit(main())
