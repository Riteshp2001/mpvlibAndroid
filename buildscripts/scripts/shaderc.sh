#!/bin/bash -e

. ../../include/path.sh
. ../../include/depinfo.sh

: "${android_api:=24}"
: "${prefix_name:=${ndk_suffix#_}}"

case "$prefix_name" in
	armv7l)
	android_abi=armeabi-v7a
	;;
	arm64)
	android_abi=arm64-v8a
	;;
	x86)
	android_abi=x86
	;;
	x86_64)
	android_abi=x86_64
	;;
	*)
		echo "Invalid architecture: $prefix_name" >&2
		exit 1
	;;
esac

shaderc_dir="$DIR/deps/shaderc/_source_$android_abi"
if [ -L "$shaderc_dir" ]; then
	echo "Refusing symlink shaderc build directory: $shaderc_dir" >&2
	exit 1
fi

if [ "$1" == "build" ]; then
	true
elif [ "$1" == "clean" ]; then
	rm -rf "$shaderc_dir" "$prefix_dir/include/shaderc"
	rm -f "$prefix_dir/lib/libshaderc.a" "$prefix_dir/lib/pkgconfig/shaderc.pc"
	exit 0
else
	exit 255
fi

ndk_dir="$DIR/sdk/android-ndk-${v_ndk}"
source_dir="$ndk_dir/sources/third_party/shaderc"
shaderc_lib="$shaderc_dir/libs/c++_static/$android_abi/libshaderc.a"
ndk_build="$ndk_dir/ndk-build"

if [ ! -d "$source_dir" ]; then
	echo "NDK shaderc sources not found: $source_dir" >&2
	exit 1
fi
if [ ! -x "$ndk_build" ]; then
	echo "ndk-build not found: $ndk_build" >&2
	exit 1
fi
if [ ! -f "$source_dir/Android.mk" ]; then
	echo "NDK shaderc Android.mk not found: $source_dir/Android.mk" >&2
	exit 1
fi

# The NDK is shared. Patch only an ABI-private copy of its pinned source.
wrapper=libshaderc_util/src/spirv_tools_wrapper.cc
original_sha=f6478d8d386226589f7bfb68f624681835dd4c1ce33fbce2654bb5c3a2bde5e2
patched_sha=1dd414a1b1696d76916db8aaf1d8c781c8e7e6238d27d7442a30fda789c1b034
patch_file="$DIR/patches/shaderc-r30-validator-friendly-names.patch"
actual_sha=$(sha256sum "$source_dir/$wrapper")
if [ "${actual_sha%% *}" != "$original_sha" ]; then
	echo "NDK shaderc wrapper does not match the pinned source." >&2
	exit 1
fi
patch_sha=$(sha256sum "$patch_file")
identity="$v_ndk_n $original_sha ${patch_sha%% *}"
if [ ! -d "$shaderc_dir" ]; then
	mkdir -p "$shaderc_dir"
	# Exclude shared obj/libs and generated headers, even after an earlier build.
	for source in Android.mk CHANGES libshaderc libshaderc_util third_party utils; do
		cp -R "$source_dir/$source" "$shaderc_dir/"
	done
	patch --batch --forward --fuzz=0 --dry-run -d "$shaderc_dir" -p1 < "$patch_file"
	patch --batch --forward --fuzz=0 -d "$shaderc_dir" -p1 < "$patch_file"
	printf '%s\n' "$identity" > "$shaderc_dir/.source-identity"
fi
actual_sha=$(sha256sum "$shaderc_dir/$wrapper")
if [ "${actual_sha%% *}" != "$patched_sha" ] ||
	[ "$(cat "$shaderc_dir/.source-identity")" != "$identity" ]; then
	echo "Private shaderc source identity changed; clean this ABI before rebuilding." >&2
	exit 1
fi
patch --batch --reverse --fuzz=0 --dry-run -d "$shaderc_dir" -p1 < "$patch_file"

"$ndk_build" -C "$shaderc_dir" \
	NDK_PROJECT_PATH=. \
	APP_BUILD_SCRIPT=Android.mk \
	APP_PLATFORM=android-$android_api \
	APP_STL=c++_static \
	APP_ABI="$android_abi" \
	libshaderc_combined \
	-j$cores
if [ ! -f "$shaderc_lib" ]; then
	echo "shaderc build output not found: $shaderc_lib" >&2
	exit 1
fi

mkdir -p "$prefix_dir/include" "$prefix_dir/lib/pkgconfig"
rm -rf "$prefix_dir/include/shaderc"
cp -R "$shaderc_dir/include/shaderc" "$prefix_dir/include/"
cp "$shaderc_lib" "$prefix_dir/lib/libshaderc.a"

cat >"$prefix_dir/lib/pkgconfig/shaderc.pc" <<SHADERCPC
prefix=/usr/local
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: shaderc
Description: Android NDK shaderc static library
Version: 2024.0
Libs: -L\${libdir} -lshaderc
Cflags: -I\${includedir}
SHADERCPC
