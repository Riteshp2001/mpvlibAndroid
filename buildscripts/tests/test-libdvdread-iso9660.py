#!/usr/bin/env python3
"""Compile actual libdvdread image-file readers against sector-authored fixtures.

The fixture supplies block I/O and a UDF lookup oracle, not an ISO parser or
an extent mapper. The new ISO module and image open/read/stat/close functions
are compiled from the source under test. No disc, network or Android is needed.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


def function(source, name):
    match = re.search(r"^[\w *]+\b" + name + r"\([^;{]*\)\s*\{", source, re.M)
    if not match:
        raise ValueError(f"Missing function: {name}")
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--image", type=Path, help="Optional ISO9660 image for manifest byte checks")
    parser.add_argument("--manifest", type=Path, help="JSON containing path/size/sha256 file entries")
    args = parser.parse_args()
    source = args.source.resolve()
    reader = (source / "src/dvd_reader.c").read_text(encoding="utf-8")
    definitions = reader[reader.index("struct dvd_reader_device_s {"):
                         reader.index("/**\n * Set the level of caching")]
    names = ["DVDImageFileSystem", "DVDImageFindFile", "DVDOpenFileISO", "DVDOpenVOBISO", "DVDFileStatISO",
             "DVDOpenFileUDF", "DVDOpenVOBUDF", "DVDFileStatVOBUDF", "DVDOpenFile",
             "DVDFileStat", "DVDISOVolumeInfo", "DVDReadBlocksUDF",
             "DVDReadBlocks", "DVDFileSeek", "DVDFileSeekForce", "DVDReadBytes", "DVDCloseFile"]
    functions = []
    for name in names:
        if re.search(r"\b" + name + r"\s*\(", reader):
            functions.append(function(reader, name))
    fixture = Path(__file__).with_name("dvdread-iso9660.c").resolve()
    with tempfile.TemporaryDirectory(prefix="dvdread-iso9660-") as directory:
        work = Path(directory)
        (work / "config.h").write_text("#pragma once\n", encoding="utf-8")
        (work / "dvdread").mkdir()
        version = (source / "src/dvdread/version.h.in").read_text(encoding="utf-8")
        for key, value in [("MAJOR", 7), ("MINOR", 0), ("MICRO", 1)]:
            version = version.replace("@DVDREAD_VERSION_" + key + "@", str(value))
        (work / "dvdread/version.h").write_text(version, encoding="utf-8")
        has_iso = (source / "src/dvd_iso9660.c").is_file()
        prefix = '#include "dvd_iso9660.h"\n' if has_iso else ""
        (work / "reader-types.h").write_text(prefix + definitions, encoding="utf-8")
        (work / "reader-functions.h").write_text("\n\n".join(functions), encoding="utf-8")
        includes = [work, source / "src", source / "msvc/include"]
        exe = work / ("dvdread-iso9660.exe" if os.name == "nt" else "dvdread-iso9660")
        files = [fixture] + ([source / "src/dvd_iso9660.c"] if has_iso else [])
        if Path(args.cc).stem in ("cl", "clang-cl"):
            command = [args.cc, "/nologo", "/std:c11", "/W4", "/WX",
                       "/D_CRT_SECURE_NO_WARNINGS", "/wd4100", "/wd4244", "/wd4267",
                       "/wd4018", "/wd4245", "/wd4996"]
            command += [f"/I{path}" for path in includes]
            command += [str(path) for path in files] + [f"/Fe:{exe}"]
        else:
            command = [args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                       "-Wno-unused-parameter", "-Wno-sign-compare"]
            for path in includes:
                command += ["-I", str(path)]
            command += [str(path) for path in files] + ["-o", str(exe)]
        subprocess.run(command, cwd=work, check=True)
        subprocess.run([str(exe)], cwd=work, check=True)
        if args.image:
            if not args.manifest:
                parser.error("--image requires --manifest")
            manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
            entries = manifest["files"]
            for entry in entries:
                output = work / "extracted.bin"
                name = entry["path"].split(";")[0]
                subprocess.run([str(exe), str(args.image.resolve()), name, str(output)],
                               cwd=work, check=True)
                data = output.read_bytes()
                assert len(data) == entry["size"], name
                assert hashlib.sha256(data).hexdigest() == entry["sha256"], name
            print(f"Actual image: {len(entries)} DVD file sizes and SHA256 hashes matched")


if __name__ == "__main__":
    main()
