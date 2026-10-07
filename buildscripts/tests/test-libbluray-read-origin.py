#!/usr/bin/env python3
"""Test the patched libbluray source ownership and cursor-preserving read API."""
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
    root = Path(__file__).resolve().parent
    source = (args.source / "src/libbluray/bluray.c").read_text(encoding="utf-8")
    definition = re.search(r"struct bd_read_source \{[^}]+\};", source).group()
    names = ("bd_register_read_origin_proc", "bd_retain_read_source", "bd_release_read_source",
             "bd_read_source", "_close_m2ts")
    unit = definition + "\n\n" + "\n\n".join(function(source, name) for name in names)
    with tempfile.TemporaryDirectory(prefix="bluray-read-origin-") as directory:
        work = Path(directory)
        (work / "bluray-read-origin-under-test.h").write_text(unit, encoding="utf-8")
        exe = work / ("test.exe" if os.name == "nt" else "test")
        fixture = root / "bluray-read-origin.c"
        if Path(args.cc).stem.lower() in ("cl", "clang-cl"):
            command = [args.cc, "/nologo", "/std:c11", "/W4", "/WX", f"/I{work}",
                       str(fixture), f"/Fe:{exe}", f"/Fo:{work / 'test.obj'}"]
        else:
            command = [args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(work),
                       str(fixture), "-o", str(exe)]
        subprocess.run(command, cwd=work, check=True)
        subprocess.run([str(exe)], cwd=work, check=True)


if __name__ == "__main__":
    main()
