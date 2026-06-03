#!/bin/bash -e

cd "$( dirname "${BASH_SOURCE[0]}" )"
. ./include/depinfo.sh

cleanbuild=0
nodeps=0
clang=1
target=mpv-android
arch=arm64

getdeps () {
	varname="dep_${1//-/_}[*]"
	echo ${!varname}
}

loadarch () {
	unset CC CXX CPATH LIBRARY_PATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH
	unset CFLAGS CXXFLAGS CPPFLAGS LDFLAGS

	local apilvl=24

	if [ "$1" == "arm64" ]; then
		export ndk_suffix=-arm64
		export ndk_triple=aarch64-linux-android
		export android_abi=arm64-v8a
		cc_triple=$ndk_triple$apilvl
		prefix_name=arm64
		export ARM_V9A=0
	elif [ "$1" == "arm64-v9a" ]; then
		# ARM v9a: same NDK triple/ABI as arm64, but compiled with SVE2+enhanced NEON
		# These libraries are shipped separately and loaded at runtime on v9a-capable SoCs
		export ndk_suffix=-arm64v9a
		export ndk_triple=aarch64-linux-android
		export android_abi=arm64-v8a
		cc_triple=$ndk_triple$apilvl
		prefix_name=arm64-v9a
		export ARM_V9A=1
	elif [ "$1" == "x86" ]; then
		export ndk_suffix=-x86
		export ndk_triple=i686-linux-android
		export android_abi=x86
		cc_triple=$ndk_triple$apilvl
		prefix_name=x86
		export ARM_V9A=0
	elif [ "$1" == "x86_64" ]; then
		export ndk_suffix=-x64
		export ndk_triple=x86_64-linux-android
		export android_abi=x86_64
		cc_triple=$ndk_triple$apilvl
		prefix_name=x86_64
		export ARM_V9A=0
	else
		echo "Invalid architecture" >&2
		exit 1
	fi

	export prefix_name
	export prefix_dir="$PWD/prefix/$prefix_name"

	if [ $clang -eq 1 ]; then
		export CC=$cc_triple-clang
		export CXX=$cc_triple-clang++
	else
		export CC=$cc_triple-gcc
		export CXX=$cc_triple-g++
	fi

	# Base linker flags — 16KB page size support for modern Android
	export LDFLAGS="-Wl,-O1,--icf=safe -Wl,-z,max-page-size=16384 -Wl,--gc-sections"

	# === Architecture-specific optimization flags ===
	if [ "$ARM_V9A" -eq 1 ]; then
		# ARM v9a: SVE2 + enhanced NEON + I8MM
		# Tuned for Cortex-X3/X4. We strictly avoid +sve2-bitperm, +sha3, +sm4, and +sme
		# as they are optional features and cause SIGILL on many Snapdragon/Dimensity SoCs.
		export CFLAGS="-march=armv9-a+sve2+lse+dotprod -mtune=cortex-x3 -O3 -flto=thin -ffast-math -fno-math-errno -fomit-frame-pointer -fno-plt -fno-semantic-interposition -ffunction-sections -fdata-sections"
		export CXXFLAGS="$CFLAGS"
		export LDFLAGS="$LDFLAGS -flto=thin -fuse-ld=lld"
	elif [[ "$ndk_triple" == "aarch64"* ]]; then
		# ARM v8a base: NEON + CRC (Safe version: no crypto to prevent crashes on budget/old SoCs)
		# Tuned for Cortex-A76 class cores
		export CFLAGS="-march=armv8-a+crc -mtune=cortex-a76 -O3 -flto=thin -ffast-math -fno-math-errno -fomit-frame-pointer -fno-plt -fno-semantic-interposition -ffunction-sections -fdata-sections"
		export CXXFLAGS="$CFLAGS"
		export LDFLAGS="$LDFLAGS -flto=thin -fuse-ld=lld"
	fi

	export AR=llvm-ar
	export RANLIB=llvm-ranlib
}

to_meson_array () {
	local flags=($1)
	local result=""
	for flag in "${flags[@]}"; do
		if [ -n "$result" ]; then
			result="$result, '$flag'"
		else
			result="'$flag'"
		fi
	done
	echo "[$result]"
}

