/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Battery powered temperature and humidity sensor, a sleepy end device:
 * Power Configuration (battery), Temperature Measurement and Relative Humidity
 * clusters on one endpoint, plus, when the device has a panel (the devicetree
 * alias "climate-display"), Thermostat User Interface Configuration that
 * controls it. The attribute numbers match the z03mmc firmware and its
 * zigbee2mqtt converters.
 *
 * The board gives:
 *   alias ambient-sensor      a temperature/humidity sensor on the Zephyr sensor API
 *   alias ambient-sensor-alt  optionally a second candidate; the first one that
 *                             probes at boot is used (board revisions with different chips)
 *   zephyr,user io-channels   the ADC channel of the battery voltage
 *   alias climate-display     optionally the panel (iot_drivers/climate_display.h)
 *   alias sw0, led0           optionally the button and the LED (see app_common.h)
 *
 * Units on the air:
 *   temperature    0.01 degC            humidity  0.01 %
 *   battery        0.1 V and 0.5 %      calibration offsets in the same units
 *
 * Deep sleep: with CONFIG_ZIGBEE_SLEEPY_DEEP every wake is a reset, so the
 * firmware measures once per wake and reports straight away, instead of relying
 * on the stack's reporting timers (they would not survive the sleep). The wake
 * interval is CONFIG_ZIGBEE_ED_POLL_INTERVAL_MS. The settings live in NV.
 */

/* The Zephyr headers go first: the Zigbee headers define macros such as abs() */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>

#include <string.h>

#include "app_common.h"
#include "app_reporting.h"
#include "drv_nv.h"

#include "os/ev_timer.h"
#include "zb_common.h"
#include "zbapi/zb_api.h"

#include "zcl/general/zcl_powerCfg.h"
#include "zcl/measument_sensing/zcl_relative_humidity.h"
#include "zcl/measument_sensing/zcl_temperature_measurement.h"
#if defined(CONFIG_ZIGBEE_OTA)
#include "ota/ota.h"
#endif

#if DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(climate_display))
#define HAS_DISPLAY 1
#include <iot_drivers/climate_display.h>
#include "zcl/hvac/zcl_thermostat_ui_cfg.h"
#endif

/* Between measurements while the device is awake anyway (not joined, or not deep sleeping) */
#define MEASURE_PERIOD_MS CONFIG_ZIGBEE_ED_POLL_INTERVAL_MS
/* The first measurement after a boot; below the stack's minimum sleep, so it is not slept through */
#define FIRST_MEASURE_MS  10

/* Alkaline or lithium coin cell: 2.2 V empty, 3.0 V full (as in the z03mmc firmware) */
#define BATTERY_MV_EMPTY 2200
#define BATTERY_MV_FULL  3000

#define TEMP_MIN_C100 (-4000)
#define TEMP_MAX_C100 12500
#define HUM_MAX_C100  10000

#define ATTR_CALIBRATION        0x0010U
#define UI_ATTR_SMILEY_ON       0x0010U
#define UI_ATTR_DISPLAY_ON      0x0011U
#define UI_ATTR_TEMP_COMF_MIN   0x0102U
#define UI_ATTR_TEMP_COMF_MAX   0x0103U
#define UI_ATTR_HUMID_COMF_MIN  0x0104U
#define UI_ATTR_HUMID_COMF_MAX  0x0105U

static const struct device *const sensor_candidates[] = {
	DEVICE_DT_GET(DT_ALIAS(ambient_sensor)),
#if DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(ambient_sensor_alt))
	DEVICE_DT_GET(DT_ALIAS(ambient_sensor_alt)),
#endif
};
static const struct device *sensor_dev;

static const struct adc_dt_spec battery_adc = ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);

#ifdef HAS_DISPLAY
static const struct device *const display = DEVICE_DT_GET(DT_ALIAS(climate_display));
#endif

static const u16 cluster_rev = ZCL_ATTR_GLOBAL_CLUSTER_REVISION_DEFAULT;

/* Power Configuration */
static u8 battery_voltage = 0xFFU;    /* 0.1 V, 0xFF: unknown */
static u8 battery_percentage = 0xFFU; /* 0.5 %, 0xFF: unknown */

