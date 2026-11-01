/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Smart plug router: relay on On/Off, a button that toggles it, and voltage,
 * current, power and energy from the energy meter aliased "energy-meter"
 * (any driver of drivers/sensor in zephyr-iot-drivers).
 *
 * Units on the air:
 *   RMS voltage    0.01 V   (divisor 100)
 *   RMS current    mA       (divisor 1000 -> A)
 *   active power   W        (divisor 1)
 *   summation      Wh       (kWh with divisor 1000)
 */

/*
 * The Zephyr headers go first: the Zigbee headers define macros such as abs()
 * that break <stdlib.h> if it is included after them.
 */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>

#include <iot_drivers/energy_meter.h>

#include "app_common.h"

#include "os/ev_timer.h"

#include "zcl/general/zcl_group.h"
#include "zcl/general/zcl_onoff.h"
#include "zcl/measument_sensing/zcl_electrical_measurement.h"
#include "zcl/smart_energy/zcl_metering.h"
#if defined(CONFIG_ZIGBEE_OTA)
#include "ota/ota.h"
#endif

#define MEASURE_PERIOD_MS 1000

static const struct gpio_dt_spec relay = GPIO_DT_SPEC_GET(DT_ALIAS(relay0), gpios);
static const struct device *const meter = DEVICE_DT_GET(DT_ALIAS(energy_meter));

static const u16 cluster_rev = ZCL_ATTR_GLOBAL_CLUSTER_REVISION_DEFAULT;

/* On/Off */
static bool onoff_state;
static u8 group_name_support;

/* Electrical Measurement */
static u16 rms_voltage;
static u16 rms_current;
static s16 active_power;
static const u16 ac_voltage_mult = 1U, ac_voltage_div = 100U;
static const u16 ac_current_mult = 1U, ac_current_div = 1000U;
static const u16 ac_power_mult = 1U, ac_power_div = 1U;

/* Metering */
static u64 summation_wh;
static const u8 metering_unit; /* 0: kWh */
static const u32 metering_mult = 1U, metering_div = 1000U;
static const u8 metering_format = 0x33U; /* 6 digits, 3 decimals */
static const u8 metering_device_type; /* 0: electric */

static const zclAttrInfo_t group_attr_tbl[] = {
	{ZCL_ATTRID_GROUP_NAME_SUPPORT, ZCL_DATA_TYPE_BITMAP8, ACCESS_CONTROL_READ,
	 &group_name_support},
	{ZCL_ATTRID_GLOBAL_CLUSTER_REVISION, ZCL_DATA_TYPE_UINT16, ACCESS_CONTROL_READ,
	 (u8 *)&cluster_rev},
};

static const zclAttrInfo_t onoff_attr_tbl[] = {
	{ZCL_ATTRID_ONOFF, ZCL_DATA_TYPE_BOOLEAN, ACCESS_CONTROL_READ | ACCESS_CONTROL_REPORTABLE,
	 (u8 *)&onoff_state},
	{ZCL_ATTRID_GLOBAL_CLUSTER_REVISION, ZCL_DATA_TYPE_UINT16, ACCESS_CONTROL_READ,
	 (u8 *)&cluster_rev},
};

static const zclAttrInfo_t em_attr_tbl[] = {
	{ZCL_ATTRID_RMS_VOLTAGE, ZCL_DATA_TYPE_UINT16,
	 ACCESS_CONTROL_READ | ACCESS_CONTROL_REPORTABLE, (u8 *)&rms_voltage},
	{ZCL_ATTRID_RMS_CURRENT, ZCL_DATA_TYPE_UINT16,
	 ACCESS_CONTROL_READ | ACCESS_CONTROL_REPORTABLE, (u8 *)&rms_current},
	{ZCL_ATTRID_ACTIVE_POWER, ZCL_DATA_TYPE_INT16,
	 ACCESS_CONTROL_READ | ACCESS_CONTROL_REPORTABLE, (u8 *)&active_power},
	{ZCL_ATTRID_AC_VOLTAGE_MULTIPLIER, ZCL_DATA_TYPE_UINT16, ACCESS_CONTROL_READ,
	 (u8 *)&ac_voltage_mult},
	{ZCL_ATTRID_AC_VOLTAGE_DIVISOR, ZCL_DATA_TYPE_UINT16, ACCESS_CONTROL_READ,
	 (u8 *)&ac_voltage_div},
	{ZCL_ATTRID_AC_CURRENT_MULTIPLIER, ZCL_DATA_TYPE_UINT16, ACCESS_CONTROL_READ,
	 (u8 *)&ac_current_mult},
	{ZCL_ATTRID_AC_CURRENT_DIVISOR, ZCL_DATA_TYPE_UINT16, ACCESS_CONTROL_READ,
	 (u8 *)&ac_current_div},
	{ZCL_ATTRID_AC_POWER_MULTIPLIER, ZCL_DATA_TYPE_UINT16, ACCESS_CONTROL_READ,
	 (u8 *)&ac_power_mult},
	{ZCL_ATTRID_AC_POWER_DIVISOR, ZCL_DATA_TYPE_UINT16, ACCESS_CONTROL_READ,
	 (u8 *)&ac_power_div},
	{ZCL_ATTRID_GLOBAL_CLUSTER_REVISION, ZCL_DATA_TYPE_UINT16, ACCESS_CONTROL_READ,
	 (u8 *)&cluster_rev},
};

