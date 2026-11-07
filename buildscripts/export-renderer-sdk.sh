#!/bin/bash -e
set -o pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
. buildscripts/include/depinfo.sh

output=build/renderer-sdk
rm -rf "$output"
mkdir -p "$output"
libplacebo_api=
# Export runs outside buildall.sh's toolchain PATH.
nm_tools=(buildscripts/sdk/android-ndk-"$v_ndk"/toolchains/llvm/prebuilt/*/bin/llvm-nm)
[ "${#nm_tools[@]}" = 1 ] && [ -x "${nm_tools[0]}" ] || {
	echo "A unique NDK llvm-nm is required for renderer SDK export." >&2
	exit 1
}

motion_source=buildscripts/deps/mpv
motion_core=video/out/gpu_next/motion.c
motion_header=video/out/gpu_next/motion.h
for required in "$motion_core" "$motion_header" LICENSE.LGPL; do
	[ -f "$motion_source/$required" ] || {
		echo "Motion SDK source is missing: $motion_source/$required" >&2
		exit 1
	}
done
motion_commit=$(git -C "$motion_source" rev-parse HEAD)
[[ "$motion_commit" =~ ^[0-9a-f]{40}$ ]] || {
	echo "Motion SDK requires a resolved mpv commit." >&2
	exit 1
}
for required in "$motion_core" "$motion_header" LICENSE.LGPL; do
	git -C "$motion_source" cat-file -e "$motion_commit:$required" || {
		echo "Motion SDK source is absent from its commit: $required" >&2
		exit 1
	}
done
motion_repository=$(git -C "$motion_source" remote get-url origin)
[ -n "$motion_repository" ] || exit 1
# The build recipe legitimately patches meson.build for the Android toolchain.
# Only the exported core, public header and license must match this commit.
git -C "$motion_source" diff --quiet HEAD -- "$motion_core" "$motion_header" LICENSE.LGPL || {
	echo "Motion SDK core, header or license differs from its source commit." >&2
	exit 1
}
motion_source_sha=$(sha256sum "$motion_source/$motion_core" | cut -d ' ' -f 1)
motion_header_sha=$(sha256sum "$motion_source/$motion_header" | cut -d ' ' -f 1)
motion_license_sha=$(sha256sum "$motion_source/LICENSE.LGPL" | cut -d ' ' -f 1)

copy_abi() {
	local prefix_name=$1
	local android_abi=$2
	local prefix="buildscripts/prefix/$prefix_name"
	local destination="$output/$android_abi"
	local config="$prefix/include/libplacebo/config.h"
	local opengl="$prefix/include/libplacebo/opengl.h"
	local vulkan="$prefix/include/libplacebo/vulkan.h"
	local libav="$prefix/include/libplacebo/utils/libav.h"
	local renderer="$prefix/include/libplacebo/renderer.h"
	local colorspace="$prefix/include/libplacebo/colorspace.h"
	local libplacebo_archive_sha
	local libplacebo_header_sha
	local symbols
	local current_api
	local receipt="$prefix/share/mpv-motion/provenance.properties"
	local archive_sha
	local field
	local expected
	local recorded
	local header

	for required in \
		"$config" \
		"$opengl" \
		"$vulkan" \
		"$libav" \
		"$renderer" \
		"$colorspace" \
		"$prefix/lib/libplacebo.a" \
		"$prefix/lib/libshaderc.a" \
		"$prefix/lib/libmpv-motion.a" \
		"$prefix/include/mpv-motion/motion.h" \
		"$receipt"; do
		[ -f "$required" ] || {
			echo "Renderer SDK input is missing: $required" >&2
			exit 1
		}
	done
	cmp -s "$motion_source/$motion_header" "$prefix/include/mpv-motion/motion.h" || {
		echo "Installed motion header differs from its source for $android_abi." >&2
		exit 1
	}
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

	for capability in CUSTOM_FRAME_MIXER SOURCE_PEAK_TIMELINE SOURCE_PEAK_IMAGE \
		SOURCE_HOOK_PARAMETERS HDR_LINEAR; do
		header=$renderer
		[ "$capability" != HDR_LINEAR ] || header=$colorspace
		grep -Eq "^#define PL_HAVE_${capability} 1([[:space:]]|$)" "$header" || {
			echo "libplacebo is missing renderer capability PL_HAVE_${capability}=1 for $android_abi." >&2
			exit 1
		}
	done
	grep -Eq '^[[:space:]]*enum mp_motion_result mp_motion_render_views\(' \
		"$prefix/include/mpv-motion/motion.h" || {
		echo "Motion SDK header is missing mp_motion_render_views for $android_abi." >&2
		exit 1
	}
	symbols=$("${nm_tools[0]}" --defined-only --extern-only --format=posix \
		"$prefix/lib/libmpv-motion.a")
	[ "$(printf '%s\n' "$symbols" | awk '
		$1 == "mp_motion_render_views" && $2 == "T" { n++ }
		END { print n+0 }')" = 1 ] || {
		echo "Motion SDK archive must define mp_motion_render_views once for $android_abi." >&2
		exit 1
	}
	libplacebo_archive_sha=$(sha256sum "$prefix/lib/libplacebo.a" | cut -d ' ' -f 1)
	libplacebo_header_sha=$(
		cd "$prefix/include"
		find libplacebo -type f -print0 \
			| LC_ALL=C sort -z \
			| xargs -0 sha256sum --binary | sed 's/ \*/  /'
	)
	libplacebo_header_sha=$(printf '%s\n' "$libplacebo_header_sha" | sha256sum | cut -d ' ' -f 1)
	archive_sha=$(sha256sum "$prefix/lib/libmpv-motion.a" | cut -d ' ' -f 1)
	for field in schema.version mpv-motion.arch mpv-motion.commit mpv-motion.source.sha256 \
		mpv-motion.header.sha256 mpv-motion.archive.sha256 \
		libplacebo.archive.sha256 libplacebo.headers.sha256; do
		case "$field" in
			schema.version) expected=1 ;;
			mpv-motion.arch) expected=$prefix_name ;;
			mpv-motion.commit) expected=$motion_commit ;;
			mpv-motion.source.sha256) expected=$motion_source_sha ;;
			mpv-motion.header.sha256) expected=$motion_header_sha ;;
			mpv-motion.archive.sha256) expected=$archive_sha ;;
			libplacebo.archive.sha256) expected=$libplacebo_archive_sha ;;
			libplacebo.headers.sha256) expected=$libplacebo_header_sha ;;
		esac
		recorded=$(awk -F= -v key="$field" '$1 == key { print $2 }' "$receipt")
		[ "$recorded" = "$expected" ] || {
			echo "Motion SDK build receipt mismatch for $android_abi: $field" >&2
			exit 1
		}
	done

	mkdir -p "$destination/include" "$destination/lib"
	cp -R "$prefix/include/libplacebo" "$destination/include/"
	cp "$prefix/lib/libplacebo.a" "$destination/lib/"
	cp "$prefix/lib/libshaderc.a" "$destination/lib/"
	cp "$prefix/lib/libmpv-motion.a" "$destination/lib/"
	cp -R "$prefix/include/mpv-motion" "$destination/include/"
	cp "$receipt" "$destination/motion-build.properties"
	(
		cd "$destination/include"
		find libplacebo -type f -print0 \
			| LC_ALL=C sort -z \
			| xargs -0 sha256sum --binary | sed 's/ \*/  /'
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
cp "$motion_source/LICENSE.LGPL" "$output/LICENSE.mpv-motion"

cat >"$output/provenance.properties" <<EOF
schema.version=3
manifest.kind=renderer-sdk-producer
libplacebo.repository=${LIBPLACEBO_GIT_URL:-https://github.com/FongMi/libplacebo.git}
libplacebo.commit=${LIBPLACEBO_GIT_COMMIT:-unknown}
libplacebo.source.dirty=false
libplacebo.api=$libplacebo_api
libplacebo.opengl=true
libplacebo.vulkan=true
mpv-motion.repository=$motion_repository
mpv-motion.commit=$motion_commit
mpv-motion.source.path=$motion_core
mpv-motion.source.sha256=$motion_source_sha
mpv-motion.header.source.path=$motion_header
mpv-motion.header.sha256=$motion_header_sha
mpv-motion.source.scope=$motion_core,$motion_header,LICENSE.LGPL
mpv-motion.source.dirty=false
mpv-motion.library.path=lib/libmpv-motion.a
mpv-motion.build.manifest=motion-build.properties
mpv-motion.header.path=include/mpv-motion/motion.h
mpv-motion.license.path=LICENSE.mpv-motion
mpv-motion.license.sha256=$motion_license_sha
shaderc.source=android-ndk
android.ndk.version=$v_ndk_n
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
