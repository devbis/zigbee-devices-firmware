#!/bin/sh
# Build a device firmware with the local Telink Zephyr checkout: the MCUboot
# loader for the chip's board, the application as a slot-0 image, then the
# signed image, the OTA file and the full flash image (scripts/package.py).
#   scripts/build.sh zigbee/plugs/tlsr8258/zbeacon-ts011f-bl0937 [-- cmake args]
# The board follows from the chip directory (tlsr8258 -> tlsr8258_generic);
# the device's <board>.overlay describes its pins.
# Paths come from the environment, or from scripts/env.local (not in git; copy
# scripts/env.example). A variable already set in the environment wins over the
# file. See the README for what each one is.
set -eu

dev=${1:?usage: build.sh <device dir> [-- cmake args]}
shift
[ "${1:-}" = "--" ] && shift

here=$(cd "$(dirname "$0")/.." && pwd)
dev=$(cd "$dev" && pwd)
chip=$(basename "$(dirname "$dev")")
case "$chip" in
tlsr8258) board=tlsr8258_generic ;;
*) echo "no board known for chip directory '$chip'" >&2; exit 1 ;;
esac

if [ -f "$here/scripts/env.local" ]; then
	# shellcheck disable=SC1091
	. "$here/scripts/env.local"
fi
need() {
	eval "[ -n \"\${$1:-}\" ]" || {
		echo "$1 is not set: export it or put it in scripts/env.local (see scripts/env.example)" >&2
		exit 1
	}
}
need ZEPHYR_BASE
need LLVM_TOOLCHAIN_PATH
need ZEPHYR_EXTRA_MODULES
west=${WEST:-west}
export ZEPHYR_BASE ZEPHYR_TOOLCHAIN_VARIANT=host

cd "$ZEPHYR_BASE"

# MCUboot module, at its place in the west workspace unless told otherwise
MCUBOOT_DIR=${MCUBOOT_DIR:-$("$west" topdir)/bootloader/mcuboot}
export MCUBOOT_DIR
toolchain="-DTOOLCHAIN_VARIANT_COMPILER=llvm -DLLVM_TOOLCHAIN_PATH=$LLVM_TOOLCHAIN_PATH"

# Loader: serial recovery is on by default; a device without the pins for it
# (or using them otherwise) turns it off with its own loader.conf/loader.overlay
loader=$here/build/loader-$(basename "$dev")
loader_args=""
# a device's own loader.conf/loader.overlay, else the ones its chip directory shares
for dir in "$dev" "$(dirname "$dev")/common"; do
	if [ -f "$dir/loader.conf" ] || [ -f "$dir/loader.overlay" ]; then
		[ -f "$dir/loader.conf" ] && loader_args="$loader_args -DEXTRA_CONF_FILE=$dir/loader.conf"
		[ -f "$dir/loader.overlay" ] && loader_args="$loader_args -DEXTRA_DTC_OVERLAY_FILE=$dir/loader.overlay"
		break
	fi
done
# shellcheck disable=SC2086
"$west" build -p auto -b "$board" -d "$loader" "$MCUBOOT_DIR/boot/zephyr" -- $toolchain $loader_args

# Date code of the Basic cluster: the day of this build (APP_BUILD_DATE overrides)
APP_BUILD_DATE=${APP_BUILD_DATE:-$(date +%Y%m%d)}

# Always from scratch: Zephyr keeps CONF_FILE in the CMake cache, and a change of the
# list in a CMakeLists.txt would otherwise be ignored by an incremental build.
out=$here/build/$(basename "$dev")
"$west" build -p always -b "$board" -d "$out" "$dev" -- \
  -DZEPHYR_EXTRA_MODULES="$ZEPHYR_EXTRA_MODULES" -DAPP_BUILD_DATE="$APP_BUILD_DATE" \
  $toolchain "$@"

PYTHON=${PYTHON:-python3} exec "$here/scripts/package.py" "$dev" "$out" "$loader/zephyr/zephyr.bin"
