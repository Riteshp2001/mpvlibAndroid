#!/usr/bin/env bash
# Builds Kodi for Android from source, in release configuration, following Kodi's
# docs/README.Android.md at the tag pinned in kodi.properties. Use the APK it produces in place of
# the official one:  ./gradlew :kodi:assembleRelease -PkodiApk.<abi>=<apk>
#
# Usage: build-kodi-source.sh <x86|arm64-v8a|armeabi-v7a> [work dir] [make jobs]
#
# Needs the host packages listed in README.Android.md, ANDROID_HOME (the NDK the README pins is
# installed with sdkmanager when missing) and JAVA_HOME pointing to JDK 17 for Kodi's Gradle 8.3.
# Patches in kodi/patches/*.patch, if any, are applied to the Kodi checkout first.
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
SRC=$WORK/xbmc-$TAG
TARBALLS=$WORK/tarballs
LOGS=$WORK/logs
BUILD_DIR=$WORK/build-$ABI
mkdir -p "$TARBALLS" "$LOGS"

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
    -storepass "$KODI_ANDROID_STORE_PASSWORD" -keyalg RSA -keysize 2048 -validity 10000 > /dev/null
fi
export ANDROID_HOME ANDROID_SDK_ROOT=$ANDROID_HOME JAVA_HOME
unset CC CXX CFLAGS CXXFLAGS CPPFLAGS LDFLAGS PKG_CONFIG_PATH

step() {
  local name=$1 t0
  shift
  t0=$(date +%s)
  echo "[$(date -Iseconds)] $ABI: $name"
  if ! "$@" > "$LOGS/$ABI-$name.log" 2>&1; then
    echo "$ABI: $name failed after $(( $(date +%s) - t0 ))s; see $LOGS/$ABI-$name.log" >&2
    tail -20 "$LOGS/$ABI-$name.log" >&2
    exit 1
  fi
}

if [ ! -d "$SRC/.git" ]; then
  step clone git -c advice.detachedHead=false clone --depth 1 --branch "$TAG" "$(prop kodi.git)" "$SRC"
  for patch in "$MODULE"/patches/*.patch; do
    [ -e "$patch" ] || continue
    step "patch-$(basename "$patch" .patch)" git -C "$SRC" apply "$patch"
  done
fi

step prefetch python3 "$MODULE/scripts/prefetch_kodi_tarballs.py" "$SRC/tools/depends" "$TARBALLS"
cd "$SRC/tools/depends"
step depends-bootstrap ./bootstrap
step depends-configure ./configure --with-tarballs="$TARBALLS" --host="$HOST" \
  --with-sdk-path="$ANDROID_HOME" --with-ndk-path="$NDK" --prefix="$WORK/xbmc-depends" --enable-debug=no
step depends make -j"$JOBS"
# The official APKs ship this binary add-on only.
step binary-addons make -j"$JOBS" -C target/binary-addons ADDONS="peripheral.joystick"
step cmake make -C target/cmakebuildsys BUILD_DIR="$BUILD_DIR"
cd "$BUILD_DIR"
step kodi make -j"$JOBS"
step apk make apk
ls -1 "$SRC"/kodiapp-*-release.apk
