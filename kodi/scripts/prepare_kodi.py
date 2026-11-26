#!/usr/bin/env python3
"""Generate the mpvlib-kodi AAR inputs from an official Kodi release.

Outputs (all under --out, regenerated from scratch on every run):
  java/     Kodi's Android Java glue, generated from tools/android/packaging/xbmc/src/*.java.in
            with package org.xbmc.kodi (libkodi.so registers its JNI natives on those classes)
            and the small set of patches needed to run inside another application.
  res/      Kodi's Android resources, renamed with a "kodi_" prefix so they cannot collide
            with (or be overridden by) resources of the host application.
  jniLibs/  lib/<abi>/*.so from the official APKs, without libc++_shared.so (the host
            application ships a newer, backward-compatible one).
  assets/   kodi/kodi-assets.zip: the APK's assets/ tree as one archive, extracted by Splash
            on first run into <cacheDir>/apk/assets (KODI_HOME), plus license/provenance files.
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.request
import zipfile
from pathlib import Path

APP_PACKAGE = "org.xbmc.kodi"
APP_NAME = "Kodi"
APP_NAME_LC = "kodi"
RES_PREFIX = "kodi_"
RENAMED_TYPES = ("layout", "drawable", "mipmap", "string", "color", "style", "xml", "dimen")
CANONICAL_ASSET_ABI = "arm64-v8a"
ZIP_TIME = (1981, 1, 1, 1, 1, 0)
SPARSE_PATHS = [
    "/tools/android/packaging/",
    "/media/applaunch_screen.png",
    "/media/icon80x80.png",
    "/LICENSE.md",
    "/LICENSES/",
    "/version.txt",
]


def log(msg):
    print(f"[prepare_kodi] {msg}", flush=True)


def fail(msg):
    print(f"[prepare_kodi] ERROR: {msg}", file=sys.stderr, flush=True)
    sys.exit(1)


def load_properties(path):
    props = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        key, _, value = line.partition("=")
        props[key.strip()] = value.strip()
    return props


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download(url, dest, expected_sha256):
    if dest.exists() and sha256_of(dest) == expected_sha256:
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(dest.suffix + ".part")
    for attempt in range(1, 6):
        try:
            log(f"downloading {url} (attempt {attempt})")
            with urllib.request.urlopen(url, timeout=60) as resp, open(tmp, "wb") as out:
                shutil.copyfileobj(resp, out, 1 << 20)
            break
        except OSError as e:
            log(f"download failed: {e}")
            if attempt == 5:
                fail(f"cannot download {url}")
            time.sleep(5 * attempt)
    actual = sha256_of(tmp)
    if actual != expected_sha256:
        tmp.unlink()
        fail(f"SHA-256 mismatch for {url}: expected {expected_sha256}, got {actual}")
    tmp.replace(dest)
    return dest


def run(cmd, cwd=None):
    subprocess.run(cmd, cwd=cwd, check=True)


def ensure_kodi_source(props, cache, override):
    if override:
        root = Path(override).resolve()
        if not (root / "tools/android/packaging/xbmc/src/Main.java.in").is_file():
            fail(f"{root} is not a Kodi source tree")
        return root
    tag = props["kodi.tag"]
    root = cache / f"xbmc-{tag}"
    marker = root / ".mpvlib-kodi-complete"
    if marker.is_file():
        return root
    if root.exists():
        shutil.rmtree(root)
    log(f"fetching Kodi {tag} packaging sources (sparse checkout)")
    run(["git", "-c", "advice.detachedHead=false", "clone", "--quiet", "--depth", "1", "--filter=blob:none", "--sparse",
         "--branch", tag, props["kodi.git"], str(root)])
    run(["git", "-C", str(root), "sparse-checkout", "set", "--no-cone", *SPARSE_PATHS])
    marker.write_text(tag + "\n", encoding="utf-8")
    return root


def kodi_commit(root):
    try:
        return subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"], check=True,
                              capture_output=True, text=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


# --------------------------------------------------------------------------------------------
# Java glue
# --------------------------------------------------------------------------------------------

def flexible(old):
    """Regex for `old` that tolerates any whitespace between tokens."""
    parts = [re.escape(p) for p in old.split()]
    return re.compile(r"\s+".join(parts))


# (file, old, new). `old` must occur exactly once (whitespace-insensitive).
JAVA_PATCHES = [
    # Provider authorities: <host applicationId>.kodi.* instead of org.xbmc.kodi.*, matching
    # the embedded manifest. A runtime value, so it is never inlined as a constant.
    ("content/XBMCContentProvider.java",
     'public static final String AUTHORITY_ROOT = "org.xbmc.kodi";',
     "public static final String AUTHORITY_ROOT = org.xbmc.kodi.KodiHost.authorityRoot();"),
    ("XBMCSearchableActivity.java",
     'Uri.parse("content://org.xbmc.kodi.media/search/" + query)',
     'Uri.parse("content://" + org.xbmc.kodi.content.XBMCMediaContentProvider.AUTHORITY'
     ' + "/search/" + query)'),
    # Autostart would launch the host application's launcher activity.
    ("XBMCBroadcastReceiver.java",
     'i = manager.getLaunchIntentForPackage("org.xbmc.kodi"); '
     "i.addCategory(Intent.CATEGORY_LAUNCHER);",
     "i = new Intent(context, Splash.class);\n"
     "        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);"),
    # Don't share /sdcard/xbmc_env.properties (data/home relocation) with an installed Kodi.
    ("XBMCProperties.java",
     '"/xbmc_env.properties"',
     '"/" + KodiHost.hostPackageName() + ".xbmc_env.properties"'),
    # "Already running" check: the component lives in the host package.
    ("Splash.java",
     'task.topActivity.toString().equalsIgnoreCase( '
     '"ComponentInfo{org.xbmc.kodi/org.xbmc.kodi.Main}"))',
     "task.topActivity != null && task.topActivity.equals(\n"
     "              new android.content.ComponentName(getPackageName(), Main.class.getName())))"),
    # First-run extraction reads Kodi's assets archive instead of every assets/ entry of
    # the host APK. Entries keep their "assets/" prefix, so KODI_HOME stays <cache>/apk/assets.
    ("Splash.java",
     "import java.util.zip.ZipFile;",
     "import java.util.zip.ZipFile;\n"
     "import java.util.zip.ZipInputStream;\n"
     "import java.io.BufferedInputStream;"),
    ("Splash.java",
     "ZipFile zip; byte[] buf = new byte[4096];",
     "ZipInputStream zip;\n"
     "      byte[] buf = new byte[65536];"),
    ("Splash.java",
     "zip = new ZipFile(sPackagePath); "
     "Enumeration<? extends ZipEntry> entries = zip.entries(); "
     "mProgress.setProgress(0); "
     "mProgress.setMax(zip.size());",
     "zip = new ZipInputStream(new BufferedInputStream(\n"
     "            getAssets().open(KodiHost.ASSETS_ZIP), 65536));\n"
     "        mProgress.setProgress(0);\n"
     "        mProgress.setMax(KodiHost.assetEntryCount(mSplash));"),
    ("Splash.java",
     "while (entries.hasMoreElements()) { "
     "// Update the progress bar "
     "publishProgress(++mProgressStatus); "
     "ZipEntry e = (ZipEntry) entries.nextElement();",
     "ZipEntry e;\n"
     "        while ((e = zip.getNextEntry()) != null)\n"
     "        {\n"
     "          // Update the progress bar\n"
     "          publishProgress(++mProgressStatus);\n"),
    ("Splash.java",
     "InputStream in = zip.getInputStream(e); "
     "FileOutputStream out = new FileOutputStream(sFullPath); "
     "while ((n = in.read(buf)) > 0) out.write(buf, 0, n); "
     "in.close(); out.close();",
     "FileOutputStream out = new FileOutputStream(sFullPath);\n"
     "            while ((n = zip.read(buf)) > 0)\n"
     "              out.write(buf, 0, n);\n\n"
     "            out.close();"),
    # The original deletes the package file on read errors (legacy OBB handling); never
    # touch the host APK.
    ("Splash.java",
     'mErrorMsg = "Cannot read package."; File obb = new File(sPackagePath); obb.delete();',
     'mErrorMsg = "Cannot read package.";'),
    # Android TV 11/12 devices get the runtime storage permission instead of all-files access.
    # Hosts commonly declare WRITE_EXTERNAL_STORAGE with maxSdkVersion 29; from API 30 it grants
    # nothing beyond read access, so request READ_EXTERNAL_STORAGE there.
    ("Splash.java",
     "requestPermissions(new String[]{Manifest.permission.WRITE_EXTERNAL_STORAGE},",
     "requestPermissions(new String[]{legacyStoragePermission()},"),
    ("Splash.java",
     "permissionCheck = checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE);",
     "permissionCheck = checkSelfPermission(legacyStoragePermission());"),
    ("Splash.java",
     "private boolean isAndroidTV()",
     "private static String legacyStoragePermission()\n"
     "  {\n"
     "    return Build.VERSION.SDK_INT >= 30 ? Manifest.permission.READ_EXTERNAL_STORAGE\n"
     "                                       : Manifest.permission.WRITE_EXTERNAL_STORAGE;\n"
     "  }\n\n"
     "  private boolean isAndroidTV()"),
]


def substitute(text, values, where):
    for key, value in values.items():
        text = text.replace(f"@{key}@", value)
    leftover = re.findall(r"@[A-Z_]+@", text)
    if leftover:
        fail(f"unresolved placeholders {sorted(set(leftover))} in {where}")
    return text


def generate_java(kodi_root, out_java):
    src = kodi_root / "tools/android/packaging/xbmc/src"
    pkg_dir = out_java / APP_PACKAGE.replace(".", "/")
    values = {"APP_PACKAGE": APP_PACKAGE, "APP_NAME": APP_NAME, "APP_NAME_LC": APP_NAME_LC}
    files = {}
    for template in sorted(src.rglob("*.java.in")):
        rel = template.relative_to(src).as_posix()[: -len(".in")]
        files[rel] = substitute(template.read_text(encoding="utf-8"), values, rel)

    for rel, old, new in JAVA_PATCHES:
        if rel not in files:
            fail(f"patch target {rel} missing")
        pattern = flexible(old)
        matches = pattern.findall(files[rel])
        if len(matches) != 1:
            fail(f"patch for {rel} matched {len(matches)} times: {old[:80]}")
        files[rel] = pattern.sub(lambda _: new, files[rel])

    for rel, text in files.items():
        if "getIdentifier(" in text:
            fail(f"{rel} looks up resources by name; the kodi_ prefix would break it")
        dest = pkg_dir / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(text, encoding="utf-8")
    return files


# --------------------------------------------------------------------------------------------
# Resources
# --------------------------------------------------------------------------------------------

def copy_file(src, dest):
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dest)


def generate_res(kodi_root, out_res):
    pkg = kodi_root / "tools/android/packaging"
    res_src = pkg / "xbmc/res"
    values = {"APP_PACKAGE": APP_PACKAGE, "APP_NAME": APP_NAME, "APP_NAME_LC": APP_NAME_LC}

    for sub in ("layout", "drawable"):
        for f in sorted((res_src / sub).iterdir()):
            copy_file(f, out_res / sub / f.name)
    # Same layout as tools/android/packaging/Makefile.in (target "res").
    copy_file(kodi_root / "media/applaunch_screen.png", out_res / "drawable/applaunch_screen.png")
    copy_file(kodi_root / "media/applaunch_screen.png",
              out_res / "drawable-xxxhdpi/applaunch_screen.png")
    copy_file(kodi_root / "media/icon80x80.png", out_res / "drawable/ic_recommendation_80dp.png")
    for density in ("hdpi", "ldpi", "mdpi", "xhdpi", "xxhdpi", "xxxhdpi"):
        copy_file(pkg / f"media/drawable-{density}/ic_launcher.png",
                  out_res / f"drawable-{density}/ic_launcher.png")
    copy_file(pkg / "media/drawable-xhdpi/banner.png", out_res / "drawable-xhdpi/banner.png")
    for name in ("strings", "colors"):
        text = substitute((pkg / f"xbmc/{name}.xml.in").read_text(encoding="utf-8"), values, name)
        (out_res / "values").mkdir(parents=True, exist_ok=True)
        (out_res / f"values/{name}.xml").write_text(text, encoding="utf-8")
    # Not generated: res/xml/searchable.xml (the global-search activity is not declared; its
    # authority cannot follow the host applicationId) and translated values-*/strings.xml (one
    # TV-channel name; they would add locales the host application does not support).


VALUES_ELEMENT = re.compile(r'<(string|color|style|dimen|integer|bool|array|string-array)'
                            r'(\s[^>]*?)name="([^"]+)"')


def prefix_resources(out_res, java_files, out_java):
    defined = {t: set() for t in RENAMED_TYPES}
    for f in sorted(out_res.rglob("*")):
        if not f.is_file():
            continue
        rtype = f.parent.name.split("-")[0]
        if rtype == "values":
            for kind, _, name in VALUES_ELEMENT.findall(f.read_text(encoding="utf-8")):
                if kind in defined:
                    defined[kind].add(name)
        elif rtype in defined:
            defined[rtype].add(f.name.split(".")[0])

    def new_name(rtype, name):
        return RES_PREFIX + name if name in defined.get(rtype, ()) else name

    xml_ref = re.compile(r"@(" + "|".join(RENAMED_TYPES) + r")/([A-Za-z0-9_.]+)")
    for f in sorted(out_res.rglob("*")):
        if not f.is_file():
            continue
        rtype = f.parent.name.split("-")[0]
        if f.suffix == ".xml":
            text = f.read_text(encoding="utf-8")
            text = xml_ref.sub(lambda m: f"@{m.group(1)}/{new_name(m.group(1), m.group(2))}", text)
            if rtype == "values":
                text = VALUES_ELEMENT.sub(
                    lambda m: f'<{m.group(1)}{m.group(2)}name="{new_name(m.group(1), m.group(3))}"',
                    text)
            f.write_text(text, encoding="utf-8")
        if rtype == "values":
            f.rename(f.with_name(RES_PREFIX + f.name))
        elif rtype in defined:
            f.rename(f.with_name(RES_PREFIX + f.name))

    # R.style.Foo_Bar would refer to style "Foo.Bar"; Kodi defines no dotted names.
    dotted = sorted(n for names in defined.values() for n in names if "." in n)
    if dotted:
        fail(f"dotted resource names are not supported: {dotted}")
    java_ref = re.compile(r"(?<![\w.])R\.(" + "|".join(RENAMED_TYPES) + r")\.(\w+)")
    pkg_dir = out_java / APP_PACKAGE.replace(".", "/")
    for rel, text in java_files.items():
        renamed = java_ref.sub(lambda m: f"R.{m.group(1)}.{new_name(m.group(1), m.group(2))}", text)
        if renamed != text:
            (pkg_dir / rel).write_text(renamed, encoding="utf-8")
    return defined


# --------------------------------------------------------------------------------------------
# Native libraries and assets
# --------------------------------------------------------------------------------------------

def extract_libs(abi, apk, out_jni):
    count = 0
    with zipfile.ZipFile(apk) as z:
        for info in z.infolist():
            parts = info.filename.split("/")
            if len(parts) != 3 or parts[0] != "lib" or not parts[2].endswith(".so"):
                continue
            if parts[1] != abi:
                fail(f"{apk} contains libraries for {parts[1]}, expected only {abi}")
            if parts[2] == "libc++_shared.so":
                continue
            dest = out_jni / abi / parts[2]
            dest.parent.mkdir(parents=True, exist_ok=True)
            with z.open(info) as src, open(dest, "wb") as out:
                shutil.copyfileobj(src, out, 1 << 20)
            count += 1
    if not (out_jni / abi / "libkodi.so").is_file():
        fail(f"{apk} has no lib/{abi}/libkodi.so")
    return count


PLATFORM_TAG = re.compile(rb"<platform>android-(?:i686|x86_64|aarch64|armv7)</platform>")


def build_assets(apk, out_assets, kodi_root, provenance):
    kodi_dir = out_assets / "kodi"
    kodi_dir.mkdir(parents=True, exist_ok=True)
    archive = kodi_dir / "kodi-assets.zip"
    entries = 0
    retagged = []
    with zipfile.ZipFile(apk) as src, \
            zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as dst:
        for info in sorted(src.infolist(), key=lambda i: i.filename):
            name = info.filename
            if not name.startswith("assets/") or name.endswith("/"):
                continue
            data = src.read(info)
            # One archive serves every ABI. The official per-ABI builds differ only in the binary
            # add-ons' <platform> tag ("android" matches all ABIs) and in x86's unused egg stubs
            # for its two extra Cryptodome modules, which Kodi loads by library name.
            if name.endswith("/addon.xml") and PLATFORM_TAG.search(data):
                data = PLATFORM_TAG.sub(b"<platform>android</platform>", data)
                retagged.append(name)
            entry = zipfile.ZipInfo(name, date_time=ZIP_TIME)
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.external_attr = 0o644 << 16
            dst.writestr(entry, data)
            entries += 1
    if entries == 0:
        fail(f"{apk} contains no assets")
    (kodi_dir / "kodi-assets.properties").write_text(
        f"entries={entries}\nsha256={sha256_of(archive)}\n", encoding="utf-8")
    copy_file(kodi_root / "LICENSE.md", kodi_dir / "LICENSE.md")
    (kodi_dir / "SOURCES.txt").write_text(provenance, encoding="utf-8")
    return entries, retagged


# --------------------------------------------------------------------------------------------

def main():
    here = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--properties", type=Path, default=here / "kodi.properties")
    parser.add_argument("--cache", type=Path,
                        default=Path(os.environ.get("MPVLIB_KODI_CACHE",
                                                    Path.home() / ".cache/mpvlib-kodi")))
    parser.add_argument("--kodi-source", help="existing Kodi source tree (default: sparse clone)")
    parser.add_argument("--abis", help="comma-separated subset of apk.abis")
    parser.add_argument("--apk", action="append", default=[], metavar="ABI=PATH",
                        help="use a local Kodi APK (e.g. a source build) for ABI")
    args = parser.parse_args()

    props = load_properties(args.properties)
    abis = [a for a in props["apk.abis"].split(",") if a]
    if args.abis:
        wanted = [a.strip() for a in args.abis.split(",") if a.strip()]
        unknown = set(wanted) - set(abis)
        if unknown:
            fail(f"unsupported ABIs {sorted(unknown)}; Kodi {props['kodi.version']} ships {abis}")
        abis = wanted
    local_apks = {}
    for item in args.apk:
        abi, _, path = item.partition("=")
        if not path or not Path(path).is_file():
            fail(f"--apk {item}: expected ABI=/path/to/kodi.apk")
        local_apks[abi] = Path(path).resolve()

    args.cache.mkdir(parents=True, exist_ok=True)
    kodi_root = ensure_kodi_source(props, args.cache, args.kodi_source)

    apks = {}
    for abi in abis:
        if abi in local_apks:
            apks[abi] = local_apks[abi]
        else:
            url = props[f"apk.{abi}.url"]
            apks[abi] = download(url, args.cache / "apk" / url.rsplit("/", 1)[1],
                                 props[f"apk.{abi}.sha256"])

    out = args.out.resolve()
    if out.exists():
        shutil.rmtree(out)
    out_java, out_res, out_jni, out_assets = (out / d for d in ("java", "res", "jniLibs", "assets"))

    java_files = generate_java(kodi_root, out_java)
    generate_res(kodi_root, out_res)
    defined = prefix_resources(out_res, java_files, out_java)
    for abi in abis:
        log(f"{abi}: {extract_libs(abi, apks[abi], out_jni)} native libraries from {apks[abi].name}")

    asset_abi = CANONICAL_ASSET_ABI if CANONICAL_ASSET_ABI in abis else abis[0]
    provenance = [
        f"Kodi {props['kodi.version']} ({props['kodi.tag']}), source commit {kodi_commit(kodi_root)}",
        "Native libraries and assets repackaged from:",
    ]
    for abi in abis:
        provenance.append(f"  {abi}: {apks[abi].name} sha256={sha256_of(apks[abi])}")
    provenance += [
        f"Assets taken from the {asset_abi} package; binary add-on <platform> tags set to android.",
        "Java glue generated from tools/android/packaging/xbmc/src with package org.xbmc.kodi;",
        "patches: see mpvlibAndroid kodi/scripts/prepare_kodi.py (JAVA_PATCHES).",
        f"Source code: https://github.com/xbmc/xbmc/tree/{props['kodi.tag']}",
        "",
    ]
    entries, retagged = build_assets(apks[asset_abi], out_assets, kodi_root, "\n".join(provenance))
    log(f"assets: {entries} entries from {apks[asset_abi].name}; retagged {retagged}")
    log("resources renamed: " + ", ".join(
        f"{t}={sorted(n)}" for t, n in defined.items() if n))
    log(f"done: {out}")


if __name__ == "__main__":
    main()
