#ifndef LOCKAMP_LOCKAMP_H
#define LOCKAMP_LOCKAMP_H
#include <asm/atomic.h>
#include <linux/cdev.h>
#include <linux/sysfs.h>

/* Class name as it appears in /sys/class  */
#define LOCKAMP_CLASS_NAME  "lockin_amplifier"
#define LOCKAMP_MSG LOCKAMP_CLASS_NAME ": "

#define LOCKAMP_ADC_SAMPLES_SIZE_S32 16384
#define LOCKAMP_ADC_SAMPLES_SIZE (LOCKAMP_ADC_SAMPLES_SIZE_S32 * sizeof(s32))
/* The following define omits various memory barrier instructions
 *
 * E.g., Removes the "dsb sy" instruction in lockamp_fifo_pop on ARM.
 * */
#define LOCKAMP_FAVOR_SPEED_OVER_SAFETY

struct circ_sample_buf {
	struct sample *buf;
	size_t capacity_n;
	ptrdiff_t head;
	ptrdiff_t tail;
};

struct csbuf_snapshot {
	size_t size_n;
	ptrdiff_t head;
	ptrdiff_t tail;
};

struct lockamp {
	struct cdev cdev;
	struct device *dev;
	dev_t chrdev_no;
	u32 __iomem *control;
	u32 __iomem *control2;
	u32 __iomem *fir;

	struct circ_sample_buf signal_buf;
	struct mutex signal_buf_m;
	struct mutex adc_buf_m;
	char *adc_buffer;
	int sample_multiplier;

	atomic_t desyncs;
	int last_desyncs;
	u64 last_start_time_ns;
};

#define LOCKAMP_ENTRIES_PER_SITE 4
struct site_sample {
	s32 hf_re;
	s32 hf_im;
	s32 lf_re;
	s32 lf_im;
} __attribute__((packed));
_Static_assert (LOCKAMP_ENTRIES_PER_SITE * sizeof(s32) == sizeof(struct site_sample), "struct 'site_sample' is not packed on this platform");

#define LOCKAMP_SITES_PER_SAMPLE 2
#define LOCKAMP_ENTRIES_PER_SAMPLE (LOCKAMP_ENTRIES_PER_SITE * LOCKAMP_SITES_PER_SAMPLE)
struct sample {
	struct site_sample sites[LOCKAMP_SITES_PER_SAMPLE];
} __attribute__((packed));
_Static_assert (LOCKAMP_SITES_PER_SAMPLE * sizeof(struct site_sample) == sizeof(struct sample), "struct 'sample' is not packed on this platform");

extern unsigned int lockamp_ma_time_step_ns;
extern u64 lockamp_fifo_read_duration;
extern u64 lockamp_fifo_read_delay;
extern int lockamp_debug1;
extern int lockamp_debug2;
extern int lockamp_debug3;
extern const size_t lockamp_fifo_capacity_n;
#define LOCKAMP_FIR_FILTER_COUNT 8
extern const s32 lockamp_fir_coefs[LOCKAMP_FIR_FILTER_COUNT][512];
extern const struct attribute_group *lockamp_attr_groups[2];
extern struct file_operations lockamp_fops;

#endif /* LOCKAMP_LOCKAMP_H */
