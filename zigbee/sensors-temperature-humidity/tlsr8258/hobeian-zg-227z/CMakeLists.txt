# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)

# Common options first, the device's prj.conf overrides them
set(CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/../../../common/base.conf
              ${CMAKE_CURRENT_LIST_DIR}/../../../common/end_device.conf
              ${CMAKE_CURRENT_LIST_DIR}/../../common/sensor.conf
              ${CMAKE_CURRENT_LIST_DIR}/prj.conf)

# Every build is an MCUboot slot-0 image (flash layout, OTA needs a loader)
set(SNIPPET tlsr8258-mcuboot)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hobeian_zg_227z)

# OTA image type of this device; the OTA file carries the same one
set(OTA_IMAGE_TYPE 0x5B04)

# Basic cluster identity. Own strings, not the stock ones. Power source 3: battery.
target_compile_definitions(app PRIVATE
  APP_MANUFACTURER="DEVBIS"
  APP_MODEL_ID="TH-HOBEIAN-ZG-227Z-AHT20"
  APP_POWER_SOURCE=0x03
)

include(${CMAKE_CURRENT_LIST_DIR}/../../common/sensor.cmake)
