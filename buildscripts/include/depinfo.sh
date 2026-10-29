#!/bin/bash -e

## Dependency versions
# Make sure to keep v_ndk and v_ndk_n in sync, both are listed on the NDK download page

v_sdk=14742923_latest
v_ndk=r30
v_ndk_n=30.0.16248370
v_sdk_platform=36
v_sdk_build_tools=36.0.0

v_lua=5.2.4
v_lcms2=2.19.1
v_unibreak=8.0
v_harfbuzz=15.0.0
v_fribidi=1.0.17
v_freetype=2.15.0
v_mbedtls=3.6.7
v_mujs=1.3.11
v_libxml2=2.15.4
v_libmysofa=1.3.5
v_libaribcaption=1.1.2
v_fontconfig=2.18.3
v_curl=8.23.0
# libbluray, libdvdread and libdvdnav stay pinned: the bundled patches target these exact releases.
v_libbluray=1.4.1
v_libiconv=1.19
v_uchardet=0.0.8
v_bzip2=1.0.8
v_xz=5.8.4
v_zstd=1.5.7
v_libarchive=3.8.10
v_libdvdread=7.0.1
v_libdvdnav=7.0.0
v_rubberband=4.0.0
v_openjoc=0.20.1
v_openjoc_sha=e3e330a2000f8d0bced3460756473c8774eb16f0
v_rust=1.98.0
v_ci_mpv=fe1806b713abc218696f0bc815d5acb14475021e


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
dep_libmysofa=()
dep_freetype2=()
dep_libaribcaption=(freetype2)
dep_uavs3d=()
dep_openjoc=()
dep_ffmpeg=(openjoc mbedtls dav1d libxml2 libmysofa libaribcaption uavs3d)
dep_fontconfig=(libxml2 freetype2)
dep_fribidi=()
dep_harfbuzz=()
dep_unibreak=()
dep_libass=(freetype2 fontconfig fribidi harfbuzz unibreak)
dep_lua=()
dep_mujs=()
dep_shaderc=()
dep_lcms2=()
dep_libplacebo=(shaderc lcms2)
dep_curl=(mbedtls)
dep_libbluray=(freetype2)
dep_libarchive=(libiconv bzip2 xz zstd)
dep_libdvdread=()
dep_libdvdnav=(libdvdread)
dep_rubberband=()
dep_mpv=(ffmpeg libass lua libplacebo curl libbluray libiconv uchardet libarchive libdvdnav rubberband mujs)
dep_mpv_android=(mpv)


## for CI workflow

# CI resolves these movable branches to immutable commits before selecting a cache.
v_ci_ffmpeg=release-9.0-fongmi
v_ci_uavs3d=e72f7c34be2f4de725c1ae498e8c4f3559e99b5b
v_ci_dav1d=master
v_ci_libass=master
v_ci_libplacebo=fongmi
# Bump when this branch's prefix recipe changes without a version change.
v_ci_prefix=6

# filename used to uniquely identify a build prefix
ci_tarball="prefix-fongmi-ndk-${v_ndk}-opengl-vulkan-shaderc-lua-${v_lua}-mujs-${v_mujs}-unibreak-${v_unibreak}-harfbuzz-${v_harfbuzz}-fribidi-${v_fribidi}-freetype-${v_freetype}-libxml2-${v_libxml2}-libmysofa-${v_libmysofa}-libaribcaption-${v_libaribcaption}-fontconfig-${v_fontconfig}-mbedtls-${v_mbedtls}-curl-${v_curl}-libbluray-${v_libbluray}-libiconv-${v_libiconv}-uchardet-${v_uchardet}-bzip2-${v_bzip2}-xz-${v_xz}-zstd-${v_zstd}-libarchive-${v_libarchive}-libdvdread-${v_libdvdread}-libdvdnav-${v_libdvdnav}-rubberband-${v_rubberband}-ffmpeg-${v_ci_ffmpeg}-prefix-${v_ci_prefix}-lcms2-${v_lcms2}.tgz"
