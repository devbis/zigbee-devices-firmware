/* SPDX-License-Identifier: Apache-2.0 */

#include "app_reporting.h"

void app_reporting_default(u8 endpoint, u16 cluster, u16 attr, u8 type, u16 min_s, u16 max_s,
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

	if (zcl_reportCfgInfoEntryFind(endpoint, cluster, attr) != NULL) {
		return;
	}
	/* little-endian, as the stack compares it */
	chg[0] = change;
	chg[1] = change >> 8;
	chg[2] = change >> 16;
	chg[3] = change >> 24;
	(void)zcl_configureReporting(endpoint, APP_HA_PROFILE_ID, cluster, &rec);
}

void app_reporting_restore(void)
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
