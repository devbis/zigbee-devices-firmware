#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Sign a built device image and produce the OTA file and the full flash image.

    package.py <device dir> <build dir> <loader bin>

Writes next to the build:
    <device>-<version>-signed.bin   MCUboot image for slot 0
    <device>-<version>.zigbee       Zigbee OTA file for the coordinator
    <device>-<version>-full.bin     loader + signed image, flashed once at 0x0

Environment: ZEPHYR_BASE (the Zephyr tree, for the OTA packer), MCUBOOT_DIR (the
mcuboot module, for imgtool), PYTHON (an interpreter with the imgtool
requirements; default: the one running this script). scripts/build.sh sets them.
"""

import os
import re
import subprocess
import sys
from pathlib import Path

# Flash layout shared with the tlsr8258-mcuboot snippet and the loader's board
# overlay: loader 0x0..0x7fff, slot 0 from 0x8000.
LAYOUTS = {
    "tlsr8258": {"slot0": 0x8000, "slot_size": 0x37000},
}
HEADER_SIZE = 0x200



def kconfig(config_text, name):
    m = re.search(rf"^{name}=(0x[0-9A-Fa-f]+|\d+)$", config_text, re.M)
    if not m:
        sys.exit(f"{name} not found in the build's .config")
    return int(m.group(1), 0)


def ota_info(path):
    """key=value lines written by plug.cmake: the numbers the firmware was built with."""
    info = dict(line.split("=", 1) for line in path.read_text().split() if "=" in line)
    return info


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    dev, build, loader = (Path(a).resolve() for a in sys.argv[1:])
    chip = dev.parent.name
    layout = LAYOUTS.get(chip) or sys.exit(f"no flash layout known for chip '{chip}'")

    for name in ("ZEPHYR_BASE", "MCUBOOT_DIR"):
        if not os.environ.get(name):
            sys.exit(f"{name} is not set (see scripts/env.example)")
    python = os.environ.get("PYTHON") or sys.executable
    imgtool = Path(os.environ["MCUBOOT_DIR"]) / "scripts/imgtool.py"
    ota_pack = Path(os.environ["ZEPHYR_BASE"]) / "scripts/zigbee_ota_pack.py"

    version = (dev / "version.txt").read_text().strip()

    info = ota_info(build / "ota_info.txt")
    image_type = int(info["image_type"], 0)
    file_version = int(info["file_version"], 0)
    # the same value the firmware reports in its node descriptor and OTA preamble
    manufacturer = kconfig((build / "zephyr/.config").read_text(), "CONFIG_ZIGBEE_MANUFACTURER_CODE")

    name = dev.name
    stem = build / f"{name}-{version}"
    signed = Path(f"{stem}-signed.bin")
    ota = Path(f"{stem}.zigbee")
    full = Path(f"{stem}-full.bin")

    subprocess.run(
        [python, str(imgtool), "sign", "--header-size", hex(HEADER_SIZE), "--pad-header",
         "--align", "4", "--slot-size", hex(layout["slot_size"]), "--version", version,
         str(build / "zephyr/zephyr.bin"), str(signed)],
        check=True)
    subprocess.run(
        [python, str(ota_pack), str(signed), str(ota), "--manufacturer", hex(manufacturer),
         "--image-type", hex(image_type), "--file-version", hex(file_version),
         "--header-string", name[:32]],
        check=True)

    boot = loader.read_bytes()
    if len(boot) > layout["slot0"]:
        sys.exit(f"loader is {len(boot):#x} bytes, the loader partition ends at {layout['slot0']:#x}")
    image = signed.read_bytes()
    full.write_bytes(boot.ljust(layout["slot0"], b"\xff") + image)
    print(f"{signed.name}: {len(image)} bytes (slot {layout['slot_size']:#x})")
    print(f"{full.name}: loader {len(boot)} + image {len(image)} bytes")


if __name__ == "__main__":
    main()
