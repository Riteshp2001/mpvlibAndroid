#!/bin/bash -e

. ../../include/path.sh
. ../../include/depinfo.sh

build=_build$ndk_suffix

if [ "$1" == "build" ]; then
	true
elif [ "$1" == "clean" ]; then
	rm -rf "$build"
	exit 0
else
	exit 255
fi

mkdir -p "$build"
cd "$build"

CFLAGS="${CFLAGS:-} -fPIC" ../configure \
	--host="$ndk_triple" \
	--prefix=/usr/local \
	--enable-static \
	--disable-shared \
	--with-pic

make -j"$cores"
make DESTDIR="$prefix_dir" install

cat >"$prefix_dir/lib/pkgconfig/iconv.pc" <<ICONVPC
prefix=/usr/local
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: iconv
Description: GNU libiconv
Version: ${v_libiconv}
Libs: -L\${libdir} -liconv -lcharset
Cflags: -I\${includedir}
ICONVPC
