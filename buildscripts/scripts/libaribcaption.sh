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

freetype_lib="$prefix_dir/lib/libfreetype.a"
freetype_include="$prefix_dir/include/freetype2"

android_cmake_setup . "$build" \
	-DARIBCC_BUILD_TESTS=OFF \
	-DARIBCC_SHARED_LIBRARY=OFF \
	-DARIBCC_USE_FREETYPE=ON \
	-DARIBCC_USE_EMBEDDED_FREETYPE=OFF \
	-DFREETYPE_LIBRARY="$freetype_lib" \
	-DFREETYPE_INCLUDE_DIR_ft2build="$freetype_include" \
	-DFREETYPE_INCLUDE_DIR_freetype2="$freetype_include"
android_cmake_build "$build"
android_cmake_install "$build"
