#!/bin/bash -e

## Dependency versions
# Make sure to keep v_ndk and v_ndk_n in sync, both are listed on the NDK download page

v_sdk=14742923_latest
v_ndk=r29
v_ndk_n=29.0.14206865
v_sdk_platform=36
v_sdk_build_tools=36.0.0

v_lua=5.2.4
v_unibreak=7.0
v_harfbuzz=14.2.1
v_fribidi=1.0.16
v_freetype=2.14.3
v_mbedtls=3.6.6
v_openssl=3.5.7
v_mujs=1.3.9
v_mpv=0.41.0


## Dependency tree
# I would've used a dict but putting arrays in a dict is not a thing

dep_mbedtls=()
dep_dav1d=()
dep_ffmpeg=(mbedtls dav1d)
dep_freetype2=()
dep_fribidi=()
dep_harfbuzz=()
dep_unibreak=()
dep_libass=(freetype2 fribidi harfbuzz unibreak)
dep_lua=()
dep_mujs=()
dep_openssl=()
dep_shaderc=()
dep_libplacebo=(shaderc)
dep_mpv=(ffmpeg libass lua libplacebo mujs)
dep_mpv_android=(mpv)


## for CI workflow

# pinned ffmpeg revision
v_ci_ffmpeg=n9.0

# filename used to uniquely identify a build prefix
ci_tarball="prefix-ndk-${v_ndk}-lua-${v_lua}-mujs-${v_mujs}-unibreak-${v_unibreak}-harfbuzz-${v_harfbuzz}-fribidi-${v_fribidi}-freetype-${v_freetype}-mbedtls-${v_mbedtls}-openssl-${v_openssl}-ffmpeg-${v_ci_ffmpeg}.tgz"
