/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _TMC2100_H_
#define _TMC2100_H_

#include <linux/gpio/consumer.h>
#include <linux/pwm.h>

struct tmc2100 {
	struct gpio_desc *cfg[6];
	struct gpio_desc *cfg6_enn, *dir, *index, *error;
	struct pwm_device *ref, *step;
};

#endif /* _TMC2100_H */

