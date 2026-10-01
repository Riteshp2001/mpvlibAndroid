#!/bin/bash -e

. ../../include/path.sh
. ../../include/depinfo.sh
. ../../include/cmake-android.sh

build=_build$ndk_suffix

if [ "$1" == "build" ]; then
	true
elif [ "$1" == "clean" ]; then
	rm -rf "$build"
	exit 0
else
	exit 255
fi

android_cmake_setup . "$build" \
	-DBUILD_TESTS=OFF \
	-DBUILD_SHARED_LIBS=OFF \
	-DBUILD_STATIC_LIBS=ON
android_cmake_build "$build"
android_cmake_install "$build"

pc="$prefix_dir/lib/pkgconfig/libmysofa.pc"
if [ ! -f "$pc" ]; then
	echo "libmysofa pkg-config metadata was not installed: $pc" >&2
	exit 1
fi

# Upstream's pkg-config version omits patch releases. Preserve the source tag
# version so packaged dependency metadata remains exact.
${SED:-sed} -i.bak -E "s/^Version: .*/Version: ${v_libmysofa}/" "$pc"
rm -f "$pc.bak"

grep -q -- '-lz' "$pc" || {
	echo "libmysofa pkg-config metadata is missing its zlib dependency" >&2
	exit 1
}
grep -q -- '-lm' "$pc" || {
	echo "libmysofa pkg-config metadata is missing its math dependency" >&2
	exit 1
}