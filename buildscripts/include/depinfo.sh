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
v_mbedtls=3.6.7
v_openssl=3.5.7
v_mujs=1.3.9
v_libxml2=2.15.3
v_libaribcaption=1.1.1
v_curl=8.21.0
v_libbluray=1.4.1
v_libiconv=1.19
v_uchardet=0.0.8
v_bzip2=1.0.8
v_xz=5.8.1
v_zstd=1.5.7
v_libarchive=3.8.7
v_libdvdread=7.0.1
v_libdvdnav=7.0.0
v_rubberband=4.0.0


## Dependency tree
# I would've used a dict but putting arrays in a dict is not a thing

dep_libiconv=()
dep_uchardet=(libiconv)
dep_bzip2=()
dep_xz=()
dep_zstd=()
dep_mbedtls=()
dep_dav1d=()
dep_libxml2=()
dep_freetype2=()
dep_libaribcaption=(freetype2)
dep_ffmpeg=(mbedtls dav1d libxml2 libaribcaption)
dep_fribidi=()
dep_harfbuzz=()
dep_unibreak=()
dep_libass=(freetype2 fribidi harfbuzz unibreak)
dep_lua=()
dep_mujs=()
dep_openssl=()
dep_shaderc=()
dep_libplacebo=(shaderc)
dep_curl=(mbedtls)
dep_libbluray=()
dep_libarchive=(libiconv bzip2 xz zstd)
dep_libdvdread=()
dep_libdvdnav=(libdvdread)
dep_rubberband=()
dep_mpv=(ffmpeg libass lua libplacebo curl libbluray libiconv uchardet libarchive libdvdnav rubberband mujs)
dep_mpv_android=(mpv)


## for CI workflow

# pinned ffmpeg revision
v_ci_ffmpeg=n9.0

# filename used to uniquely identify a build prefix
ci_tarball="prefix-ndk-${v_ndk}-lua-${v_lua}-mujs-${v_mujs}-unibreak-${v_unibreak}-harfbuzz-${v_harfbuzz}-fribidi-${v_fribidi}-freetype-${v_freetype}-libxml2-${v_libxml2}-libaribcaption-${v_libaribcaption}-mbedtls-${v_mbedtls}-curl-${v_curl}-libbluray-${v_libbluray}-libiconv-${v_libiconv}-uchardet-${v_uchardet}-bzip2-${v_bzip2}-xz-${v_xz}-zstd-${v_zstd}-libarchive-${v_libarchive}-libdvdread-${v_libdvdread}-libdvdnav-${v_libdvdnav}-rubberband-${v_rubberband}-openssl-${v_openssl}-ffmpeg-${v_ci_ffmpeg}.tgz"