/* Temperature Measurement and Relative Humidity */
static s16 temperature = (s16)0x8000;
static const s16 temperature_min = TEMP_MIN_C100;
static const s16 temperature_max = TEMP_MAX_C100;
static s16 temperature_cal;
static u16 humidity = 0xFFFFU;
static const u16 humidity_min;
static const u16 humidity_max = HUM_MAX_C100;
static s16 humidity_cal;

#ifdef HAS_DISPLAY
/* Thermostat User Interface Configuration */
static struct {
	u8 display_mode; /* 0 degC, 1 degF */
	u8 keypad_lockout;
	u8 smiley_on;
	u8 display_on;
	s16 temp_comf_min; /* 0.01 degC */
	s16 temp_comf_max;
	u16 humid_comf_min; /* 0.01 % */
	u16 humid_comf_max;
} ui = {
	.smiley_on = 1U,
	.display_on = 1U,
	.temp_comf_min = 2100,
	.temp_comf_max = 2600,
	.humid_comf_min = 3000,
	.humid_comf_max = 6000,
};
#endif

#define R   ACCESS_CONTROL_READ
#define RW  (ACCESS_CONTROL_READ | ACCESS_CONTROL_WRITE)
#define RR  (ACCESS_CONTROL_READ | ACCESS_CONTROL_REPORTABLE)

#define ATTR(id, type, access, var) {(id), ZCL_DATA_TYPE_##type, (access), (u8 *)&(var)}
#define REV ATTR(ZCL_ATTRID_GLOBAL_CLUSTER_REVISION, UINT16, R, cluster_rev)

static const zclAttrInfo_t power_attr_tbl[] = {
	ATTR(ZCL_ATTRID_BATTERY_VOLTAGE, UINT8, RR, battery_voltage),
	ATTR(ZCL_ATTRID_BATTERY_PERCENTAGE_REMAINING, UINT8, RR, battery_percentage),
	REV,
};

static const zclAttrInfo_t temperature_attr_tbl[] = {
	ATTR(ZCL_TEMPERATURE_MEASUREMENT_ATTRID_MEASUREDVALUE, INT16, RR, temperature),
	ATTR(ZCL_TEMPERATURE_MEASUREMENT_ATTRID_MINMEASUREDVALUE, INT16, R, temperature_min),
	ATTR(ZCL_TEMPERATURE_MEASUREMENT_ATTRID_MAXMEASUREDVALUE, INT16, R, temperature_max),
	ATTR(ATTR_CALIBRATION, INT16, RW, temperature_cal),
	REV,
};

static const zclAttrInfo_t humidity_attr_tbl[] = {
	ATTR(ZCL_RELATIVE_HUMIDITY_ATTRID_MEASUREDVALUE, UINT16, RR, humidity),
	ATTR(ZCL_RELATIVE_HUMIDITY_ATTRID_MINMEASUREDVALUE, UINT16, R, humidity_min),
	ATTR(ZCL_RELATIVE_HUMIDITY_ATTRID_MAXMEASUREDVALUE, UINT16, R, humidity_max),
	ATTR(ATTR_CALIBRATION, INT16, RW, humidity_cal),
	REV,
};

#ifdef HAS_DISPLAY
static const zclAttrInfo_t ui_attr_tbl[] = {
	ATTR(ZCL_ATTRID_HVAC_UI_CFG_TEMPERATURE_DISPLAY_MODE, ENUM8, RW, ui.display_mode),
	ATTR(ZCL_ATTRID_HVAC_UI_CFG_KEYPAD_LOCKOUT, ENUM8, RW, ui.keypad_lockout),
	ATTR(UI_ATTR_SMILEY_ON, BOOLEAN, RW, ui.smiley_on),
	ATTR(UI_ATTR_DISPLAY_ON, BOOLEAN, RW, ui.display_on),
	ATTR(UI_ATTR_TEMP_COMF_MIN, INT16, RW, ui.temp_comf_min),
	ATTR(UI_ATTR_TEMP_COMF_MAX, INT16, RW, ui.temp_comf_max),
	ATTR(UI_ATTR_HUMID_COMF_MIN, UINT16, RW, ui.humid_comf_min),
	ATTR(UI_ATTR_HUMID_COMF_MAX, UINT16, RW, ui.humid_comf_max),
	REV,
};
#endif

