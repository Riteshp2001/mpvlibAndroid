#!/bin/bash -e

. ../../include/path.sh

build=_build$ndk_suffix

if [ "$1" == "build" ]; then
	true
elif [ "$1" == "clean" ]; then
	rm -rf "$build"
	exit 0
else
	exit 255
fi

# Compile a single static, Android-targeted LittleCMS2 with no optional
# image codecs, utilities, or GPL-only plugins.
unset CC CXX
meson setup "$build" --cross-file "$prefix_dir/crossfile.txt" \
	-Ddefault_library=static \
	-Dtests=disabled -Djpeg=disabled -Dtiff=disabled \
	-Dutils=false -Dfastfloat=false -Dthreaded=false

ninja -C "$build" -j"$cores"
DESTDIR="$prefix_dir" ninja -C "$build" install

test -f "$prefix_dir/lib/liblcms2.a" || {
	echo "LittleCMS2 static archive was not installed" >&2
	exit 1
}
test -f "$prefix_dir/lib/pkgconfig/lcms2.pc" || {
	echo "LittleCMS2 pkg-config metadata was not installed" >&2
	exit 1
}
pkg-config --modversion lcms2 | grep -Fx "$v_lcms2"
"${INSTALL:-install}" -Dm644 LICENSE "$prefix_dir/share/licenses/lcms2/LICENSE"
