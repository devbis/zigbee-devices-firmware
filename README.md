zigbee-devices-firmware
=======================

Firmware for Zigbee (and later other) devices on TLSR8258 and friends, built
from the drivers and libraries of `zephyr-iot-drivers` on the Telink Zephyr
fork (`zephyr-tlsr8258`, branch `zigbee-generic-stack`).

    zigbee/
      common/                            chip-independent code of every device
        app_common.c/h                   bootstrap hooks, Basic, Identify, led0, buttons
        app_reporting.c/h                restore and default attribute reporting
        app.cmake, router.conf           versions, OTA info, sources; router, OTA, NVS settings
      plugs/
        common/                          plug.c, plug.conf, plug.cmake
        <chip>/<device>/                 one directory per plug model
      thermostats/
        common/                          thermostat.c, thermostat.conf, thermostat.cmake
        <chip>/<device>/                 one directory per thermostat module
          CMakeLists.txt                 identity (manufacturer, model, power source)
          prj.conf                       differences from the common .conf
          <board>.overlay                pins of this device
          loader.conf, loader.overlay    its MCUboot loader (serial recovery off here)
    scripts/build.sh                     build with the local Telink checkout
    tests/emulator/                      thermostat run in the mcuemu emulator

There is no board directory: a device is the chip's generic board
(`tlsr8258_generic`, 512 KiB flash) plus its overlay. The board follows from
the `<chip>` directory in `scripts/build.sh`. Code that depends on the chip but
is shared between devices of that chip goes to `zigbee/<kind>/<chip>/common/`
when it appears.

Devices
-------

| Device | SoC | Measures with |
|---|---|---|
| `zigbee/plugs/tlsr8258/zbeacon-ts011f-bl0937` | TLSR8258 | BL0937 |
| `zigbee/thermostats/tlsr8258/tuya-zt3l` | TLSR8258 (Tuya ZT3L module) | the thermostat's own MCU over UART |

Build and OTA
-------------

    scripts/build.sh zigbee/plugs/tlsr8258/zbeacon-ts011f-bl0937
    scripts/build.sh zigbee/plugs/tlsr8258/zbeacon-ts011f-bl0937 -- -DEXTRA_CONF_FILE=debug.conf

Every device is built for MCUboot, so OTA always works. The script builds the
loader (`build/loader-<device>`), then the application as a slot-0 image, then
`scripts/package.py` writes into `build/<device>/`:

| File | Use |
|---|---|
| `<device>-<ver>-full.bin` | loader at 0x0 + signed image at 0x8000; program once with the programmer |
| `<device>-<ver>.zigbee` | Zigbee OTA file for the coordinator |
| `<device>-<ver>-signed.bin` | the MCUboot image inside both |

The IEEE address (0x76000) and the Zigbee NV (0x7c000) are outside the image;
do not erase them when programming.

Release: raise `version.txt` in the device directory (`major.minor.build`),
build, give the `.zigbee` file to the coordinator (see Versions below). The OTA identity is the device's `APP_OTA_MANUFACTURER_CODE`
and `APP_OTA_IMAGE_TYPE` in its `CMakeLists.txt`; they end up in the `.zigbee`
header and in the firmware. The router asks for an update periodically,
writes it to slot 1, MCUboot installs it, and the new image confirms itself
30 s after it is on the network, or the loader goes back to the old one.

Serial recovery
---------------

