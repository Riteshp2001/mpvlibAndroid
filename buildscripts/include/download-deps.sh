#!/bin/bash -e

. ./include/depinfo.sh

[ -z "$IN_CI" ] && IN_CI=0
[ -z "$WGET" ] && WGET=wget

mkdir -p deps && cd deps

clone_ci_commit() {
	local repository=$1
	local expected_commit=$2
	local directory=$3
	local clone_mode=${4:-}

	if ! (
		set -e
		git init -q "$directory"
		git -C "$directory" remote add origin "$repository"
		git -C "$directory" fetch -q --depth=1 origin "$expected_commit"
		git -C "$directory" checkout -q --detach FETCH_HEAD
		if [[ "$clone_mode" == recursive ]]; then
			git -C "$directory" submodule update -q \
				--init --recursive --depth=1
		fi
		[[ $(git -C "$directory" rev-parse --verify 'HEAD^{commit}') == \
			"$expected_commit" ]]
	); then
		echo "Failed to check out $repository commit $expected_commit." >&2
		rm -rf "$directory"
		return 1
	fi
}

# mbedtls
if [ ! -d mbedtls ]; then
	mkdir mbedtls
	$WGET https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$v_mbedtls/mbedtls-$v_mbedtls.tar.bz2 -O - | \
		tar -xj -C mbedtls --strip-components=1
fi

# dav1d
if [ ! -d dav1d ]; then
	if [ "$IN_CI" -eq 1 ]; then
		: "${DAV1D_GIT_COMMIT:?DAV1D_GIT_COMMIT must be set in CI}"
		clone_ci_commit \
			"${DAV1D_GIT_URL:-https://github.com/videolan/dav1d}" \
			"$DAV1D_GIT_COMMIT" dav1d
	else
		git clone --branch "$v_ci_dav1d" \
			"${DAV1D_GIT_URL:-https://github.com/videolan/dav1d}" dav1d
	fi
fi

# ffmpeg
if [ ! -d ffmpeg ]; then
	if [ "$IN_CI" -eq 1 ]; then
		: "${FFMPEG_GIT_COMMIT:?FFMPEG_GIT_COMMIT must be set in CI}"
		clone_ci_commit \
			"${FFMPEG_GIT_URL:-https://github.com/FongMi/FFmpeg.git}" \
			"$FFMPEG_GIT_COMMIT" ffmpeg
	else
		git clone --branch "$v_ci_ffmpeg" \
			"${FFMPEG_GIT_URL:-https://github.com/FongMi/FFmpeg.git}" ffmpeg
	fi
fi

# uavs3d
if [ ! -d uavs3d ]; then
	clone_ci_commit https://github.com/uavs3/uavs3d.git "$v_ci_uavs3d" uavs3d
fi
if [ "$(git -C uavs3d rev-parse HEAD)" != "$v_ci_uavs3d" ]; then
	echo "uavs3d source revision does not match the pinned build dependency." >&2
	exit 1
fi

# freetype2
if [ ! -d freetype2 ]; then
	mkdir freetype2
	freetype_archive="../sources/freetype-$v_freetype.tar.gz"
	if [ ! -f "$freetype_archive" ]; then
		echo "Missing local Freetype archive: $freetype_archive" >&2
		exit 1
	fi
	tar -xz -f "$freetype_archive" -C freetype2 --strip-components=1
fi

# fribidi
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

# libmysofa
if [ ! -d libmysofa ]; then
	mkdir libmysofa
	$WGET https://github.com/hoene/libmysofa/archive/refs/tags/v${v_libmysofa}.tar.gz -O - | \
		tar -xz -C libmysofa --strip-components=1
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
../include/patch-libbluray.sh libbluray

if [ ! -d libiconv ]; then
	mkdir libiconv
	$WGET https://mirrors.ocf.berkeley.edu/gnu/libiconv/libiconv-${v_libiconv}.tar.gz -O - | \
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

libdvdread_patch=../patches/libdvdread-7.0.1-iso9660.patch
if patch --batch --forward --fuzz=0 --dry-run -d libdvdread -p1 < "$libdvdread_patch" >/dev/null 2>&1; then
	patch --batch --forward --fuzz=0 -d libdvdread -p1 < "$libdvdread_patch"
elif ! patch --batch --reverse --fuzz=0 --dry-run -d libdvdread -p1 < "$libdvdread_patch" >/dev/null 2>&1; then
	echo "libdvdread source does not match $libdvdread_patch." >&2
	exit 1
fi

# libdvdnav
if [ ! -d libdvdnav ]; then
	mkdir libdvdnav
	$WGET https://downloads.videolan.org/pub/videolan/libdvdnav/${v_libdvdnav}/libdvdnav-${v_libdvdnav}.tar.xz -O - | \
		tar -xJ -C libdvdnav --strip-components=1
fi

