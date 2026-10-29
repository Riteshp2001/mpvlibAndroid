#!/bin/bash -e

. ../../include/path.sh
. ../../include/depinfo.sh

case "$android_abi" in
	armeabi-v7a) rust_target=armv7-linux-androideabi ;;
	arm64-v8a) rust_target=aarch64-linux-android ;;
	x86) rust_target=i686-linux-android ;;
	x86_64) rust_target=x86_64-linux-android ;;
	*) echo "Unsupported Android ABI: $android_abi" >&2; exit 1 ;;
esac

case "$1" in
	build) ;;
	clean) cargo "+$v_rust" clean --release --target "$rust_target"; exit 0 ;;
	*) echo "Unknown OpenJOC build action: $1" >&2; exit 1 ;;
esac

rustup target add --toolchain "$v_rust" "$rust_target"
linker_variable="CARGO_TARGET_$(echo "$rust_target" | tr '[:lower:]-' '[:upper:]_')_LINKER"
export "$linker_variable=$CC"
export RUSTFLAGS="${RUSTFLAGS:-} -C link-arg=-Wl,-z,max-page-size=16384 -C link-arg=-Wl,-soname,libopenjoc_capi.so"
cargo "+$v_rust" build --locked --release --target "$rust_target" -p openjoc-capi

mkdir -p "$prefix_dir/include" "$prefix_dir/lib/pkgconfig"
install -m 0644 crates/openjoc-capi/include/openjoc.h "$prefix_dir/include/openjoc.h"
install -m 0755 "target/$rust_target/release/libopenjoc_capi.so" "$prefix_dir/lib/libopenjoc_capi.so"
cat >"$prefix_dir/lib/pkgconfig/openjoc.pc" <<EOF
prefix=$prefix_dir
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: openjoc
Description: Experimental E-AC-3 JOC decoder and binaural renderer
Version: $v_openjoc
Libs: -L\${libdir} -lopenjoc_capi
Cflags: -I\${includedir}
EOF

notices="$prefix_dir/share/openjoc"
mkdir -p "$notices"
install -m 0644 LICENSE "$notices/LICENSE"
install -m 0644 THIRD_PARTY_NOTICES.md "$notices/THIRD_PARTY_NOTICES.md"
printf 'OpenJOC %s\nSource: https://github.com/chyinan/OpenJOC/tree/%s\n' \
	"$v_openjoc" "$v_openjoc_sha" >"$notices/SOURCE.txt"
