#!/bin/bash -e

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
BUILD="$DIR/.."
MPV_ANDROID="$DIR/../.."

. $BUILD/include/path.sh
. $BUILD/include/depinfo.sh
. $BUILD/include/build_config.sh

if [ "$1" == "build" ]; then
	true
elif [ "$1" == "clean" ]; then
	rm -rf $MPV_ANDROID/{app,.}/build $MPV_ANDROID/app/src/main/{libs,obj}
	rm -rf $MPV_ANDROID/app/src/main/assets/native-v9a
	exit 0
else
	exit 255
fi

[ -n "$ANDROID_SIGNING_KEY" ] && BUNDLE=1

nativeprefix () {
	if [ -f $BUILD/prefix/$1/lib/libmpv.so ]; then
		echo $BUILD/prefix/$1
	else
		echo >&2 "Warning: libmpv.so not found in native prefix for $1, support will be omitted"
	fi
}

pythonassetdir () {
	case "$1" in
		arm64) echo "$MPV_ANDROID/app/src/main/assets/py.arm64-v8a" ;;
		arm64-v9a) echo "$MPV_ANDROID/app/src/main/assets/py.arm64-v8a" ;; # v9a shares arm64 python
		x86) echo "$MPV_ANDROID/app/src/main/assets/py.x86" ;;
		x86_64) echo "$MPV_ANDROID/app/src/main/assets/py.x86_64" ;;
		*) return 1 ;;
	esac
}

check_python_assets () {
	local dir
	dir=$(pythonassetdir "$1") || return 1
	if [ ! -f "$dir/python3" ]; then
		echo >&2 "Error: missing python runtime for $1 at $dir/python3"
		echo >&2 "Build it first: ./buildall.sh --arch $1 python"
		exit 1
	fi
	if ! compgen -G "$dir/python3*.zip" > /dev/null; then
		echo >&2 "Error: missing python stdlib zip for $1 in $dir"
		echo >&2 "Build it first: ./buildall.sh --arch $1 python"
		exit 1
	fi
}

prefix64=$(nativeprefix "arm64")
prefix64_v9a=$(nativeprefix "arm64-v9a")
prefix_x64=$(nativeprefix "x86_64")
prefix_x86=$(nativeprefix "x86")

if [[ -z "$prefix64" && -z "$prefix64_v9a" && -z "$prefix_x64" && -z "$prefix_x86" ]]; then
	echo >&2 "Error: no mpv library detected."
	exit 255
fi

[ -n "$prefix64" ] && check_python_assets "arm64"
# v9a doesn't need separate python assets — shares with arm64
[ -n "$prefix_x64" ] && check_python_assets "x86_64"
[ -n "$prefix_x86" ] && check_python_assets "x86"



bash "$BUILD/scripts/write_versions.sh" $ndk_suffix

PREFIX64=$prefix64 PREFIX_X64=$prefix_x64 PREFIX_X86=$prefix_x86 \
ndk-build -C app/src/main -j$cores

# === ARM v9a optimized libraries — ship as assets for runtime loading ===
if [ -n "$prefix64_v9a" ]; then
	echo "Packaging ARM v9a optimized libraries into assets..."
	v9a_asset_dir="$MPV_ANDROID/app/src/main/assets/native-v9a"
	mkdir -p "$v9a_asset_dir"

	# Copy all shared libraries from v9a prefix to assets
	for so in "$prefix64_v9a"/lib/lib*.so; do
		[ -f "$so" ] || continue
		local_name=$(basename "$so")
		# Strip version suffixes (e.g., libavcodec.so.61 -> libavcodec.so)
		# We need the unversioned .so for System.load()
		base_name="${local_name%%.*}.so"
		if [ -L "$so" ]; then
			# Follow symlinks to get the actual library
			real_so=$(readlink -f "$so")
			cp -v "$real_so" "$v9a_asset_dir/$base_name"
		else
			cp -v "$so" "$v9a_asset_dir/$base_name"
		fi
	done

	# Build a dedicated arm64-v9a version of libplayer.so
	echo "Building ARM v9a optimized libplayer.so..."
	PREFIX64="$prefix64_v9a" \
	ndk-build -C app/src/main -j$cores APP_ABI=arm64-v8a \
		NDK_OUT="$MPV_ANDROID/app/src/main/obj-v9a" \
		NDK_LIBS_OUT="$MPV_ANDROID/app/src/main/libs-v9a"

	# Copy the v9a-optimized libplayer.so to the v9a assets folder
	cp -v "$MPV_ANDROID/app/src/main/libs-v9a/arm64-v8a/libplayer.so" "$v9a_asset_dir/libplayer.so"

	# Clean up temporary directories
	rm -rf "$MPV_ANDROID/app/src/main/obj-v9a" "$MPV_ANDROID/app/src/main/libs-v9a"

	echo "ARM v9a libraries packaged at: $v9a_asset_dir"
	ls -lh "$v9a_asset_dir/"
fi

targets=(assembleDebug)
if [ -z "$DONT_BUILD_RELEASE" ]; then
	targets+=(assembleRelease)
	[ -n "$BUNDLE" ] && targets+=(bundleRelease)
fi
./gradlew "${targets[@]}"

if [ -n "$ANDROID_SIGNING_KEY" ]; then
	cd "${MPV_ANDROID}/app/build/outputs/apk"
	apksigner=${ANDROID_HOME}/build-tools/${v_sdk_build_tools}/apksigner
	for v in default api29; do
		pushd $v
		# sign the universal debug APK
		"$apksigner" sign --ks "${ANDROID_SIGNING_KEY}" \
			--in debug/app-$v-universal-debug.apk --out debug/app-$v-universal-debug-signed.apk
		# but all of the release APKs
		for apk in release/*-unsigned.apk; do
			"$apksigner" sign --ks "${ANDROID_SIGNING_KEY}" \
				--in $apk --out ${apk/-unsigned/-signed}
		done
		popd
	done
	# and the bundle
	cd ../bundle
	if [ -n "$BUNDLE" ]; then
		if [ -z "$ANDROID_SIGNING_ALIAS" ]; then
			echo >&2 "Error: ANDROID_SIGNING_ALIAS must be set to use jarsigner"
			exit 1
		fi
		pushd defaultRelease
		jarsigner -keystore "${ANDROID_SIGNING_KEY}" -signedjar \
			app-default-release-signed.aab app-default-release.aab \
			"${ANDROID_SIGNING_ALIAS}"
		popd
	fi
fi
