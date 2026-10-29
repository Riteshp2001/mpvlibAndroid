#!/bin/bash -e

. ../../include/path.sh
. ../../include/depinfo.sh

if ! git apply --reverse --check ../../patches/openjoc_shared_hrtf.patch 2>/dev/null; then
	git apply ../../patches/openjoc_shared_hrtf.patch
fi

case "$ndk_triple" in
	arm-linux-androideabi) rust_target=armv7-linux-androideabi ;;
	aarch64-linux-android) rust_target=aarch64-linux-android ;;
	i686-linux-android) rust_target=i686-linux-android ;;
	x86_64-linux-android) rust_target=x86_64-linux-android ;;
	*) echo "Unsupported Android toolchain: $ndk_triple" >&2; exit 1 ;;
esac

case "$1" in
	build) ;;
	clean) cargo "+$v_rust" clean --release --target "$rust_target"; exit 0 ;;
	*) echo "Unknown OpenJOC build action: $1" >&2; exit 1 ;;
esac

rustup target add --toolchain "$v_rust" "$rust_target"
features=$(cargo "+$v_rust" tree --locked --color never -p openjoc-capi -e features --prefix none)
if grep -Fq 'feature "embedded-builtin-hrtf"' <<<"$features"; then
	echo "Embedded HRTF data is still enabled; refusing to duplicate it in every ABI." >&2
	exit 1
fi
if [ "$prefix_name" = armv7l ]; then
	host_target=$(rustc "+$v_rust" -vV | sed -n 's/^host: //p')
	env -u CC -u CXX -u AR -u RANLIB -u CFLAGS -u CXXFLAGS -u LDFLAGS -u RUSTFLAGS \
		cargo "+$v_rust" test --locked --target "$host_target" \
		-p openjoc-capi --test shared_hrtf -- --test-threads=1
fi
linker_variable="CARGO_TARGET_$(echo "$rust_target" | tr '[:lower:]-' '[:upper:]_')_LINKER"
export "$linker_variable=$CC"
export RUSTFLAGS="${RUSTFLAGS:-} -C link-arg=-Wl,-z,max-page-size=16384 -C link-arg=-Wl,-soname,libopenjoc_capi.so"
cargo "+$v_rust" build --locked --release --target "$rust_target" -p openjoc-capi

mkdir -p "$prefix_dir/include" "$prefix_dir/lib/pkgconfig"
install -m 0644 crates/openjoc-capi/include/openjoc.h "$prefix_dir/include/openjoc.h"
install -m 0755 "target/$rust_target/release/libopenjoc_capi.so" "$prefix_dir/lib/libopenjoc_capi.so"
cat >"$prefix_dir/lib/pkgconfig/openjoc.pc" <<EOF
prefix=/usr/local
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
printf 'Local patch: openjoc_shared_hrtf.patch (validated external HRTF resources)\n' >>"$notices/SOURCE.txt"

models="$prefix_dir/../share/openjoc"
mkdir -p "$models"
for model in sadie-ii-d1-ku100.ojhrtf sadie-ii-d2-kemar.ojhrtf; do
	install -m 0644 "crates/openjoc-sofa/assets/$model" "$models/$model"
done
(cd "$models" && sha256sum sadie-ii-d1-ku100.ojhrtf sadie-ii-d2-kemar.ojhrtf > MODELS.sha256)
