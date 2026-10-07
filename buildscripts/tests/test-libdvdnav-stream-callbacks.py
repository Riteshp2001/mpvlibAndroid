#!/usr/bin/env python3
"""Compile and run the stream callback reset regression against patched libdvdnav sources.

Usage: python3 tests/test-libdvdnav-stream-callbacks.py deps/libdvdnav [--cc cc]
Uses a host compiler; the dependency build invokes this before cross compiling.
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
    end = match.end()
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    vm = (args.source / "src/vm/vm.c").read_text()
    functions = [function(vm, "vm_reset")]
    fixture = Path(__file__).resolve().with_name("dvdnav-stream-callbacks.c")
    with tempfile.TemporaryDirectory(prefix="dvdnav-stream-callbacks-") as directory:
        work = Path(directory)
        (work / "dvdnav-stream-callbacks-under-test.h").write_text("\n\n".join(functions))
        exe = work / ("dvdnav-stream-callbacks.exe" if os.name == "nt" else "dvdnav-stream-callbacks")
        if Path(args.cc).stem in ("cl", "clang-cl"):
            command = [args.cc, "/nologo", "/std:c11", "/W4", "/WX",
                       f"/I{work}", str(fixture), f"/Fe:{exe}", f"/Fo:{work / 'test.obj'}"]
        else:
            command = [args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                       "-I", str(work), str(fixture), "-o", str(exe)]
        subprocess.run(command, cwd=work, check=True, timeout=30)
        subprocess.run([str(exe)], cwd=work, check=True, timeout=30)


if __name__ == "__main__":
    main()
