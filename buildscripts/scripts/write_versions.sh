#!/bin/bash -e

# Credit goes to jmir1
# Export the versions of the native components packaged into the AAR.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

. buildscripts/include/depinfo.sh

case "$1" in
	""|-arm64|-x86|-x64) ;;
	*) echo "Unsupported NDK suffix: $1" >&2; exit 1 ;;
esac

packaged_prefixes=()
for prefix in armv7l arm64 x86 x86_64; do
	[ -f "buildscripts/prefix/$prefix/lib/libmpv.so" ] && packaged_prefixes+=("$prefix")
done
if [ ${#packaged_prefixes[@]} -eq 0 ]; then
	echo "No packaged ABI prefixes were found." >&2
	exit 1
fi

pc_version() {
	local package=$1
	local found=
	local prefix
	local version
	for prefix in "${packaged_prefixes[@]}"; do
		local pc_dir="buildscripts/prefix/$prefix/lib/pkgconfig"
		version=$(PKG_CONFIG_LIBDIR="$pc_dir" PKG_CONFIG_PATH= pkg-config --modversion "$package" 2>/dev/null || true)
		if [ -z "$version" ]; then
			echo "$package version metadata is missing for packaged ABI $prefix" >&2
			return 1
		fi
		if [ -n "$found" ] && [ "$found" != "$version" ]; then
			echo "$package version differs between packaged ABIs: $found != $version" >&2
			return 1
		fi
		found=$version
	done
	printf '%s\n' "$found"
}

require_version() {
	local component=$1
	local version=$2
	case "$version" in
		""|unknown|*'$'*|*'%'*)
			echo "Unable to determine $component version; refusing to package incomplete version metadata." >&2
			exit 1
			;;
	esac
}

# mpv.pc reports the libmpv client API version, not the mpv player version.
# Reading every packaged prefix also rejects stale or mixed-version multi-ABI AARs.
MPV_CLIENT_API_VERSION=$(pc_version mpv)
MPV_CLIENT_API_VERSION=${MPV_CLIENT_API_VERSION%.0}

# Meson writes the version used by the compiled library to this generated header.
MPV_VERSION_HEADER="buildscripts/deps/mpv/_build$1/common/version.h"
MPV_VERSION=
if [ -f "$MPV_VERSION_HEADER" ]; then
	MPV_VERSION=$(sed -n 's/^#define VERSION "\(.*\)"/\1/p' "$MPV_VERSION_HEADER" | head -n 1)
	MPV_VERSION=${MPV_VERSION#v}
fi

# Match mpv's version generation when the generated header is unavailable.
if [ -z "$MPV_VERSION" ]; then
	MPV_VERSION=$(git -C buildscripts/deps/mpv describe \
		--abbrev=9 \
		--tags \
		--dirty \
		--match 'v0.*' 2>/dev/null || true)
	MPV_VERSION=${MPV_VERSION#v}
fi

if [ -z "$MPV_VERSION" ] && [ -f buildscripts/deps/mpv/MPV_VERSION ]; then
	MPV_VERSION=$(sed 's/-UNKNOWN$//' buildscripts/deps/mpv/MPV_VERSION)
fi

# Installed pkg-config files survive CI prefix cache restores and describe the
# artifacts that are actually packaged rather than whichever sources are present.
LIBPLACEBO_VERSION=$(pc_version libplacebo)
LIBASS_VERSION=$(pc_version libass)
DAV1D_VERSION=$(pc_version dav1d)

# Keep FFmpeg's release version separate from libavcodec's ABI version.
FFMPEG_VERSION=${v_ci_ffmpeg#n}
LIBAVCODEC_VERSION=$(pc_version libavcodec)

# Read pinned dependencies from installed artifacts as well, preventing retained
# local source directories from being mislabeled after a configured pin changes.
MBEDTLS_VERSION=$(pc_version mbedtls)
LUA_VERSION=$(pc_version lua)
MUJS_VERSION=$(pc_version mujs)
FREETYPE_VERSION=$(pc_version freetype2)
FRIBIDI_VERSION=$(pc_version fribidi)
HARFBUZZ_VERSION=$(pc_version harfbuzz)
LIBUNIBREAK_VERSION=$(pc_version libunibreak)
# Keep the public Versions API stable while making the omitted native
# dependency explicit to consumers of this branch.
SHADERC_VERSION=disabled
NDK_VERSION=$v_ndk

require_version mpv "$MPV_VERSION"
require_version "libmpv client API" "$MPV_CLIENT_API_VERSION"
require_version FFmpeg "$FFMPEG_VERSION"
require_version libavcodec "$LIBAVCODEC_VERSION"
require_version libplacebo "$LIBPLACEBO_VERSION"
require_version libass "$LIBASS_VERSION"
require_version dav1d "$DAV1D_VERSION"
require_version MbedTLS "$MBEDTLS_VERSION"
require_version Lua "$LUA_VERSION"
require_version MuJS "$MUJS_VERSION"
require_version FreeType "$FREETYPE_VERSION"
require_version FriBidi "$FRIBIDI_VERSION"
require_version HarfBuzz "$HARFBUZZ_VERSION"
require_version libunibreak "$LIBUNIBREAK_VERSION"
require_version "Android NDK" "$NDK_VERSION"

# Get the build date from mpv's compiled object file.
DATE=unknown
OBJ_FILE="buildscripts/deps/mpv/_build$1/libmpv.so.p/common_version.c.o"
if [ -f "$OBJ_FILE" ]; then
	RODATA_SECTION=$(readelf "$OBJ_FILE" -S 2>/dev/null | grep .rodata || true)
	if [ -n "$RODATA_SECTION" ]; then
		START_RODATA=0x$(echo "$RODATA_SECTION" | cut -d ' ' -f 27)
		START=0x$(readelf "$OBJ_FILE" -s 2>/dev/null | grep mpv_builddate | cut -d ' ' -f 7)
		SIZE=$(readelf "$OBJ_FILE" -s 2>/dev/null | grep mpv_builddate | cut -d ' ' -f 11)
		if [ -n "$START" ] && [ -n "$SIZE" ]; then
			SKIP=$((START_RODATA + START - 1))
			dd if="$OBJ_FILE" of=date.txt bs=1 skip=$SKIP count=$SIZE 2>/dev/null
			DATE=$(cat date.txt 2>/dev/null)
			rm -f date.txt
		fi
	fi
fi

versions_file=app/src/main/java/is/xyz/mpv/Utils.kt

escape_sed_replacement() {
	printf '%s' "$1" | sed 's/[&|\\]/\\&/g'
}

write_version() {
	local property=$1
	local value
	value=$(escape_sed_replacement "$2")
	if ! grep -q "^[[:space:]]*$property = \"" "$versions_file"; then
		echo "Version property '$property' is missing from $versions_file." >&2
		exit 1
	fi
	${SED:-sed} -i -E 's|^([[:space:]]*'"$property"' = ).*(,)$|\1"'"$value"'"\2|' "$versions_file"
}

# Replace complete property assignments so repeated local builds refresh values
# instead of retaining metadata written by the first build in a checkout.
write_version mpv "$MPV_VERSION"
write_version mpvClientApi "$MPV_CLIENT_API_VERSION"
write_version buildDate "$DATE"
write_version ffmpeg "$FFMPEG_VERSION"
write_version libAvcodec "$LIBAVCODEC_VERSION"
write_version libPlacebo "$LIBPLACEBO_VERSION"
write_version libAss "$LIBASS_VERSION"
write_version dav1d "$DAV1D_VERSION"
write_version mbedTls "$MBEDTLS_VERSION"
write_version lua "$LUA_VERSION"
write_version muJs "$MUJS_VERSION"
write_version freeType "$FREETYPE_VERSION"
write_version friBidi "$FRIBIDI_VERSION"
write_version harfBuzz "$HARFBUZZ_VERSION"
write_version libUnibreak "$LIBUNIBREAK_VERSION"
write_version shaderc "$SHADERC_VERSION"
write_version androidNdk "$NDK_VERSION"

if grep -qE '%[A-Z0-9_]+%' "$versions_file"; then
	echo "Unresolved version placeholders remain in $versions_file." >&2
	exit 1
fi
