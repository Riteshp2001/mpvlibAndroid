#!/bin/bash -e

. ../../include/path.sh

if [ "$1" == "build" ]; then
	true
elif [ "$1" == "clean" ]; then
	rm -rf _build$ndk_suffix
	exit 0
else
	exit 255
fi

mkdir -p _build$ndk_suffix
cd _build$ndk_suffix

echo "Building MuJS"
echo "CC=$CC"
echo "AR=$AR"
echo "RANLIB=$RANLIB"
echo "prefix_dir=$prefix_dir"

# Build MuJS as static library.
# IMPORTANT: use CC and CFLAGS from buildall.sh.
# Do not manually override CC here.
"$CC" $CFLAGS -fPIC -c ../one.c -o one.o
"$AR" rcs libmujs.a one.o
"$RANLIB" libmujs.a

mkdir -p "$prefix_dir/lib"
mkdir -p "$prefix_dir/include"
mkdir -p "$prefix_dir/lib/pkgconfig"

cp libmujs.a "$prefix_dir/lib/"
cp ../mujs.h "$prefix_dir/include/"

# IMPORTANT:
# This repo uses pkg-config with prefix sysroot style.
# So prefix should be /usr/local, not the absolute GitHub runner path.
cat > "$prefix_dir/lib/pkgconfig/mujs.pc" <<EOF
prefix=/usr/local
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: MuJS
Description: Lightweight JavaScript interpreter
Version: ${v_mujs}
Libs: -L\${libdir} -lmujs
Cflags: -I\${includedir}
EOF

echo "MuJS installed successfully:"
ls -l "$prefix_dir/lib/libmujs.a"
ls -l "$prefix_dir/include/mujs.h"
cat "$prefix_dir/lib/pkgconfig/mujs.pc"