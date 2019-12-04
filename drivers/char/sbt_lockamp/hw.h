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
#define LOCKAMP_REG_FIFO_SIZE       0x004
#define LOCKAMP_REG_FIFO_DATA       0x008
#define LOCKAMP_REG_GEN1_SCALE      0x00C
#define LOCKAMP_REG_GEN2_SCALE      0x038 /* Note the jump in address */
#define LOCKAMP_REG_ADC_BUFFER      0x010
/* Registers 0x014 to 0x027 (both inclusive) are deprecated */
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

#define LOCKAMP_REG_FIR_COEF_BASE   0x800

/* Note that the time step is exact. That is, it has no fractional part
 * which is why it can be safely stored in an integer. */
#define LOCKAMP_BASE_TIME_STEP      2728
#define LOCKAMP_GEN_SCALE_MIN 0
/* s18 max */
#define LOCKAMP_GEN_SCALE_MAX 131071

struct lockamp_gen_control
{
	u8 scale;
	u8 step_int;
	u8 step_frac;
	u8 lock_phase;
};

extern struct lockamp_gen_control LOCKAMP_GEN1_CONTROL;
extern struct lockamp_gen_control LOCKAMP_GEN2_CONTROL;

static inline u32 lockamp_version(struct lockamp *lockamp)
{
	return ioread32(lockamp->control + LOCKAMP_REG_VERSION);
}

static inline u32 lockamp_fifo_size_s32(struct lockamp *lockamp)
{
	return ioread32(lockamp->control + LOCKAMP_REG_FIFO_SIZE);
}

