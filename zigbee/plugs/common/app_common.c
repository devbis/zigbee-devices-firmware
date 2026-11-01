/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_common.h"
#include "app_bdb.h"

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zigbee/zb_bootstrap.h>

#include "zbapi/zb_api.h"
#include "zcl/general/zcl_basic.h"
#include "zcl/general/zcl_identify.h"
#include "os/ev_timer.h"
#include "zb_common.h"

LOG_MODULE_REGISTER(app_common);

#define APP_FACTORY_RESET_HOLD_MS 5000
#define APP_BUTTON_DEBOUNCE_MS    30
#define APP_IDENTIFY_TICK_MS      500

/* ------------------------------------------------------------------ */
/* Basic cluster                                                       */
/* ------------------------------------------------------------------ */

static const u8 basic_zcl_ver = 3U;
static const u8 basic_app_ver = APP_RELEASE;
static const u8 basic_stack_ver = 2U;
static const u8 basic_hw_ver = 1U;
static const u8 basic_power_src = APP_POWER_SOURCE;
static u8 basic_dev_enabled = 1U;
static const u16 cluster_rev = ZCL_ATTR_GLOBAL_CLUSTER_REVISION_DEFAULT;

/* ZCL character strings carry their length in the first byte. */
static u8 basic_mfr_name[1U + sizeof(APP_MANUFACTURER)];
static u8 basic_model_id[1U + sizeof(APP_MODEL_ID)];
static u8 basic_sw_build[1U + sizeof(APP_SW_BUILD_ID)];
static u8 basic_date_code[1U + sizeof(APP_DATE_CODE)];

static const zclAttrInfo_t basic_attr_tbl[] = {
	{ZCL_ATTRID_BASIC_ZCL_VER, ZCL_DATA_TYPE_UINT8, ACCESS_CONTROL_READ, (u8 *)&basic_zcl_ver},
	{ZCL_ATTRID_BASIC_APP_VER, ZCL_DATA_TYPE_UINT8, ACCESS_CONTROL_READ, (u8 *)&basic_app_ver},
	{ZCL_ATTRID_BASIC_STACK_VER, ZCL_DATA_TYPE_UINT8, ACCESS_CONTROL_READ,
	 (u8 *)&basic_stack_ver},
	{ZCL_ATTRID_BASIC_HW_VER, ZCL_DATA_TYPE_UINT8, ACCESS_CONTROL_READ, (u8 *)&basic_hw_ver},
	{ZCL_ATTRID_BASIC_MFR_NAME, ZCL_DATA_TYPE_CHAR_STR, ACCESS_CONTROL_READ,
	 (u8 *)basic_mfr_name},
	{ZCL_ATTRID_BASIC_MODEL_ID, ZCL_DATA_TYPE_CHAR_STR, ACCESS_CONTROL_READ,
	 (u8 *)basic_model_id},
	{ZCL_ATTRID_BASIC_DATE_CODE, ZCL_DATA_TYPE_CHAR_STR, ACCESS_CONTROL_READ,
	 (u8 *)basic_date_code},
	{ZCL_ATTRID_BASIC_POWER_SOURCE, ZCL_DATA_TYPE_ENUM8, ACCESS_CONTROL_READ,
	 (u8 *)&basic_power_src},
	{ZCL_ATTRID_BASIC_DEV_ENABLED, ZCL_DATA_TYPE_BOOLEAN,
	 ACCESS_CONTROL_READ | ACCESS_CONTROL_WRITE, (u8 *)&basic_dev_enabled},
	{ZCL_ATTRID_BASIC_SW_BUILD_ID, ZCL_DATA_TYPE_CHAR_STR, ACCESS_CONTROL_READ,
	 (u8 *)basic_sw_build},
	{ZCL_ATTRID_GLOBAL_CLUSTER_REVISION, ZCL_DATA_TYPE_UINT16, ACCESS_CONTROL_READ,
	 (u8 *)&cluster_rev},
};

/* ------------------------------------------------------------------ */
/* led0                                                                */
/* ------------------------------------------------------------------ */

#if DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(led0))
#define APP_HAS_LED 1
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#endif

static bool led_user_on;
static bool identify_active;
static bool identify_blink_on;

static void led_apply(void)
{
#ifdef APP_HAS_LED
	bool on = identify_active ? identify_blink_on : led_user_on;

	gpio_pin_set_dt(&led, on ? 1 : 0);
#endif
}

void app_led_set(bool on)
{
	led_user_on = on;
	led_apply();
}

/* ------------------------------------------------------------------ */
/* Identify cluster                                                    */
/* ------------------------------------------------------------------ */

