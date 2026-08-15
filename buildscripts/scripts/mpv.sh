#!/bin/bash -e

. ../../include/path.sh

build=_build$ndk_suffix

if [ "$1" == "build" ]; then
	true
elif [ "$1" == "clean" ]; then
	rm -rf $build
	exit 0
else
	exit 255
fi

# Android provides Vulkan, but no pkgconfig file.
mkdir -p "$prefix_dir"/lib/pkgconfig
cat >"$prefix_dir"/lib/pkgconfig/vulkan.pc <<END
Name: Vulkan
Description:
Version: 1.3.275
Libs: -lvulkan
Cflags:
END

unset CC CXX # meson wants these unset

export LDFLAGS="$LDFLAGS -L$prefix_dir/lib"
export CPPFLAGS="$CPPFLAGS -I$prefix_dir/include"

echo "Checking MuJS before building mpv..."

if [ ! -f "$prefix_dir/lib/libmujs.a" ]; then
	echo "Error: libmujs.a not found at $prefix_dir/lib/libmujs.a" >&2
	echo "Build mujs first: ./buildall.sh --arch $prefix_name mujs" >&2
	exit 1
fi

if [ ! -f "$prefix_dir/include/mujs.h" ]; then
	echo "Error: mujs.h not found at $prefix_dir/include/mujs.h" >&2
	exit 1
fi

if [ ! -f "$prefix_dir/lib/pkgconfig/mujs.pc" ]; then
	echo "Error: mujs.pc not found at $prefix_dir/lib/pkgconfig/mujs.pc" >&2
	exit 1
fi

echo "pkg-config check for mujs:"
pkg-config --libs mujs
pkg-config --cflags mujs

meson setup $build --cross-file "$prefix_dir"/crossfile.txt \
	--default-library shared \
	-Diconv=disabled \
	-Dlua=enabled \
	-Djavascript=enabled \
	-Dlibcurl=enabled \
	-Dvulkan=enabled \
	-Dlibmpv=true \
	-Dcplayer=false \
	-Dmanpage-build=disabled

ninja -C $build -j$cores

if [ -f $build/libmpv.a ]; then
	echo >&2 "Meson produced static libmpv.a instead of shared libmpv.so, forcing rebuild."
	$0 clean
	exec $0 build
fi

DESTDIR="$prefix_dir" ninja -C $build install
