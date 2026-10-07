#!/usr/bin/env python3
"""Run the real libdvdnav chapter catalog on authored PGC/PTT structures.

Uses the pinned dvdread and dvdnav headers. Disc opening/IFO ownership are
fixture callbacks; chapter iteration and DVD time conversion are production.
"""
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
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--dvdread-source", type=Path)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    source = args.source.resolve()
    dvdread = (args.dvdread_source or source.parent / "libdvdread").resolve()
    functions = [function((source / "src/dvdnav.c").read_text(), "dvdnav_convert_time"),
                 function((source / "src/searching.c").read_text(), "dvdnav_describe_title_chapters")]
    fixture = Path(__file__).with_name("dvdnav-title-chapters.c").resolve()
    with tempfile.TemporaryDirectory(prefix="dvdnav-title-chapters-") as directory:
        work = Path(directory)
        (work / "dvdnav-title-chapters-under-test.h").write_text("\n\n".join(functions))
        for component, origin, replacements in [
            ("dvdnav", source, {"DVDNAV_MAJOR": 7, "DVDNAV_MINOR": 0, "DVDNAV_SUB": 0}),
            ("dvdread", dvdread, {"DVDREAD_VERSION_MAJOR": 7, "DVDREAD_VERSION_MINOR": 0, "DVDREAD_VERSION_MICRO": 1})]:
            version = (origin / f"src/{component}/version.h.in").read_text()
            for name, value in replacements.items():
                version = version.replace("@" + name + "@", str(value))
            (work / component).mkdir()
            (work / component / "version.h").write_text(version)
        includes = [work, source / "src", dvdread / "src"]
        exe = work / ("dvdnav-title-chapters.exe" if os.name == "nt" else "dvdnav-title-chapters")
        if Path(args.cc).stem in ("cl", "clang-cl"):
            (work / "config.h").write_text("#pragma once\n")
            (work / "sys").mkdir()
            (work / "sys/time.h").write_text("#include <winsock2.h>\n")
            command = [args.cc, "/nologo", "/std:c11", "/W4", "/WX", "/DHAVE_GETTIMEOFDAY",
                       "/D_CRT_SECURE_NO_WARNINGS", "/wd4100", "/wd4018"]
            command += [f"/I{path}" for path in includes]
            command += [str(fixture), f"/Fe:{exe}", f"/Fo:{work / 'test.obj'}"]
        else:
            command = [args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-sign-compare"]
            for path in includes:
                command += ["-I", str(path)]
            command += [str(fixture), "-o", str(exe), "-pthread"]
        subprocess.run(command, cwd=work, check=True)
        subprocess.run([str(exe)], cwd=work, check=True)


if __name__ == "__main__":
    main()
