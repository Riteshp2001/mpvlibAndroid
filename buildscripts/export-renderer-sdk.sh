#!/bin/bash -e

cd "$(dirname "${BASH_SOURCE[0]}")/.."
. buildscripts/include/depinfo.sh

output=build/renderer-sdk
libplacebo_api=
rm -rf "$output"
mkdir -p "$output"

copy_abi() {
	local prefix_name=$1
	local android_abi=$2
	local prefix="buildscripts/prefix/$prefix_name"
	local destination="$output/$android_abi"
	local config="$prefix/include/libplacebo/config.h"
	local opengl="$prefix/include/libplacebo/opengl.h"
	local vulkan="$prefix/include/libplacebo/vulkan.h"
	local libav="$prefix/include/libplacebo/utils/libav.h"
	local current_api
	local header

	for required in \
		"$config" \
		"$opengl" \
		"$vulkan" \
		"$libav" \
		"$prefix/lib/libplacebo.a" \
		"$prefix/lib/libshaderc.a"; do
		[ -f "$required" ] || {
			echo "Renderer SDK input is missing: $required" >&2
			exit 1
		}
	done
	current_api=$(awk '$1 == "#define" && $2 == "PL_API_VER" { print $3; exit }' "$config")
	if [[ ! "$current_api" =~ ^[0-9]+$ ]]; then
		echo "A numeric libplacebo API is required for $android_abi." >&2
		exit 1
	fi
	if [[ -n "$libplacebo_api" && "$current_api" != "$libplacebo_api" ]]; then
		echo "libplacebo API differs between renderer SDK ABIs." >&2
		exit 1
	fi
	libplacebo_api=$current_api
	for capability in \
		'bool external_yuv;' \
		'bool disable_storage;' \
		'struct pl_vulkan_ycbcr_params {' \
		'const struct pl_vulkan_ycbcr_params *ycbcr;' \
		'pl_map_avframe_dovi_metadata('; do
		case "$capability" in
			'bool external_yuv;') header=$opengl ;;
			'pl_map_avframe_dovi_metadata(') header=$libav ;;
			*) header=$vulkan ;;
		esac
		grep -Fq "$capability" "$header" || {
			echo "libplacebo is missing renderer capability '$capability' for $android_abi." >&2
			exit 1
		}
	done
	for backend in OPENGL VULKAN; do
		grep -Eq "^#define PL_HAVE_${backend} 1([[:space:]]|$)" "$config" || {
			echo "${backend}-enabled libplacebo is required for $android_abi." >&2
			exit 1
		}
	done
	grep -Eq '^#define PL_HAVE_SHADERC 1([[:space:]]|$)' "$config" || {
		echo "shaderc-enabled libplacebo is required for $android_abi." >&2
		exit 1
	}

	mkdir -p "$destination/include" "$destination/lib"
	cp -R "$prefix/include/libplacebo" "$destination/include/"
	cp "$prefix/lib/libplacebo.a" "$destination/lib/"
	cp "$prefix/lib/libshaderc.a" "$destination/lib/"
	(
		cd "$destination/include"
		find libplacebo -type f -print0 \
			| sort -z \
			| xargs -0 sha256sum
	) >"$destination/headers.sha256"
}

copy_abi armv7l armeabi-v7a
copy_abi arm64 arm64-v8a

license=buildscripts/prefix/arm64/share/licenses/libplacebo/LICENSE
[ -f "$license" ] || {
	echo "libplacebo license is missing: $license" >&2
	exit 1
}
cp "$license" "$output/LICENSE"

cat >"$output/provenance.properties" <<EOF
schema.version=3
manifest.kind=renderer-sdk-producer
libplacebo.repository=${LIBPLACEBO_GIT_URL:-https://github.com/FongMi/libplacebo.git}
libplacebo.commit=${LIBPLACEBO_GIT_COMMIT:-unknown}
libplacebo.source.dirty=false
libplacebo.api=$libplacebo_api
libplacebo.opengl=true
libplacebo.vulkan=true
shaderc.source=android-ndk
android.ndk.version=$v_ndk
renderer-sdk.abi.list=armeabi-v7a,arm64-v8a
renderer-sdk.library.path=lib/libplacebo.a
renderer-sdk.shaderc.library.path=lib/libshaderc.a
renderer-sdk.headers.path=include/libplacebo
renderer-sdk.headers.manifest=headers.sha256
renderer-sdk.license.path=LICENSE
renderer-sdk.cflags=-DPL_STATIC -pthread
renderer-sdk.link.libraries=shaderc,vulkan,android,mediandk,log,dl,m,c++
EOF

(
	cd "$output"
	find . -type f ! -name files.sha256 -print0 \
		| sort -z \
		| xargs -0 sha256sum
) >"$output/files.sha256"
