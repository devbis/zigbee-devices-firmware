/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * Code shared by the Zigbee device samples (light, switch, contact sensor):
 * the bootstrap hooks, the Basic and Identify clusters, the led0 output and
 * the sw0/sw1 buttons.
 *
 * Each device defines APP_MANUFACTURER, APP_MODEL_ID and APP_POWER_SOURCE
 * (Basic cluster) in its CMakeLists.txt and implements app_device_register();
 * plug.cmake defines APP_RELEASE, APP_SW_BUILD_ID and APP_DATE_CODE.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "platform/zephyr/compiler_zephyr.h"
#include <zephyr/zigbee/zb_types.h>
#include "af/zb_af.h"
#include "zcl/zcl_const.h"
/* zcl.h declares the reporting API only once zcl_config.h has defined ZCL_REPORT */
#include "zcl/zcl_config.h"
#include "zcl/zcl.h"

#define APP_ENDPOINT      0x01U
#define APP_HA_PROFILE_ID 0x0104U

/* Basic and Identify occupy the first entries of every cluster table. */
#define APP_COMMON_CLUSTER_NUM 2U

/* Called while the ZCL layer is being set up; registers the device endpoint. */
void app_device_register(void);

/* The Basic model id, when it is not known at build time. At most 32 characters. */
void app_basic_set_model_id(const char *model_id);

/* Fill clusters[0] with Basic and clusters[1] with Identify. */
void app_common_clusters(zcl_specClusterInfo_t *clusters);

/* Initialize ZCL, register the endpoint and the clusters. */
void app_endpoint_register(af_simple_descriptor_t *desc, zcl_specClusterInfo_t *clusters,
			   uint8_t num, zcl_hookFn_t hook);

/* Drive led0 (a no-op on boards without one). Identify blinking overrides it. */
void app_led_set(bool on);

/*
 * Called on the Zigbee thread for every edge of sw0 (idx 0) and sw1 (idx 1).
 * A press of sw0 held for 5 s resets the device to factory defaults.
 */
typedef void (*app_button_cb_t)(uint8_t idx, bool pressed);

void app_buttons_init(app_button_cb_t cb);
