#!/bin/bash -e

. ./include/depinfo.sh

[ -z "$IN_CI" ] && IN_CI=0
[ -z "$WGET" ] && WGET=wget

mkdir -p deps && cd deps

# mbedtls - use git clone with correct directory structure
if [ ! -d mbedtls ]; then
	git clone --depth 1 --branch mbedtls-$v_mbedtls https://github.com/Mbed-TLS/mbedtls.git mbedtls-tmp
	mv mbedtls-tmp mbedtls
	# Initialize submodules (required for config.py and build scripts)
	git -C mbedtls submodule update --init --recursive
fi

# dav1d (canonical repo, GitHub is read-only mirror)
[ ! -d dav1d ] && git clone https://github.com/videolan/dav1d

# ffmpeg
if [ ! -d ffmpeg ]; then
	git clone https://github.com/FFmpeg/FFmpeg ffmpeg
	[ $IN_CI -eq 1 ] && git -C ffmpeg checkout $v_ci_ffmpeg
fi
if ! git -C ffmpeg apply --reverse --check ../../patches/ffmpeg_force_mpegts.patch 2>/dev/null; then
	git -C ffmpeg apply ../../patches/ffmpeg_force_mpegts.patch
fi

# freetype2
if [ ! -d freetype2 ]; then
	mkdir freetype2
	$WGET https://download.savannah.gnu.org/releases/freetype/freetype-$v_freetype.tar.gz -O - | \
		tar -xz -C freetype2 --strip-components=1
fi

# fribidi - use vX.Y.Z tag format for releases
if [ ! -d fribidi ]; then
	mkdir fribidi
	$WGET https://github.com/fribidi/fribidi/releases/download/v$v_fribidi/fribidi-$v_fribidi.tar.xz -O - | \
		tar -xJ -C fribidi --strip-components=1
fi

# harfbuzz
if [ ! -d harfbuzz ]; then
	mkdir harfbuzz
	$WGET https://github.com/harfbuzz/harfbuzz/releases/download/$v_harfbuzz/harfbuzz-$v_harfbuzz.tar.xz -O - | \
		tar -xJ -C harfbuzz --strip-components=1
fi

# unibreak
if [ ! -d unibreak ]; then
	mkdir unibreak
	$WGET https://github.com/adah1972/libunibreak/releases/download/libunibreak_${v_unibreak//./_}/libunibreak-${v_unibreak}.tar.gz -O - | \
		tar -xz -C unibreak --strip-components=1
fi

# libxml2
if [ ! -d libxml2 ]; then
	mkdir libxml2
	$WGET https://gitlab.gnome.org/GNOME/libxml2/-/archive/v${v_libxml2}/libxml2-v${v_libxml2}.tar.gz -O - | \
		tar -xz -C libxml2 --strip-components=1
fi

# libaribcaption
if [ ! -d libaribcaption ]; then
	mkdir libaribcaption
	$WGET https://github.com/xqq/libaribcaption/archive/refs/tags/v${v_libaribcaption}.tar.gz -O - | \
		tar -xz -C libaribcaption --strip-components=1
fi

# libbluray
if [ ! -d libbluray ]; then
	mkdir libbluray
	$WGET https://downloads.videolan.org/pub/videolan/libbluray/${v_libbluray}/libbluray-${v_libbluray}.tar.xz -O - | \
		tar -xJ -C libbluray --strip-components=1
fi

# libiconv
if [ ! -d libiconv ]; then
	mkdir libiconv
	$WGET https://ftp.gnu.org/pub/gnu/libiconv/libiconv-${v_libiconv}.tar.gz -O - | \
		tar -xz -C libiconv --strip-components=1
fi

# uchardet
if [ ! -d uchardet ]; then
	mkdir uchardet
	$WGET https://gitlab.freedesktop.org/uchardet/uchardet/-/archive/v${v_uchardet}/uchardet-v${v_uchardet}.tar.gz -O - | \
		tar -xz -C uchardet --strip-components=1
fi

# bzip2
if [ ! -d bzip2 ]; then
	mkdir bzip2
	$WGET https://sourceware.org/pub/bzip2/bzip2-${v_bzip2}.tar.gz -O - | \
		tar -xz -C bzip2 --strip-components=1
fi

# xz
if [ ! -d xz ]; then
	mkdir xz
	$WGET https://github.com/tukaani-project/xz/releases/download/v${v_xz}/xz-${v_xz}.tar.xz -O - | \
		tar -xJ -C xz --strip-components=1
fi

# zstd
if [ ! -d zstd ]; then
	mkdir zstd
	$WGET https://github.com/facebook/zstd/releases/download/v${v_zstd}/zstd-${v_zstd}.tar.gz -O - | \
		tar -xz -C zstd --strip-components=1
fi

# libarchive
if [ ! -d libarchive ]; then
	mkdir libarchive
	$WGET https://github.com/libarchive/libarchive/releases/download/v${v_libarchive}/libarchive-${v_libarchive}.tar.xz -O - | \
		tar -xJ -C libarchive --strip-components=1
fi

# libdvdread
if [ ! -d libdvdread ]; then
	mkdir libdvdread
	$WGET https://downloads.videolan.org/pub/videolan/libdvdread/${v_libdvdread}/libdvdread-${v_libdvdread}.tar.xz -O - | \
		tar -xJ -C libdvdread --strip-components=1
fi

# libdvdnav
if [ ! -d libdvdnav ]; then
	mkdir libdvdnav
	$WGET https://downloads.videolan.org/pub/videolan/libdvdnav/${v_libdvdnav}/libdvdnav-${v_libdvdnav}.tar.xz -O - | \
		tar -xJ -C libdvdnav --strip-components=1
fi

# rubberband
if [ ! -d rubberband ]; then
	mkdir rubberband
	$WGET https://github.com/breakfastquay/rubberband/archive/refs/tags/v${v_rubberband}.tar.gz -O - | \
		tar -xz -C rubberband --strip-components=1
fi

# libass - use GitHub mirror
[ ! -d libass ] && git clone https://github.com/libass/libass

# lua - use 5.2.x (mpv requires < 5.3)
if [ ! -d lua ]; then
	mkdir lua
	$WGET https://www.lua.org/ftp/lua-$v_lua.tar.gz -O - | \
		tar -xz -C lua --strip-components=1
fi

# mujs
if [ ! -d mujs ]; then
	mkdir mujs
	$WGET https://mujs.com/downloads/mujs-$v_mujs.tar.gz -O - | \
		tar -xz -C mujs --strip-components=1
fi

# openssl
if [ ! -d openssl ]; then
	mkdir openssl
	$WGET https://github.com/openssl/openssl/releases/download/openssl-$v_openssl/openssl-$v_openssl.tar.gz -O - | \
		tar -xz -C openssl --strip-components=1
fi

# libplacebo - use GitHub mirror (haasn/libplacebo)
[ ! -d libplacebo ] && git clone --recursive https://github.com/haasn/libplacebo

# curl
if [ ! -d curl ]; then
	mkdir curl
	$WGET https://curl.se/download/curl-$v_curl.tar.gz -O - | \
		tar -xz -C curl --strip-components=1
fi

# mpv
[ ! -d mpv ] && git clone https://github.com/mpv-player/mpv
if ! git -C mpv apply --reverse --check ../../patches/mpv_video_shaders.patch 2>/dev/null; then
	git -C mpv apply ../../patches/mpv_video_shaders.patch
fi

cd ..
