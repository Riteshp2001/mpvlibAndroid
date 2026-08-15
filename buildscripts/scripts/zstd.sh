#!/bin/bash -e

. ../../include/path.sh
. ../../include/depinfo.sh

clean_build_outputs () {
	make -C lib clean || :
	rm -f lib/libzstd.a
}

if [ "$1" == "build" ]; then
	true
elif [ "$1" == "clean" ]; then
	clean_build_outputs
	exit 0
else
	exit 255
fi

# zstd builds in-tree, so remove any previous ABI objects before each build.
clean_build_outputs
make -C lib -j"$cores" libzstd.a \
	CC="$CC" \
	AR="$AR" \
	RANLIB="$RANLIB" \
	CFLAGS="${CFLAGS:-} -fPIC -O2"

mkdir -p "$prefix_dir/include" "$prefix_dir/lib/pkgconfig"
cp lib/zstd.h lib/zdict.h lib/zstd_errors.h "$prefix_dir/include/"
cp lib/libzstd.a "$prefix_dir/lib/"

cat >"$prefix_dir/lib/pkgconfig/libzstd.pc" <<ZSTDPC
prefix=/usr/local
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: zstd
Description: Fast lossless compression algorithm library
Version: ${v_zstd}
Libs: -L\${libdir} -lzstd
Cflags: -I\${includedir}
ZSTDPC
