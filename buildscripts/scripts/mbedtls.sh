#!/bin/bash -e

. ../../include/path.sh
. ../../include/depinfo.sh

if [ "$1" == "build" ]; then
	true
elif [ "$1" == "clean" ]; then
	make clean
	exit 0
else
	exit 255
fi

$0 clean # separate building not supported, always clean
if [[ "$ndk_triple" == "i686"* ]]; then
	./scripts/config.py unset MBEDTLS_AESNI_C
else
	./scripts/config.py set MBEDTLS_AESNI_C
fi
./scripts/config.py set MBEDTLS_PLATFORM_DEV_RANDOM '"/dev/urandom"'

make -j$cores no_test
make DESTDIR="$prefix_dir" install

# The Makefile install does not provide pkg-config metadata, but the AAR
# version manifest reads installed artifacts for every packaged ABI.
mkdir -p "$prefix_dir/lib/pkgconfig"
cat >"$prefix_dir/lib/pkgconfig/mbedtls.pc" <<MBEDTLSPC
prefix=/usr/local
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: Mbed TLS
Description: Lightweight cryptographic and SSL/TLS library
Version: ${v_mbedtls}
Libs: -L\${libdir} -lmbedtls -lmbedx509 -lmbedcrypto
Cflags: -I\${includedir}
MBEDTLSPC
