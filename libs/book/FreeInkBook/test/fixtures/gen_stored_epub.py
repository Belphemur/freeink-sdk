#!/usr/bin/env python3
"""Packs an EPUB stage directory with every entry STORED (method 0).

The host suite's `zip` shim always deflates, and deflated entries make the
renderer pay a 32 KB inflate window — a stored twin is what lets an arena-
tight render test run at all. Usage: gen_stored_epub.py <stage-dir> <out.epub>
"""
import os
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    stage, out = sys.argv[1], sys.argv[2]
    if os.path.exists(out):
        os.remove(out)
    # OCF: mimetype must be first and stored.
    with open(os.path.join(stage, "mimetype"), "rb") as f:
        mimetype = f.read()
    import zipfile

    with zipfile.ZipFile(out, "w") as z:
        z.writestr(zipfile.ZipInfo("mimetype"), mimetype, compress_type=zipfile.ZIP_STORED)
        for root, _, files in os.walk(stage):
            for name in sorted(files):
                path = os.path.join(root, name)
                arc = os.path.relpath(path, stage)
                if arc == "mimetype":
                    continue
                z.write(path, arc, compress_type=zipfile.ZIP_STORED)
    return 0


if __name__ == "__main__":
    sys.exit(main())
