/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Attribute reporting set-up shared by the devices. The stack never restores
 * its reporting table from NV and never runs the report timers on its own: a
 * device calls app_reporting_restore() once after registering its endpoints,
 * adds its defaults, and calls report_handler() every second.
 */
#pragma once

#include "app_common.h"

/*
 * Restore the table the coordinator configured before the reset and drop the
 * entries that no longer match an attribute of this build.
 */
void app_reporting_restore(void);

/*
 * Configure a default report unless the attribute already has an entry, so a
 * configuration from the coordinator wins. @p change is the reportable change
 * of an analog attribute (ignored for the others). A report goes out only when
 * the coordinator has bound the cluster.
 */
void app_reporting_default(u8 endpoint, u16 cluster, u16 attr, u8 type, u16 min_s, u16 max_s,
			   u32 change);
