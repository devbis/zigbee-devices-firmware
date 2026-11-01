# SPDX-License-Identifier: Apache-2.0
#
# Chip-independent part of every Zigbee firmware: include() it from the device
# CMakeLists.txt after find_package(Zephyr) and project().
#
# The device defines APP_MANUFACTURER, APP_MODEL_ID and APP_POWER_SOURCE
# (Basic cluster) with target_compile_definitions() and OTA_IMAGE_TYPE as a
# variable before including, and has a version.txt ("major.minor.build").
#
# Versions follow the ZCL OTA file version, as in the Telink applications:
#   APP_RELEASE    major.minor as two nibbles, 1.1 -> 0x11
#   APP_BUILD      the build number
#   STACK_RELEASE, STACK_BUILD   the Zigbee stack the image is built with
#   file version = APP_RELEASE << 24 | APP_BUILD << 16 | STACK_RELEASE << 8 | STACK_BUILD
# The Basic cluster gets SW Build ID "<stack>-<app>", e.g. 3001-0100, and the
# build date as Date Code (YYYYMMDD).

set(ZB_COMMON_DIR ${CMAKE_CURRENT_LIST_DIR})
set(ZB_SAMPLES_DIR ${ZEPHYR_BASE}/samples/zigbee)

target_sources(app PRIVATE
  ${ZB_COMMON_DIR}/app_common.c
  ${ZB_COMMON_DIR}/app_reporting.c
  # commissioning and rejoin policy of the Zephyr fork
  ${ZB_SAMPLES_DIR}/zigbee_shell/src/app_bdb.c
)
target_include_directories(app PRIVATE
  ${ZB_COMMON_DIR}
  ${ZB_SAMPLES_DIR}/zigbee_shell/src
  ${ZEPHYR_BASE}/subsys/zigbee
  ${ZEPHYR_BASE}/subsys/zigbee/include
  ${ZEPHYR_BASE}/subsys/zigbee/common/includes
  ${ZEPHYR_BASE}/subsys/zigbee/platform/zephyr
)

# ---- versions
set(STACK_RELEASE 0x30)  # stack 3.0
set(STACK_BUILD 0x01)

file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/version.txt FW_VERSION LIMIT_COUNT 1)
string(REPLACE "." ";" FW_PARTS ${FW_VERSION})
list(LENGTH FW_PARTS FW_PARTS_N)
if(NOT FW_PARTS_N EQUAL 3)
  message(FATAL_ERROR "version.txt must be major.minor.build, got '${FW_VERSION}'")
endif()
list(GET FW_PARTS 0 FW_MAJOR)
list(GET FW_PARTS 1 FW_MINOR)
list(GET FW_PARTS 2 FW_BUILD)
if(FW_MAJOR GREATER 15 OR FW_MINOR GREATER 15 OR FW_BUILD GREATER 255)
  message(FATAL_ERROR "version.txt: major and minor are at most 15, build at most 255")
endif()
math(EXPR APP_RELEASE "(${FW_MAJOR} << 4) | ${FW_MINOR}")
set(APP_BUILD ${FW_BUILD})
math(EXPR FILE_VERSION
     "(${APP_RELEASE} << 24) | (${APP_BUILD} << 16) | (${STACK_RELEASE} << 8) | ${STACK_BUILD}")

function(hex2 out value)
  math(EXPR v "${value}" OUTPUT_FORMAT HEXADECIMAL)  # 0x1f
  string(SUBSTRING "${v}" 2 -1 digits)
  string(TOUPPER "${digits}" digits)
  string(LENGTH "${digits}" n)
  if(n EQUAL 1)
    set(digits "0${digits}")
  endif()
  set(${out} "${digits}" PARENT_SCOPE)
endfunction()
hex2(H_SR ${STACK_RELEASE})
hex2(H_SB ${STACK_BUILD})
hex2(H_AR ${APP_RELEASE})
hex2(H_AB ${APP_BUILD})
set(SW_BUILD_ID "${H_SR}${H_SB}-${H_AR}${H_AB}")

# Build date: set APP_BUILD_DATE (YYYYMMDD) to override (scripts/build.sh does);
# otherwise the date of the CMake run.
if(NOT APP_BUILD_DATE)
  string(TIMESTAMP APP_BUILD_DATE "%Y%m%d")
endif()
if(NOT APP_BUILD_DATE MATCHES "^[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]$")
  message(FATAL_ERROR "APP_BUILD_DATE must be YYYYMMDD, got '${APP_BUILD_DATE}'")
endif()

target_compile_definitions(app PRIVATE
  APP_RELEASE=${APP_RELEASE}
  APP_BUILD=${APP_BUILD}
  APP_SW_BUILD_ID="${SW_BUILD_ID}"
  APP_DATE_CODE="${APP_BUILD_DATE}"
  APP_OTA_FILE_VERSION=${FILE_VERSION}U
  APP_OTA_IMAGE_TYPE=${OTA_IMAGE_TYPE}
)

# What scripts/package.py puts into the OTA file: the very same numbers
file(WRITE ${CMAKE_BINARY_DIR}/ota_info.txt
     "file_version=${FILE_VERSION}\nimage_type=${OTA_IMAGE_TYPE}\nsw_build_id=${SW_BUILD_ID}\n")