static const zclAttrInfo_t metering_attr_tbl[] = {
	{ZCL_ATTRID_CURRENT_SUMMATION_DELIVERD, ZCL_DATA_TYPE_UINT48,
	 ACCESS_CONTROL_READ | ACCESS_CONTROL_REPORTABLE, (u8 *)&summation_wh},
	{ZCL_ATTRID_UNIT_OF_MEASURE, ZCL_DATA_TYPE_ENUM8, ACCESS_CONTROL_READ,
	 (u8 *)&metering_unit},
	{ZCL_ATTRID_MULTIPLIER, ZCL_DATA_TYPE_UINT24, ACCESS_CONTROL_READ, (u8 *)&metering_mult},
	{ZCL_ATTRID_DIVISOR, ZCL_DATA_TYPE_UINT24, ACCESS_CONTROL_READ, (u8 *)&metering_div},
	{ZCL_ATTRID_SUMMATION_FORMATTING, ZCL_DATA_TYPE_BITMAP8, ACCESS_CONTROL_READ,
	 (u8 *)&metering_format},
	{ZCL_ATTRID_METERING_DEVICE_TYPE, ZCL_DATA_TYPE_BITMAP8, ACCESS_CONTROL_READ,
	 (u8 *)&metering_device_type},
	{ZCL_ATTRID_GLOBAL_CLUSTER_REVISION, ZCL_DATA_TYPE_UINT16, ACCESS_CONTROL_READ,
	 (u8 *)&cluster_rev},
};

static u16 in_clusters[] = {
	ZCL_CLUSTER_GEN_BASIC,
	ZCL_CLUSTER_GEN_IDENTIFY,
	ZCL_CLUSTER_GEN_GROUPS,
	ZCL_CLUSTER_GEN_ON_OFF,
	ZCL_CLUSTER_MS_ELECTRICAL_MEASUREMENT,
	ZCL_CLUSTER_SE_METERING,
};

#if defined(CONFIG_ZIGBEE_OTA)
static u16 out_clusters[] = {
	ZCL_CLUSTER_OTA,
};
#endif

static af_simple_descriptor_t simple_desc = {
	.app_profile_id = APP_HA_PROFILE_ID,
	.app_dev_id = 0x0051U, /* Smart Plug */
	.endpoint = APP_ENDPOINT,
	.app_dev_ver = 1U,
	.reserved = 0U,
	.app_in_cluster_count = ARRAY_SIZE(in_clusters),
	.app_in_cluster_lst = in_clusters,
#if defined(CONFIG_ZIGBEE_OTA)
	.app_out_cluster_count = ARRAY_SIZE(out_clusters),
	.app_out_cluster_lst = out_clusters,
#else
	.app_out_cluster_count = 0U,
	.app_out_cluster_lst = NULL,
#endif
};

#if defined(CONFIG_ZIGBEE_OTA)
/*
 * Identity the server matches the OTA file against. The device's
 * CMakeLists.txt defines APP_OTA_FILE_VERSION (from its version.txt) and
 * APP_OTA_IMAGE_TYPE, the manufacturer code is CONFIG_ZIGBEE_MANUFACTURER_CODE;
 * they must equal the fields of the .ota file header. totalImageSize is unused by the client.
 */
static ota_preamble_t ota_preamble = {
	.fileVer = APP_OTA_FILE_VERSION,
	.totalImageSize = 0U,
	.imageType = APP_OTA_IMAGE_TYPE,
	.manufacturerCode = CONFIG_ZIGBEE_MANUFACTURER_CODE,
};

static void ota_event(u8 evt, u8 status)
{
	if (evt == OTA_EVT_COMPLETE && status == ZCL_STA_SUCCESS) {
		/* MCUboot verifies and starts the image in slot 1 */
		ota_mcuReboot();
	}
}

static ota_callBack_t ota_cb = {
	.processMsgCbFunc = ota_event,
};
#endif

