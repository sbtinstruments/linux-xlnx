/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
    stepper.h - Linux kernel modules for stepper motor drivers

    This file declares helper functions for the sysfs class "stepper",
    for use by stepper motor drivers.

    Copyright (C) 2019 Frederik Peter Aalund <fpa@sbtinstruments.com>
*/

#ifndef _STEPPER_H_
#define _STEPPER_H_

struct device;
struct attribute_group;

struct stepper_vel_cfg {
	int rate_of_change;
	int shift_delay_ms;
	int min;
	int max;
};

struct stepper_ops {
	int (*set_velocity)(struct device *dev, int velocity);
};

struct device *
stepper_device_register(struct device *dev, const char *name,
                        void *drvdata, struct stepper_ops *ops,
                        struct stepper_vel_cfg *cfg);
struct device *
devm_stepper_device_register(struct device *dev,
                             const char *name, void *drvdata,
                             struct stepper_ops *ops,
                             struct stepper_vel_cfg *cfg);

void stepper_device_unregister(struct device *dev);
void devm_stepper_device_unregister(struct device *dev);

#endif
