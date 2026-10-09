/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SBT Instruments Lock-in Amplifier: the sample stream
 *
 * Copyright (c) 2026, SBT Instruments A/S
 */
#ifndef _LOCKAMP_STREAM_H_
#define _LOCKAMP_STREAM_H_

#include <linux/device.h>

#include "lockin_amplifier.h"

int lockamp_stream_init(struct lockamp *lockamp, struct device *dev);
void lockamp_stream_exit(struct lockamp *lockamp);
int lockamp_stream_start(struct lockamp *lockamp);
void lockamp_stream_stop(struct lockamp *lockamp);
size_t lockamp_stream_available(struct lockamp *lockamp);
ssize_t lockamp_stream_read(struct lockamp *lockamp, char __user *buffer,
			    size_t length);

#endif /* _LOCKAMP_STREAM_H_ */
