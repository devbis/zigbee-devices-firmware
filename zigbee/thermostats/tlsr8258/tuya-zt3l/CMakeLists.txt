# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)

# Common options first, the device's prj.conf overrides them
set(CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/../../../common/router.conf
              ${CMAKE_CURRENT_LIST_DIR}/../../common/thermostat.conf
              ${CMAKE_CURRENT_LIST_DIR}/prj.conf)

# Every build is an MCUboot slot-0 image (flash layout, OTA needs a loader)
set(SNIPPET tlsr8258-mcuboot)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tuya_zt3l_thermostat)

# OTA image type of this device; the OTA file carries the same one
set(OTA_IMAGE_TYPE 0x5A02)

# Basic cluster identity. The model id is set at run time from the thermostat
# MCU's product id (Tuya_Thermostat_rXX); APP_MODEL_ID is what it shows until then.
target_compile_definitions(app PRIVATE
  APP_MANUFACTURER="DEVBIS"
  APP_MODEL_ID="Tuya_Thermostat"
  APP_POWER_SOURCE=0x01
)

include(${CMAKE_CURRENT_LIST_DIR}/../../common/thermostat.cmake)
