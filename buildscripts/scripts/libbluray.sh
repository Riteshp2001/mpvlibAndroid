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

unset CC CXX # meson wants these unset

if grep -q "java_arch = 'arm'" meson.build; then
	${SED:-sed} -i.bak "/^elif host_machine.cpu_family() == 'aarch64'$/,/^elif / s/java_arch = 'arm'/java_arch = 'aarch64'/" meson.build
	rm -f meson.build.bak
fi
if [ "$ndk_suffix" = _arm64 ] && ! grep -A1 "^elif host_machine.cpu_family() == 'aarch64'$" meson.build | grep -q "java_arch = 'aarch64'"; then
	echo "libbluray aarch64 JVM search path was not patched" >&2
	exit 1
fi

meson setup $build --cross-file "$prefix_dir"/crossfile.txt \
	-Denable_tools=false \
	-Denable_docs=false \
	-Denable_devtools=false \
	-Denable_examples=false \
	-Dbdj_jar=disabled \
	-Dembed_udfread=false \
	-Dfontconfig=disabled \
	-Dfreetype=enabled \
	-Dlibxml2=disabled

ninja -C $build -j$cores
DESTDIR="$prefix_dir" ninja -C $build install

pc="$prefix_dir/lib/pkgconfig/libbluray.pc"
if ! grep -q -- "-ludfread" "$pc"; then
	${SED:-sed} -i.bak '/^Libs:/ s/$/ -ludfread/' "$pc"
	rm -f "$pc.bak"
fi