static inline u32 lockamp_fifo_size_n(struct lockamp *lockamp)
{
	/*
	 * Note that the FIFO contains s32 elements. Therefore, it is possible that
	 * there are not enough elements to construct a complete sample.
	 * E.g., 3 entries of 's32's. Fortunately, when we divide by
	 * entries_per_sample, we automatically truncate 'incomplete' samples away.
	 */
	return lockamp_fifo_size_s32(lockamp) / LOCKAMP_ENTRIES_PER_SAMPLE;
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

static inline s32 lockamp_get_gen_scale(struct lockamp *lockamp, struct lockamp_gen_control *gen)
{
	return ioread32(lockamp->control + gen->scale) >> 14; /* Only 18 MSB are used */
}

static inline void lockamp_set_gen_scale(struct lockamp *lockamp, struct lockamp_gen_control *gen, u32 value)
{
	iowrite32(value << 14, lockamp->control + gen->scale); /* Only 18 MSB are used */
}

static inline u32 lockamp_get_gen_step_int(struct lockamp *lockamp, struct lockamp_gen_control *gen)
{
	return ioread32(lockamp->control + gen->step_int);
}

static inline void lockamp_set_gen_step_int(struct lockamp *lockamp, struct lockamp_gen_control *gen, u32 value)
{
	iowrite32(value, lockamp->control + gen->step_int);
}

static inline u16 lockamp_get_gen_step_frac(struct lockamp *lockamp, struct lockamp_gen_control *gen)
{
	return ioread16(lockamp->control + gen->step_frac);
}

static inline void lockamp_set_gen_step_frac(struct lockamp *lockamp, struct lockamp_gen_control *gen, u16 value)
{
	iowrite16(value, lockamp->control + gen->step_frac);
}

static inline u32 lockamp_get_gen_lock_phase(struct lockamp *lockamp, struct lockamp_gen_control *gen)
{
	return ioread32(lockamp->control + gen->lock_phase);
}

static inline void lockamp_set_gen_lock_phase(struct lockamp *lockamp, struct lockamp_gen_control *gen, u32 value)
{
	iowrite32(value, lockamp->control + gen->lock_phase);
}

static inline u32 lockamp_get_ma_length(struct lockamp *lockamp)
{
	return ioread32(lockamp->control + LOCKAMP_REG_MA_LENGTH);
}

static inline void lockamp_set_ma_length(struct lockamp *lockamp, u32 value)
{
	iowrite32(value, lockamp->control + LOCKAMP_REG_MA_LENGTH);
}

static inline u32 lockamp_get_ma_scale(struct lockamp *lockamp)
{
	return ioread32(lockamp->control + LOCKAMP_REG_MA_SCALE);
}

static inline void lockamp_set_ma_scale(struct lockamp *lockamp, u32 value)
{
	iowrite32(value, lockamp->control + LOCKAMP_REG_MA_SCALE);
}

static inline u32 lockamp_get_cic_length(struct lockamp *lockamp)
{
	return ioread32(lockamp->control + LOCKAMP_REG_CIC_LENGTH);
}

static inline void lockamp_set_cic_length(struct lockamp *lockamp, u32 value)
{
	iowrite32(value, lockamp->control + LOCKAMP_REG_CIC_LENGTH);
}

static inline u32 lockamp_get_cic_scale(struct lockamp *lockamp)
{
	return ioread32(lockamp->control + LOCKAMP_REG_CIC_SCALE);
}

static inline void lockamp_set_cic_scale(struct lockamp *lockamp, u32 value)
{
	iowrite32(value, lockamp->control + LOCKAMP_REG_CIC_SCALE);
}

static inline u32 lockamp_get_dac_data_bits(struct lockamp *lockamp)
{
	return ioread32(lockamp->control + LOCKAMP_REG_DAC_DATA_BITS);
}

static inline void lockamp_set_dac_data_bits(struct lockamp *lockamp, u32 value)
{
	iowrite32(value, lockamp->control + LOCKAMP_REG_DAC_DATA_BITS);
}

static inline s32 lockamp_get_debug1(struct lockamp *lockamp)
{
	return ioread32(lockamp->control + LOCKAMP_REG_CONFIG_CONTROL);
}

static inline void lockamp_set_debug1(struct lockamp *lockamp, u32 value)
{
	iowrite32(value, lockamp->control + LOCKAMP_REG_CONFIG_CONTROL);
}

static inline u32 lockamp_get_fir_cycles(struct lockamp *lockamp)
{
	return ioread32(lockamp->control + LOCKAMP_REG_FIR_CYCLES) & 0b111111111;
}

static inline void lockamp_set_fir_cycles(struct lockamp *lockamp, u32 value)
{
	iowrite32(value, lockamp->control + LOCKAMP_REG_FIR_CYCLES);
}

u32 lockamp_get_decimation(struct lockamp *lockamp);
int lockamp_set_decimation(struct lockamp *lockamp, u32 value);

static inline unsigned int lockamp_get_time_step_ns(struct lockamp *lockamp)
{
	return lockamp_get_decimation(lockamp) * LOCKAMP_BASE_TIME_STEP;
}

static inline u64 lockamp_get_duration_ns(struct lockamp *lockamp, size_t size_n)
{
	return lockamp_get_time_step_ns(lockamp) * size_n;
}

static inline unsigned long lockamp_get_read_delay_ns(struct lockamp *lockamp)
{
	/* The time it takes to read half of the FIFO. */
	return LOCKAMP_FIFO_CAPACITY_N / 2 * lockamp_get_time_step_ns(lockamp);
}

static inline s32 lockamp_fifo_pop(struct lockamp *lockamp)
{
#ifdef CONFIG_SBT_LOCKAMP_FIFO_POP_RELAXED
	return readl_relaxed(lockamp->control + LOCKAMP_REG_FIFO_DATA);
#else
	return ioread32(lockamp->control + LOCKAMP_REG_FIFO_DATA);
#endif
}

static __maybe_unused inline s32 lockamp_fifo_pop_dbg(struct lockamp *lockamp)
{
	u32 raw;
	s32 result; /* signed, so that right-shifts will be arithmetic */
#ifdef CONFIG_SBT_LOCKAMP_FIFO_POP_RELAXED
	raw = readl_relaxed(lockamp->control + LOCKAMP_REG_FIFO_DATA);
#else
	raw = ioread32(lockamp->control + LOCKAMP_REG_FIFO_DATA);
#endif
	/* Bits 29:31 (the 3 MSBs) are for debug. Null these bits by bitwise
	 * left-shift. Also, convert from u32 to s32. We assume that the compiler
	 * does a reinterpretative cast. I.e., all bits are unchanged and only
	 * the interpretation of the bits is changed (from unsigned to signed).
	 */
	result = raw << 3;
	/* After nulling the debug bits, we need to right-shift bits 0:28 back
	 * into place. Furthermore, said bits must be sign-extended during the
	 * shift. We use a bitwise right-shift to do the sign extension. Note
	 * that right-shifts may or may not sign-extend. This depends on the
	 * compiler (but it is usually the case). It works for GCC 7.2 targeting
	 * arm-linux-gnueabihf.
	 * Reference: https://stackoverflow.com/a/7636/554283
	 */
	result >>= 3;
	return result;
}

static inline void lockamp_fifo_pop_sample(struct lockamp *lockamp, struct sample *s)
{
	int i;
	for (i = 0; LOCKAMP_SITES_PER_SAMPLE > i; ++i) {
		s->sites[i].hf_re = lockamp_fifo_pop(lockamp) * lockamp->sample_multipliers[i];
		s->sites[i].hf_im = lockamp_fifo_pop(lockamp) * lockamp->sample_multipliers[i];
		s->sites[i].lf_re = lockamp_fifo_pop(lockamp) * lockamp->sample_multipliers[i];
		s->sites[i].lf_im = lockamp_fifo_pop(lockamp) * lockamp->sample_multipliers[i];
	}
}

extern size_t lockamp_fifo_move_to_sbuf(struct lockamp *lockamp);
extern void lockamp_set_filter_coefficients(struct lockamp *lockamp, const s32 *coefs);
extern void lockamp_get_adc_samples(struct lockamp *lockamp, s32 *adc_samples);

#endif /* _LOCKAMP_HW_H_ */
