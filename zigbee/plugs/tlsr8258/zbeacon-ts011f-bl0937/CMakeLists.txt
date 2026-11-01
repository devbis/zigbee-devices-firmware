# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)

# Common options first, the device's prj.conf overrides them
set(CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/../../../common/router.conf
              ${CMAKE_CURRENT_LIST_DIR}/../../common/plug.conf
              ${CMAKE_CURRENT_LIST_DIR}/prj.conf)

# Every build is an MCUboot slot-0 image (flash layout, OTA needs a loader)
set(SNIPPET tlsr8258-mcuboot)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zbeacon_ts011f_bl0937)

# OTA image type of this device; the OTA file carries the same one
set(OTA_IMAGE_TYPE 0x5A01)

# Basic cluster identity. Own strings, not the stock "Zbeacon"/"TS011F".
target_compile_definitions(app PRIVATE
  APP_MANUFACTURER="DEVBIS"
  APP_MODEL_ID="Plug-ZBEACON-TS011F-BL0937"
  APP_POWER_SOURCE=0x01
)

include(${CMAKE_CURRENT_LIST_DIR}/../../common/plug.cmake)