static u16 identify_time;
static ev_timer_event_t *identify_timer;

static const zclAttrInfo_t identify_attr_tbl[] = {
	{ZCL_ATTRID_IDENTIFY_TIME, ZCL_DATA_TYPE_UINT16,
	 ACCESS_CONTROL_READ | ACCESS_CONTROL_WRITE, (u8 *)&identify_time},
	{ZCL_ATTRID_GLOBAL_CLUSTER_REVISION, ZCL_DATA_TYPE_UINT16, ACCESS_CONTROL_READ,
	 (u8 *)&cluster_rev},
};

static void identify_stop(void)
{
	identify_active = false;
	led_apply();
}

/* Blinks every tick and counts the Identify Time attribute down once a second. */
static int identify_tick(void *arg)
{
	static bool second;

	ARG_UNUSED(arg);

	identify_blink_on = !identify_blink_on;
	led_apply();

	second = !second;
	if (second) {
		return APP_IDENTIFY_TICK_MS;
	}

	if (identify_time > 0U) {
		identify_time--;
	}
	if (identify_time == 0U) {
		identify_timer = NULL;
		identify_stop();
		return -1;
	}

	return APP_IDENTIFY_TICK_MS;
}

static status_t identify_cb(zclIncomingAddrInfo_t *addr_info, u8 cmd_id, void *payload)
{
	ARG_UNUSED(addr_info);

	if ((cmd_id != ZCL_CMD_IDENTIFY) || (payload == NULL)) {
		return ZCL_STA_SUCCESS;
	}

	identify_time = ((zcl_identifyCmd_t *)payload)->identifyTime;

	if (identify_time == 0U) {
		if (identify_timer != NULL) {
			(void)ev_timer_taskCancel(&identify_timer);
		}
		identify_stop();
	} else if (identify_timer == NULL) {
		identify_active = true;
		identify_blink_on = true;
		led_apply();
		identify_timer = ev_timer_taskPost(identify_tick, NULL, APP_IDENTIFY_TICK_MS);
	}

	return ZCL_STA_SUCCESS;
}

void app_common_clusters(zcl_specClusterInfo_t *clusters)
{
	basic_mfr_name[0] = sizeof(APP_MANUFACTURER) - 1U;
	memcpy(&basic_mfr_name[1], APP_MANUFACTURER, sizeof(APP_MANUFACTURER) - 1U);
	basic_model_id[0] = sizeof(APP_MODEL_ID) - 1U;
	memcpy(&basic_model_id[1], APP_MODEL_ID, sizeof(APP_MODEL_ID) - 1U);
	basic_sw_build[0] = sizeof(APP_SW_BUILD_ID) - 1U;
	memcpy(&basic_sw_build[1], APP_SW_BUILD_ID, sizeof(APP_SW_BUILD_ID) - 1U);
	basic_date_code[0] = sizeof(APP_DATE_CODE) - 1U;
	memcpy(&basic_date_code[1], APP_DATE_CODE, sizeof(APP_DATE_CODE) - 1U);

	clusters[0] = (zcl_specClusterInfo_t){
		.clusterId = ZCL_CLUSTER_GEN_BASIC,
		.manuCode = 0U,
		.attrNum = ARRAY_SIZE(basic_attr_tbl),
		.attrTbl = basic_attr_tbl,
		.clusterRegisterFunc = zcl_basic_register,
		.clusterAppCb = NULL,
	};
	clusters[1] = (zcl_specClusterInfo_t){
		.clusterId = ZCL_CLUSTER_GEN_IDENTIFY,
		.manuCode = 0U,
		.attrNum = ARRAY_SIZE(identify_attr_tbl),
		.attrTbl = identify_attr_tbl,
		.clusterRegisterFunc = zcl_identify_register,
		.clusterAppCb = identify_cb,
	};
}

void zcl_rx_handler(void *p);

void app_endpoint_register(af_simple_descriptor_t *desc, zcl_specClusterInfo_t *clusters,
			   uint8_t num, zcl_hookFn_t hook)
{
#ifdef APP_HAS_LED
	if (gpio_is_ready_dt(&led)) {
		(void)gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	}
#endif

	zcl_init(hook);

	if (af_simpleDescGet(APP_ENDPOINT) == NULL) {
		if (!af_endpointRegister(APP_ENDPOINT, desc, (af_endpoint_cb_t)zcl_rx_handler,
					 NULL)) {
			LOG_WRN("endpoint %u register failed", APP_ENDPOINT);
		}
	}

	zcl_register(APP_ENDPOINT, num, clusters);
}

/* ------------------------------------------------------------------ */
/* Buttons                                                             */
/* ------------------------------------------------------------------ */

