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
v_harfbuzz=14.2.0
v_fribidi=1.0.16
v_freetype=2.14.3
v_mbedtls=3.6.5
v_openssl=3.5.5
v_python=3.12.3
v_ytdlp=2026.03.17
v_curl=8.20.0
v_mujs=1.3.9

# New codec library versions (FFmpeg n8.1.1 ecosystem)
v_vvdec=2.3.0
v_mpeghdec=1.0.2
v_libiamf=1.0.0
v_liblcevc=0.4.1
v_dav1d=1.5.3


## Dependency tree
# I would've used a dict but putting arrays in a dict is not a thing

dep_mbedtls=()
dep_dav1d=()
dep_vvdec=()
dep_mpeghdec=()
dep_libiamf=()
dep_liblcevc=()
dep_ffmpeg=(mbedtls dav1d vvdec mpeghdec libiamf liblcevc shaderc)
dep_freetype2=()
dep_fribidi=()
dep_harfbuzz=()
dep_unibreak=()
dep_libass=(freetype2 fribidi harfbuzz unibreak)
dep_lua=()
dep_mujs=()
dep_openssl=()
dep_python=(openssl)
dep_curl=(mbedtls)
dep_shaderc=()
dep_libplacebo=(shaderc)
dep_mpv=(ffmpeg libass lua libplacebo mujs curl)
dep_mpv_android=(mpv python)


## for CI workflow

# pinned ffmpeg revision
v_ci_ffmpeg=n8.1.1

# filename used to uniquely identify a build prefix
ci_tarball="prefix-all-ndk-${v_ndk}-lua-${v_lua}-mujs-${v_mujs}-unibreak-${v_unibreak}-harfbuzz-${v_harfbuzz}-fribidi-${v_fribidi}-freetype-${v_freetype}-mbedtls-${v_mbedtls}-openssl-${v_openssl}-python-${v_python}-curl-${v_curl}-vvdec-${v_vvdec}-mpeghdec-${v_mpeghdec}-libiamf-${v_libiamf}-liblcevc-${v_liblcevc}-dav1d-${v_dav1d}-ffmpeg-${v_ci_ffmpeg}-v2.tgz"
