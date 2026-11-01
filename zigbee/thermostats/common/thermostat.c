/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Floor-heating thermostat router. The thermostat itself is a Tuya MCU with
 * a display and a relay; this firmware is the Zigbee side. The model (one of
 * 13, r01..r0D of tuya_thermostat_zrd) is found from the MCU's product id, the
 * datapoint tables and codecs are lib/tuya_thermostat of zephyr-iot-drivers.
 *
 * Datapoints from the MCU arrive on the system work queue; they are queued and
 * applied on the Zigbee thread, where attributes may be set. Writes from the
 * coordinator go the other way through iotdrv_tt_to_dp().
 *
 * Endpoint 1 (thermostat 0x0301): Basic, Identify, Groups, Level Control (A),
 * Thermostat with the manufacturer attributes 0xF000..0xF010, User Interface
 * Configuration, Fan Control, Relative Humidity; out: Time, OTA. Endpoint 2:
 * Level Control (B). Scenes and the manufacturer name attribute 0xF00E of the
 * stock firmware are not implemented.
 */

/* Zephyr headers go first, see plug.c */
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <iot_drivers/tuya_mcu.h>
#include <iot_drivers/tuya_thermostat.h>
#include <iot_drivers/tuya_uart.h>

#include "app_common.h"
#include "app_reporting.h"

#include "os/ev_timer.h"
#include "zb_common.h"
#include "zbapi/zb_api.h"

#include "zcl/general/zcl_group.h"
#include "zcl/general/zcl_level.h"
#include "zcl/general/zcl_time.h"
#include "zcl/hvac/zcl_fan_control.h"
#include "zcl/hvac/zcl_thermostat.h"
#include "zcl/hvac/zcl_thermostat_ui_cfg.h"
#include "zcl/measument_sensing/zcl_relative_humidity.h"
#if defined(CONFIG_ZIGBEE_OTA)
#include "ota/ota.h"
#endif

#define APP_ENDPOINT2 0x02U
#define DP_HEADER     4U /* id, type, length (2) */

#define TICK_MS          1000
#define TIME_RETRY_S     10U
#define TIME_REFRESH_S   3600U
/* Seconds from 1970 to the Zigbee epoch, 2000-01-01 */
#define ZIGBEE_EPOCH     946684800U
#define TIME_INVALID     0xFFFFFFFFU

/* manufacturer attributes of the stock firmware */
#define TH_ATTR_SENSOR_USED      0xF000U
#define TH_ATTR_FROST_PROTECT    0xF001U
#define TH_ATTR_HEAT_PROTECT     0xF002U
#define TH_ATTR_ECO_MODE         0xF003U
#define TH_ATTR_ECO_HEAT_TEMP    0xF004U
#define TH_ATTR_FROST_ONOFF      0xF005U
#define TH_ATTR_SETTINGS_RESET   0xF006U
#define TH_ATTR_SCHEDULE_MODE    0xF007U
#define TH_ATTR_SOUND            0xF008U
#define TH_ATTR_LEVEL            0xF009U
#define TH_ATTR_INVERSION        0xF00AU
#define TH_ATTR_ECO_COOL_TEMP    0xF00BU
#define TH_ATTR_EXT_CALIBRATION  0xF00CU
#define TH_ATTR_MODE_KEYLOCK     0xF00DU
#define TH_ATTR_SCREEN_OFF_TIME  0xF00FU
#define TH_ATTR_LED_INDICATOR    0xF010U
#define FAN_ATTR_CONTROL         0xF000U
#define HUM_ATTR_OFFSET          0xF005U

#define SYS_MODE_OFF  0U
#define SYS_MODE_HEAT 4U

static const struct device *const mcu_uart = DEVICE_DT_GET(DT_ALIAS(tuya_uart));

static const u16 cluster_rev = ZCL_ATTR_GLOBAL_CLUSTER_REVISION_DEFAULT;

/* ------------------------------------------------------------------ */
/* Attribute storage                                                   */
/* ------------------------------------------------------------------ */

static u8 group_name_support;

static struct {
	s16 local_temp;
	s16 outdoor_temp;
	s16 abs_min;
	s16 abs_max;
	s16 min_limit;
	s16 max_limit;
	s8 calibration;
	s16 heat_setpoint;
	s16 cool_setpoint;
	u8 control_sequence;
	u8 system_mode;
	u8 prog_mode;
	u16 running_state;
	s8 dead_band;
	u8 start_of_week;
	s8 weekly_transitions;
	s8 daily_transitions;
	u8 sensor_used;
	s16 frost_protect;
	s16 heat_protect;
	u8 eco_mode;
	s16 eco_heat;
	s16 eco_cool;
	u8 frost_onoff;
	u8 settings_reset;
	u8 sound;
	u8 inversion;
	u8 schedule_mode;
	u8 level;
	s8 ext_calibration;
	u8 mode_keylock;
	u8 screen_off_time;
	u8 led_indicator;
} th = {
	.abs_min = 500,
	.abs_max = 3500,
	.min_limit = 500,
	.max_limit = 3500,
	.heat_setpoint = 2200,
	.cool_setpoint = 2200,
	.control_sequence = 2U, /* heating only */
	.start_of_week = 1U,
	.weekly_transitions = 28,
	.daily_transitions = 4,
};

static u8 ui_display_mode;
static u8 ui_keypad_lockout;
static u8 fan_mode;
static u8 fan_mode_sequence = 5U; /* auto */
static u8 fan_control;
static u16 humidity = 0xFFFFU;
static const u16 humidity_min;
static const u16 humidity_max = 0x2710U;
static s16 humidity_offset;
static u8 level_a_current = 6U, level_b_current = 6U;
static const u8 level_min;
static const u8 level_max = 8U;
static u8 level_a_options, level_b_options;
static u32 time_utc = TIME_INVALID;
static u32 time_local = TIME_INVALID;

