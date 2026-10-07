#!/usr/bin/env python3
"""Exercise pinned libdvdnav time search and first NAV/payload delivery in C.

Usage: python3 tests/test-libdvdnav-time-search-floor.py deps/libdvdnav
The sibling libdvdread source supplies the pinned DVD structure definitions.
The host fixture supplies disc I/O and decoded NAV fields; this is not a disc
parser, video decoder, Android runtime, or network latency test.
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


def version_header(source, destination, substitutions):
    text = source.read_text()
    for name, value in substitutions.items():
        text = text.replace("@" + name + "@", str(value))
    destination.parent.mkdir(exist_ok=True)
    destination.write_text(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--dvdread-source", type=Path)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    source = args.source.resolve()
    dvdread = (args.dvdread_source or source.parent / "libdvdread").resolve()
    nav = (source / "src/dvdnav.c").read_text()
    vm = (source / "src/vm/vm.c").read_text()
    searching = (source / "src/searching.c").read_text()
    play = (source / "src/vm/play.c").read_text()
    functions = [function(play, "play_Cell"), function(vm, "vm_jump_cell_block"),
                 function(vm, "vm_position_get")]
    functions += [function(nav, name) for name in (
        "dvdnav_convert_time", "dvdnav_decode_packet", "dvdnav_get_vobu",
        "dvdnav_cell_group_start", "dvdnav_read_seek_nav", "dvdnav_seek_cell_matches",
        "dvdnav_selected_angle_cell", "dvdnav_time_search_floor_internal",
        "dvdnav_time_search_floor",
        "dvdnav_get_current_time", "cell_has_commands_or_ends_pgc",
        "get_next_cache_block", "dvdnav_get_next_cache_block")]
    functions += [function(searching, name) for name in (
        "dvdnav_scan_admap", "dvdnav_time_search", "dvdnav_admap_get",
        "dvdnav_tmap_get", "dvdnav_tmap_get_entry", "dvdnav_admap_search",
        "dvdnav_tmap_search", "dvdnav_cell_find", "dvdnav_admap_interpolate_vobu",
        "dvdnav_tmap_calc_time_for_tmap_entry", "dvdnav_tmap_get_entries_for_sector",
        "dvdnav_find_vobu_by_tmap", "dvdnav_find_vobu_by_cell_boundaries",
        "dvdnav_jump_to_sector_by_time")]
    fixture = Path(__file__).resolve().with_name("dvdnav-time-search-floor.c")
    with tempfile.TemporaryDirectory(prefix="dvdnav-time-floor-") as directory:
        work = Path(directory)
        (work / "dvdnav-time-search-floor-under-test.h").write_text("\n\n".join(functions))
        version_header(source / "src/dvdnav/version.h.in", work / "dvdnav/version.h",
                       {"DVDNAV_MAJOR": 7, "DVDNAV_MINOR": 0, "DVDNAV_SUB": 0})
        version_header(dvdread / "src/dvdread/version.h.in", work / "dvdread/version.h",
                       {"DVDREAD_VERSION_MAJOR": 7, "DVDREAD_VERSION_MINOR": 0,
                        "DVDREAD_VERSION_MICRO": 1})
        includes = [work, source / "src", dvdread / "src"]
        exe = work / ("dvdnav-time-floor.exe" if os.name == "nt" else "dvdnav-time-floor")
        if Path(args.cc).stem in ("cl", "clang-cl"):
            # dvdread's MSVC headers require config.h; decoder.h needs timeval.
            (work / "config.h").write_text("#pragma once\n")
            (work / "sys").mkdir()
            (work / "sys/time.h").write_text("#include <winsock2.h>\n")
            command = [args.cc, "/nologo", "/std:c11", "/W4", "/WX",
                       "/DHAVE_GETTIMEOFDAY", "/wd4100", "/wd4018", "/wd4244",
                       "/wd4245", "/wd4267", "/wd4389", "/wd4456", "/wd4459",
                       "/wd4706", "/wd4996"]
            command += [f"/I{path}" for path in includes]
            command += [str(fixture), f"/Fe:{exe}", f"/Fo:{work / 'test.obj'}"]
        else:
            command = [args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                       "-Wno-unused-parameter", "-Wno-sign-compare"]
            for path in includes:
                command += ["-I", str(path)]
            command += [str(fixture), "-o", str(exe), "-pthread"]
        subprocess.run(command, cwd=work, check=True)
        subprocess.run([str(exe)], cwd=work, check=True)


if __name__ == "__main__":
    main()
