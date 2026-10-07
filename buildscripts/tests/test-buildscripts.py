#!/usr/bin/env python3
"""Check buildscript failure propagation and install-tool selection on the host."""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest


class BuildscriptTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="mpv-buildscripts-")
        self.root = Path(self.temporary.name).resolve()
        assert self.root.parent == Path(tempfile.gettempdir()).resolve()
        assert self.root.name.startswith("mpv-buildscripts-")
        self.addCleanup(self.temporary.cleanup)
        self.environment = os.environ.copy()
        for name in ("BASH_ENV", "ENV", "SHELLOPTS", "INSTALL"):
            self.environment.pop(name, None)
        self.environment.update(cores="1", MPV_ANDROID_NATIVE_ONLY="1",
                                TRACE_FILE=(self.root / "trace.txt").as_posix())
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.copy_source("include/path.sh")
        self.write("buildscripts/include/depinfo.sh",
                   "v_ndk=fixture\nv_ndk_n=fixture\ndep_failure=()\n"
                   "dep_mpv_android=()\nv_ci_uavs3d=" + "1" * 40 + "\n")

    def write(self, name, text, executable=False):
        path = self.root / name
        assert path.resolve().is_relative_to(self.root)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text.encode("utf-8"))
        if executable:
            path.chmod(0o755)
        return path

    def copy_source(self, name):
        # Model the LF checkout used by Linux CI on Windows hosts too.
        text = (self.buildscripts / name).read_text(encoding="utf-8")
        return self.write("buildscripts/" + name, text, executable=True)

    def tool(self, name, body):
        return self.write("bin/" + name, "#!/bin/bash -e\n" + body, executable=True)

    def run_shell(self, command, directory=None):
        command = ('export PATH="$(cd ' + shlex.quote(self.bin.as_posix())
                   + ' && pwd):$PATH"\n' + command)
        return subprocess.run([self.bash, "-e", "-c", command],
                              cwd=directory or self.root, env=self.environment,
                              text=True, encoding="utf-8", capture_output=True, timeout=30)

    def trace(self):
        path = self.root / "trace.txt"
        return path.read_text().splitlines() if path.exists() else []

    def prepare_dispatcher(self):
        script = self.copy_source("buildall.sh")
        toolchain = self.root / "buildscripts/sdk/android-ndk-fixture/toolchains/llvm/prebuilt/fixture/bin"
        toolchain.mkdir(parents=True)
        self.write("buildscripts/prefix/arm64/lib/libmpv.so", "fixture\n")
        self.tool("pkg-config", "exit 0\n")
        return shlex.quote(script.as_posix()) + " -n --arch arm64"

    def test_recipe_failure(self):
        command = self.prepare_dispatcher()
        (self.root / "buildscripts/deps/failure").mkdir(parents=True)
        self.write("buildscripts/scripts/failure.sh",
                   '#!/bin/bash -e\nprintf "%s\\n" "$1" >> "$TRACE_FILE"\n'
                   'false\nprintf continued >> "$TRACE_FILE"\n', executable=True)
        for phase, option in (("build", ""), ("clean", "--clean")):
            with self.subTest(phase=phase):
                (self.root / "trace.txt").unlink(missing_ok=True)
                result = self.run_shell(command + " " + option + " failure")
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertEqual(self.trace(), [phase])

    def test_recipe_success(self):
        command = self.prepare_dispatcher()
        (self.root / "buildscripts/deps/failure").mkdir(parents=True)
        self.write("buildscripts/scripts/failure.sh",
                   '#!/bin/bash -e\nprintf "%s\\n" "$1" >> "$TRACE_FILE"\n',
                   executable=True)
        result = self.run_shell(command + " failure")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.trace(), ["build"])

    def test_native_only_failure(self):
        command = self.prepare_dispatcher()
        self.copy_source("scripts/mpv-android.sh")
        self.tool("ndk-build", "exit 7\n")
        result = self.run_shell(command + " mpv-android")
        self.assertEqual(result.returncode, 7, result.stdout + result.stderr)
        self.assertNotIn("Skipping Gradle APK build", result.stdout)

    def test_workflow_native_build_order(self):
        workflow = (self.buildscripts.parent / ".github/workflows/build.yml").read_text()
        match = re.search(
            r"(?m)^      - name: Build AAR for all Android architectures\n"
            r"        run: \|\n((?:          .*\n|[ \t]*\n)+)", workflow)
        self.assertIsNotNone(match)
        command = "\n".join(line[10:] for line in match.group(1).splitlines())
        self.write("buildscripts/buildall.sh",
                   '#!/bin/bash -e\nprintf "%s\\n" "$*" >> "$TRACE_FILE"\n',
                   executable=True)
        for enabled in ("false", "true"):
            with self.subTest(x86=enabled):
                (self.root / "trace.txt").unlink(missing_ok=True)
                self.write("buildscripts/include/build_config.sh",
                           "ENABLE_X86_ARCH=" + enabled + "\n")
                result = self.run_shell(command)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                arches = ["armv7l", "arm64"]
                if enabled == "true":
                    arches.extend(["x86", "x86_64"])
                self.assertEqual(self.trace(),
                                 ["--arch " + arch + " -n mpv" for arch in arches]
                                 + ["-n mpv-android"])

    def test_version_metadata_arch_suffixes(self):
        self.copy_source("scripts/write_versions.sh")
        for suffix in ("_armv7l", "_arm64", "_x86", "_x86_64"):
            with self.subTest(suffix=suffix):
                result = self.run_shell(
                    "bash buildscripts/scripts/write_versions.sh " + suffix)
                self.assertEqual(result.returncode, 1)
                self.assertIn("No packaged ABI prefixes were found.", result.stderr)
                self.assertNotIn("Unsupported NDK suffix", result.stderr)

    def test_patch_callers(self):
        self.copy_source("include/patch-libbluray.sh")
        patches = ("still-eof", "checked-seek", "menu-info", "read-origin")
        for name in patches:
            self.write("buildscripts/patches/libbluray-1.4.1-" + name + ".patch", "fixture\n")
        directory = self.root / "buildscripts/deps/libbluray"
        directory.mkdir(parents=True)
        self.tool("patch", 'case " $* " in *" --dry-run "*) exit 0 ;; esac\n'
                  'if [ ! -f "$TRACE_FILE" ]; then\n'
                  '    echo failed > "$TRACE_FILE"\n    exit 7\nfi\n'
                  'echo continued >> "$TRACE_FILE"\n')
        for caller, cwd in (("include/download-deps.sh", directory.parent),
                            ("build-bdj-jars.sh", directory.parent.parent)):
            with self.subTest(caller=caller):
                (self.root / "trace.txt").unlink(missing_ok=True)
                lines = (self.buildscripts / caller).read_text().splitlines()
                command = next(line for line in lines if "include/patch-libbluray.sh" in line)
                result = self.run_shell("source_dir=deps/libbluray\n" + command, cwd)
                self.assertEqual(result.returncode, 7, result.stdout + result.stderr)
                self.assertEqual(self.trace(), ["failed"])

    def test_sdk_export_failure(self):
        self.copy_source("export-renderer-sdk.sh")
        headers = {"config.h": "#define PL_API_VER 1\n#define PL_HAVE_OPENGL 1\n"
                   "#define PL_HAVE_VULKAN 1\n#define PL_HAVE_SHADERC 1\n",
                   "opengl.h": "bool external_yuv;\n",
                   "vulkan.h": "bool disable_storage;\nstruct pl_vulkan_ycbcr_params {\n"
                   "const struct pl_vulkan_ycbcr_params *ycbcr;\n",
                   "utils/libav.h": "pl_map_avframe_dovi_metadata(\n"}
        for arch in ("armv7l", "arm64"):
            prefix = "buildscripts/prefix/" + arch
            for name, text in headers.items():
                self.write(prefix + "/include/libplacebo/" + name, text)
            for name in ("libplacebo.a", "libshaderc.a"):
                self.write(prefix + "/lib/" + name, "fixture\n")
        self.write("buildscripts/prefix/arm64/share/licenses/libplacebo/LICENSE", "license\n")
        self.tool("cp", 'echo copy >> "$TRACE_FILE"\nexit 7\n')
        workflow = (self.buildscripts.parent / ".github/workflows/build.yml").read_text()
        match = re.search(r"(?m)^\s*- name: Export renderer SDK\s*\n\s*run: (.+)$", workflow)
        self.assertIsNotNone(match)
        assert (self.root / "build/renderer-sdk").resolve().is_relative_to(self.root)
        result = self.run_shell(match.group(1))
        self.assertEqual(result.returncode, 7, result.stdout + result.stderr)
        self.assertEqual(self.trace(), ["copy"])
        self.assertFalse((self.root / "build/renderer-sdk/provenance.properties").exists())

    def check_installers(self, selected):
        real_install = subprocess.check_output([self.bash, "-c", "command -v install"],
                                               text=True, encoding="utf-8").strip()
        self.environment["REAL_INSTALL"] = real_install
        self.environment["INSTALL"] = "ginstall" if selected else ""
        body = 'echo %s >> "$TRACE_FILE"\nexec "$REAL_INSTALL" "$@"\n'
        self.tool("ginstall", body % "ginstall")
        self.tool("install", 'exit 99\n' if selected else body % "install")
        for name in ("meson", "ninja", "cmake"):
            self.tool(name, "exit 0\n")
        self.tool("git", "echo " + "1" * 40 + "\n")
        self.copy_source("include/cmake-android.sh")
        prefix = self.root / "buildscripts/prefix/arm64"
        self.environment.update(prefix_dir=prefix.as_posix(), prefix_name="arm64",
                                ndk_suffix="_fixture")
        self.write("buildscripts/prefix/arm64/lib/libshaderc.a", "fixture\n")
        for recipe, license_name in (("libplacebo", "LICENSE"), ("uavs3d", "COPYING")):
            with self.subTest(recipe=recipe):
                (self.root / "trace.txt").unlink(missing_ok=True)
                self.copy_source("scripts/" + recipe + ".sh")
                base = "buildscripts/deps/" + recipe
                self.write(base + "/" + license_name, "license\n")
                self.write(base + "/_build_fixture/src/include/libplacebo/config.h",
                           "#define PL_HAVE_OPENGL 1\n#define PL_HAVE_VULKAN 1\n")
                self.write("buildscripts/prefix/arm64/lib/pkgconfig/" + recipe + ".pc", "Libs:\n")
                command = "../../scripts/" + recipe + ".sh build"
                result = self.run_shell(command, self.root / base)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(self.trace(), ["ginstall" if selected else "install"])
                target = prefix / "share/licenses" / recipe / license_name
                self.assertEqual(target.read_text(), "license\n")
                if os.name != "nt":
                    self.assertEqual(target.stat().st_mode & 0o777, 0o644)

    def test_selected_installers(self):
        self.check_installers(selected=True)

    def test_fallback_installers(self):
        self.check_installers(selected=False)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--buildscripts", type=Path, required=True)
    parser.add_argument("--bash", default="bash")
    args = parser.parse_args()
    BuildscriptTests.buildscripts = args.buildscripts.resolve()
    BuildscriptTests.bash = args.bash
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(BuildscriptTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())
