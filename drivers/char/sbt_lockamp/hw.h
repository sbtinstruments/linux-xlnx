/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SBT Instruments Lock-in Amplifier
 *
 * Copyright (c) 2019, Frederik Peter Aalund <fpa@sbtinstruments.com>
 */
#ifndef _LOCKAMP_HW_H_
#define _LOCKAMP_HW_H_

#include <asm/io.h>
#include <linux/platform_device.h>

#include "lockin_amplifier.h"

#define LOCKAMP_REG_VERSION         0x000
/* 0x004 and 0x008 were the FIFO fill level and the FIFO pop (retired) */
#define LOCKAMP_REG_GEN1_SCALE      0x00C
#define LOCKAMP_REG_GEN2_SCALE      0x038 /* Note the jump in address */
#define LOCKAMP_REG_ADC_BUFFER      0x010
/* The sample stream (sbt/drupe#40) */
#define LOCKAMP_REG_STREAM_CONTROL  0x01C
#define LOCKAMP_STREAM_ENABLE       BIT(0)
#define LOCKAMP_REG_STREAM_PERIOD   0x020 /* Samples per packet (tlast) */
#define LOCKAMP_REG_SAMPLE_SIGN     0x024 /* Bit n set: negate site n */
#define LOCKAMP_REG_STREAM_STATUS   0x03C
#define LOCKAMP_STREAM_OVERFLOW     BIT(0) /* Sticky; write 1 to clear */
#define LOCKAMP_VERSION_STREAM      0xABBA0023
/* Registers 0x014 to 0x018 (both inclusive) are deprecated */
#define LOCKAMP_REG_DAC_DATA_BITS   0x028
#define LOCKAMP_REG_HB_FILTERS      0x02C
#define LOCKAMP_REG_FIR_CYCLES      0x030
#define LOCKAMP_REG_CONFIG_CONTROL  0x034

#define LOCKAMP_REG_MA_LENGTH       0x040
#define LOCKAMP_REG_CIC_LENGTH      0x044
#define LOCKAMP_REG_MA_SCALE        0x048
#define LOCKAMP_REG_CIC_SCALE       0x04C
#define LOCKAMP_REG_GEN1_STEP_INT   0x050
#define LOCKAMP_REG_GEN1_STEP_FRAC  0x054
#define LOCKAMP_REG_GEN1_LOCK_PHASE 0x058 /* 16 bit register */
#define LOCKAMP_REG_GEN2_STEP_INT   0x060
#define LOCKAMP_REG_GEN2_STEP_FRAC  0x064
#define LOCKAMP_REG_GEN2_LOCK_PHASE 0x068 /* 16 bit register */

#define LOCKAMP_REG_DEBUG0          0x080
#define LOCKAMP_REG_DEBUG_CONTROL   0x084
#define LOCKAMP_MA_RESET_BIT        BIT(1)

#define LOCKAMP_REG_FIR_COEF_BASE   0x800

/* Corresponding to 125 MHz */
#define LOCKAMP_BASE_TIME_STEP      8
/* How long the ADC's clock takes to start after a reload, with margin */
#define LOCKAMP_RELOAD_CLOCK_MS     20
#define LOCKAMP_GEN_SCALE_MIN       0
/* s18 max */
#define LOCKAMP_GEN_SCALE_MAX       131071

struct lockamp_gen_control
{
	u8 scale;
	u8 step_int;
	u8 step_frac;
	u8 lock_phase;
};

extern struct lockamp_gen_control LOCKAMP_GEN1_CONTROL;
extern struct lockamp_gen_control LOCKAMP_GEN2_CONTROL;

void lockamp_reset(struct lockamp *lockamp);

static inline int lockamp_version(struct lockamp *lockamp, u32 *value)
{
	return regmap_read(lockamp->regmap, LOCKAMP_REG_VERSION, value);
}

