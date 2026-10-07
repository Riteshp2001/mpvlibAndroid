#!/usr/bin/env python3
"""Run checked time/chapter seek regressions against patched libbluray sources.

Usage: python3 tests/test-libbluray-checked-seek.py deps/libbluray [--cc cc]

Extracts the production API, internal seek and BD-J entry functions. The API
cases inject stream results; the stream cases also execute _seek_stream with
mocked file operations and execute buffered reads after failed seeks. Clip
opening/cleanup and queued angles are covered; disc parsing, real I/O and
Android playback are outside this host test's scope.
"""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def function(source, name):
    # These pinned C functions contain no braces in strings or comments.
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
    args = parser.parse_args()
    source = (args.source / "src/libbluray/bluray.c").read_text(encoding="utf-8")
    names = ("_seek_internal", "_seek_time", "bd_seek_time",
             "bd_seek_time_checked", "bd_bdj_seek", "_seek_chapter",
             "bd_seek_chapter", "bd_seek_chapter_checked")
    api = "\n\n".join(function(source, name) for name in names)
    fixture = Path(__file__).resolve().with_name("bluray-checked-seek.c")
    with tempfile.TemporaryDirectory(prefix="bluray-checked-seek-") as directory:
        for stream in (False, True):
            work = Path(directory) / ("stream" if stream else "api")
            work.mkdir()
            extracted = ""
            if stream:
                extracted = "\n\n".join(function(source, name) for name in (
                    "bd_release_read_source", "_close_m2ts", "_open_m2ts", "_seek_stream", "_bd_read")) + "\n\n"
            (work / "bluray-checked-seek-under-test.h").write_text(
                extracted + api, encoding="utf-8")
            exe = work / ("test.exe" if os.name == "nt" else "test")
            if Path(args.cc).stem.lower() in ("cl", "clang-cl"):
                command = [args.cc, "/nologo", "/std:c11", "/W4", "/WX",
                           f"/DTEST_STREAM={int(stream)}", f"/I{work}", str(fixture),
                           f"/Fe:{exe}", f"/Fo:{work / 'test.obj'}"]
            else:
                command = [args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                           f"-DTEST_STREAM={int(stream)}", "-I", str(work),
                           str(fixture), "-o", str(exe)]
            subprocess.run(command, cwd=work, check=True)
            subprocess.run([str(exe)], cwd=work, check=True)


if __name__ == "__main__":
    main()
