#!/usr/bin/env python3
"""Link the seven FFmpeg libraries once, retaining their public symbol versions."""

import argparse
from pathlib import Path
import re
import shlex
import shutil
import subprocess


LIBRARIES = ("avdevice", "avfilter", "avformat", "avcodec",
             "swresample", "swscale", "avutil")
DOVI_SYMBOLS = {"av_dovi_rpu_parser_" + name
                for name in ("alloc", "parse", "flush", "free")}


def readelf(tool, option, path):
    return subprocess.check_output([tool, option, "--wide", str(path)],
                                   text=True, encoding="utf-8")


def exports(tool, path):
    result = {}
    for line in readelf(tool, "--dyn-syms", path).splitlines():
        fields = line.split()
        if len(fields) < 8 or fields[4] not in ("GLOBAL", "WEAK"):
            continue
        if fields[6] in ("UND", "ABS"):
            continue
        name, separator, version = fields[7].partition("@@")
        if not separator:
            raise ValueError(f"Unversioned public symbol in {path}: {fields[7]}")
        result[name] = version
    return result


def consolidate(build_dir, prefix, cxx, ldflags, libraries, tool):
    expected = {}
    reference_needed = set()
    version_nodes = []
    archives = []
    metadata = {}
    for library in LIBRARIES:
        base = "lib" + library
        archive = build_dir / base / (base + ".a")
        if not archive.is_file():
            raise ValueError(f"Missing FFmpeg PIC archive: {archive}")
        archives.append(str(archive))
        version_map = (build_dir / base / (base + ".ver")).read_text()
        match = re.fullmatch(r"\s*(LIB[A-Z]+_[0-9]+)\s*\{.*\};\s*",
                             version_map, re.DOTALL)
        if not match:
            raise ValueError(f"Unexpected FFmpeg version map: {base}.ver")
        version = match.group(1)
        symbols = exports(tool, build_dir / base / (base + ".so"))
        dynamic = readelf(tool, "--dynamic", build_dir / base / (base + ".so"))
        reference_needed.update(re.findall(r"\(NEEDED\).*\[([^]]+)\]", dynamic))
        if symbols.get(library + "_version") != version:
            raise ValueError(f"Missing versioned public API in {base}.so")
        if set(symbols.values()) != {version}:
            raise ValueError(f"Unexpected symbol versions in {base}.so")
        duplicates = expected.keys() & symbols.keys()
        if duplicates:
            raise ValueError(f"Conflicting FFmpeg public symbols: {sorted(duplicates)}")
        expected.update(symbols)
        version_nodes.append((version, sorted(symbols)))
        pc = prefix / "lib/pkgconfig" / (base + ".pc")
        text = pc.read_text()
        for field in ("Requires", "Requires.private", "Libs", "Libs.private"):
            value = " -L${libdir} -lffmpeg" if field == "Libs" else ""
            text, count = re.subn(r"(?m)^" + re.escape(field) + r":.*$",
                                  field + ":" + value, text)
            if count != 1:
                raise ValueError(f"Missing or duplicate {field} in {pc}")
        metadata[pc] = text
    if not DOVI_SYMBOLS <= expected.keys():
        raise ValueError("FFmpeg is missing the Dolby Vision RPU parser API")

    # Expanding each library's map against its own shared build avoids assigning
    # av* symbols from another library to the wrong LIBAV*_major version node.
    merged_map = build_dir / "ffmpeg-shared.ver"
    lines = []
    for index, (version, symbols) in enumerate(version_nodes):
        lines.extend([version + " {", "    global:"])
        lines.extend("        " + name + ";" for name in symbols)
        if index == len(version_nodes) - 1:
            lines.extend(["    local:", "        *;"])
        lines.append("};")
    merged_map.write_text("\n".join(lines) + "\n")
    # Shared-enabled FFmpeg archives contain duplicate private SHLIBOBJS. Root
    # every public API explicitly, then let normal archive extraction choose
    # one copy of those private objects instead of forcing all copies in.
    roots = build_dir / "ffmpeg-shared.roots"
    roots.write_text("".join("--undefined=" + name + "\n" for name in sorted(expected)))
    output = build_dir / "libffmpeg.so"
    old_flags = {"-l" + name for name in LIBRARIES}
    private_libraries = [flag for flag in libraries if flag not in old_flags]
    subprocess.run(shlex.split(cxx) + ["-shared", "-o", str(output)]
                   + shlex.split(ldflags)
                   + ["-Wl,-soname,libffmpeg.so", "-Wl,-Bsymbolic", "-Wl,--as-needed",
                      "-Wl,--no-undefined",
                      "-Wl,--no-undefined-version", "-Wl,--version-script," + str(merged_map),
                      "-Wl,@" + str(roots), "-Wl,--start-group"] + archives
                   + private_libraries + ["-Wl,--end-group"], check=True)
    actual = exports(tool, output)
    if actual != expected:
        missing = sorted(expected.items() - actual.items())
        extra = sorted(actual.items() - expected.items())
        raise ValueError(f"FFmpeg export mismatch: missing={missing}, extra={extra}")
    dynamic = readelf(tool, "--dynamic", output)
    needed = set(re.findall(r"\(NEEDED\).*\[([^]]+)\]", dynamic))
    old_libraries = {"lib" + name + ".so" for name in LIBRARIES}
    if needed & old_libraries:
        raise ValueError(f"Aggregate still depends on split FFmpeg: {sorted(needed)}")
    if needed - reference_needed:
        raise ValueError(f"Aggregate introduced shared dependencies: {sorted(needed - reference_needed)}")
    if "libc++_shared.so" not in needed:
        raise ValueError("Aggregate does not use the producer's shared C++ runtime")
    if not re.search(r"\(SONAME\).*\[libffmpeg\.so\]", dynamic):
        raise ValueError("Unexpected aggregate FFmpeg SONAME")

    # Replace the installed split artifacts only after the complete aggregate
    # and all seven consumer metadata files have passed validation.
    shutil.copyfile(output, prefix / "lib/libffmpeg.so")
    for pc, text in metadata.items():
        pc.write_text(text)
    for library in LIBRARIES:
        for suffix in (".so", ".a"):
            (prefix / "lib" / ("lib" + library + suffix)).unlink()
    print(f"FFmpeg shared core: {len(expected)} versioned exports, seven libraries -> one")


