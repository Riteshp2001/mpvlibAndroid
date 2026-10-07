#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
. include/depinfo.sh

source_dir="deps/libbluray"
if [[ ! -f "$source_dir/src/libbluray/bdj/build.xml" ]]; then
  mkdir -p "$source_dir"
  archive="build/libbluray-${v_libbluray}.tar.xz"
  mkdir -p build
  curl --fail --location --silent --show-error \
    "https://download.videolan.org/pub/videolan/libbluray/${v_libbluray}/libbluray-${v_libbluray}.tar.xz" \
    --output "$archive"
  echo "76b5dc40097f28dca4ebb009c98ed51321b2927453f75cc72cf74acd09b9f449  $archive" |
    sha256sum --check -
  tar -xJf "$archive" -C "$source_dir" --strip-components=1
fi
grep -Fq "version: '${v_libbluray}'" "$source_dir/meson.build" || {
  echo "libbluray source version does not match ${v_libbluray}" >&2
  exit 1
}

include/patch-libbluray.sh "$source_dir"

[[ "$(javac -version 2>&1)" == javac\ 1.8.* ]] || {
  echo "BD-J jars require JDK 8" >&2
  exit 1
}
command -v ant >/dev/null

output="$PWD/build/bdj"
mkdir -p "$output"
ant -f "$source_dir/src/libbluray/bdj/build.xml" \
  "-Dbuild=$output/classes" \
  "-Ddist=$output" \
  '-Dsrc_awt=:java-j2se:java-build-support' \
  '-Djava_version_asm=1.5' \
  '-Djava_version_bdj=1.4' \
  "-Djavac_path=$(command -v javac)" \
  '-Djavac_arg=-Xlint:-deprecation' \
  "-Dversion=j2se-${v_libbluray}"

test -s "$output/libbluray-j2se-${v_libbluray}.jar"
test -s "$output/libbluray-awt-j2se-${v_libbluray}.jar"
