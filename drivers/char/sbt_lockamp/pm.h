#ifndef _LOCKAMP_PM_H
#define _LOCKAMP_PM_H

#include <linux/pm_runtime.h>

#include "lockin_amplifier.h"

static inline int lockamp_pm_get(struct lockamp *lockamp)
{
	return pm_runtime_get_sync(lockamp->dev->parent);
}

static inline void lockamp_pm_put(struct lockamp *lockamp)
{
	pm_runtime_mark_last_busy(lockamp->dev->parent);
	pm_runtime_put_autosuspend(lockamp->dev->parent); /* Ignore return value */
}

#endif