def verify_package(directory, tool):
    abis = {"armeabi-v7a": ("ELF32", "ARM"),
            "arm64-v8a": ("ELF64", "AArch64"),
            "x86": ("ELF32", "Intel 80386"),
            "x86_64": ("ELF64", "Advanced Micro Devices X86-64")}
    if directory.name not in abis:
        raise ValueError(f"Unsupported native package ABI: {directory.name}")
    elf_class, machine = abis[directory.name]
    expected = {"libffmpeg.so", "libmpv.so", "libplayer.so", "libc++_shared.so"}
    actual = {path.name for path in directory.glob("*.so")}
    if actual != expected:
        raise ValueError(f"Unexpected native package in {directory}: "
                         f"missing={sorted(expected - actual)}, extra={sorted(actual - expected)}")
    old_libraries = {"lib" + name + ".so" for name in LIBRARIES}
    for name in sorted(expected):
        header = readelf(tool, "--file-header", directory / name)
        if (not re.search(r"(?m)^\s*Class:\s+" + elf_class + r"\s*$", header)
                or not re.search(r"(?m)^\s*Machine:\s+" + re.escape(machine) + r"\s*$", header)):
            raise ValueError(f"ELF ABI mismatch in {directory.name}/{name}")
        dynamic = readelf(tool, "--dynamic", directory / name)
        needed = set(re.findall(r"\(NEEDED\).*\[([^]]+)\]", dynamic))
        if needed & old_libraries:
            raise ValueError(f"{name} still needs split FFmpeg: {sorted(needed & old_libraries)}")
        if name in ("libmpv.so", "libplayer.so") and "libffmpeg.so" not in needed:
            raise ValueError(f"{name} does not use the shared FFmpeg core")
        if not re.search(r"\(SONAME\).*\[" + re.escape(name) + r"\]", dynamic):
            raise ValueError(f"Unexpected SONAME in {name}")
        segments = readelf(tool, "--program-headers", directory / name).splitlines()
        alignments = [int(line.split()[-1], 16) for line in segments
                      if line.split() and line.split()[0] == "LOAD"]
        # Android's 16 KB page-size requirement applies to its 64-bit ABIs.
        required_alignment = 16384 if elf_class == "ELF64" else 4096
        if not alignments or min(alignments) < required_alignment:
            raise ValueError(f"Insufficient ELF page alignment in {name}")
    if not DOVI_SYMBOLS <= exports(tool, directory / "libffmpeg.so").keys():
        raise ValueError("Packaged FFmpeg is missing the Dolby Vision RPU parser API")
    print(f"Native package verified: {directory.name}, four shared libraries")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    link = subparsers.add_parser("link")
    link.add_argument("--build-dir", type=Path, required=True)
    link.add_argument("--prefix", type=Path, required=True)
    link.add_argument("--cxx", required=True)
    link.add_argument("--ldflags", default="")
    link.add_argument("--libs-file", type=Path, required=True)
    link.add_argument("--readelf", default="llvm-readelf")
    verify = subparsers.add_parser("verify-package")
    verify.add_argument("directory", type=Path)
    verify.add_argument("--readelf", default="llvm-readelf")
    args = parser.parse_args()
    if args.command == "link":
        consolidate(args.build_dir.resolve(), args.prefix.resolve(), args.cxx, args.ldflags,
                    shlex.split(args.libs_file.read_text()), args.readelf)
    else:
        verify_package(args.directory, args.readelf)


if __name__ == "__main__":
    main()
