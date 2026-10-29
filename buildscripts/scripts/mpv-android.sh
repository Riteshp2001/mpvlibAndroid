#!/bin/bash -e

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
BUILD="$DIR/.."
MPV_ANDROID="$DIR/../.."

. $BUILD/include/path.sh
. $BUILD/include/depinfo.sh

if [ "$1" == "build" ]; then
	true
elif [ "$1" == "clean" ]; then
	rm -rf $MPV_ANDROID/{app,.}/build $MPV_ANDROID/app/src/main/{libs,obj}
	exit 0
else
	exit 255
fi

nativeprefix () {
	if [ -f $BUILD/prefix/$1/lib/libmpv.so ]; then
		echo $BUILD/prefix/$1
	else
		echo >&2 "Warning: libmpv.so not found in native prefix for $1, support will be omitted"
	fi
}

prefix32=$(nativeprefix "armv7l")
prefix64=$(nativeprefix "arm64")
prefix_x64=$(nativeprefix "x86_64")
prefix_x86=$(nativeprefix "x86")

if [[ -z "$prefix32" && -z "$prefix64" && -z "$prefix_x64" && -z "$prefix_x86" ]]; then
	echo >&2 "Error: no mpv library detected."
	exit 255
fi

notices_prefix=${prefix64:-${prefix32:-${prefix_x64:-$prefix_x86}}}
mkdir -p app/src/main/assets/openjoc
for notice in LICENSE THIRD_PARTY_NOTICES.md SOURCE.txt; do
	install -m 0644 "$notices_prefix/share/openjoc/$notice" "app/src/main/assets/openjoc/$notice"
done

for model in sadie-ii-d1-ku100.ojhrtf sadie-ii-d2-kemar.ojhrtf MODELS.sha256; do
	install -m 0644 "$BUILD/prefix/share/openjoc/$model" "app/src/main/assets/openjoc/$model"
done

PREFIX32="$prefix32" PREFIX64="$prefix64" PREFIX_X64="$prefix_x64" PREFIX_X86="$prefix_x86" \
ndk-build -C app/src/main -j$cores

for abi in armeabi-v7a arm64-v8a x86_64 x86; do
	if [ -d "app/src/main/libs/$abi" ]; then
		python3 "$BUILD/include/ffmpeg-shared.py" verify-package "app/src/main/libs/$abi"
	fi
done

if [ -n "$MPV_ANDROID_NATIVE_ONLY" ]; then
	echo "Skipping Gradle AAR build because MPV_ANDROID_NATIVE_ONLY is set."
	exit 0
fi

chmod +x "$BUILD/scripts/write_versions.sh"
"$BUILD/scripts/write_versions.sh" "$ndk_suffix"

### Java parts
# Android's gradle plugin needs both of these to correctly strip libraries.
# We could pass them directly to Gradle but by using this file it will persist
# inside Android Studio too.
printf '%s\n' \
	"# This file is automatically written by the build scripts, and read using Gradle" \
	"ndkVersion=$v_ndk_n" "ndkRoot=$ANDROID_NDK_ROOT" >ndk.properties

targets=(assembleDebug)
if [ -z "$DONT_BUILD_RELEASE" ]; then
	targets+=(assembleRelease)
fi
./gradlew "${targets[@]}"