for libdvdnav_patch in ../patches/libdvdnav-7.0.0-go-up-hop.patch \
	../patches/libdvdnav-7.0.0-pgc-cell-duration.patch \
	../patches/libdvdnav-7.0.0-menu-availability.patch \
	../patches/libdvdnav-7.0.0-vm-copy-failure.patch \
	../patches/libdvdnav-7.0.0-time-search-floor.patch \
	../patches/libdvdnav-7.0.0-menu-audio-count.patch \
	../patches/libdvdnav-7.0.0-title-chapter-duration.patch \
	../patches/libdvdnav-7.0.0-preserve-stream-callbacks.patch \
	../patches/libdvdnav-7.0.0-active-logical-stream.patch \
	../patches/libdvdnav-7.0.0-title-scope.patch \
	../patches/libdvdnav-7.0.0-stream-attributes.patch \
	../patches/libdvdnav-7.0.0-block-replay.patch; do
	if patch --batch --forward --fuzz=0 --dry-run -d libdvdnav -p1 < "$libdvdnav_patch" >/dev/null 2>&1; then
		patch --batch --forward --fuzz=0 -d libdvdnav -p1 < "$libdvdnav_patch"
	elif ! patch --batch --reverse --fuzz=0 --dry-run -d libdvdnav -p1 < "$libdvdnav_patch" >/dev/null 2>&1; then
		echo "libdvdnav source does not match $libdvdnav_patch." >&2
		exit 1
	fi
done

# rubberband
if [ ! -d rubberband ]; then
	mkdir rubberband
	$WGET https://github.com/breakfastquay/rubberband/archive/refs/tags/v${v_rubberband}.tar.gz -O - | \
		tar -xz -C rubberband --strip-components=1
fi

# libass
if [ ! -d libass ]; then
	if [ "$IN_CI" -eq 1 ]; then
		: "${LIBASS_GIT_COMMIT:?LIBASS_GIT_COMMIT must be set in CI}"
		clone_ci_commit \
			"${LIBASS_GIT_URL:-https://github.com/libass/libass}" \
			"$LIBASS_GIT_COMMIT" libass
	else
		git clone --branch "$v_ci_libass" \
			"${LIBASS_GIT_URL:-https://github.com/libass/libass}" libass
	fi
fi

# lua
check_sha256() {
	local digest
	if command -v sha256sum >/dev/null; then
		digest=$(sha256sum "$1")
	else
		digest=$(shasum -a 256 "$1")
	fi
	[[ ${digest%% *} == "$2" ]]
}

download_lua() {
	local archive=lua-$v_lua.tar.gz
	local checksum=b9e2e4aad6789b3b63a056d442f7b39f0ecfca3ae0f1fc0ae4e9614401b69f4b
	local url

	for url in \
		"https://www.lua.org/ftp/$archive" \
		"https://mirror.bazel.build/www.lua.org/ftp/$archive"
	do
		rm -f "$archive"
		if $WGET "$url" -O "$archive" && check_sha256 "$archive" "$checksum"
		then
			rm -rf lua
			mkdir lua
			if tar -xz -C lua --strip-components=1 -f "$archive"; then
				rm "$archive"
				return 0
			fi
		fi
	done

	rm -rf lua "$archive"
	return 1
}

if [ ! -f lua/src/lua.h ]; then
	download_lua
fi

# mujs
if [ ! -d mujs ]; then
	mkdir mujs
	$WGET https://mujs.com/downloads/mujs-$v_mujs.tar.gz -O - | \
		tar -xz -C mujs --strip-components=1
fi

# shaderc is built from the NDK-provided sources; this placeholder keeps it in
# the dependency graph without cloning an extra copy.
mkdir -p shaderc

# libplacebo
if [ ! -d libplacebo ]; then
	if [ "$IN_CI" -eq 1 ]; then
		: "${LIBPLACEBO_GIT_COMMIT:?LIBPLACEBO_GIT_COMMIT must be set in CI}"
		clone_ci_commit \
			"${LIBPLACEBO_GIT_URL:-https://github.com/FongMi/libplacebo.git}" \
			"$LIBPLACEBO_GIT_COMMIT" libplacebo recursive
	else
		git clone --recursive --branch "$v_ci_libplacebo" \
			"${LIBPLACEBO_GIT_URL:-https://github.com/FongMi/libplacebo.git}" libplacebo
	fi
fi

# curl
if [ ! -d curl ]; then
	mkdir curl
	$WGET https://curl.se/download/curl-$v_curl.tar.gz -O - | \
		tar -xz -C curl --strip-components=1
fi

# mpv
: "${MPV_GIT_URL:=https://github.com/FongMi/mpv.git}"
: "${MPV_GIT_REF:=fongmi}"
if [ ! -d mpv ]; then
	if [ -n "$MPV_GIT_REF" ]; then
		git clone --branch "$MPV_GIT_REF" "$MPV_GIT_URL" mpv
	else
		git clone "$MPV_GIT_URL" mpv
	fi
fi

cd ..
