#!/usr/bin/env python3
"""Check effective logical stream identity against the production DVD VM getters."""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def function(source, name):
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
    vm = (args.source / "src/vm/vmget.c").read_text()
    api = (args.source / "src/dvdnav.c").read_text()
    functions = [function(vm, name) for name in (
        "vm_get_audio_stream", "vm_get_subp_stream", "vm_get_audio_active_stream",
        "vm_get_subp_active_stream")]
    functions += [function(api, name) for name in (
        "get_number_of_stream_attributes",
        "dvdnav_get_active_logical_stream")]
    fixture = Path(__file__).resolve().with_name("dvdnav-logical-selection.c")
    with tempfile.TemporaryDirectory(prefix="dvdnav-logical-selection-") as directory:
        work = Path(directory)
        (work / "dvdnav-logical-selection-under-test.h").write_text("\n\n".join(functions))
        exe = work / ("test.exe" if os.name == "nt" else "test")
        if Path(args.cc).stem in ("cl", "clang-cl"):
            command = [args.cc, "/nologo", "/std:c17", "/W4", "/WX", f"/I{work}",
                       str(fixture), f"/Fe:{exe}", f"/Fo:{work / 'test.obj'}"]
        else:
            command = [args.cc, "-std=c17", "-Wall", "-Wextra", "-Werror",
                       "-I", str(work), str(fixture), "-o", str(exe)]
        subprocess.run(command, cwd=work, check=True, timeout=60)
        subprocess.run([str(exe)], cwd=work, check=True, timeout=30)


if __name__ == "__main__":
    main()