#define R  ACCESS_CONTROL_READ
#define RW (ACCESS_CONTROL_READ | ACCESS_CONTROL_WRITE)
#define RR (ACCESS_CONTROL_READ | ACCESS_CONTROL_REPORTABLE)
#define RWR (RW | ACCESS_CONTROL_REPORTABLE)

#define ATTR(id, type, access, var) {(id), ZCL_DATA_TYPE_##type, (access), (u8 *)&(var)}
#define REV ATTR(ZCL_ATTRID_GLOBAL_CLUSTER_REVISION, UINT16, R, cluster_rev)

static const zclAttrInfo_t group_attr_tbl[] = {
	ATTR(ZCL_ATTRID_GROUP_NAME_SUPPORT, BITMAP8, R, group_name_support),
	REV,
};

static const zclAttrInfo_t level_a_attr_tbl[] = {
	ATTR(ZCL_ATTRID_LEVEL_CURRENT_LEVEL, UINT8, RR, level_a_current),
	ATTR(ZCL_ATTRID_LEVEL_MIN_LEVEL, UINT8, R, level_min),
	ATTR(ZCL_ATTRID_LEVEL_MAX_LEVEL, UINT8, R, level_max),
	ATTR(ZCL_ATTRID_LEVEL_OPTIONS, BITMAP8, RW, level_a_options),
	REV,
};

static const zclAttrInfo_t level_b_attr_tbl[] = {
	ATTR(ZCL_ATTRID_LEVEL_CURRENT_LEVEL, UINT8, RR, level_b_current),
	ATTR(ZCL_ATTRID_LEVEL_MIN_LEVEL, UINT8, R, level_min),
	ATTR(ZCL_ATTRID_LEVEL_MAX_LEVEL, UINT8, R, level_max),
	ATTR(ZCL_ATTRID_LEVEL_OPTIONS, BITMAP8, RW, level_b_options),
	REV,
};

static const zclAttrInfo_t time_attr_tbl[] = {
	ATTR(ZCL_ATTRID_TIME, UTC, RW, time_utc),
	ATTR(ZCL_ATTRID_LOCAL_TIME, UINT32, R, time_local),
	REV,
};

static const zclAttrInfo_t thermostat_attr_tbl[] = {
	ATTR(ZCL_ATTRID_HVAC_THERMOSTAT_LOCAL_TEMPERATURE, INT16, RR, th.local_temp),
	ATTR(ZCL_ATTRID_HVAC_THERMOSTAT_OUTDOOR_TEMPERATURE, INT16, RR, th.outdoor_temp),
	ATTR(ZCL_ATTRID_HVAC_THERMOSTAT_ABS_MIN_HEAT_SETPOINT_LIMIT, INT16, R, th.abs_min),
	ATTR(ZCL_ATTRID_HVAC_THERMOSTAT_ABS_MAX_HEAT_SETPOINT_LIMIT, INT16, R, th.abs_max),
	ATTR(ZCL_ATTRID_HVAC_THERMOSTAT_MIN_HEAT_SETPOINT_LIMIT, INT16, RWR, th.min_limit),
	ATTR(ZCL_ATTRID_HVAC_THERMOSTAT_MAX_HEAT_SETPOINT_LIMIT, INT16, RWR, th.max_limit),
	ATTR(ZCL_ATTRID_HVAC_THERMOSTAT_LOCAL_TEMP_CALIBRATION, INT8, RWR, th.calibration),
	ATTR(ZCL_ATTRID_HVAC_THERMOSTAT_OCCUPIED_HEATING_SETPOINT, INT16, RWR, th.heat_setpoint),
	ATTR(ZCL_ATTRID_HVAC_THERMOSTAT_OCCUPIED_COOLING_SETPOINT, INT16, RWR, th.cool_setpoint),
	ATTR(ZCL_ATTRID_HVAC_THERMOSTAT_CTRL_SEQUENCE_OF_OPERATION, ENUM8, R,
	     th.control_sequence),
	ATTR(ZCL_ATTRID_HVAC_THERMOSTAT_SYS_MODE, ENUM8, RWR, th.system_mode),
	ATTR(ZCL_ATTRID_HVAC_THERMOSTAT_PROGRAMMING_OPERATION_MODE, BITMAP8, RWR, th.prog_mode),
	ATTR(ZCL_ATTRID_HVAC_THERMOSTAT_RUNNING_STATE, BITMAP16, RR, th.running_state),
	ATTR(ZCL_ATTRID_HVAC_THERMOSTAT_MIN_SETPOINT_DEAD_BAND, INT8, RWR, th.dead_band),
	ATTR(ZCL_ATTRID_HVAC_THERMOSTAT_START_OF_WEEK, ENUM8, R, th.start_of_week),
	ATTR(ZCL_ATTRID_HVAC_THERMOSTAT_NUM_OF_WEEKLY_TRANSITIONS, INT8, R,
	     th.weekly_transitions),
	ATTR(ZCL_ATTRID_HVAC_THERMOSTAT_NUM_OF_DAILY_TRANSITIONS, INT8, R, th.daily_transitions),
	ATTR(TH_ATTR_SENSOR_USED, ENUM8, RWR, th.sensor_used),
	ATTR(TH_ATTR_FROST_PROTECT, INT16, RWR, th.frost_protect),
	ATTR(TH_ATTR_HEAT_PROTECT, INT16, RWR, th.heat_protect),
	ATTR(TH_ATTR_ECO_MODE, ENUM8, RWR, th.eco_mode),
	ATTR(TH_ATTR_ECO_HEAT_TEMP, INT16, RWR, th.eco_heat),
	ATTR(TH_ATTR_ECO_COOL_TEMP, INT16, RWR, th.eco_cool),
	ATTR(TH_ATTR_FROST_ONOFF, BOOLEAN, RWR, th.frost_onoff),
	ATTR(TH_ATTR_SETTINGS_RESET, BOOLEAN, RWR, th.settings_reset),
	ATTR(TH_ATTR_SOUND, BOOLEAN, RWR, th.sound),
	ATTR(TH_ATTR_INVERSION, BOOLEAN, RWR, th.inversion),
	ATTR(TH_ATTR_SCHEDULE_MODE, ENUM8, RWR, th.schedule_mode),
	ATTR(TH_ATTR_LEVEL, ENUM8, RWR, th.level),
	ATTR(TH_ATTR_EXT_CALIBRATION, INT8, RWR, th.ext_calibration),
	ATTR(TH_ATTR_MODE_KEYLOCK, BOOLEAN, RWR, th.mode_keylock),
	ATTR(TH_ATTR_SCREEN_OFF_TIME, ENUM8, RW, th.screen_off_time),
	ATTR(TH_ATTR_LED_INDICATOR, BOOLEAN, RW, th.led_indicator),
	REV,
};

