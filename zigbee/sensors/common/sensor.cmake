# SPDX-License-Identifier: Apache-2.0
#
# Chip-independent part of every sensor firmware: include() it from the device
# CMakeLists.txt after find_package(Zephyr) and project(), with what
# zigbee/common/app.cmake asks of the device.

include(${CMAKE_CURRENT_LIST_DIR}/../../common/app.cmake)

target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/sensor.c)
