/* SPDX-License-Identifier: Apache-2.0 */

#include "app_reporting.h"

void app_reporting_default(uint8_t endpoint, uint16_t cluster, uint16_t attr, uint8_t type,
			   uint16_t min_s, uint16_t max_s, uint32_t change)
{
	uint8_t chg[REPORTABLE_CHANGE_MAX_ANALOG_SIZE] = {0};
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
	uint8_t used = 0U;

	zcl_reportingTabInit();

	for (uint8_t i = 0; i < ZCL_REPORTING_TABLE_NUM; i++) {
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
