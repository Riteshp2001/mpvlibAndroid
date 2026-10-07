#!/usr/bin/env python3
"""Check that R8 preserves the JNI-only MPVLib.Stream methods.

This isolates the app's shrinker rules with an equivalent Java interface;
it does not build an APK or exercise Android playback.
"""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import zipfile


LIBRARY = """package is.xyz.mpv;
import java.nio.ByteBuffer;
public final class MPVLib {
    public interface Stream {
        int read(ByteBuffer buffer);
        long seek(long position);
        long size();
        void cancel();
        void close();
    }
    public static Stream openStream(String uri) { return null; }
    public native void create();
}
"""

CHECKER = """public final class CheckStream {
    public static void main(String[] args) throws Exception {
        Class<?> stream = Class.forName("is.xyz.mpv.MPVLib$Stream");
        stream.getMethod("read", java.nio.ByteBuffer.class);
        stream.getMethod("seek", long.class);
        stream.getMethod("size");
        stream.getMethod("cancel");
        stream.getMethod("close");
        System.out.println("Stream JNI keep rules: 5 methods preserved");
    }
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--r8-jar", required=True, type=Path)
    parser.add_argument("--android-jar", required=True, type=Path)
    parser.add_argument("--rules", type=Path, default=(
        Path(__file__).resolve().parents[2] / "app/consumer-rules.pro"))
    parser.add_argument("--java", default="java")
    parser.add_argument("--javac", default="javac")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="stream-jni-r8-") as directory:
        work = Path(directory)
        library = work / "MPVLib.java"
        checker = work / "CheckStream.java"
        library.write_text(LIBRARY, encoding="utf-8")
        checker.write_text(CHECKER, encoding="utf-8")
        classes, checks, output = (work / name for name in ("classes", "checks", "output"))
        for path in (classes, checks, output):
            path.mkdir()
        subprocess.run([args.javac, "--release", "8", "-d", str(classes), str(library)], check=True)
        subprocess.run([args.javac, "--release", "8", "-d", str(checks), str(checker)], check=True)
        archive = work / "input.jar"
        with zipfile.ZipFile(archive, "w") as jar:
            for path in classes.rglob("*.class"):
                jar.write(path, path.relative_to(classes).as_posix())
        subprocess.run([
            args.java, "-cp", str(args.r8_jar.resolve()), "com.android.tools.r8.R8",
            "--release", "--classfile", "--lib", str(args.android_jar.resolve()),
            "--pg-conf", str(args.rules.resolve()), "--output", str(output), str(archive),
        ], check=True)
        subprocess.run([
            args.java, "-cp", os.pathsep.join((str(output), str(checks))), "CheckStream",
        ], check=True)


if __name__ == "__main__":
    main()