static u16 in_clusters[] = {
	ZCL_CLUSTER_GEN_BASIC,
	ZCL_CLUSTER_GEN_IDENTIFY,
	ZCL_CLUSTER_GEN_POWER_CFG,
	ZCL_CLUSTER_MS_TEMPERATURE_MEASUREMENT,
	ZCL_CLUSTER_MS_RELATIVE_HUMIDITY,
#ifdef HAS_DISPLAY
	ZCL_CLUSTER_HVAC_USER_INTERFACE_CONFIG,
#endif
};

#if defined(CONFIG_ZIGBEE_OTA)
static u16 out_clusters[] = {
	ZCL_CLUSTER_OTA,
};
#endif

static af_simple_descriptor_t simple_desc = {
	.app_profile_id = APP_HA_PROFILE_ID,
	.app_dev_id = 0x0302U, /* Temperature Sensor */
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
/* See plug.c: the OTA identity the server matches the file against */
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

/* ------------------------------------------------------------------ */
/* Settings in NV                                                      */
/* ------------------------------------------------------------------ */

/*
 * A deep-sleeping device starts from scratch on every wake, so what the
 * coordinator wrote (calibration, the panel's settings) is read back from NV
 * before the clusters register, and written there at once when it changes:
 * a timer would not survive the sleep.
 */
#define SETTINGS_ITEM    0x72U
#define SETTINGS_VERSION 1U

struct settings {
	u8 version;
	s16 temperature_cal;
	s16 humidity_cal;
#ifdef HAS_DISPLAY
	u8 display_mode, keypad_lockout, smiley_on, display_on;
	s16 temp_comf_min, temp_comf_max;
	u16 humid_comf_min, humid_comf_max;
#endif
	u8 crc;
};

static u8 crc8(const u8 *p, size_t n)
{
	u8 crc = 0U;

	while (n-- > 0U) {
		crc ^= *p++;
		for (int i = 0; i < 8; i++) {
			crc = (crc & 0x80U) ? (u8)((crc << 1) ^ 0x07U) : (u8)(crc << 1);
		}
	}
	return crc;
}

static void settings_save(void)
{
	struct settings c;

	memset(&c, 0, sizeof(c));
	c.version = SETTINGS_VERSION;
	c.temperature_cal = temperature_cal;
	c.humidity_cal = humidity_cal;
#ifdef HAS_DISPLAY
	c.display_mode = ui.display_mode;
	c.keypad_lockout = ui.keypad_lockout;
	c.smiley_on = ui.smiley_on;
	c.display_on = ui.display_on;
	c.temp_comf_min = ui.temp_comf_min;
	c.temp_comf_max = ui.temp_comf_max;
	c.humid_comf_min = ui.humid_comf_min;
	c.humid_comf_max = ui.humid_comf_max;
#endif
	c.crc = crc8((const u8 *)&c, offsetof(struct settings, crc));

	/* NVS skips a write of what it already holds */
	(void)nv_flashWriteNew(1, NV_MODULE_APP, SETTINGS_ITEM, sizeof(c), (u8 *)&c);
}

static void settings_load(void)
{
	struct settings c;

	if (nv_flashReadNew(1, NV_MODULE_APP, SETTINGS_ITEM, sizeof(c), (u8 *)&c) != NV_SUCC ||
	    c.version != SETTINGS_VERSION ||
	    c.crc != crc8((const u8 *)&c, offsetof(struct settings, crc))) {
		return;
	}

	temperature_cal = c.temperature_cal;
	humidity_cal = c.humidity_cal;
#ifdef HAS_DISPLAY
	ui.display_mode = c.display_mode;
	ui.keypad_lockout = c.keypad_lockout;
	ui.smiley_on = c.smiley_on;
	ui.display_on = c.display_on;
	ui.temp_comf_min = c.temp_comf_min;
	ui.temp_comf_max = c.temp_comf_max;
	ui.humid_comf_min = c.humid_comf_min;
	ui.humid_comf_max = c.humid_comf_max;
#endif
}

/* ------------------------------------------------------------------ */
/* Measurement                                                         */
/* ------------------------------------------------------------------ */

static const struct device *sensor_pick(void)
{
	if (sensor_dev == NULL) {
		for (size_t i = 0; i < ARRAY_SIZE(sensor_candidates); i++) {
			if (device_is_ready(sensor_candidates[i])) {
				sensor_dev = sensor_candidates[i];
				break;
			}
		}
	}

	return sensor_dev;
}

/* sensor_value in whole units -> hundredths */
static int32_t to_centi(const struct sensor_value *v)
{
	return v->val1 * 100 + v->val2 / 10000;
}

/* Fetch the sensor and apply the calibration; false when it does not answer */
static bool read_ambient(s16 *t, u16 *h)
{
	const struct device *dev = sensor_pick();
	struct sensor_value tv, hv;

	if (dev == NULL || sensor_sample_fetch(dev) != 0 ||
	    sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &tv) != 0 ||
	    sensor_channel_get(dev, SENSOR_CHAN_HUMIDITY, &hv) != 0) {
		return false;
	}

	*t = (s16)CLAMP(to_centi(&tv) + temperature_cal, TEMP_MIN_C100, TEMP_MAX_C100);
	*h = (u16)CLAMP(to_centi(&hv) + humidity_cal, 0, HUM_MAX_C100);
	return true;
}

