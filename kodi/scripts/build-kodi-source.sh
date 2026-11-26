#!/usr/bin/env bash
# Builds Kodi for Android from source, in release configuration, following Kodi's
# docs/README.Android.md at the tag pinned in kodi.properties. Use the APK it produces in place of
# the official one:  ./gradlew :kodi:assembleRelease -PkodiApk.<abi>=<apk>
#
# Usage: build-kodi-source.sh <x86|arm64-v8a|armeabi-v7a> [work dir] [make jobs]
#
# Needs the host packages listed in README.Android.md, ANDROID_HOME (the NDK the README pins is
# installed with sdkmanager when missing) and JAVA_HOME pointing to JDK 17 for Kodi's Gradle 8.3.
# Each ABI gets its own checkout under <work dir>/<abi>; patches in kodi/patches/*.patch are
# applied to it. Completed steps are skipped when the script runs again, so it resumes after a
# failure; delete <work dir>/<abi> to start over.
set -euo pipefail

ABI=${1:?usage: build-kodi-source.sh <x86|arm64-v8a|armeabi-v7a> [work dir] [make jobs]}
WORK=$(mkdir -p "${2:-$HOME/kodi-android-build}" && cd "${2:-$HOME/kodi-android-build}" && pwd)
JOBS=${3:-$(nproc)}
MODULE=$(cd "$(dirname "$0")/.." && pwd)
NDK_VERSION=21.4.7075529

case "$ABI" in
  x86) HOST=i686-linux-android ;;
  arm64-v8a) HOST=aarch64-linux-android ;;
  armeabi-v7a) HOST=arm-linux-androideabi ;;
  *) echo "unsupported ABI $ABI" >&2; exit 1 ;;
esac

prop() { sed -n "s/^$1=//p" "$MODULE/kodi.properties"; }
TAG=$(prop kodi.tag)
TARBALLS=$WORK/tarballs
SRC=$WORK/$ABI/xbmc
BUILD_DIR=$WORK/$ABI/build
LOGS=$WORK/$ABI/logs
STAMPS=$WORK/$ABI/stamps
mkdir -p "$TARBALLS" "$LOGS" "$STAMPS"

: "${ANDROID_HOME:?set ANDROID_HOME to the Android SDK}"
: "${JAVA_HOME:?set JAVA_HOME to JDK 17}"
"$JAVA_HOME/bin/java" -version 2>&1 | grep -q 'version "17' ||
  { echo "JAVA_HOME must be JDK 17 (Kodi $TAG uses Gradle 8.3)" >&2; exit 1; }
NDK=$ANDROID_HOME/ndk/$NDK_VERSION
if [ ! -d "$NDK" ]; then
  yes | "$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" --sdk_root="$ANDROID_HOME" "ndk;$NDK_VERSION" > "$LOGS/ndk-install.log"
fi

# make apk signs with these; README.Android.md section 3.3 creates the same debug key.
export KODI_ANDROID_STORE_FILE=${KODI_ANDROID_STORE_FILE:-$WORK/debug.keystore}
export KODI_ANDROID_STORE_PASSWORD=${KODI_ANDROID_STORE_PASSWORD:-android}
export KODI_ANDROID_KEY_ALIAS=${KODI_ANDROID_KEY_ALIAS:-androiddebugkey}
export KODI_ANDROID_KEY_PASSWORD=${KODI_ANDROID_KEY_PASSWORD:-android}
if [ ! -f "$KODI_ANDROID_STORE_FILE" ]; then
  "$JAVA_HOME/bin/keytool" -genkey -keystore "$KODI_ANDROID_STORE_FILE" -alias "$KODI_ANDROID_KEY_ALIAS" \
    -dname "CN=Android Debug,O=Android,C=US" -keypass "$KODI_ANDROID_KEY_PASSWORD" \
    -storepass "$KODI_ANDROID_STORE_PASSWORD" -keyalg RSA -keysize 2048 -validity 10000 > /dev/null 2>&1
fi
export ANDROID_HOME ANDROID_SDK_ROOT=$ANDROID_HOME JAVA_HOME
unset CC CXX CFLAGS CXXFLAGS CPPFLAGS LDFLAGS PKG_CONFIG_PATH

run() {
  local name=$1 t0
  shift
  t0=$(date +%s)
  echo "[$(date -Iseconds)] $ABI: $name"
  if ! "$@" > "$LOGS/$name.log" 2>&1; then
    echo "$ABI: $name failed after $(( $(date +%s) - t0 ))s; see $LOGS/$name.log" >&2
    tail -20 "$LOGS/$name.log" >&2
    exit 1
  fi
}

# Like run, but skipped once it has succeeded.
step() {
  if [ -e "$STAMPS/$1" ]; then
    echo "$ABI: $1 already done"
    return
  fi
  run "$@"
  touch "$STAMPS/$1"
}

# run calls these in a condition, where set -e does not apply: they return on the first failure.
clone() {
  local patch
  rm -rf "$SRC" || return
  git -c advice.detachedHead=false clone --depth 1 --branch "$TAG" "$(prop kodi.git)" "$SRC" || return
  for patch in "$MODULE"/patches/*.patch; do
    [ -e "$patch" ] || continue
    echo "applying $(basename "$patch")"
    git -C "$SRC" apply "$patch" || return
  done
}

configure_depends() (
  cd "$SRC/tools/depends" &&
    ./bootstrap &&
    ./configure --with-tarballs="$TARBALLS" --host="$HOST" --with-sdk-path="$ANDROID_HOME" \
      --with-ndk-path="$NDK" --prefix="$WORK/$ABI/depends" --enable-debug=no
)

# The Gradle wrapper gives up after a 10 s network timeout. Put the distribution that Kodi's
# packaging uses where the wrapper looks for it, downloading it with retries.
fetch_gradle() {
  local url name dir
  url=$(sed -n 's/^distributionUrl=//p' "$SRC/tools/android/packaging/gradle/wrapper/gradle-wrapper.properties" |
    sed 's/\\:/:/g')
  name=$(basename "$url" .zip)
  dir=${GRADLE_USER_HOME:-$HOME/.gradle}/wrapper/dists/$name/$(python3 -c '
import hashlib, sys
n, digits, out = int(hashlib.md5(sys.argv[1].encode()).hexdigest(), 16), "0123456789abcdefghijklmnopqrstuvwxyz", ""
while n:
    n, r = divmod(n, 36)
    out = digits[r] + out
print(out)' "$url")
  if [ -e "$dir/$name.zip.ok" ] || [ -s "$dir/$name.zip" ]; then
    return
  fi
  mkdir -p "$dir" &&
    curl -fsSL --retry 10 --retry-connrefused --connect-timeout 15 -o "$dir/$name.zip.part" "$url" &&
    mv "$dir/$name.zip.part" "$dir/$name.zip"
}

step clone clone
run prefetch python3 "$MODULE/scripts/prefetch_kodi_tarballs.py" "$SRC/tools/depends" "$TARBALLS"
step configure configure_depends
step depends make -C "$SRC/tools/depends" -j"$JOBS"
# The official APKs ship this binary add-on only.
step binary-addons make -C "$SRC/tools/depends/target/binary-addons" -j"$JOBS" ADDONS="peripheral.joystick"
step cmake make -C "$SRC/tools/depends/target/cmakebuildsys" BUILD_DIR="$BUILD_DIR"
step kodi make -C "$BUILD_DIR" -j"$JOBS"
run gradle fetch_gradle
run apk make -C "$BUILD_DIR" apk
ls -1 "$SRC"/kodiapp-*-release.apk
