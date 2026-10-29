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
			tar -xzf "$CACHE_FOLDER/data.tgz" -C prefix && return 0
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
		for x in ${dep_mpv[@]}; do
			msg "Building $x for $arch"
			./buildall.sh --arch "$arch" "$x"
		done
	done

	if [[ "$CACHE_MODE" == folder && -w "$CACHE_FOLDER" ]]; then
		msg "Compressing the prefix"
		tar -cvzf "$CACHE_FOLDER/data.tgz" -C prefix .
		echo "$ci_cache_identifier" >"$CACHE_FOLDER/id.txt"
	fi
}

export WGET="wget --progress=bar:force"

if [ "$1" = "export" ]; then
	# export variable with unique cache identifier
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
		git clone --depth 1 https://github.com/mpv-player/mpv deps/mpv
	else
		git -C deps/mpv fetch --depth 1 origin master
		git -C deps/mpv reset --hard origin/master
	fi
	git -C deps/mpv apply ../../patches/mpv_video_shaders.patch

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

msg "Building mpv"
./buildall.sh -n mpv || {
	# show logfile if configure failed
	[ ! -f deps/mpv/_build/config.h ] && cat deps/mpv/_build/meson-logs/meson-log.txt
	exit 1
}

for arch in armv7l arm64 x86 x86_64; do
	if [ -d "prefix/$arch" ]; then
		msg "Building python for $arch"
		./buildall.sh --arch "$arch" -n python
	fi
done

msg "Building mpv-android"
./buildall.sh -n

exit 0