static void relay_set(bool on)
{
	onoff_state = on;
	gpio_pin_set_dt(&relay, on ? 1 : 0);
	app_led_set(on);
	zcl_setAttrVal(APP_ENDPOINT, ZCL_CLUSTER_GEN_ON_OFF, ZCL_ATTRID_ONOFF, (u8 *)&onoff_state);
}

static status_t onoff_cb(zclIncomingAddrInfo_t *addr_info, u8 cmd_id, void *payload)
{
	ARG_UNUSED(addr_info);
	ARG_UNUSED(payload);

	switch (cmd_id) {
	case ZCL_CMD_ONOFF_ON:
		relay_set(true);
		break;
	case ZCL_CMD_ONOFF_OFF:
		relay_set(false);
		break;
	case ZCL_CMD_ONOFF_TOGGLE:
		relay_set(!onoff_state);
		break;
	default:
		return ZCL_STA_UNSUP_CLUSTER_COMMAND;
	}

	return ZCL_STA_SUCCESS;
}

static void button_cb(uint8_t idx, bool pressed)
{
	if (idx == 0U && pressed) {
		relay_set(!onoff_state);
	}
}

/* sensor_value in whole units -> thousandths */
static int64_t to_milli(const struct sensor_value *v)
{
	return (int64_t)v->val1 * 1000 + v->val2 / 1000;
}

static void publish_u16(u16 cluster, u16 attr, u16 *cur, int64_t value)
{
	u16 v = (u16)CLAMP(value, 0, UINT16_MAX);

	if (v != *cur) {
		*cur = v;
		zcl_setAttrVal(APP_ENDPOINT, cluster, attr, (u8 *)cur);
	}
}

/*
 * Runs on the Zigbee thread, where attributes must be set. The stack arms its
 * report timers only from report_handler(), which nothing else calls.
 */
static int measure_tick(void *arg)
{
	struct sensor_value v;
	int64_t power_w;

	ARG_UNUSED(arg);

	if (!device_is_ready(meter) || sensor_sample_fetch(meter) != 0) {
		report_handler();
		return MEASURE_PERIOD_MS;
	}

	if (sensor_channel_get(meter, SENSOR_CHAN_VOLTAGE, &v) == 0) {
		/* mV -> 0.01 V */
		publish_u16(ZCL_CLUSTER_MS_ELECTRICAL_MEASUREMENT, ZCL_ATTRID_RMS_VOLTAGE,
			    &rms_voltage, to_milli(&v) / 10);
	}
	if (sensor_channel_get(meter, SENSOR_CHAN_CURRENT, &v) == 0) {
		publish_u16(ZCL_CLUSTER_MS_ELECTRICAL_MEASUREMENT, ZCL_ATTRID_RMS_CURRENT,
			    &rms_current, to_milli(&v));
	}
	if (sensor_channel_get(meter, SENSOR_CHAN_POWER, &v) == 0) {
		power_w = (to_milli(&v) + 500) / 1000;
		s16 p = (s16)CLAMP(power_w, INT16_MIN, INT16_MAX);

		if (p != active_power) {
			active_power = p;
			zcl_setAttrVal(APP_ENDPOINT, ZCL_CLUSTER_MS_ELECTRICAL_MEASUREMENT,
				       ZCL_ATTRID_ACTIVE_POWER, (u8 *)&active_power);
		}
	}
	if (sensor_channel_get(meter, IOTDRV_SENSOR_CHAN_ENERGY, &v) == 0) {
		/* kWh with a microkilowatt-hour fraction -> Wh */
		u64 wh = (u64)v.val1 * 1000U + (u64)v.val2 / 1000U;

		if (wh != summation_wh) {
			summation_wh = wh;
			zcl_setAttrVal(APP_ENDPOINT, ZCL_CLUSTER_SE_METERING,
				       ZCL_ATTRID_CURRENT_SUMMATION_DELIVERD,
				       (u8 *)&summation_wh);
		}
	}

	report_handler();
	return MEASURE_PERIOD_MS;
}

/*
 * Reporting. The table the coordinator configured is restored from NV after a
 * reset (the stack never calls zcl_reportingTabInit() itself), entries that no
 * longer match an attribute of this build are dropped, and the defaults below
 * fill what is missing, so a configuration from the coordinator wins. A report
 * goes out only when the coordinator has bound the cluster: the stack looks
 * the binding up and sends to it.
 */