/* The battery voltage in millivolts, or 0 */
static int read_battery_mv(void)
{
	int16_t mv = 0;
	struct adc_sequence seq = {
		.buffer = &mv,
		.buffer_size = sizeof(mv),
	};

	if (!adc_is_ready_dt(&battery_adc) || adc_channel_setup_dt(&battery_adc) != 0 ||
	    adc_sequence_init_dt(&battery_adc, &seq) != 0 || adc_read_dt(&battery_adc, &seq) != 0) {
		return 0;
	}

	return mv;
}

static void battery_update(void)
{
	int mv = read_battery_mv();

	if (mv <= 0) {
		return;
	}

	battery_voltage = (u8)CLAMP((mv + 50) / 100, 0, 0xFE);
	/* 0.5 % steps: 0..200 */
	battery_percentage = (u8)CLAMP((mv - BATTERY_MV_EMPTY) * 200 / (BATTERY_MV_FULL - BATTERY_MV_EMPTY),
				       0, 200);
	zcl_setAttrVal(APP_ENDPOINT, ZCL_CLUSTER_GEN_POWER_CFG, ZCL_ATTRID_BATTERY_VOLTAGE,
		       &battery_voltage);
	zcl_setAttrVal(APP_ENDPOINT, ZCL_CLUSTER_GEN_POWER_CFG,
		       ZCL_ATTRID_BATTERY_PERCENTAGE_REMAINING, &battery_percentage);
}

#ifdef HAS_DISPLAY
static void display_update(void)
{
	if (!device_is_ready(display)) {
		return;
	}

	iotdrv_climate_display_clear(display);

	if (ui.display_on && temperature != (s16)0x8000) {
		bool fahrenheit = (ui.display_mode == ZCL_UI_CFG_DISPLAY_MODE_FAHRENHEIT);
		int32_t t10;
		int pct = battery_percentage / 2;

		if (fahrenheit) {
			t10 = ((int32_t)temperature * 9 / 5 + 3200) / 10;
		} else {
			t10 = temperature / 10;
		}
		iotdrv_climate_display_big_number_x10(display, (int16_t)t10);
		iotdrv_climate_display_unit(display, fahrenheit ? IOTDRV_CLIMATE_UNIT_F
								 : IOTDRV_CLIMATE_UNIT_C);
		if (humidity != 0xFFFFU) {
			iotdrv_climate_display_small_number(display, (int16_t)(humidity / 100), true);
		}
		if (ui.smiley_on && humidity != 0xFFFFU) {
			bool comfort = temperature >= ui.temp_comf_min &&
				       temperature <= ui.temp_comf_max &&
				       humidity >= ui.humid_comf_min && humidity <= ui.humid_comf_max;

			iotdrv_climate_display_smiley(display, comfort ? IOTDRV_CLIMATE_SMILEY_HAPPY
								       : IOTDRV_CLIMATE_SMILEY_SAD);
		}
		/* The battery symbol shows only when it runs low */
		if (battery_percentage != 0xFFU && pct <= 20) {
			iotdrv_climate_display_battery(display, (int8_t)pct);
		}
	}

	(void)iotdrv_climate_display_flush(display);
}
#else
static inline void display_update(void)
{
}
#endif

