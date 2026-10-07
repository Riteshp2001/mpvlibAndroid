#!/bin/bash -e

. ../../include/path.sh
. ../../include/depinfo.sh
. ../../include/cmake-android.sh

build="_build$ndk_suffix"

if [ "$1" == "clean" ]; then
	rm -rf "$build"
	exit 0
elif [ "$1" != "build" ]; then
	exit 255
fi

if [ "$(git rev-parse HEAD)" != "$v_ci_uavs3d" ]; then
	echo "uavs3d source revision does not match the pinned build dependency." >&2
	exit 1
fi

neon_args=()
[[ "$prefix_name" == armv7l ]] && neon_args=(-DANDROID_ARM_NEON=ON)

android_cmake_setup . "$build" -DBUILD_SHARED_LIBS=OFF -DCOMPILE_10BIT=ON "${neon_args[@]}"
cmake --build "$build" --target uavs3d --parallel "$cores"
android_cmake_install "$build"
pc="$prefix_dir/lib/pkgconfig/uavs3d.pc"
[ -f "$pc" ] || { echo "uavs3d pkg-config metadata is missing." >&2; exit 1; }
${SED:-sed} -i 's/ -lpthread//g' "$pc"
"${INSTALL:-install}" -Dm644 COPYING "$prefix_dir/share/licenses/uavs3d/COPYING"