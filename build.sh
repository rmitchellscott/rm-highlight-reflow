#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
xovi_repo=${XOVI_REPO:-$HOME/github/asivery/xovi}
aarch64_toolchain=${AARCH64_TOOLCHAIN:-eeems/remarkable-toolchain:5.8.203-aarch64}
armv7_toolchain=${ARMV7_TOOLCHAIN:-eeems/remarkable-toolchain:latest-rm1}

build_with() {
	local toolchain=$1 sdk=$2 arch=$3
	docker run --rm --platform linux/amd64 -e LC_ALL=C.UTF-8 -v "$PWD":/src -v "$xovi_repo":/xovi:ro -w /src "$toolchain" bash -c '
set -e
export XOVI_REPO=/xovi
. /opt/codex/'"$sdk"'/*/environment-setup-*
rm -rf build/obj build/moc build/xovi Makefile .qmake.stash
qmake6 >/dev/null
make >/dev/null
mkdir -p out
mv highlight-reflow.so out/highlight-reflow-'"$arch"'.so
rm -rf build/obj build/moc build/xovi Makefile .qmake.stash
echo "built out/highlight-reflow-'"$arch"'.so"'
}

build_with "$aarch64_toolchain" ferrari aarch64
build_with "$armv7_toolchain" rm1 armv7
