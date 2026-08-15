#!/bin/bash -e
set -euo pipefail

VERSION_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION_GRADLE="$VERSION_ROOT/app/build.gradle"

read_property() {
	local key=$1
	case "$key" in
		versionName)
			sed -n 's/^[[:space:]]*version[[:space:]]*=[[:space:]]*"\([^"]*\)"[[:space:]]*$/\1/p' "$VERSION_GRADLE" | head -n 1
			;;
		versionCode)
			sed -n 's/^[[:space:]]*versionCode[[:space:]]*\([0-9][0-9]*\)[[:space:]]*$/\1/p' "$VERSION_GRADLE" | head -n 1
			;;
		*)
			return 1
			;;
	esac
}

version_name() {
	read_property versionName
}

version_code() {
	read_property versionCode
}

validate_semver() {
	local value=${1:-}
	local numeric='(0|[1-9][0-9]*)'
	local prerelease='(0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)'
	local build='[0-9A-Za-z-]+'
	local semver="^${numeric}\\.${numeric}\\.${numeric}(-${prerelease}(\\.${prerelease})*)?(\\+${build}(\\.${build})*)?$"
	[[ "$value" =~ $semver ]]
}

validate_config() {
	local name code
	name=$(version_name)
	code=$(version_code)
	validate_semver "$name" || {
		echo "Invalid versionName in app/build.gradle: $name" >&2
		return 1
	}
	[[ "$code" =~ ^[1-9][0-9]*$ ]] || {
		echo "Invalid versionCode in app/build.gradle: $code" >&2
		return 1
	}
	(( code <= 2100000000 )) || {
		echo "versionCode in app/build.gradle exceeds Android's maximum: $code" >&2
		return 1
	}
}

validate_tag() {
	local tag=${1:-}
	[[ "$tag" == v* ]] || {
		echo "Release tags must use the vMAJOR.MINOR.PATCH format: $tag" >&2
		return 1
	}
	local tag_version=${tag#v}
	validate_semver "$tag_version" || {
		echo "Release tag is not valid SemVer: $tag" >&2
		return 1
	}
	validate_config
	[[ "$tag_version" == "$(version_name)" ]] || {
		echo "Release tag $tag does not match configured version $(version_name)" >&2
		return 1
	}
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
	case "${1:-validate}" in
		name)
			version_name
			;;
		code)
			version_code
			;;
		validate)
			validate_config
			echo "$(version_name) ($(version_code))"
			;;
		validate-tag)
			validate_tag "${2:-}"
			echo "Release tag ${2:-} matches $(version_name)"
			;;
		*)
			echo "Usage: $0 {name|code|validate|validate-tag TAG}" >&2
			exit 2
			;;
	esac
fi
