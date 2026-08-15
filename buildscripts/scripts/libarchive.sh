#!/bin/bash -e

. ../../include/path.sh
. ../../include/depinfo.sh
. ../../include/cmake-android.sh

build=_build$ndk_suffix

if [ "$1" == "build" ]; then
	true
elif [ "$1" == "clean" ]; then
	rm -rf "$build"
	exit 0
else
	exit 255
fi

android_cmake_setup . "$build" \
	-DBUILD_SHARED_LIBS=OFF \
	-DENABLE_TEST=OFF \
	-DENABLE_TAR=OFF \
	-DENABLE_CPIO=OFF \
	-DENABLE_CAT=OFF \
	-DENABLE_UNZIP=OFF \
	-DENABLE_ACL=OFF \
	-DENABLE_XATTR=OFF \
	-DENABLE_ZLIB=ON \
	-DENABLE_BZip2=ON \
	-DBZIP2_INCLUDE_DIR="$prefix_dir/include" \
	-DBZIP2_LIBRARIES="$prefix_dir/lib/libbz2.a" \
	-DENABLE_LZMA=ON \
	-DLIBLZMA_INCLUDE_DIR="$prefix_dir/include" \
	-DLIBLZMA_LIBRARY="$prefix_dir/lib/liblzma.a" \
	-DENABLE_LZ4=OFF \
	-DENABLE_LZO=OFF \
	-DENABLE_ZSTD=ON \
	-DZSTD_INCLUDE_DIR="$prefix_dir/include" \
	-DZSTD_LIBRARY="$prefix_dir/lib/libzstd.a" \
	-DENABLE_LIBB2=OFF \
	-DENABLE_OPENSSL=OFF \
	-DENABLE_MBEDTLS=OFF \
	-DENABLE_NETTLE=OFF \
	-DENABLE_CNG=OFF \
	-DENABLE_LIBXML2=ON \
	-DLIBXML2_INCLUDE_DIR="$prefix_dir/include/libxml2" \
	-DLIBXML2_LIBRARIES="$prefix_dir/lib/libxml2.a" \
	-DENABLE_EXPAT=OFF \
	-DENABLE_PCREPOSIX=OFF \
	-DENABLE_PCRE2POSIX=OFF \
	-DENABLE_ICONV=ON \
	-DICONV_INCLUDE_DIR="$prefix_dir/include" \
	-DLIBICONV_PATH="$prefix_dir/lib/libiconv.a" \
	-DENABLE_WERROR=OFF

android_cmake_build "$build"
android_cmake_install "$build"

pc="$prefix_dir/lib/pkgconfig/libarchive.pc"
if [ -f "$pc" ]; then
	for lib in -lbz2 -llzma -lzstd -liconv -lcharset; do
		if ! grep -q -- "^Libs:.*$lib" "$pc"; then
			${SED:-sed} -i.bak "/^Libs:/ s/$/ $lib/" "$pc"
			rm -f "$pc.bak"
		fi
	done
fi