static const zclAttrInfo_t ui_attr_tbl[] = {
	ATTR(0x0000, ENUM8, RWR, ui_display_mode),
	ATTR(0x0001, ENUM8, RWR, ui_keypad_lockout),
	REV,
};

static const zclAttrInfo_t fan_attr_tbl[] = {
	ATTR(0x0000, ENUM8, RWR, fan_mode),
	ATTR(0x0001, ENUM8, RW, fan_mode_sequence),
	ATTR(FAN_ATTR_CONTROL, BOOLEAN, RWR, fan_control),
	REV,
};

static const zclAttrInfo_t humidity_attr_tbl[] = {
	ATTR(ZCL_RELATIVE_HUMIDITY_ATTRID_MEASUREDVALUE, UINT16, RR, humidity),
	ATTR(ZCL_RELATIVE_HUMIDITY_ATTRID_MINMEASUREDVALUE, UINT16, R, humidity_min),
	ATTR(ZCL_RELATIVE_HUMIDITY_ATTRID_MAXMEASUREDVALUE, UINT16, R, humidity_max),
	ATTR(HUM_ATTR_OFFSET, INT16, RWR, humidity_offset),
	REV,
};

/* ------------------------------------------------------------------ */
/* Field <-> attribute map                                             */
/* ------------------------------------------------------------------ */

#define SGN       0x01U /* signed */
#define ONLY_UINT 0x02U /* only where the model's field is a plain value */
#define NOT_UINT  0x04U /* only where it is a temperature */

#define TH_CL  ZCL_CLUSTER_HAVC_THERMOSTAT
#define UI_CL  ZCL_CLUSTER_HAVC_USER_INTERFACE_CONFIG
#define FAN_CL ZCL_CLUSTER_HAVC_FAN_CONTROL
#define HUM_CL ZCL_CLUSTER_MS_RELATIVE_HUMIDITY

struct amap {
	u8 field;
	u8 ep;
	u8 size;
	u8 flags;
	u16 cluster;
	u16 attr;
};

