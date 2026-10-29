#!/usr/bin/env python3
"""Write a release manifest for tuya_mcu_dfu.

  python make_manifest.py --version 1.2.0 --chip-family MS51FC0AE \
      --name "My MCU firmware" firmware.bin > manifest.json

The image path in the manifest is relative to the manifest (same directory
by default), so the manifest and image can be uploaded side by side, e.g. as
GitHub release assets. Several --image/--chip-family pairs make several builds.
"""
import argparse
import hashlib
import json
import os
import sys


def build(path, chip_family, release_url, summary, rel_to):
    with open(path, "rb") as f:
        data = f.read()
    return {
        "chipFamily": chip_family,
        "ota": {
            "path": os.path.relpath(path, rel_to).replace(os.sep, "/"),
            "md5": hashlib.md5(data).hexdigest(),
            "size": len(data),
            "release_url": release_url,
            "summary": summary,
        },
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("images", nargs="+", help="firmware image(s) as sent to the MCU")
    ap.add_argument("--version", required=True, help="version the MCU reports after the update")
    ap.add_argument("--chip-family", action="append", required=True,
                    help="chipFamily per image (repeat in the same order)")
    ap.add_argument("--name", default="MCU firmware")
    ap.add_argument("--release-url", default="")
    ap.add_argument("--summary", default="")
    ap.add_argument("-o", "--output", help="manifest file (default: stdout; paths are then "
                    "relative to the current directory)")
    a = ap.parse_args()
    if len(a.chip_family) != len(a.images):
        ap.error("give one --chip-family per image")
    rel_to = os.path.dirname(os.path.abspath(a.output)) if a.output else os.getcwd()
    m = {
        "name": a.name,
        "version": a.version,
        "builds": [build(p, c, a.release_url, a.summary, rel_to) for p, c in zip(a.images, a.chip_family)],
    }
    text = json.dumps(m, indent=2) + "\n"
    if a.output:
        with open(a.output, "w") as f:
            f.write(text)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
