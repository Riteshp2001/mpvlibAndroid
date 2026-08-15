#!/bin/bash

android_cmake_abi () {
	case "$prefix_name" in
		armv7l)
			echo armeabi-v7a
			;;
		arm64)
			echo arm64-v8a
			;;
		x86)
			echo x86
			;;
		x86_64)
			echo x86_64
			;;
		*)
			echo "Invalid architecture: $prefix_name" >&2
			return 1
			;;
	esac
}

android_cmake_setup () {
	local source_dir=$1
	local build_dir=$2
	shift 2

	local ndk_dir="$DIR/sdk/android-ndk-${v_ndk}"
	local android_abi
	android_abi=$(android_cmake_abi)

	cmake -S "$source_dir" -B "$build_dir" \
		-DCMAKE_TOOLCHAIN_FILE="$ndk_dir/build/cmake/android.toolchain.cmake" \
		-DANDROID_ABI="$android_abi" \
		-DANDROID_PLATFORM="android-${android_api:-24}" \
		-DANDROID_STL=c++_shared \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX=/usr/local \
		-DCMAKE_INSTALL_LIBDIR=lib \
		-DCMAKE_PREFIX_PATH="$prefix_dir/usr/local;$prefix_dir" \
		-DCMAKE_POSITION_INDEPENDENT_CODE=ON \
		"$@"
}

android_cmake_build () {
	local build_dir=$1
	cmake --build "$build_dir" --parallel "$cores"
}

android_cmake_install () {
	local build_dir=$1
	DESTDIR="$prefix_dir" cmake --install "$build_dir"
}