static void reporting_default(u16 cluster, u16 attr, u8 type, u16 min_s, u16 max_s,
			      u32 change)
{
	u8 chg[REPORTABLE_CHANGE_MAX_ANALOG_SIZE] = {0};
	zclCfgReportRec_t rec = {
		.direction = ZCL_SEND_ATTR_REPORTS,
		.attrID = attr,
		.dataType = type,
		.minReportInt = min_s,
		.maxReportInt = max_s,
		.timeoutPeriod = 0U,
		.reportableChange = chg,
	};

	if (zcl_reportCfgInfoEntryFind(APP_ENDPOINT, cluster, attr) != NULL) {
		return;
	}
	/* little-endian, as the stack compares it */
	chg[0] = change;
	chg[1] = change >> 8;
	chg[2] = change >> 16;
	chg[3] = change >> 24;
	(void)zcl_configureReporting(APP_ENDPOINT, APP_HA_PROFILE_ID, cluster, &rec);
}

static void reporting_restore(void)
{
	u8 used = 0U;

	zcl_reportingTabInit();

	for (u8 i = 0; i < ZCL_REPORTING_TABLE_NUM; i++) {
		reportCfgInfo_t *e = &reportingTab.reportCfgInfo[i];

		if (!e->used) {
			continue;
		}
		if (zcl_findAttribute(e->endPoint, e->clusterID, e->attrID) == NULL) {
			zcl_reportCfgInfoEntryClear(e);
			continue;
		}
		used++;
	}
	reportingTab.reportNum = used;
}

static void reporting_defaults(void)
{
	const u16 em = ZCL_CLUSTER_MS_ELECTRICAL_MEASUREMENT;

	reporting_default(ZCL_CLUSTER_GEN_ON_OFF, ZCL_ATTRID_ONOFF, ZCL_DATA_TYPE_BOOLEAN, 0U,
			  300U, 0U);
	/* 1 V, 50 mA, 2 W */
	reporting_default(em, ZCL_ATTRID_RMS_VOLTAGE, ZCL_DATA_TYPE_UINT16, 10U, 300U, 100U);
	reporting_default(em, ZCL_ATTRID_RMS_CURRENT, ZCL_DATA_TYPE_UINT16, 10U, 300U, 50U);
	reporting_default(em, ZCL_ATTRID_ACTIVE_POWER, ZCL_DATA_TYPE_INT16, 10U, 300U, 2U);
	/*
	 * The stack's change check has no 48-bit case, so the energy goes out
	 * at the maximum interval only (the change value is ignored).
	 */
	reporting_default(ZCL_CLUSTER_SE_METERING, ZCL_ATTRID_CURRENT_SUMMATION_DELIVERD,
			  ZCL_DATA_TYPE_UINT48, 60U, 600U, 0U);
}

static zcl_specClusterInfo_t clusters[APP_COMMON_CLUSTER_NUM + 4U];

void app_device_register(void)
{
	if (gpio_is_ready_dt(&relay)) {
		(void)gpio_pin_configure_dt(&relay, GPIO_OUTPUT_INACTIVE);
	}

	app_common_clusters(clusters);

	clusters[2] = (zcl_specClusterInfo_t){
		.clusterId = ZCL_CLUSTER_GEN_GROUPS,
		.attrNum = ARRAY_SIZE(group_attr_tbl),
		.attrTbl = group_attr_tbl,
		.clusterRegisterFunc = zcl_group_register,
	};
	clusters[3] = (zcl_specClusterInfo_t){
		.clusterId = ZCL_CLUSTER_GEN_ON_OFF,
		.attrNum = ARRAY_SIZE(onoff_attr_tbl),
		.attrTbl = onoff_attr_tbl,
		.clusterRegisterFunc = zcl_onOff_register,
		.clusterAppCb = onoff_cb,
	};
	clusters[4] = (zcl_specClusterInfo_t){
		.clusterId = ZCL_CLUSTER_MS_ELECTRICAL_MEASUREMENT,
		.attrNum = ARRAY_SIZE(em_attr_tbl),
		.attrTbl = em_attr_tbl,
		.clusterRegisterFunc = zcl_electricalMeasure_register,
	};
	clusters[5] = (zcl_specClusterInfo_t){
		.clusterId = ZCL_CLUSTER_SE_METERING,
		.attrNum = ARRAY_SIZE(metering_attr_tbl),
		.attrTbl = metering_attr_tbl,
		.clusterRegisterFunc = zcl_metering_register,
	};

	app_endpoint_register(&simple_desc, clusters, ARRAY_SIZE(clusters), NULL);
	reporting_restore();
	reporting_defaults();
	app_buttons_init(button_cb);

#if defined(CONFIG_ZIGBEE_OTA)
	ota_init(OTA_TYPE_CLIENT, &simple_desc, &ota_preamble, &ota_cb);
	ota_queryStart(OTA_PERIODIC_QUERY_INTERVAL);
#endif

	(void)ev_timer_taskPost(measure_tick, NULL, MEASURE_PERIOD_MS);
}
