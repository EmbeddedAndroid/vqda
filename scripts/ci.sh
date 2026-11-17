#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
#
# ci.sh [out dir]: what the container and the CI workflow run. Builds and
# tests natively, again under ASan and UBSan and under TSan, then builds
# for aarch64 (cross unless the host is aarch64) and installs that build
# to the out dir (default ./out).
set -eu
out=${1:-out}

setup() {
	if [ -d "$1" ]; then
		meson setup --wipe "$@"
	else
		meson setup "$@"
	fi
}

setup build-native --werror
meson test -C build-native --print-errorlogs

setup build-asan --werror -Db_sanitize=address,undefined -Db_lundef=false
meson test -C build-asan --print-errorlogs

# TSan maps its shadow at fixed addresses, which ASLR can collide with,
# and disabling ASLR needs a personality call a default container refuses
if setarch -R true 2>/dev/null; then
	setup build-tsan --werror -Db_sanitize=thread -Db_lundef=false
	setarch -R meson test -C build-tsan --print-errorlogs
else
	echo "ci.sh: TSan skipped: cannot disable ASLR here" >&2
fi

if [ "$(uname -m)" = aarch64 ]; then
	setup build-aarch64 --werror --prefix=/usr
else
	setup build-aarch64 --werror --prefix=/usr \
		--cross-file cross/aarch64-linux-gnu.ini
fi
meson compile -C build-aarch64
DESTDIR="$(realpath -m "$out")" meson install -C build-aarch64
