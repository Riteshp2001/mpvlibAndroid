#!/usr/bin/env python3
"""Compile the production JNI property/shutdown flow with deterministic API boundaries."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mpv", required=True, type=Path)
    parser.add_argument("--jni", required=True, type=Path)
    parser.add_argument("--cxx", default="c++")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    platform = "win32" if os.name == "nt" else "linux"
    with tempfile.TemporaryDirectory(prefix="jni-context-lifetime-") as directory:
        work = Path(directory)
        (work / "android").mkdir()
        (work / "android/log.h").write_text(
            "#pragma once\n#define ANDROID_LOG_ERROR 6\n#define ANDROID_LOG_VERBOSE 2\n"
            'extern "C" int __android_log_print(int, const char *, const char *, ...);\n',
            encoding="utf-8")
        output = work / ("test.exe" if os.name == "nt" else "test")
        standard = "c++14" if os.name == "nt" else "c++11"
        command = [args.cxx, "-std=" + standard, "-O1", "-I" + str(root),
                   "-I" + str(work), "-I" + str(args.mpv.resolve() / "include"),
                   "-I" + str(args.jni.resolve()),
                   "-I" + str(args.jni.resolve() / platform),
                   str(Path(__file__).with_name("jni-context-lifetime.cpp")),
                   "-o", str(output)]
        if os.name == "nt" and Path(args.cxx).stem.lower() in ("clang", "clang++"):
            command[1:1] = ["--target=x86_64-pc-windows-msvc", "-fuse-ld=link"]
        else:
            command.append("-pthread")
        subprocess.run(command, check=True, timeout=60)
        subprocess.run([str(output)], check=True, timeout=10)


if __name__ == "__main__":
    main()
