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

unset CC CXX # meson wants these unset

meson setup "$build" --cross-file "$prefix_dir"/crossfile.txt \
	--default-library static \
	-Dauto_features=disabled \
	-Dfft=builtin \
	-Dresampler=builtin \
	-Djni=disabled \
	-Dladspa=disabled \
	-Dlv2=disabled \
	-Dvamp=disabled \
	-Dcmdline=disabled \
	-Dtests=disabled

ninja -C "$build" -j"$cores"
DESTDIR="$prefix_dir" ninja -C "$build" install

pc="$prefix_dir/lib/pkgconfig/rubberband.pc"
if [ -f "$pc" ] && ! grep -q '^Libs:.*-lc++' "$pc"; then
	${SED:-sed} -i.bak '/^Libs:/ s/$/ -lc++ -latomic/' "$pc"
	rm -f "$pc.bak"
fi