/* A field may be on several attributes (the set point is on heating and cooling) */
static const struct amap amap[] = {
	{IOTDRV_TT_F_TEMP, APP_ENDPOINT, 2, SGN, TH_CL, ZCL_ATTRID_HVAC_THERMOSTAT_LOCAL_TEMPERATURE},
	{IOTDRV_TT_F_TEMP_OUT, APP_ENDPOINT, 2, SGN, TH_CL,
	 ZCL_ATTRID_HVAC_THERMOSTAT_OUTDOOR_TEMPERATURE},
	{IOTDRV_TT_F_SETPOINT, APP_ENDPOINT, 2, SGN, TH_CL,
	 ZCL_ATTRID_HVAC_THERMOSTAT_OCCUPIED_HEATING_SETPOINT},
	{IOTDRV_TT_F_SETPOINT, APP_ENDPOINT, 2, SGN, TH_CL,
	 ZCL_ATTRID_HVAC_THERMOSTAT_OCCUPIED_COOLING_SETPOINT},
	{IOTDRV_TT_F_MIN, APP_ENDPOINT, 2, SGN, TH_CL, ZCL_ATTRID_HVAC_THERMOSTAT_MIN_HEAT_SETPOINT_LIMIT},
	{IOTDRV_TT_F_MAX, APP_ENDPOINT, 2, SGN, TH_CL, ZCL_ATTRID_HVAC_THERMOSTAT_MAX_HEAT_SETPOINT_LIMIT},
	{IOTDRV_TT_F_DEADZONE, APP_ENDPOINT, 1, SGN, TH_CL,
	 ZCL_ATTRID_HVAC_THERMOSTAT_MIN_SETPOINT_DEAD_BAND},
	{IOTDRV_TT_F_CALIBRATION, APP_ENDPOINT, 1, SGN, TH_CL,
	 ZCL_ATTRID_HVAC_THERMOSTAT_LOCAL_TEMP_CALIBRATION},
	{IOTDRV_TT_F_PROG, APP_ENDPOINT, 1, 0, TH_CL,
	 ZCL_ATTRID_HVAC_THERMOSTAT_PROGRAMMING_OPERATION_MODE},
	{IOTDRV_TT_F_SENSOR, APP_ENDPOINT, 1, 0, TH_CL, TH_ATTR_SENSOR_USED},
	{IOTDRV_TT_F_FROST_PROTECT, APP_ENDPOINT, 2, SGN | NOT_UINT, TH_CL, TH_ATTR_FROST_PROTECT},
	{IOTDRV_TT_F_FROST_PROTECT, APP_ENDPOINT, 1, ONLY_UINT, TH_CL, TH_ATTR_FROST_ONOFF},
	{IOTDRV_TT_F_HEAT_PROTECT, APP_ENDPOINT, 2, SGN, TH_CL, TH_ATTR_HEAT_PROTECT},
	{IOTDRV_TT_F_ECO_MODE, APP_ENDPOINT, 1, 0, TH_CL, TH_ATTR_ECO_MODE},
	{IOTDRV_TT_F_ECO_HEAT_TEMP, APP_ENDPOINT, 2, SGN, TH_CL, TH_ATTR_ECO_HEAT_TEMP},
	{IOTDRV_TT_F_ECO_COOL_TEMP, APP_ENDPOINT, 2, SGN, TH_CL, TH_ATTR_ECO_COOL_TEMP},
	{IOTDRV_TT_F_SETTINGS_RESET, APP_ENDPOINT, 1, 0, TH_CL, TH_ATTR_SETTINGS_RESET},
	{IOTDRV_TT_F_SOUND, APP_ENDPOINT, 1, 0, TH_CL, TH_ATTR_SOUND},
	{IOTDRV_TT_F_INVERSION, APP_ENDPOINT, 1, 0, TH_CL, TH_ATTR_INVERSION},
	/* r06 and r08 keep the schedule mode where the others have Monday's schedule */
	{IOTDRV_TT_F_SCHEDULE_MODE, APP_ENDPOINT, 1, ONLY_UINT, TH_CL, TH_ATTR_SCHEDULE_MODE},
	{IOTDRV_TT_F_LEVEL_A, APP_ENDPOINT, 1, 0, TH_CL, TH_ATTR_LEVEL},
	{IOTDRV_TT_F_LEVEL_B, APP_ENDPOINT2, 1, 0, ZCL_CLUSTER_GEN_LEVEL_CONTROL,
	 ZCL_ATTRID_LEVEL_CURRENT_LEVEL},
	{IOTDRV_TT_F_EXT_CALIBRATION, APP_ENDPOINT, 1, SGN, TH_CL, TH_ATTR_EXT_CALIBRATION},
	{IOTDRV_TT_F_MODE_LOCK, APP_ENDPOINT, 1, 0, TH_CL, TH_ATTR_MODE_KEYLOCK},
	{IOTDRV_TT_F_SCREEN_TIME, APP_ENDPOINT, 1, 0, TH_CL, TH_ATTR_SCREEN_OFF_TIME},
	{IOTDRV_TT_F_LED_INDICATOR, APP_ENDPOINT, 1, 0, TH_CL, TH_ATTR_LED_INDICATOR},
	{IOTDRV_TT_F_FAN_MODE, APP_ENDPOINT, 1, 0, FAN_CL, 0x0000},
	{IOTDRV_TT_F_FAN_CONTROL, APP_ENDPOINT, 1, 0, FAN_CL, FAN_ATTR_CONTROL},
	{IOTDRV_TT_F_LOCKUNLOCK, APP_ENDPOINT, 1, 0, UI_CL, 0x0001},
	{IOTDRV_TT_F_HUMIDITY, APP_ENDPOINT, 2, 0, HUM_CL, ZCL_RELATIVE_HUMIDITY_ATTRID_MEASUREDVALUE},
	{IOTDRV_TT_F_HUMIDITY_OFFSET, APP_ENDPOINT, 2, SGN, HUM_CL, HUM_ATTR_OFFSET},
};

static int model = -1; /* index into iotdrv_tt_models, -1 until the MCU has told */

static bool amap_applies(const struct amap *m, int field_kind)
{
	if ((m->flags & ONLY_UINT) != 0U) {
		return field_kind == IOTDRV_TT_K_UINT;
	}
	if ((m->flags & NOT_UINT) != 0U) {
		return field_kind != IOTDRV_TT_K_UINT && field_kind != IOTDRV_TT_K_NONE;
	}
	return field_kind != IOTDRV_TT_K_NONE;
}

static void attr_set(u8 ep, u16 cluster, u16 attr, u8 size, s32 value)
{
	u8 buf[2] = {(u8)value, (u8)(value >> 8)};

	(void)size;
	(void)zcl_setAttrVal(ep, cluster, attr, buf);
}

/* A value of the MCU on the attributes that carry it */
static void publish_field(enum iotdrv_tt_field field, s32 value)
{
	int kind = iotdrv_tt_field_kind(model, field);

	for (size_t i = 0; i < ARRAY_SIZE(amap); i++) {
		const struct amap *m = &amap[i];

		if (m->field == field && amap_applies(m, kind)) {
			attr_set(m->ep, m->cluster, m->attr, m->size, value);
		}
	}
}

/* ------------------------------------------------------------------ */
/* MCU link                                                            */
/* ------------------------------------------------------------------ */

static struct iotdrv_tuya_mcu mcu;

enum ev_kind {
	EV_READY,
	EV_DP,
	EV_RESET,
};

#define EV_DATA_MAX 64U
#define EV_PUT_TIMEOUT_MS 500

struct ev {
	u8 kind;
	u8 id;
	u8 type;
	u16 len;
	u8 data[EV_DATA_MAX];
};

K_MSGQ_DEFINE(ev_q, sizeof(struct ev), 6, 4);

static struct iotdrv_tt_schedule schedule;
static bool net_joined;

static void ev_drain(void *arg);

/*
 * Called on the system work queue. A frame can carry more datapoints than the queue
 * holds: wait for the Zigbee thread to take some instead of dropping the rest.
 */
static void ev_put(const struct ev *e)
{
	if (k_msgq_put(&ev_q, e, K_MSEC(EV_PUT_TIMEOUT_MS)) == 0) {
		(void)tl_zbTaskPost(ev_drain, NULL);
	} else {
		printk("tt: event dropped\n");
	}
}

static void on_ready(struct iotdrv_tuya_mcu *m, const char *product, void *user)
{
	struct ev e = {.kind = EV_READY};
	int n;

	ARG_UNUSED(m);
	ARG_UNUSED(user);

	n = iotdrv_tt_product_from_info((const u8 *)product, strlen(product), (char *)e.data, 16U);
	e.len = (n > 0) ? (u16)n : 0U;
	ev_put(&e);
}

