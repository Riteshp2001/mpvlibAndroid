#!/bin/bash -e
set -o pipefail

. ../../include/path.sh

build=_build$ndk_suffix

check_iconv_files () {
	for file in "$prefix_dir/include/iconv.h" "$prefix_dir/lib/libiconv.a" "$prefix_dir/lib/libcharset.a"; do
		if [ ! -f "$file" ]; then
			echo "Missing libiconv file: $file" >&2
			exit 1
		fi
	done
}

patch_mpv_iconv_dependency () {
	local iconv_dep

	# Meson's built-in iconv dependency does not consult iconv.pc, and find_library()
	# has no extra search dirs here. Provide the just-built static libiconv directly.
	iconv_dep="iconv = declare_dependency(compile_args: ['-I$prefix_dir/include'], link_args: ['$prefix_dir/lib/libiconv.a', '$prefix_dir/lib/libcharset.a'])"
	${SED:-sed} -i.bak \
		-e "/^iconv = dependency('iconv', required: get_option('iconv'))$/c\\$iconv_dep" \
		-e "/^iconv = declare_dependency(compile_args: \['-I.*\/include'\], link_args: \['.*\/libiconv\.a', '.*\/libcharset\.a'\])$/c\\$iconv_dep" \
		meson.build
	rm -f meson.build.bak
}

motion_receipt="$prefix_dir/share/mpv-motion/provenance.properties"
if [ "$1" == "build" ]; then
	# A failed rebuild must not leave an older archive accepted as current.
	rm -f "$motion_receipt"
elif [ "$1" == "clean" ]; then
	rm -rf "$build"
	exit 0
else
	exit 255
fi

if ! git apply --reverse --check ../../patches/mpv_video_shaders.patch 2>/dev/null; then
	git apply ../../patches/mpv_video_shaders.patch
fi

if ! git apply --reverse --check ../../patches/mpv_android_fdsan_fork.patch 2>/dev/null; then
	git apply ../../patches/mpv_android_fdsan_fork.patch
fi

if ! git apply --reverse --check ../../patches/mpv_lsfg_layer.patch 2>/dev/null; then
	git apply ../../patches/mpv_lsfg_layer.patch
fi

unset CC CXX # meson wants these unset

check_iconv_files
if [ ! -f "$prefix_dir/lib/libmujs.a" ] ||
		[ ! -f "$prefix_dir/include/mujs.h" ] ||
		[ ! -f "$prefix_dir/lib/pkgconfig/mujs.pc" ]; then
	echo "MuJS dependency is incomplete in $prefix_dir." >&2
	exit 1
fi

patch_mpv_iconv_dependency

# FongMi's renderer SDK ships mpv's GPU motion core; record exactly which source built it.
# The patches above must leave these files alone.
motion_core=video/out/gpu_next/motion.c
motion_header=video/out/gpu_next/motion.h
motion_commit=$(git rev-parse HEAD)
motion_source_sha=$(sha256sum "$motion_core" | cut -d ' ' -f 1)
motion_header_sha=$(sha256sum "$motion_header" | cut -d ' ' -f 1)
git diff --quiet HEAD -- "$motion_core" "$motion_header" LICENSE.LGPL || {
	echo "Motion SDK source differs from its commit." >&2
	exit 1
}

meson setup "$build" --cross-file "$prefix_dir"/crossfile.txt \
	--default-library shared \
	-D{iconv,uchardet}=enabled \
	-D{libarchive,dvdnav}=enabled \
	-Dlua=luajit \
	-D{libcurl,rubberband}=enabled \
	-Djavascript=enabled \
	-Dlibmpv=true -Dcplayer=false \
	-Dlibbluray=enabled \
	-Dvulkan=enabled \
	-Dmanpage-build=disabled

if ! grep -Eq '^#define HAVE_CLONE 0$' "$build/config.h"; then
	echo "Android libmpv configured with unsafe HAVE_CLONE; refusing to build." >&2
	exit 1
fi

ninja -C "$build" -j"$cores"

if readelf --wide --dyn-syms "$build/libmpv.so" | grep -Eq '[[:space:]]clone@LIBC([[:space:]]|$)'; then
	echo "Android libmpv still imports clone@LIBC; refusing to package it." >&2
	exit 1
fi
if ! readelf --wide --dyn-syms "$build/libmpv.so" | grep -Eq '[[:space:]]fork@LIBC([[:space:]]|$)'; then
	echo "Android libmpv does not import fork@LIBC; subprocess fallback is missing." >&2
	exit 1
fi

if [ -f "$build/libmpv.a" ]; then
	echo >&2 "Meson produced static libmpv.a; forcing a clean rebuild."
	$0 clean
	exec $0 build
fi
DESTDIR="$prefix_dir" ninja -C "$build" install

# Bind the installed archive to the source actually used by this build.
[ "$motion_commit" = "$(git rev-parse HEAD)" ] &&
[ "$motion_source_sha" = "$(sha256sum "$motion_core" | cut -d ' ' -f 1)" ] &&
[ "$motion_header_sha" = "$(sha256sum "$motion_header" | cut -d ' ' -f 1)" ] &&
git diff --quiet HEAD -- "$motion_core" "$motion_header" LICENSE.LGPL &&
cmp -s "$motion_header" "$prefix_dir/include/mpv-motion/motion.h" || {
	echo "Motion SDK source or installed header changed during build." >&2
	exit 1
}
motion_archive_sha=$(sha256sum "$prefix_dir/lib/libmpv-motion.a" | cut -d ' ' -f 1)
mkdir -p "$(dirname "$motion_receipt")"
cat >"$motion_receipt.tmp" <<EOF
schema.version=1
mpv-motion.arch=$prefix_name
mpv-motion.commit=$motion_commit
mpv-motion.source.sha256=$motion_source_sha
mpv-motion.header.sha256=$motion_header_sha
mpv-motion.archive.sha256=$motion_archive_sha
EOF
mv "$motion_receipt.tmp" "$motion_receipt"
