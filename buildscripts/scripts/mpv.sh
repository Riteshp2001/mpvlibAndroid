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

if ! git apply --reverse --check ../../patches/mpv_video_shaders.patch 2>/dev/null; then
	git apply ../../patches/mpv_video_shaders.patch
fi

case "$prefix_dir" in
	"$DIR"/prefix/*) ;;
	*) echo "Invalid build prefix: $prefix_dir" >&2; exit 1 ;;
esac

rm -f "$prefix_dir/lib/pkgconfig/vulkan.pc"

check_iconv_files () {
	for file in "$prefix_dir/include/iconv.h" "$prefix_dir/lib/libiconv.a" "$prefix_dir/lib/libcharset.a"; do
		if [ ! -f "$file" ]; then
			echo "Missing libiconv file: $file" >&2
			exit 1
		fi
	done
}

patch_mpv_iconv_dependency () {
	local iconv_dep

	# Meson's built-in iconv dependency does not consult iconv.pc, and find_library()
	# has no extra search dirs here. Provide the just-built static libiconv directly.
	iconv_dep="iconv = declare_dependency(compile_args: ['-I$prefix_dir/include'], link_args: ['$prefix_dir/lib/libiconv.a', '$prefix_dir/lib/libcharset.a'])"
	${SED:-sed} -i.bak \
		-e "/^iconv = dependency('iconv', required: get_option('iconv'))$/c\\$iconv_dep" \
		-e "/^iconv = declare_dependency(compile_args: \['-I.*\/include'\], link_args: \['.*\/libiconv\.a', '.*\/libcharset\.a'\])$/c\\$iconv_dep" \
		meson.build
	rm -f meson.build.bak
}

unset CC CXX # meson wants these unset

export LDFLAGS="$LDFLAGS -L$prefix_dir/lib"
export CPPFLAGS="$CPPFLAGS -I$prefix_dir/include"

check_iconv_files
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

patch_mpv_iconv_dependency

meson setup $build --cross-file "$prefix_dir"/crossfile.txt \
	--default-library shared \
	-D{iconv,uchardet}=enabled \
	-D{libarchive,dvdnav}=enabled \
	-D{lua,libcurl,rubberband}=enabled \
	-Djavascript=enabled \
	-Dlibmpv=true -Dcplayer=false \
	-Dlibbluray=enabled \
	-Dvulkan=disabled \
	-Dmanpage-build=disabled

ninja -C $build -j$cores

if readelf -d "$build/libmpv.so" 2>/dev/null | grep -Fq libvulkan.so; then
	echo "Vulkan linkage detected in $build/libmpv.so; refusing to package it." >&2
	exit 1
fi

if [ -f $build/libmpv.a ]; then
	echo >&2 "Meson produced static libmpv.a instead of shared libmpv.so, forcing rebuild."
	$0 clean
	exec $0 build
fi

DESTDIR="$prefix_dir" ninja -C $build install
