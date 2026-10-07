#!/bin/bash -e

# go to buildscripts root folder
cd "$( dirname "${BASH_SOURCE[0]}" )/.."

. ./include/depinfo.sh
. ./include/build_config.sh

ci_build_arches=(
	armv7l
	arm64
)
[ "$ENABLE_X86_ARCH" = "true" ] && ci_build_arches+=(
	x86
	x86_64
)

ci_arch_tag=$(IFS=-; echo "${ci_build_arches[*]}")
ci_cache_identifier="${ci_tarball%.tgz}-abi-${ci_arch_tag}.tgz"

msg() {
	printf '==> %s\n' "$1"
}

ffmpeg_repository=${FFMPEG_GIT_URL:-https://github.com/FongMi/FFmpeg.git}
dav1d_repository=${DAV1D_GIT_URL:-https://github.com/videolan/dav1d}
libass_repository=${LIBASS_GIT_URL:-https://github.com/libass/libass}
libplacebo_repository=${LIBPLACEBO_GIT_URL:-https://github.com/FongMi/libplacebo.git}

resolve_ci_commit() {
	local configured_commit=$1
	local repository=$2
	local branch=$3
	local name=$4
	local result
	local commit=$configured_commit

	if [[ -z "$commit" ]]; then
		result=$(git ls-remote --exit-code "$repository" "refs/heads/$branch") || {
			echo "Failed to resolve $name branch $branch from $repository." >&2
			return 1
		}
		if [[ $(printf '%s\n' "$result" | grep -c .) != 1 ]]; then
			echo "Expected exactly one ref for $name branch $branch." >&2
			return 1
		fi
		commit=${result%%[[:space:]]*}
	fi
	if [[ ! "$commit" =~ ^[0-9a-fA-F]{40}$ ]]; then
		echo "Invalid $name commit: $commit" >&2
		return 1
	fi
	printf '%s\n' "$commit" | tr '[:upper:]' '[:lower:]'
}

fetch_prefix() {
	if [[ "$CACHE_MODE" == folder ]]; then
		local text=
		if [ -f "$CACHE_FOLDER/id.txt" ]; then
			text=$(cat "$CACHE_FOLDER/id.txt")
		else
			echo "Cache seems to be empty"
		fi
		printf 'Expecting "%s",\nfound     "%s".\n' "$ci_cache_identifier" "$text"
		if [[ "$text" == "$ci_cache_identifier" ]]; then
			rm -rf prefix
			mkdir -p prefix
			if tar -xzf "$CACHE_FOLDER/data.tgz" -C prefix; then
				return 0
			fi
			echo "Failed to extract cached prefix; rebuilding."
			rm -rf prefix
			mkdir -p prefix
		fi
	fi
	return 1
}

build_prefix() {
	msg "Building the prefix ($ci_cache_identifier)..."

	msg "Fetching deps"
	IN_CI=1 ./include/download-deps.sh

	# Build everything mpv depends on for every packaged ABI, but not mpv itself.
	for arch in "${ci_build_arches[@]}"; do
		msg "Building dependency prefix for $arch"
		./buildall.sh --arch "$arch" --only-deps mpv
	done

	if [[ "$CACHE_MODE" == folder && -w "$CACHE_FOLDER" ]]; then
		msg "Compressing the prefix"
		tar -cvzf "$CACHE_FOLDER/data.tgz" -C prefix .
		echo "$ci_cache_identifier" >"$CACHE_FOLDER/id.txt"
	fi
}

export WGET="wget --progress=bar:force"
: "${MPV_GIT_URL:=https://github.com/FongMi/mpv.git}"
: "${MPV_GIT_REF:=fongmi}"

if [[ "$1" == export || "$1" == install ]]; then
	FFMPEG_GIT_COMMIT=$(resolve_ci_commit \
		"${FFMPEG_GIT_COMMIT:-}" "$ffmpeg_repository" "$v_ci_ffmpeg" FFmpeg)
	DAV1D_GIT_COMMIT=$(resolve_ci_commit \
		"${DAV1D_GIT_COMMIT:-}" "$dav1d_repository" "$v_ci_dav1d" dav1d)
	LIBASS_GIT_COMMIT=$(resolve_ci_commit \
		"${LIBASS_GIT_COMMIT:-}" "$libass_repository" "$v_ci_libass" libass)
	LIBPLACEBO_GIT_COMMIT=$(resolve_ci_commit \
		"${LIBPLACEBO_GIT_COMMIT:-}" "$libplacebo_repository" \
		"$v_ci_libplacebo" libplacebo)
	export FFMPEG_GIT_COMMIT DAV1D_GIT_COMMIT LIBASS_GIT_COMMIT
	export LIBPLACEBO_GIT_COMMIT
	native_source_id=$(printf '%s\n' \
		"ffmpeg=$FFMPEG_GIT_COMMIT" \
		"uavs3d=$v_ci_uavs3d" \
		"dav1d=$DAV1D_GIT_COMMIT" \
		"libass=$LIBASS_GIT_COMMIT" \
		"libplacebo=$LIBPLACEBO_GIT_COMMIT" | sha256sum)
	native_source_id=${native_source_id%%[[:space:]]*}
	if [[ ! "$native_source_id" =~ ^[0-9a-f]{64}$ ]]; then
		echo "Failed to compute native source cache identifier." >&2
		exit 1
	fi
	prefix_recipe_manifest=$(
		for recipe in buildall.sh include/*.sh patches/*.patch scripts/*.sh; do
			case "$recipe" in
				include/ci.sh|scripts/mpv.sh|scripts/mpv-android.sh) continue ;;
			esac
			sha256sum "$recipe" || exit 1
		done
	)
	prefix_recipe_id=$(printf '%s\n' "$prefix_recipe_manifest" | sha256sum)
	prefix_recipe_id=${prefix_recipe_id%%[[:space:]]*}
	if [[ ! "$prefix_recipe_id" =~ ^[0-9a-f]{64}$ ]]; then
		echo "Failed to compute dependency recipe cache identifier." >&2
		exit 1
	fi
	ci_cache_identifier=$(printf '%s\n' "$ci_tarball" "$native_source_id" \
		"$prefix_recipe_id" "$ci_arch_tag" | sha256sum)
	ci_cache_identifier=${ci_cache_identifier%%[[:space:]]*}
	if [[ ! "$ci_cache_identifier" =~ ^[0-9a-f]{64}$ ]]; then
		echo "Failed to compute dependency cache identifier." >&2
		exit 1
	fi
	ci_cache_identifier="prefix-${ci_cache_identifier}-abi-${ci_arch_tag}.tgz"
fi

if [ "$1" = "export" ]; then
	# Export the exact native source revisions used by the cache and build steps.
	echo "FFMPEG_GIT_COMMIT=$FFMPEG_GIT_COMMIT"
	echo "UAVS3D_GIT_COMMIT=$v_ci_uavs3d"
	echo "DAV1D_GIT_COMMIT=$DAV1D_GIT_COMMIT"
	echo "LIBASS_GIT_COMMIT=$LIBASS_GIT_COMMIT"
	echo "LIBPLACEBO_GIT_COMMIT=$LIBPLACEBO_GIT_COMMIT"
	echo "CACHE_IDENTIFIER=$ci_cache_identifier"
	exit 0
elif [ "$1" = "install" ]; then
	# install deps
	if [[ -n "$ANDROID_HOME" && -d "$ANDROID_HOME" ]]; then
		msg "Linking existing SDK"
		mkdir -p sdk
		ln -sfv "$ANDROID_HOME" sdk/android-sdk-linux
	fi

	msg "Fetching SDK + NDK"
	IN_CI=1 ./include/download-sdk.sh

	msg "Fetching mpv"
	if [ ! -d deps/mpv ]; then
		git clone --depth 1 --branch "$MPV_GIT_REF" "$MPV_GIT_URL" deps/mpv
	else
		git -C deps/mpv fetch --depth 1 origin "$MPV_GIT_REF"
		git -C deps/mpv reset --hard FETCH_HEAD
	fi

	msg "Trying to fetch existing prefix"
	mkdir -p prefix
	fetch_prefix || build_prefix
	exit 0
elif [ "$1" = "build" ]; then
	# run build
	:
else
	exit 1
fi

for arch in "${ci_build_arches[@]}"; do
	msg "Building mpv for $arch"
	./buildall.sh --arch "$arch" -n mpv || {
		build_dir="deps/mpv/_build_$arch"
		[ ! -f "$build_dir/config.h" ] && \
			[ -f "$build_dir/meson-logs/meson-log.txt" ] && \
			cat "$build_dir/meson-logs/meson-log.txt"
		exit 1
	}
done

msg "Building mpv-android"
./buildall.sh -n

exit 0