The loader has serial recovery on UART0 (TX PB1, RX PA0) by default. A device
that has no such pins, or uses them otherwise, turns it off with `loader.conf`
and `loader.overlay` in its directory; `build.sh` passes them to the loader
build when they exist. The Zbeacon TS011F plug does: only SWS is wired out and
its LED and button sit on those pins. Building such a loader prints a harmless
Kconfig warning about the `BOOT_SERIAL_UART` choice (the board's default
selects it, the device's `MCUBOOT_SERIAL=n` leaves the choice empty).

The loader's board configuration lives in the Telink checkout
(`bootloader/mcuboot/boot/zephyr/boards/tlsr8258_generic.*`); the flash layout
there and in the `tlsr8258-mcuboot` snippet must stay equal.

Local paths
-----------

No path of any machine is in the repository. `scripts/build.sh` takes them from
the environment, or from `scripts/env.local` (ignored by git; copy
`scripts/env.example`). A variable set in the environment wins over the file,
so the file is optional.

| Variable | Meaning |
|---|---|
| `ZEPHYR_BASE` | the Zephyr tree |
| `LLVM_TOOLCHAIN_PATH` | LLVM that compiles for the TC32 target (the directory with `bin/clang`) |
| `ZEPHYR_EXTRA_MODULES` | `zephyr-iot-drivers` (drivers and libraries) |
| `WEST`, `PYTHON` | `west`, and a Python with the imgtool requirements; default from `PATH` |
| `MCUBOOT_DIR` | the mcuboot module; default `bootloader/mcuboot` in the west workspace (`west topdir`) |

`west.yml` is for a proper workspace later.

Versions
--------

Numbers follow the ZCL OTA file version, as in the Telink applications. They
come from `version.txt` (`major.minor.build`, major and minor at most 15, build
at most 255) and the stack constants in `zigbee/common/app.cmake`:

| Number | Value | Example (`0.1.0`, stack 3.0 build 01) |
|---|---|---|
| `APP_RELEASE` | major.minor as two nibbles | `0x01` |
| `APP_BUILD` | build | `0x00` |
| `STACK_RELEASE`, `STACK_BUILD` | the stack the image is built with | `0x30`, `0x01` |
| OTA file version | `APP_RELEASE<<24 \| APP_BUILD<<16 \| STACK_RELEASE<<8 \| STACK_BUILD` | `0x01003001` |
| Basic: Application Version | `APP_RELEASE` | `1` |
| Basic: SW Build ID | `<stack release><stack build>-<app release><app build>` in hex | `3001-0100` |
| Basic: Date Code | the day of the build, `YYYYMMDD` | `20261101` |

The build date is the day `scripts/build.sh` ran (`APP_BUILD_DATE=YYYYMMDD`
overrides it); a bare `west build` takes the day of the CMake run. No git is
needed.

Basic cluster
-------------

Manufacturer name, model id and power source are `APP_MANUFACTURER`,
`APP_MODEL_ID` and `APP_POWER_SOURCE` in the device's `CMakeLists.txt`.
Commissioning policy still comes from the Zephyr fork's `zigbee_shell` sample
(`app_bdb.c`).

Thermostats
-----------

`tuya-zt3l` is the Zigbee module of a Tuya floor-heating thermostat; the
thermostat is a separate MCU that talks to the module over UART (9600 baud, TX
PB1, RX PB7). One image serves all 13 models of `tuya_thermostat_zrd`
(r01..r0D): the model comes from the MCU's product id, and the Basic model id
becomes `Tuya_Thermostat_rXX` at run time, so Zigbee2MQTT recognizes it by
that name as it does for the stock firmware. The datapoint tables and codecs
are `lib/tuya_thermostat` of `zephyr-iot-drivers`, the protocol is
`lib/tuya_mcu`.

Endpoint 1 (thermostat): Basic, Identify, Groups, Level Control, Thermostat
with the manufacturer attributes `0xF000..0xF010`, User Interface
Configuration, Fan Control, Relative Humidity; out: Time, OTA. Endpoint 2:
Level Control. The module asks the coordinator for the time and answers the MCU's
time requests; it reports the network state to the MCU and resets its network
settings when the MCU asks.

Not done: Scenes, the manufacturer name attribute `0xF00E` of the stock
firmware, schedule events of model r08 (its schedule is a state machine, the
codecs return "not supported"), persistence of settings in the module, Touchlink.
A Set Weekly Schedule with fewer transitions than the model keeps repeats the
last one in the remaining slots. Not run on hardware.

Emulator test
-------------

`tests/emulator/run_thermostat.py` runs a debug build (`-DEXTRA_CONF_FILE=debug.conf`,
the firmware prints what it does to RTT) in the `mcuemu` emulator next to a
coordinator and plays the thermostat's MCU on the UART: it answers the product
query with the product id of each model, reports the model's datapoints and
checks that the firmware picks the model, acknowledges the report and applies
every datapoint. All 13 models pass (12 datapoints of r08 are applied, the four
schedule-event datapoints are the unsupported ones). What it cannot see: the
Zigbee side (the network is encrypted) and writes from the coordinator.

    scripts/build.sh zigbee/thermostats/tlsr8258/tuya-zt3l -- \
        -DEXTRA_CONF_FILE=$PWD/zigbee/thermostats/tlsr8258/tuya-zt3l/debug.conf
    tests/emulator/run_thermostat.py --mcuemu .../mcuemu \
        --image build/tuya-zt3l/tuya-zt3l-0.1.0-full.bin --elf build/tuya-zt3l/zephyr/zephyr.elf \
        --coordinator .../samplegw_tb03f.bin --models .../lib/tuya_thermostat/tt_models.c

Open items for `zbeacon-ts011f-bl0937`
--------------------------------------

- BL0937 calibration is the datasheet default, not the stock one from
  `zigbee-rs-plugs/products/zbeacon-ts011f-512k` (explicit gains, 2 353 211
  pulses/kWh). The driver binding needs explicit scales to carry them over.
- The energy total is not persisted: it restarts from zero after a reboot.
- No protections (over/under voltage, over current) and no relay state
  restore yet.
- Default reporting is set for on/off, voltage, current, power and energy, but
  only reaches a coordinator that has bound the clusters.
- The flash layout is the one of the fork's `tlsr8258_generic` board; it was
  not compared with the stock layout of this plug.
- OTA is built and packaged but the whole path (loader start, download, install,
  confirm, revert) is not run on hardware yet. The image type `0x5A01` is a
  placeholder to settle before releasing.
- Untested on hardware.
