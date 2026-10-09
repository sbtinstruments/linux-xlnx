/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SBT Instruments Lock-in Amplifier
 *
 * Copyright (c) 2019, Frederik Peter Aalund <fpa@sbtinstruments.com>
 */
#include <asm/io.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/ioport.h>
#include <linux/log2.h>

#include "hw.h"

struct lockamp_gen_control LOCKAMP_GEN1_CONTROL = {
	.scale = LOCKAMP_REG_GEN1_SCALE,
	.step_int = LOCKAMP_REG_GEN1_STEP_INT,
	.step_frac = LOCKAMP_REG_GEN1_STEP_FRAC,
	.lock_phase = LOCKAMP_REG_GEN1_LOCK_PHASE,
};

struct lockamp_gen_control LOCKAMP_GEN2_CONTROL = {
	.scale = LOCKAMP_REG_GEN2_SCALE,
	.step_int = LOCKAMP_REG_GEN2_STEP_INT,
	.step_frac = LOCKAMP_REG_GEN2_STEP_FRAC,
	.lock_phase = LOCKAMP_REG_GEN2_LOCK_PHASE,
};

void lockamp_reset(struct lockamp *lockamp)
{
	gpiod_set_value(lockamp->reset, 1);
	msleep(1); /* Arbitrary */
	gpiod_set_value(lockamp->reset, 0);
}

int lockamp_set_decimation(struct lockamp *lockamp, u32 value)
{
	int ret;
	u32 hb_filters;
	u32 fir_cycles;
	/* 1:  Sample rate ~367 KHz (time step: 2728 ns)
	 * 2:  Sample rate ~184 KHz (time step: 5456 ns)
	 * 4:  Sample rate  ~92 KHz (time step: 10912 ns)
	 * 8:  Sample rate  ~46 KHz (time step: 21824 ns)
	 * 16: Sample rate  ~23 KHz (time step: 43648 ns)
	 */
	if (!(1 == value || 2 == value || 4 == value || 8 == value || 16 == value)) {
		return -EINVAL;
	}
	/*
	 * 1  -> 0
	 * 2  -> 1
	 * 4  -> 2
	 * 8  -> 3
	 * 16 -> 4
	 */
	hb_filters = ilog2(value);
	/* Set half-band filters */
	ret = regmap_write(lockamp->regmap, LOCKAMP_REG_HB_FILTERS, hb_filters);
	if (ret < 0) {
		return ret;
	}
	/* Set FIR cycles accordingly */
	fir_cycles = min(511, 341 * (1 << hb_filters) / 8 - 6);
	ret = lockamp_set_fir_cycles(lockamp, fir_cycles);
	if (ret < 0) {
		return ret;
	}
	return 0;
}

int lockamp_get_decimation(struct lockamp *lockamp, u32 *value)
{
	u32 hb_filters;
	int ret = regmap_read(lockamp->regmap, LOCKAMP_REG_HB_FILTERS, &hb_filters);
	if (ret < 0) {
		return 0;
	}
	/*
	 * 0 -> 1
	 * 1 -> 2
	 * 2 -> 4
	 * 3 -> 8
	 * 4 -> 16
	 */
	*value = 1 << hb_filters;
	return 0;
}

int lockamp_set_fir_coefs(struct lockamp *lockamp, const s32 *coefs)
{
	return regmap_bulk_write(lockamp->regmap, LOCKAMP_REG_FIR_COEF_BASE,
	                         coefs, LOCKAMP_FIR_COEF_LEN);
}

/* Raw read (not through regmap) */
void lockamp_get_adc_samples(struct lockamp *lockamp, s32 *adc_samples)
{
	int i;
	/* Reset ADC data acquisition */
	iowrite32(0, lockamp->control + LOCKAMP_REG_ADC_BUFFER);
	/* Read ADC data */
	for (i = 0; LOCKAMP_ADC_SAMPLES_SIZE_S32 > i; ++i) {
		adc_samples[i] = ioread32(lockamp->control + LOCKAMP_REG_ADC_BUFFER);
	}
}
