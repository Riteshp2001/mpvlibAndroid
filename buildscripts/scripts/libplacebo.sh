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

case "$prefix_dir" in
	"$DIR"/prefix/*) ;;
	*) echo "Invalid build prefix: $prefix_dir" >&2; exit 1 ;;
esac

rm -f \
	"$prefix_dir/lib/libshaderc.a" \
	"$prefix_dir/lib/libshaderc_combined.a" \
	"$prefix_dir/lib/pkgconfig/shaderc.pc" \
	"$prefix_dir/lib/pkgconfig/shaderc_combined.pc"

unset CC CXX
meson setup $build --cross-file "$prefix_dir"/crossfile.txt \
	-Dopengl=enabled \
	-Dvulkan=disabled \
	-Dshaderc=disabled \
	-Dlcms=enabled -Ddemos=false

# The feature must be compiled in, not merely installed in the sysroot.
grep -Eq '^#define PL_HAVE_LCMS 1([[:space:]]|$)' "$build/src/include/libplacebo/config.h" || {
	echo "LittleCMS2 support is disabled in libplacebo" >&2
	exit 1
}

ninja -C $build -j$cores
DESTDIR="$prefix_dir" ninja -C $build install

# add missing library for static linking
# this isn't "-lstdc++" due to a meson bug: https://github.com/mesonbuild/meson/issues/11300
${SED:-sed} '/^Libs:/ s|$| -llcms2 -lc++|' "$prefix_dir/lib/pkgconfig/libplacebo.pc" -i

# libplacebo is static; propagate lcms2 to libmpv's shared linker invocation.
pkg-config --libs libplacebo | grep -Fq -- '-llcms2' || {
	echo "libplacebo pkg-config metadata is missing -llcms2" >&2
	exit 1
}