#if DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(sw0)) || DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(sw1))
#define APP_HAS_BUTTONS 1

struct app_button {
	struct gpio_dt_spec spec;
	struct gpio_callback cb;
	struct k_work_delayable debounce;
	struct k_work_delayable long_press;
	uint8_t idx;
	bool pressed;
};

static struct app_button buttons[] = {
#if DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(sw0))
	{.spec = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios), .idx = 0U},
#endif
#if DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(sw1))
	{.spec = GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios), .idx = 1U},
#endif
};

static app_button_cb_t button_user_cb;

static void button_task(void *arg)
{
	uintptr_t v = (uintptr_t)arg;

	if (button_user_cb != NULL) {
		button_user_cb((uint8_t)(v >> 1), (v & 1U) != 0U);
	}
}

static void factory_reset_task(void *arg)
{
	ARG_UNUSED(arg);
	zb_factoryReset();
}

static void button_long_press(struct k_work *work)
{
	ARG_UNUSED(work);
	(void)tl_zbTaskPost(factory_reset_task, NULL);
}

static void button_debounce(struct k_work *work)
{
	struct app_button *btn =
		CONTAINER_OF(k_work_delayable_from_work(work), struct app_button, debounce);
	bool pressed = gpio_pin_get_dt(&btn->spec) > 0;

	if (pressed == btn->pressed) {
		return;
	}
	btn->pressed = pressed;

	if (btn->idx == 0U) {
		if (pressed) {
			(void)k_work_reschedule(&btn->long_press, K_MSEC(APP_FACTORY_RESET_HOLD_MS));
		} else {
			(void)k_work_cancel_delayable(&btn->long_press);
		}
	}

	(void)tl_zbTaskPost(button_task, (void *)(uintptr_t)(((uintptr_t)btn->idx << 1) | pressed));
}

static void button_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	struct app_button *btn = CONTAINER_OF(cb, struct app_button, cb);

	ARG_UNUSED(port);
	ARG_UNUSED(pins);
	(void)k_work_reschedule(&btn->debounce, K_MSEC(APP_BUTTON_DEBOUNCE_MS));
}
#endif /* DT_NODE_HAS_STATUS_OKAY(sw0) || sw1 */

void app_buttons_init(app_button_cb_t cb)
{
#ifdef APP_HAS_BUTTONS
	button_user_cb = cb;

	for (size_t i = 0; i < ARRAY_SIZE(buttons); i++) {
		struct app_button *btn = &buttons[i];

		if (!gpio_is_ready_dt(&btn->spec)) {
			continue;
		}

		k_work_init_delayable(&btn->debounce, button_debounce);
		k_work_init_delayable(&btn->long_press, button_long_press);
		(void)gpio_pin_configure_dt(&btn->spec, GPIO_INPUT);
		gpio_init_callback(&btn->cb, button_isr, BIT(btn->spec.pin));
		(void)gpio_add_callback(btn->spec.port, &btn->cb);
		(void)gpio_pin_interrupt_configure_dt(&btn->spec, GPIO_INT_EDGE_BOTH);
	}
#else
	ARG_UNUSED(cb);
	LOG_WRN("the board has no sw0 or sw1 alias");
#endif
}

/* ------------------------------------------------------------------ */
/* Bootstrap hooks                                                     */
/* ------------------------------------------------------------------ */

void zb_platform_app_register_endpoints(void)
{
	app_device_register();
}

const char *zb_platform_app_basic_mfr_name(void)
{
	return APP_MANUFACTURER;
}

const char *zb_platform_app_basic_model_id(void)
{
	return APP_MODEL_ID;
}

bool zb_platform_app_get_fixed_join_target(struct zb_platform_bdb_fixed_target *target)
{
	ARG_UNUSED(target);
	return false;
}

bool zb_platform_app_get_join_profile(struct zb_platform_bdb_join_profile *profile)
{
	return app_bdb_get_join_profile(profile);
}

void zb_platform_app_bootstrap_ready(void)
{
	app_bdb_bootstrap_ready();
}

bool zb_platform_app_enable_radio_smoke_probe(void)
{
	return false;
}

bool zb_platform_app_should_start_commissioning(void)
{
	return app_bdb_should_start_commissioning();
}

void zb_platform_app_start_commissioning(void)
{
	app_bdb_start_commissioning();
}

void zb_platform_app_network_left(void)
{
	app_bdb_network_left();
}

void zb_platform_app_bdb_commissioning_status(uint8_t status, bool joined_network)
{
	app_bdb_commissioning_status(status, joined_network);
}

int main(void)
{
	return 0;
}