/* Measure and publish the attributes: the part that also runs while not joined */
static void measure(void)
{
	s16 t;
	u16 h;

	battery_update();

	if (read_ambient(&t, &h)) {
		temperature = t;
		humidity = h;
		zcl_setAttrVal(APP_ENDPOINT, ZCL_CLUSTER_MS_TEMPERATURE_MEASUREMENT,
			       ZCL_TEMPERATURE_MEASUREMENT_ATTRID_MEASUREDVALUE, (u8 *)&temperature);
		zcl_setAttrVal(APP_ENDPOINT, ZCL_CLUSTER_MS_RELATIVE_HUMIDITY,
			       ZCL_RELATIVE_HUMIDITY_ATTRID_MEASUREDVALUE, (u8 *)&humidity);
	}

	display_update();
}

/* ------------------------------------------------------------------ */
/* Reporting                                                           */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_ZIGBEE_SLEEPY_DEEP)
/*
 * Send what the coordinator bound the cluster for, at once. After a deep sleep
 * the stack's reporting table has lost its timers and previous values, so it
 * would hold the report back until a minimum interval the device never lives to see.
 */
static void report_now(u16 cluster, const zclReport_t *attrs, u8 n)
{
	struct {
		u8 numAttr;
		zclReport_t attr[2];
	} report = {.numAttr = n};
	epInfo_t dst;

	if (n > ARRAY_SIZE(report.attr) || !zb_bindingTblSearched(cluster, APP_ENDPOINT)) {
		return;
	}

	memcpy(report.attr, attrs, n * sizeof(attrs[0]));
	memset(&dst, 0, sizeof(dst));
	dst.dstAddrMode = APS_DSTADDR_EP_NOTPRESETNT;
	dst.profileId = APP_HA_PROFILE_ID;
	(void)zcl_sendReportAttrsCmd(APP_ENDPOINT, &dst, TRUE, ZCL_FRAME_SERVER_CLIENT_DIR, cluster,
				     (zclReportCmd_t *)&report);
}

static void report_all(void)
{
	zclReport_t t = {ZCL_TEMPERATURE_MEASUREMENT_ATTRID_MEASUREDVALUE, ZCL_DATA_TYPE_INT16,
			 (u8 *)&temperature};
	zclReport_t h = {ZCL_RELATIVE_HUMIDITY_ATTRID_MEASUREDVALUE, ZCL_DATA_TYPE_UINT16,
			 (u8 *)&humidity};
	zclReport_t b[2] = {
		{ZCL_ATTRID_BATTERY_VOLTAGE, ZCL_DATA_TYPE_UINT8, &battery_voltage},
		{ZCL_ATTRID_BATTERY_PERCENTAGE_REMAINING, ZCL_DATA_TYPE_UINT8, &battery_percentage},
	};

	if (temperature != (s16)0x8000) {
		report_now(ZCL_CLUSTER_MS_TEMPERATURE_MEASUREMENT, &t, 1);
	}
	if (humidity != 0xFFFFU) {
		report_now(ZCL_CLUSTER_MS_RELATIVE_HUMIDITY, &h, 1);
	}
	if (battery_voltage != 0xFFU) {
		report_now(ZCL_CLUSTER_GEN_POWER_CFG, b, 2);
	}
}
#endif /* CONFIG_ZIGBEE_SLEEPY_DEEP */

/* Defaults for a coordinator that never configures reporting; its configuration wins. */
static void reporting_defaults(void)
{
	/* 0.5 degC, 2 %; battery every 6 h at the latest */
	app_reporting_default(APP_ENDPOINT, ZCL_CLUSTER_MS_TEMPERATURE_MEASUREMENT,
			      ZCL_TEMPERATURE_MEASUREMENT_ATTRID_MEASUREDVALUE, ZCL_DATA_TYPE_INT16,
			      10U, 3600U, 50U);
	app_reporting_default(APP_ENDPOINT, ZCL_CLUSTER_MS_RELATIVE_HUMIDITY,
			      ZCL_RELATIVE_HUMIDITY_ATTRID_MEASUREDVALUE, ZCL_DATA_TYPE_UINT16, 10U,
			      3600U, 200U);
	app_reporting_default(APP_ENDPOINT, ZCL_CLUSTER_GEN_POWER_CFG, ZCL_ATTRID_BATTERY_VOLTAGE,
			      ZCL_DATA_TYPE_UINT8, 3600U, 21600U, 1U);
	app_reporting_default(APP_ENDPOINT, ZCL_CLUSTER_GEN_POWER_CFG,
			      ZCL_ATTRID_BATTERY_PERCENTAGE_REMAINING, ZCL_DATA_TYPE_UINT8, 3600U,
			      21600U, 2U);
}