static void on_dp(struct iotdrv_tuya_mcu *m, const struct iotdrv_tuya_dp *dp, void *user)
{
	struct ev e = {.kind = EV_DP, .id = dp->id, .type = dp->type, .len = dp->len};

	ARG_UNUSED(m);
	ARG_UNUSED(user);

	if (dp->len > EV_DATA_MAX) {
		return;
	}
	memcpy(e.data, dp->data, dp->len);
	ev_put(&e);
}

static void on_reset_request(struct iotdrv_tuya_mcu *m, void *user)
{
	struct ev e = {.kind = EV_RESET};

	ARG_UNUSED(m);
	ARG_UNUSED(user);
	ev_put(&e);
}

static struct iotdrv_tuya_listener listener = {
	.on_ready = on_ready,
	.on_dp = on_dp,
	.on_reset_request = on_reset_request,
};

/* Nonzero means heating in r06 and r0A, zero in the others */
static bool run_state_heating(s32 value)
{
	bool inverted = (model == 5) || (model == 9);

	return inverted ? (value != 0) : (value == 0);
}

static void publish_running(bool heating)
{
	u16 v = heating ? 1U : 0U;

	th.running_state = v;
	(void)zcl_setAttrVal(APP_ENDPOINT, TH_CL, ZCL_ATTRID_HVAC_THERMOSTAT_RUNNING_STATE,
			     (u8 *)&v);
}

static void publish_onoff(bool on)
{
	u8 mode = on ? SYS_MODE_HEAT : SYS_MODE_OFF;

	(void)zcl_setAttrVal(APP_ENDPOINT, TH_CL, ZCL_ATTRID_HVAC_THERMOSTAT_SYS_MODE, &mode);
	if (!on) {
		publish_running(false);
	}
}

static void send_field(enum iotdrv_tt_field field, s32 value)
{
	u8 buf[IOTDRV_TUYA_MAX_DATA];
	int n = iotdrv_tt_to_dp(model, field, value, buf, sizeof(buf));

	printk("tt: write field %d = %d -> %d\n", (int)field, (int)value, n);
	if (n > 0) {
		(void)iotdrv_tuya_mcu_send_dp(&mcu, buf, (size_t)n);
	}
}

static void model_apply(int idx)
{
	const struct iotdrv_tt_model *md = &iotdrv_tt_models[idx];

	model = idx;
	printk("tt: model %s\n", md->name);
	app_basic_set_model_id(md->name);
	th.abs_min = md->abs_min;
	th.abs_max = md->abs_max;
	th.min_limit = MAX(th.min_limit, md->abs_min);
	th.max_limit = MIN(th.max_limit, md->abs_max);
	if (iotdrv_tt_schedule_transitions(idx, IOTDRV_TT_DAY_MON) > 4U) {
		th.daily_transitions = 6;
		th.weekly_transitions = 6 * 7;
	}
	(void)iotdrv_tuya_mcu_query_dps(&mcu);
}

static void dp_apply(const struct ev *e)
{
	const struct iotdrv_tt_dp *d;
	uint8_t days;
	int field;
	s32 value;

	if (model < 0) {
		return;
	}

	if (iotdrv_tt_schedule_from_dp(model, e->id, e->data, e->len, &schedule, &days) !=
	    -ENOENT) {
		return;
	}

	field = iotdrv_tt_find_field(&iotdrv_tt_models[model], e->id);
	if (field < 0) {
		return;
	}
	d = &iotdrv_tt_models[model].dp[field];
	if (d->type != e->type) {
		return; /* the original matches the id and the type */
	}

	if (iotdrv_tt_from_dp(model, field, e->data, e->len, &value) != 0) {
		return;
	}
	printk("tt: dp %u field %d = %d\n", e->id, field, (int)value);

	switch (field) {
	case IOTDRV_TT_F_ONOFF:
		publish_onoff(value != 0);
		break;
	case IOTDRV_TT_F_RUNSTATE:
		publish_running(run_state_heating(value));
		break;
	default:
		publish_field(field, value);
		break;
	}
}

