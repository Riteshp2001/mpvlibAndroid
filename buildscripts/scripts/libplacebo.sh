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

unset CC CXX
if [ ! -f "$prefix_dir/lib/libshaderc.a" ]; then
	echo "shaderc dependency is missing: $prefix_dir/lib/libshaderc.a" >&2
	exit 1
fi
export CFLAGS="-I$prefix_dir/include ${CFLAGS:-}"
export CXXFLAGS="-I$prefix_dir/include ${CXXFLAGS:-}"
export LDFLAGS="-L$prefix_dir/lib ${LDFLAGS:-}"
meson setup "$build" --cross-file "$prefix_dir"/crossfile.txt \
	-Dopengl=enabled -Dvulkan=enabled \
	-Dshaderc=enabled -Dglslang=disabled \
	-Dlcms=enabled -Ddemos=false

config_header="$build/src/include/libplacebo/config.h"
for backend in OPENGL VULKAN; do
	grep -Eq \
		"^#define PL_HAVE_${backend} 1([[:space:]]|$)" \
		"$config_header" || {
		echo "libplacebo backend PL_HAVE_${backend}=1 is missing." >&2
		exit 1
	}
done

# The feature must be compiled in, not merely installed in the sysroot.
grep -Eq '^#define PL_HAVE_LCMS 1([[:space:]]|$)' "$config_header" || {
	echo "LittleCMS2 support is disabled in libplacebo" >&2
	exit 1
}

ninja -C "$build" -j"$cores"
destdir=${LIBPLACEBO_DESTDIR:-$prefix_dir}
install_prefix=$prefix_dir
if [ -n "${LIBPLACEBO_DESTDIR:-}" ]; then
	install_prefix=$destdir/usr/local
fi
DESTDIR="$destdir" ninja -C "$build" install
"${INSTALL:-install}" -Dm644 LICENSE \
	"$install_prefix/share/licenses/libplacebo/LICENSE"

# add missing libraries for static linking
# this isn't "-lstdc++" due to a meson bug: https://github.com/mesonbuild/meson/issues/11300
${SED:-sed} '/^Libs:/ s|$| -lshaderc -llcms2 -lc++|' "$install_prefix/lib/pkgconfig/libplacebo.pc" -i

# Ensure static LittleCMS2 reaches libmpv through pkg-config's regular Libs.
grep -Eq '^Libs:.*[[:space:]]-llcms2([[:space:]]|$)' "$install_prefix/lib/pkgconfig/libplacebo.pc" || {
	echo "libplacebo pkg-config metadata is missing -llcms2" >&2
	exit 1
}
