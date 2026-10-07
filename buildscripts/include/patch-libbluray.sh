#!/bin/bash -e

patch_dir="$(cd "$(dirname "$0")/../patches" && pwd)"
for patch_file in "$patch_dir/libbluray-1.4.1-still-eof.patch" \
	"$patch_dir/libbluray-1.4.1-checked-seek.patch" \
	"$patch_dir/libbluray-1.4.1-menu-info.patch" \
	"$patch_dir/libbluray-1.4.1-read-origin.patch"; do
	if patch --batch --forward --fuzz=0 --dry-run -d "$1" -p1 < "$patch_file" >/dev/null 2>&1; then
		patch --batch --forward --fuzz=0 -d "$1" -p1 < "$patch_file"
	elif ! patch --batch --reverse --fuzz=0 --dry-run -d "$1" -p1 < "$patch_file" >/dev/null 2>&1; then
		echo "libbluray source does not match $patch_file." >&2
		exit 1
	fi
done