static inline int lockamp_adjust_gen_scale(s32* scale)
{
	static const s32 sensitivity_threshold = 42;
	if (LOCKAMP_GEN_SCALE_MIN > *scale || LOCKAMP_GEN_SCALE_MAX < *scale) {
		return -ERANGE;
	}
	/*
     * values in [-sensitivity_threshold;-1] and [1;sensitivity_threshold] are
     * seemingly erroneous. Setting them to 0 provides a more meaningful
     * result.
     */
	if (-sensitivity_threshold <= *scale && sensitivity_threshold >= *scale) {
		*scale = 0;
	}
	return 0;
}

static inline int lockamp_get_gen_scale(struct lockamp *lockamp,
                                        struct lockamp_gen_control *gen,
                                        u32 *value)
{
	int ret = regmap_read(lockamp->regmap, gen->scale, value);
	if (ret < 0) {
		return ret;
	}
	/* Only 18 MSB are used */
	*value >>= 14;
	return 0;
}

static inline int lockamp_set_gen_scale(struct lockamp *lockamp,
                                        struct lockamp_gen_control *gen,
                                        u32 value)
{
	/* Only 18 MSB are used */
	value <<= 14;
	return regmap_write(lockamp->regmap, gen->scale, value);
}

static inline int lockamp_get_gen_step_int(struct lockamp *lockamp,
                                           struct lockamp_gen_control *gen,
                                           u32 *value)
{
	return regmap_read(lockamp->regmap, gen->step_int, value);
}

static inline int lockamp_set_gen_step_int(struct lockamp *lockamp,
                                           struct lockamp_gen_control *gen,
                                           u32 value)
{
	return regmap_write(lockamp->regmap, gen->step_int, value);
}

static inline int lockamp_get_gen_step_frac(struct lockamp *lockamp,
                                            struct lockamp_gen_control *gen,
                                            u16 *value)
{
	u32 value32;
	int ret = regmap_read(lockamp->regmap, gen->step_frac, &value32);
	if (ret < 0) {
		return ret;
	}
	*value = value32;
	return 0;
}

static inline int lockamp_set_gen_step_frac(struct lockamp *lockamp,
                                            struct lockamp_gen_control *gen,
                                            u16 value)
{
	u32 value32 = value;
	return regmap_write(lockamp->regmap, gen->step_frac, value32);
}

static inline int lockamp_get_gen_lock_phase(struct lockamp *lockamp,
                                             struct lockamp_gen_control *gen,
                                             u32 *value)
{
	return regmap_read(lockamp->regmap, gen->lock_phase, value);
}

static inline int lockamp_set_gen_lock_phase(struct lockamp *lockamp,
                                             struct lockamp_gen_control *gen,
                                             u32 value)
{
	return regmap_write(lockamp->regmap, gen->lock_phase, value);
}

static inline int lockamp_get_ma_length(struct lockamp *lockamp, u32 *value)
{
	return regmap_read(lockamp->regmap, LOCKAMP_REG_MA_LENGTH, value);
}

static inline int lockamp_set_ma_length(struct lockamp *lockamp, u32 value)
{
	return regmap_write(lockamp->regmap, LOCKAMP_REG_MA_LENGTH, value);
}

static inline int lockamp_get_ma_scale(struct lockamp *lockamp, u32 *value)
{
	return regmap_read(lockamp->regmap, LOCKAMP_REG_MA_SCALE, value);
}

static inline int lockamp_set_ma_scale(struct lockamp *lockamp, u32 value)
{
	return regmap_write(lockamp->regmap, LOCKAMP_REG_MA_SCALE, value);
}

static inline int lockamp_get_cic_length(struct lockamp *lockamp, u32 *value)
{
	return regmap_read(lockamp->regmap, LOCKAMP_REG_CIC_LENGTH, value);
}

static inline int lockamp_set_cic_length(struct lockamp *lockamp, u32 value)
{
	return regmap_write(lockamp->regmap, LOCKAMP_REG_CIC_LENGTH, value);
}

static inline int lockamp_get_cic_scale(struct lockamp *lockamp, u32 *value)
{
	return regmap_read(lockamp->regmap, LOCKAMP_REG_CIC_SCALE, value);
}

static inline int lockamp_set_cic_scale(struct lockamp *lockamp, u32 value)
{
	return regmap_write(lockamp->regmap, LOCKAMP_REG_CIC_SCALE, value);
}

