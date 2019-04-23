#ifndef _LOCKAMP_HW_H_
#define _LOCKAMP_HW_H_

#include <linux/platform_device.h>
#include <asm/io.h>

#include "lockin_amplifier.h"

/* Note that the time step is exact. That is, it has no fractional part
 * which is why it can be safely stored in an integer. */
#define LOCKAMP_BASE_TIME_STEP 2728
#define LOCKAMP_GENERATOR_SCALE_MIN 0
/* s18 max */
#define LOCKAMP_GENERATOR_SCALE_MAX 131071

extern int lockamp_init(struct lockamp *lockamp, struct platform_device *pdev);
extern void lockamp_exit(struct lockamp *lockamp);

static inline u32 lockamp_version(struct lockamp *lockamp)
{
	return ioread32(lockamp->control);
}

static inline u32 lockamp_fifo_size_s32(struct lockamp *lockamp)
{
	return ioread32(lockamp->control + 1);
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

static inline int lockamp_adjust_generator_scale(s32* scale)
{
	static const s32 sensitivity_threshold = 42;
	if (LOCKAMP_GENERATOR_SCALE_MIN > *scale
	                                  || LOCKAMP_GENERATOR_SCALE_MAX < *scale) {
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

static inline s32 lockamp_get_generator1_scale(struct lockamp *lockamp)
{
	return ioread32(lockamp->control + 3) >> 14;
}

static inline void lockamp_set_generator1_scale(struct lockamp *lockamp, s32 scale)
{
	iowrite32(scale << 14, lockamp->control + 3);
}

static inline s32 lockamp_get_generator2_scale(struct lockamp *lockamp)
{
	return ioread32(lockamp->control2 + 7) >> 14;
}

static inline void lockamp_set_generator2_scale(struct lockamp *lockamp, s32 scale)
{
	iowrite32(scale << 14, lockamp->control2 + 7);
}

static inline u32 lockamp_get_generator1_step(struct lockamp *lockamp)
{
	return ioread32(lockamp->control2 + 1);
}

static inline void lockamp_set_generator1_step(struct lockamp *lockamp, u32 value)
{
	iowrite32(value, lockamp->control2 + 1);
}

static inline u32 lockamp_get_generator2_step(struct lockamp *lockamp)
{
	return ioread32(lockamp->control2 + 2);
}

static inline void lockamp_set_generator2_step(struct lockamp *lockamp, u32 value)
{
	iowrite32(value, lockamp->control2 + 2);
}

static inline u32 lockamp_get_dac_data_bits(struct lockamp *lockamp)
{
	return ioread32(lockamp->control2 + 3);
}

static inline void lockamp_set_dac_data_bits(struct lockamp *lockamp, u32 value)
{
	iowrite32(value, lockamp->control2 + 3);
}

static inline s32 lockamp_get_debug1(struct lockamp *lockamp)
{
	return ioread32(lockamp->control2 + 6);
}

static inline void lockamp_set_debug1(struct lockamp *lockamp, u32 value)
{
	iowrite32(value, lockamp->control2 + 6);
}

static inline u32 lockamp_get_fir_cycles(struct lockamp *lockamp)
{
	return ioread32(lockamp->control2 + 5) & 0b111111111;
}

static inline void lockamp_set_fir_cycles(struct lockamp *lockamp, u32 value)
{
	iowrite32(value, lockamp->control2 + 5);
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
	return lockamp_fifo_capacity_n / 2 * lockamp_get_time_step_ns(lockamp);
}

static inline u32 lockamp_fifo_pop(struct lockamp *lockamp)
{
#ifdef LOCKAMP_FAVOR_SPEED_OVER_SAFETY
	return *(s32 *)(lockamp->control + 2);
#else
	return ioread32(lockamp->control + 2);
#endif
}

static inline void lockamp_fifo_pop_sample(struct lockamp *lockamp, struct sample *s)
{
	int i;
	for (i = 0; LOCKAMP_SITES_PER_SAMPLE > i; ++i) {
		s->sites[i].hf_re = lockamp_fifo_pop(lockamp) * lockamp->sample_multiplier;
		s->sites[i].hf_im = lockamp_fifo_pop(lockamp) * lockamp->sample_multiplier;
		s->sites[i].lf_re = lockamp_fifo_pop(lockamp) * lockamp->sample_multiplier;
		s->sites[i].lf_im = lockamp_fifo_pop(lockamp) * lockamp->sample_multiplier;
	}
}

extern size_t lockamp_fifo_move_to_sbuf(struct lockamp *lockamp);
extern void lockamp_set_filter_coefficients(struct lockamp *lockamp, const s32 coefficients[512]);
extern void lockamp_get_adc_samples(struct lockamp *lockamp, s32 adc_samples[LOCKAMP_ADC_SAMPLES_SIZE_S32]);

#endif /* _LOCKAMP_HW_H_ */