setup_prefix () {
	mkdir -p "$prefix_dir"
	# enforce flat structure (/usr/local -> /)
	# Always re-create: cache restore may replace symlinks with real directories
	if [ ! -L "$prefix_dir/usr" ]; then
		rm -rf "$prefix_dir/usr"
		ln -s . "$prefix_dir/usr"
	fi
	if [ ! -L "$prefix_dir/local" ]; then
		rm -rf "$prefix_dir/local"
		ln -s . "$prefix_dir/local"
	fi

	local cpu_family=${ndk_triple%%-*}
	[ "$cpu_family" == "i686" ] && cpu_family=x86

	if ! command -v pkg-config >/dev/null; then
		echo "pkg-config not provided!"
		return 1
	fi

	# Determine CPU tuning for meson cross-file
	local cpu_tune="${CC%%-*}"
	if [ "$ARM_V9A" -eq 1 ]; then
		cpu_tune="cortex-x3"
	fi

	# Convert CFLAGS and LDFLAGS into Meson-compatible array format
	local c_args_meson=$(to_meson_array "$CFLAGS")
	local cpp_args_meson=$(to_meson_array "$CXXFLAGS")
	local link_args_meson=$(to_meson_array "$LDFLAGS")

	# meson wants to be spoonfed this file, so create it ahead of time
	# also define: release build, static libs and no source downloads at runtime(!!!)
	cat >"$prefix_dir/crossfile.tmp" <<CROSSFILE
[built-in options]
buildtype = 'release'
default_library = 'static'
wrap_mode = 'nodownload'
prefix = '/usr/local'
c_args = $c_args_meson
cpp_args = $cpp_args_meson
c_link_args = $link_args_meson
cpp_link_args = $link_args_meson
[binaries]
c = '$CC'
cpp = '$CXX'
ar = 'llvm-ar'
nm = 'llvm-nm'
strip = 'llvm-strip'
pkgconfig = 'pkg-config'
pkg-config = 'pkg-config'
[host_machine]
system = 'android'
cpu_family = '$cpu_family'
cpu = '$cpu_tune'
endian = 'little'
CROSSFILE
	# also avoid rewriting it needlessly
	if cmp -s "$prefix_dir"/crossfile.{tmp,txt}; then
		rm "$prefix_dir/crossfile.tmp"
	else
		mv "$prefix_dir"/crossfile.{tmp,txt}
	fi
}

build () {
	if [ $1 != "mpv-android" ] && [ ! -d deps/$1 ]; then
		printf >&2 '\e[1;31m%s\e[m\n' "Target $1 not found"
		return 1
	fi
	if [ $nodeps -eq 0 ]; then
		printf >&2 '\e[1;34m%s\e[m\n' "Preparing $1..."
		local deps=$(getdeps $1)
		echo >&2 "Dependencies: $deps"
		for dep in $deps; do
			build $dep
		done
	fi
	printf >&2 '\e[1;34m%s\e[m\n' "Building $1..."
	if [ "$1" == "mpv-android" ]; then
		pushd ..
		BUILDSCRIPT=buildscripts/scripts/$1.sh
	else
		pushd deps/$1
		BUILDSCRIPT=../../scripts/$1.sh
	fi
	[ $cleanbuild -eq 1 ] && bash -e "$BUILDSCRIPT" clean
	bash -e "$BUILDSCRIPT" build
	popd
}

usage () {
	printf '%s\n' \
		"Usage: buildall.sh [options] [target]" \
		"Builds the specified target (default: $target)" \
		"-n             Do not build dependencies" \
		"--clean        Clean build dirs before compiling" \
		"--gcc          Use gcc compiler (unsupported!)" \
		"--arch <arch>  Build for specified architecture (default: $arch; supported: arm64, arm64-v9a, x86, x86_64)"
	exit 0
}

while [ $# -gt 0 ]; do
	case "$1" in
		--clean)
		cleanbuild=1
		;;
		-n|--no-deps)
		nodeps=1
		;;
		--gcc)
		clang=0
		;;
		--arch)
		shift
		arch=$1
		;;
		-h|--help)
		usage
		;;
		-*)
		echo "Unknown flag $1" >&2
		exit 1
		;;
		*)
		target=$1
		;;
	esac
	shift
done

loadarch $arch
setup_prefix
build $target

[ "$target" == "mpv-android" ] && \
	ls -lh ../app/build/outputs/aar/*.aar

exit 0