static void ev_drain(void *arg)
{
	struct ev e;

	ARG_UNUSED(arg);

	while (k_msgq_get(&ev_q, &e, K_NO_WAIT) == 0) {
		switch (e.kind) {
		case EV_READY: {
			int idx = iotdrv_tt_model_by_product((const char *)e.data);

			/* an unknown product gets the first model, as the stock firmware does */
			model_apply(idx < 0 ? 0 : idx);
			break;
		}
		case EV_DP:
			dp_apply(&e);
			break;
		case EV_RESET:
			printk("tt: reset request\n");
			zb_factoryReset();
			break;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */

static int time_for_mcu(void *ctx, uint32_t *utc, uint32_t *local)
{
	ARG_UNUSED(ctx);

	if (time_utc == TIME_INVALID || time_local == TIME_INVALID) {
		return -ENODATA;
	}
	*utc = time_utc + ZIGBEE_EPOCH;
	*local = time_local + ZIGBEE_EPOCH;
	return 0;
}

static void time_request(void)
{
	/* a zclReadCmd_t is a count and a flexible array of attribute ids */
	u8 raw[sizeof(zclReadCmd_t) + 2U * sizeof(u16)] __aligned(4);
	zclReadCmd_t *rd = (zclReadCmd_t *)raw;
	epInfo_t dst;

	TL_SETSTRUCTCONTENT(dst, 0);
	dst.profileId = APP_HA_PROFILE_ID;
	dst.dstAddrMode = APS_SHORT_DSTADDR_WITHEP;
	dst.dstEp = APP_ENDPOINT;
	dst.dstAddr.shortAddr = 0x0000U; /* the coordinator */

	rd->numAttr = 2U;
	rd->attrID[0] = ZCL_ATTRID_TIME;
	rd->attrID[1] = ZCL_ATTRID_LOCAL_TIME;
	(void)zcl_read(APP_ENDPOINT, &dst, ZCL_CLUSTER_GEN_TIME, MANUFACTURER_CODE_NONE, 0U, 0U,
		       ZCL_SEQ_NUM, rd);
}

static u32 le32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void time_read_rsp(const zclReadRspCmd_t *rsp)
{
	for (u8 i = 0; i < rsp->numAttr; i++) {
		const zclReadRspStatus_t *a = &rsp->attrList[i];

		if (a->status != ZCL_STA_SUCCESS) {
			continue;
		}
		if (a->attrID == ZCL_ATTRID_TIME) {
			time_utc = le32(a->data);
		} else if (a->attrID == ZCL_ATTRID_LOCAL_TIME) {
			time_local = le32(a->data);
		}
	}
}

/* ------------------------------------------------------------------ */
/* Writes from the coordinator                                         */
/* ------------------------------------------------------------------ */

static s32 attr_value(const u8 *data, u8 size, bool is_signed)
{
	if (size == 1U) {
		return is_signed ? (s32)(s8)data[0] : (s32)data[0];
	}
	return is_signed ? (s32)(s16)(data[0] | (data[1] << 8)) : (s32)(data[0] | (data[1] << 8));
}

static void write_attr(u8 ep, u16 cluster, u16 attr, const u8 *data)
{
	int kind;

	if (cluster == TH_CL && attr == ZCL_ATTRID_HVAC_THERMOSTAT_SYS_MODE) {
		send_field(IOTDRV_TT_F_ONOFF, data[0] != SYS_MODE_OFF ? 1 : 0);
		return;
	}

	for (size_t i = 0; i < ARRAY_SIZE(amap); i++) {
		const struct amap *m = &amap[i];

		if (m->ep != ep || m->cluster != cluster || m->attr != attr) {
			continue;
		}
		kind = iotdrv_tt_field_kind(model, m->field);
		if (!amap_applies(m, kind)) {
			continue;
		}
		s32 v = attr_value(data, m->size, (m->flags & SGN) != 0U);

		send_field(m->field, v);
		if (m->field == IOTDRV_TT_F_SETPOINT) {
			/* heating and cooling are one set point */
			u16 other = (attr == ZCL_ATTRID_HVAC_THERMOSTAT_OCCUPIED_HEATING_SETPOINT)
					    ? ZCL_ATTRID_HVAC_THERMOSTAT_OCCUPIED_COOLING_SETPOINT
					    : ZCL_ATTRID_HVAC_THERMOSTAT_OCCUPIED_HEATING_SETPOINT;

			attr_set(ep, cluster, other, 2U, v);
		}
		return;
	}
}

static void zcl_hook(zclIncoming_t *in)
{
	u16 cluster = in->msg->indInfo.cluster_id;
	u8 ep = in->msg->indInfo.dst_ep;

	switch (in->hdr.cmd) {
	case ZCL_CMD_WRITE:
		if (model >= 0) {
			const zclWriteCmd_t *w = in->attrCmd;

			for (u8 i = 0; i < w->numAttr; i++) {
				write_attr(ep, cluster, w->attrList[i].attrID,
					   w->attrList[i].attrData);
			}
		}
		break;
	case ZCL_CMD_READ_RSP:
		if (cluster == ZCL_CLUSTER_GEN_TIME) {
			time_read_rsp(in->attrCmd);
		}
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------ */
/* Thermostat commands: weekly schedule, set point raise/lower         */
/* ------------------------------------------------------------------ */

static u16 rd16(const u8 *p)
{
	return (u16)(p[0] | (p[1] << 8));
}

static void send_schedule(uint8_t days)
{
	u8 buf[IOTDRV_TUYA_MAX_DATA * 2];
	int n = iotdrv_tt_schedule_to_dp(model, &schedule, days, buf, sizeof(buf));
	size_t off = 0U;
	struct iotdrv_tuya_dp dp;

	if (n <= 0) {
		return;
	}
	/* several datapoints back to back: send them one by one */
	while (iotdrv_tuya_dp_next(buf, (size_t)n, &off, &dp) == 0) {
		size_t start = (size_t)(dp.data - buf) - DP_HEADER;

		(void)iotdrv_tuya_mcu_send_dp(&mcu, &buf[start], DP_HEADER + dp.len);
	}
}

static status_t set_weekly_schedule(const zcl_thermostat_setWeeklyScheduleCmd_t *cmd)
{
	const u8 *p = (const u8 *)cmd->sequenceMode.pHeatMode;
	/* heat only: 4 bytes a transition; both: 6 bytes, the heat set point first after the time */
	u8 stride = (cmd->modeForSequence == BOTH_SETPOINT_FIELD_PRESENT) ? 6U : 4U;
	u8 days = cmd->dayOfWeekForSequence & 0x7FU;
	u8 applied = 0U;

	if (model < 0 || p == NULL || cmd->numOfTransForSequence == 0U ||
	    (cmd->modeForSequence & HEAT_SERPOINT_FIELD_PRESENT) == 0U) {
		return ZCL_STA_INVALID_FIELD;
	}

	for (u8 d = 0; d < 7U; d++) {
		u8 bit = BIT(d);
		unsigned int limit = iotdrv_tt_schedule_transitions(model, bit);
		struct iotdrv_tt_transition *day = schedule.day[d];

		if ((days & bit) == 0U || limit == 0U) {
			continue;
		}
		for (unsigned int t = 0; t < IOTDRV_TT_TRANSITIONS; t++) {
			/* slots the controller did not give repeat its last transition */
			unsigned int src = MIN(t, (unsigned int)cmd->numOfTransForSequence - 1U);

			day[t].minutes = rd16(&p[src * stride]);
			day[t].centi = (s16)rd16(&p[src * stride + 2U]);
		}
		applied |= bit;
	}
	if (applied == 0U) {
		return ZCL_STA_INVALID_FIELD;
	}

	send_schedule(applied);
	return ZCL_STA_SUCCESS;
}

static void get_weekly_schedule(zclIncomingAddrInfo_t *ai, const zcl_thermostat_getWeeklyScheduleCmd_t *cmd)
{
	epInfo_t dst;

	TL_SETSTRUCTCONTENT(dst, 0);
	dst.profileId = ai->profileId;
	dst.dstAddrMode = APS_SHORT_DSTADDR_WITHEP;
	dst.dstEp = ai->srcEp;
	dst.dstAddr.shortAddr = ai->srcAddr;

	/* one response per day: the days of a response share their transitions */
	for (u8 d = 0; d < 7U; d++) {
		u8 bit = BIT(d);
		unsigned int n = iotdrv_tt_schedule_transitions(model, bit);
		heatMode_t tr[IOTDRV_TT_TRANSITIONS];
		zcl_thermostat_getWeeklyScheduleRspCmd_t rsp;

		if ((cmd->daysToReturn & bit) == 0U || n == 0U) {
			continue;
		}
		for (unsigned int t = 0; t < n; t++) {
			tr[t].transTime = schedule.day[d][t].minutes;
			tr[t].heatSetpoint = (u16)schedule.day[d][t].centi;
		}
		TL_SETSTRUCTCONTENT(rsp, 0);
		rsp.sequenceMode.pHeatMode = tr;
		rsp.numOfTransForSequence = (u8)n;
		rsp.dayOfWeekForSequence = bit;
		rsp.modeForSequence = HEAT_SERPOINT_FIELD_PRESENT;
		(void)zcl_thermostat_getWeeklyScheduleRspCmdSend(APP_ENDPOINT, &dst, 1U,
								 ai->seqNum, &rsp);
	}
}

static status_t thermostat_cb(zclIncomingAddrInfo_t *ai, u8 cmd_id, void *payload)
{
	switch (cmd_id) {
	case ZCL_CMD_THERMOSTAT_SETPOINT_RAISE_LOWER: {
		const zcl_thermostat_setpointRaiseLowerCmd_t *c = payload;

		if (model < 0 || c->mode == ADJUST_COOL_SETPOINT) {
			return ZCL_STA_INVALID_FIELD;
		}
		/* amount is in tenths of a degree */
		send_field(IOTDRV_TT_F_SETPOINT, th.heat_setpoint + (s32)c->amount * 10);
		return ZCL_STA_SUCCESS;
	}
	case ZCL_CMD_THERMOSTAT_SET_WEEKLY_SCHEDULE:
		return set_weekly_schedule(payload);
	case ZCL_CMD_THERMOSTAT_GET_WEEKLY_SCHEDULE:
		if (model < 0) {
			return ZCL_STA_FAILURE;
		}
		get_weekly_schedule(ai, payload);
		return ZCL_STA_SUCCESS;
	case ZCL_CMD_THERMOSTAT_CLEAR_WEEKLY_SCHEDULE:
		return ZCL_STA_SUCCESS; /* the controller keeps its schedule, as the stock firmware */
	default:
		return ZCL_STA_UNSUP_CLUSTER_COMMAND;
	}
}

/* ------------------------------------------------------------------ */
/* Endpoints                                                           */
/* ------------------------------------------------------------------ */

static u16 in_clusters[] = {
	ZCL_CLUSTER_GEN_BASIC,
	ZCL_CLUSTER_GEN_IDENTIFY,
	ZCL_CLUSTER_GEN_GROUPS,
	ZCL_CLUSTER_GEN_LEVEL_CONTROL,
	ZCL_CLUSTER_HAVC_THERMOSTAT,
	ZCL_CLUSTER_HAVC_USER_INTERFACE_CONFIG,
	ZCL_CLUSTER_HAVC_FAN_CONTROL,
	ZCL_CLUSTER_MS_RELATIVE_HUMIDITY,
};

static u16 out_clusters[] = {
	ZCL_CLUSTER_GEN_TIME,
#if defined(CONFIG_ZIGBEE_OTA)
	ZCL_CLUSTER_OTA,
#endif
};

static af_simple_descriptor_t simple_desc = {
	.app_profile_id = APP_HA_PROFILE_ID,
	.app_dev_id = 0x0301U, /* Thermostat */
	.endpoint = APP_ENDPOINT,
	.app_dev_ver = 1U,
	.reserved = 0U,
	.app_in_cluster_count = ARRAY_SIZE(in_clusters),
	.app_in_cluster_lst = in_clusters,
	.app_out_cluster_count = ARRAY_SIZE(out_clusters),
	.app_out_cluster_lst = out_clusters,
};

static u16 level_b_in_clusters[] = {
	ZCL_CLUSTER_GEN_LEVEL_CONTROL,
};

static af_simple_descriptor_t simple_desc2 = {
	.app_profile_id = APP_HA_PROFILE_ID,
	.app_dev_id = 0x0301U,
	.endpoint = APP_ENDPOINT2,
	.app_dev_ver = 1U,
	.reserved = 0U,
	.app_in_cluster_count = ARRAY_SIZE(level_b_in_clusters),
	.app_in_cluster_lst = level_b_in_clusters,
	.app_out_cluster_count = 0U,
	.app_out_cluster_lst = NULL,
};

#if defined(CONFIG_ZIGBEE_OTA)
/* See plug.c: the OTA file header carries the same identity */
static ota_preamble_t ota_preamble = {
	.fileVer = APP_OTA_FILE_VERSION,
	.totalImageSize = 0U,
	.imageType = APP_OTA_IMAGE_TYPE,
	.manufacturerCode = CONFIG_ZIGBEE_MANUFACTURER_CODE,
};

static void ota_event(u8 evt, u8 status)
{
	if (evt == OTA_EVT_COMPLETE && status == ZCL_STA_SUCCESS) {
		ota_mcuReboot();
	}
}

static ota_callBack_t ota_cb = {
	.processMsgCbFunc = ota_event,
};
#endif

static void reporting_defaults(void)
{
	/* 0.5 degrees on the temperature and the set point */
	app_reporting_default(APP_ENDPOINT, TH_CL, ZCL_ATTRID_HVAC_THERMOSTAT_LOCAL_TEMPERATURE,
			      ZCL_DATA_TYPE_INT16, 10U, 300U, 50U);
	app_reporting_default(APP_ENDPOINT, TH_CL,
			      ZCL_ATTRID_HVAC_THERMOSTAT_OCCUPIED_HEATING_SETPOINT,
			      ZCL_DATA_TYPE_INT16, 1U, 300U, 50U);
	app_reporting_default(APP_ENDPOINT, TH_CL, ZCL_ATTRID_HVAC_THERMOSTAT_SYS_MODE,
			      ZCL_DATA_TYPE_ENUM8, 0U, 300U, 0U);
	app_reporting_default(APP_ENDPOINT, TH_CL, ZCL_ATTRID_HVAC_THERMOSTAT_RUNNING_STATE,
			      ZCL_DATA_TYPE_BITMAP16, 0U, 300U, 0U);
	app_reporting_default(APP_ENDPOINT, TH_CL,
			      ZCL_ATTRID_HVAC_THERMOSTAT_PROGRAMMING_OPERATION_MODE,
			      ZCL_DATA_TYPE_BITMAP8, 0U, 300U, 0U);
}

static zcl_specClusterInfo_t clusters[APP_COMMON_CLUSTER_NUM + 7U];
static zcl_specClusterInfo_t clusters2[1];

#define CLUSTER(idx, id, tbl, reg, cb)                                                            \
	(clusters[idx] = (zcl_specClusterInfo_t){.clusterId = (id),                               \
						 .attrNum = ARRAY_SIZE(tbl),                      \
						 .attrTbl = (tbl),                                \
						 .clusterRegisterFunc = (reg),                    \
						 .clusterAppCb = (cb)})

/* ------------------------------------------------------------------ */
/* Once a second: report, clock, network state                         */
/* ------------------------------------------------------------------ */

static int tick(void *arg)
{
	static u32 seconds, since_time;
	bool joined = zb_isDeviceJoinedNwk();

	ARG_UNUSED(arg);

	seconds++;
	if (joined != net_joined) {
		net_joined = joined;
		(void)iotdrv_tuya_mcu_set_net_status(&mcu, joined ? IOTDRV_TUYA_NET_JOINED
								  : IOTDRV_TUYA_NET_NOT_JOINED);
		since_time = 0U;
	}

	if (time_utc != TIME_INVALID && time_local != TIME_INVALID) {
		time_utc++;
		time_local++;
	}

	if (joined) {
		bool have = time_utc != TIME_INVALID;
		u32 period = have ? TIME_REFRESH_S : TIME_RETRY_S;

		if (since_time++ % period == 0U) {
			time_request();
		}
	}

	report_handler();
	return TICK_MS;
}

void app_device_register(void)
{
	app_common_clusters(clusters);

	CLUSTER(2, ZCL_CLUSTER_GEN_GROUPS, group_attr_tbl, zcl_group_register, NULL);
	CLUSTER(3, ZCL_CLUSTER_GEN_LEVEL_CONTROL, level_a_attr_tbl, zcl_level_register, NULL);
	CLUSTER(4, ZCL_CLUSTER_HAVC_THERMOSTAT, thermostat_attr_tbl, zcl_thermostat_register,
		thermostat_cb);
	CLUSTER(5, ZCL_CLUSTER_HAVC_USER_INTERFACE_CONFIG, ui_attr_tbl,
		zcl_thermostat_ui_cfg_register, NULL);
	CLUSTER(6, ZCL_CLUSTER_HAVC_FAN_CONTROL, fan_attr_tbl, zcl_fan_control_register, NULL);
	CLUSTER(7, ZCL_CLUSTER_MS_RELATIVE_HUMIDITY, humidity_attr_tbl,
		zcl_relative_humidity_register, NULL);

	CLUSTER(8, ZCL_CLUSTER_GEN_TIME, time_attr_tbl, zcl_time_register, NULL);

	app_endpoint_register(&simple_desc, clusters, ARRAY_SIZE(clusters), zcl_hook);

	clusters2[0] = (zcl_specClusterInfo_t){
		.clusterId = ZCL_CLUSTER_GEN_LEVEL_CONTROL,
		.attrNum = ARRAY_SIZE(level_b_attr_tbl),
		.attrTbl = level_b_attr_tbl,
		.clusterRegisterFunc = zcl_level_register,
	};
	if (af_endpointRegister(APP_ENDPOINT2, &simple_desc2, (af_endpoint_cb_t)zcl_rx_handler,
				NULL)) {
		zcl_register(APP_ENDPOINT2, ARRAY_SIZE(clusters2), clusters2);
	}

	app_reporting_restore();
	reporting_defaults();

#if defined(CONFIG_ZIGBEE_OTA)
	ota_init(OTA_TYPE_CLIENT, &simple_desc, &ota_preamble, &ota_cb);
	ota_queryStart(OTA_PERIODIC_QUERY_INTERVAL);
#endif

	iotdrv_tuya_mcu_init(&mcu, 0U);
	iotdrv_tuya_mcu_set_time_source(&mcu, time_for_mcu, NULL);
	iotdrv_tuya_mcu_add_listener(&mcu, &listener);
	if (device_is_ready(mcu_uart) && iotdrv_tuya_uart_bind(mcu_uart, &mcu) == 0) {
		(void)iotdrv_tuya_mcu_start(&mcu);
	}

	(void)ev_timer_taskPost(tick, NULL, TICK_MS);
}
