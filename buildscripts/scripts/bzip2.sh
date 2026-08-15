#!/bin/bash -e

. ../../include/path.sh
. ../../include/depinfo.sh

clean_build_outputs () {
	make clean || :
	rm -f ./*.o libbz2.a
}

if [ "$1" == "build" ]; then
	true
elif [ "$1" == "clean" ]; then
	clean_build_outputs
	exit 0
else
	exit 255
fi

# bzip2 builds in-tree, so remove any previous ABI objects before each build.
clean_build_outputs
make -j"$cores" libbz2.a \
	CC="$CC" \
	AR="$AR" \
	RANLIB="$RANLIB" \
	CFLAGS="${CFLAGS:-} -fPIC -O2"

mkdir -p "$prefix_dir/include" "$prefix_dir/lib/pkgconfig"
cp bzlib.h "$prefix_dir/include/"
cp libbz2.a "$prefix_dir/lib/"

cat >"$prefix_dir/lib/pkgconfig/bzip2.pc" <<BZIP2PC
prefix=/usr/local
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: bzip2
Description: Lossless block-sorting data compression library
Version: ${v_bzip2}
Libs: -L\${libdir} -lbz2
Cflags: -I\${includedir}
BZIP2PC