static inline int lockamp_get_dac_data_bits(struct lockamp *lockamp, u32 *value)
{
	return regmap_read(lockamp->regmap, LOCKAMP_REG_DAC_DATA_BITS, value);
}

static inline int lockamp_set_dac_data_bits(struct lockamp *lockamp, u32 value)
{
	return regmap_write(lockamp->regmap, LOCKAMP_REG_DAC_DATA_BITS, value);
}

static inline int lockamp_get_debug_control(struct lockamp *lockamp, u32 *value)
{
	return regmap_read(lockamp->regmap, LOCKAMP_REG_DEBUG_CONTROL, value);
}

static inline int lockamp_set_debug_control(struct lockamp *lockamp, u32 value)
{
	return regmap_write(lockamp->regmap, LOCKAMP_REG_DEBUG_CONTROL, value);
}

static inline int lockamp_get_config(struct lockamp *lockamp, u32 *value)
{
	return regmap_read(lockamp->regmap, LOCKAMP_REG_CONFIG_CONTROL, value);
}

static inline int lockamp_set_config(struct lockamp *lockamp, u32 value)
{
	return regmap_write(lockamp->regmap, LOCKAMP_REG_CONFIG_CONTROL, value);
}

static inline int lockamp_get_fir_cycles(struct lockamp *lockamp, u32 *value)
{
	int ret;
	ret = regmap_read(lockamp->regmap, LOCKAMP_REG_FIR_CYCLES, value);
	if (ret < 0) {
		return ret;
	}
	/* Only the 9 LSBs are used */
	*value &= 0b111111111;
	return 0;
}

static inline int lockamp_set_fir_cycles(struct lockamp *lockamp, u32 value)
{
	/* Only the 9 LSBs are used */
	value &= 0b111111111;
	return regmap_write(lockamp->regmap, LOCKAMP_REG_FIR_CYCLES, value);
}

int lockamp_get_decimation(struct lockamp *lockamp, u32 *value);
int lockamp_set_decimation(struct lockamp *lockamp, u32 value);

static inline int lockamp_get_time_step_ns(struct lockamp *lockamp, unsigned int *value)
{
	int ret;
	u32 cic_length;
	u32 decimation;
	ret = lockamp_get_cic_length(lockamp, &cic_length);
	if (ret < 0) {
		return ret;
	}
	ret = lockamp_get_decimation(lockamp, &decimation);
	if (ret < 0) {
		return ret;
	}
	/* The data time step depends on three things:
	 *   1) The input clock (125 MHz freq; 8 ns time step)
	 *   2) The length of the CIC filter
	 *   3) The number of half band filters (indirectly, the so-called
	 *      decimation factor).
	 */
	*value = LOCKAMP_BASE_TIME_STEP * cic_length * decimation;
	return 0;
}

static inline int lockamp_get_duration_ns(struct lockamp *lockamp, size_t size_n, u64 *value)
{
	unsigned int time_step_ns;
	int ret = lockamp_get_time_step_ns(lockamp, &time_step_ns);
	if (ret < 0) {
		return ret;
	}
	*value = time_step_ns * size_n;
	return 0;
}

static inline int lockamp_reset_ma_filter(struct lockamp *lockamp)
{
	int ret;
	/* Assert reset pin */
	ret = regmap_update_bits(lockamp->regmap, LOCKAMP_REG_DEBUG_CONTROL,
	                         LOCKAMP_MA_RESET_BIT, LOCKAMP_MA_RESET_BIT);
	if (ret < 0) {
		return 0;
	}
	/* Arbitrary delay */
	msleep(1);
	/* Deassert reset pin */
	ret = regmap_update_bits(lockamp->regmap, LOCKAMP_REG_DEBUG_CONTROL,
	                         LOCKAMP_MA_RESET_BIT, 0x0);
	return ret;
}

extern int lockamp_set_fir_coefs(struct lockamp *lockamp, const s32 *coefs);
extern void lockamp_get_adc_samples(struct lockamp *lockamp, s32 *adc_samples);

#endif /* _LOCKAMP_HW_H_ */
