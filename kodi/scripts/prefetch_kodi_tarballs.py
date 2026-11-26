#!/usr/bin/env python3
"""Pre-fetch every tarball Kodi's tools/depends needs, in parallel, from several Kodi mirrors.

Usage: prefetch_kodi_tarballs.py <xbmc>/tools/depends <tarballs dir>

The depends Makefiles download one archive at a time through mirrors.kodi.tv, whose redirect
targets can stall for minutes. This fetches each archive with short connect timeouts, tries the
next mirror on failure, verifies its SHA-512 and writes the <archive>.sha512 file with which the
Makefiles treat it as downloaded.
"""
import hashlib
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

if len(sys.argv) != 3:
    sys.exit(__doc__)
DEPENDS = Path(sys.argv[1])
TARBALLS = Path(sys.argv[2])
MIRRORS = [
    "https://mirrors.dotsrc.org/kodi",
    "https://ftp.fau.de/xbmc",
    "https://mirror.netcologne.de/xbmc",
    "https://ftp.nluug.nl/mediaplayer/xbmc",
    "https://ftp.snt.utwente.nl/pub/software/xbmc",
    "https://www.mirrorservice.org/sites/mirrors.xbmc.org",
    "https://mirror.umd.edu/xbmc",
    "https://mirrors.xmission.com/kodi",
    "https://mirror.math.princeton.edu/pub/xbmc",
    "https://ftp.hosteurope.de/mirror/xbmc.org",
    "https://kodi.mirror.garr.it",
    "http://mirrors.kodi.tv",
]
VAR = re.compile(r"\$\(([A-Za-z0-9_]+)\)")


def parse(path):
    values = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.match(r"\s*([A-Z0-9_]+)\s*[:?]?=\s*(.*?)\s*$", line)
        if m:
            values[m.group(1)] = m.group(2)
    for _ in range(5):
        for k, v in values.items():
            values[k] = VAR.sub(lambda m: values.get(m.group(1), m.group(0)), v)
    return values


def sha512(path):
    h = hashlib.sha512()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def urls_for(values):
    archive = values["ARCHIVE"]
    if "FULL_URL" in values and "$(" not in values["FULL_URL"]:
        return [values["FULL_URL"]]
    base = values.get("BASE_URL", "")
    if base and "$(" not in base and "mirrors.kodi.tv" not in base:
        return [f"{base}/{archive}"]
    return [f"{m}/build-deps/sources/{archive}" for m in MIRRORS]


def fetch(item):
    archive, digest, urls = item
    dest = TARBALLS / archive
    if dest.is_file() and sha512(dest) == digest:
        (TARBALLS / f"{archive}.sha512").write_text(f"{digest}  {archive}\n")
        return archive, "cached"
    tmp = TARBALLS / f".{archive}.part"
    for url in urls:
        rc = subprocess.run(
            ["curl", "-fsSL", "--connect-timeout", "8", "--max-time", "900",
             "--retry", "2", "--retry-delay", "2", "-o", str(tmp), url],
            capture_output=True).returncode
        if rc == 0 and tmp.is_file() and sha512(tmp) == digest:
            tmp.replace(dest)
            (TARBALLS / f"{archive}.sha512").write_text(f"{digest}  {archive}\n")
            return archive, f"ok {url}"
    tmp.unlink(missing_ok=True)
    return archive, "FAILED"


def main():
    TARBALLS.mkdir(parents=True, exist_ok=True)
    items = {}
    for version_file in sorted(DEPENDS.glob("*/*/*-VERSION")):
        values = parse(version_file)
        archive, digest = values.get("ARCHIVE"), values.get("SHA512")
        if not archive or not digest or "$(" in archive:
            print(f"skip {version_file}: ARCHIVE={archive}")
            continue
        items[archive] = (archive, digest, urls_for(values))
    print(f"{len(items)} archives")
    failed = []
    with ThreadPoolExecutor(max_workers=6) as pool:
        for archive, status in pool.map(fetch, items.values()):
            print(f"{status:>8.60}  {archive}", flush=True)
            if status == "FAILED":
                failed.append(archive)
    print(f"failed: {failed}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