/*
 * Runs on the Zigbee thread, where attributes must be set. Joined and deep
 * sleeping: one measurement and report per wake, then no timer is left and the
 * stack puts the device to sleep. Otherwise it repeats.
 */
static int measure_tick(void *arg)
{
	ARG_UNUSED(arg);

	measure();

#if defined(CONFIG_ZIGBEE_SLEEPY_DEEP)
	if (zb_isDeviceJoinedNwk()) {
		report_all();
		return -1;
	}
#else
	/* Awake all the time: the stack's own reporting table does the work */
	report_handler();
#endif

	return MEASURE_PERIOD_MS;
}

/* ------------------------------------------------------------------ */
/* Cluster callbacks                                                   */
/* ------------------------------------------------------------------ */

/* Runs after every foundation command: a write from the coordinator is final here */
static void zcl_hook(zclIncoming_t *in)
{
	if (in->hdr.cmd != ZCL_CMD_WRITE && in->hdr.cmd != ZCL_CMD_WRITE_UNDIVIDED &&
	    in->hdr.cmd != ZCL_CMD_WRITE_NO_RSP) {
		return;
	}

	temperature_cal = CLAMP(temperature_cal, -TEMP_MAX_C100, TEMP_MAX_C100);
	humidity_cal = CLAMP(humidity_cal, -HUM_MAX_C100, HUM_MAX_C100);
	settings_save();
	/* Show the effect now, not after the next wake */
	measure();
}

static zcl_specClusterInfo_t clusters[APP_COMMON_CLUSTER_NUM + 3U
#ifdef HAS_DISPLAY
				      + 1U
#endif
];

void app_device_register(void)
{
	size_t n = APP_COMMON_CLUSTER_NUM;

	settings_load();

	app_common_clusters(clusters);

	clusters[n++] = (zcl_specClusterInfo_t){
		.clusterId = ZCL_CLUSTER_GEN_POWER_CFG,
		.attrNum = ARRAY_SIZE(power_attr_tbl),
		.attrTbl = power_attr_tbl,
		.clusterRegisterFunc = zcl_powerCfg_register,
	};
	clusters[n++] = (zcl_specClusterInfo_t){
		.clusterId = ZCL_CLUSTER_MS_TEMPERATURE_MEASUREMENT,
		.attrNum = ARRAY_SIZE(temperature_attr_tbl),
		.attrTbl = temperature_attr_tbl,
		.clusterRegisterFunc = zcl_temperature_measurement_register,
	};
	clusters[n++] = (zcl_specClusterInfo_t){
		.clusterId = ZCL_CLUSTER_MS_RELATIVE_HUMIDITY,
		.attrNum = ARRAY_SIZE(humidity_attr_tbl),
		.attrTbl = humidity_attr_tbl,
		.clusterRegisterFunc = zcl_relative_humidity_register,
	};
#ifdef HAS_DISPLAY
	clusters[n++] = (zcl_specClusterInfo_t){
		.clusterId = ZCL_CLUSTER_HVAC_USER_INTERFACE_CONFIG,
		.attrNum = ARRAY_SIZE(ui_attr_tbl),
		.attrTbl = ui_attr_tbl,
		.clusterRegisterFunc = zcl_thermostat_ui_cfg_register,
	};
#endif

	app_endpoint_register(&simple_desc, clusters, n, zcl_hook);
	app_reporting_restore();
	reporting_defaults();
	app_buttons_init(NULL);

#if defined(CONFIG_ZIGBEE_OTA)
	ota_init(OTA_TYPE_CLIENT, &simple_desc, &ota_preamble, &ota_cb);
#if !defined(CONFIG_ZIGBEE_SLEEPY_DEEP)
	/* A deep-sleeping device starts over on every wake: the server's Image Notify drives it */
	ota_queryStart(OTA_PERIODIC_QUERY_INTERVAL);
#endif
#endif

	(void)ev_timer_taskPost(measure_tick, NULL, FIRST_MEASURE_MS);
}
