#include <asm/io.h>
#include <linux/circ_buf.h>
#include <linux/ioport.h>
#include <linux/log2.h>
#include <linux/platform_device.h>

#include "hw.h"

#define LOCKAMP_FIFO_CAPACITY 131072
const size_t lockamp_fifo_capacity_n = LOCKAMP_FIFO_CAPACITY / sizeof(struct sample);

int lockamp_init(struct lockamp *lockamp, struct platform_device *pdev)
{
	struct resource *resource;
	/* lockamp-control1 */
	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, "lockamp-control1");
	if (NULL == resource) {
		dev_alert(lockamp->dev, "Unable to get resource 'lockamp-control1' from the platform device.\n");
		return -ENOMEM;
	}
	lockamp->control = devm_ioremap_resource(lockamp->dev, resource);
	if (IS_ERR(lockamp->control)) {
		dev_alert(lockamp->dev, "Unable to ioremap resource 'lockamp-control1'.\n");
		return PTR_ERR(lockamp->control);
	}
	/* lockamp-control2 */
	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, "lockamp-control2");
	if (NULL == resource) {
		dev_alert(lockamp->dev, "Unable to get resource 'lockamp-control2' from the platform device.\n");
		return -ENOMEM;
	}
	lockamp->control2 = devm_ioremap_resource(lockamp->dev, resource);
	if (IS_ERR(lockamp->control)) {
		dev_alert(lockamp->dev, "Unable to ioremap resource 'lockamp-control2'.\n");
		return PTR_ERR(lockamp->control);
	}
	/* lockamp-fir */
	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, "lockamp-fir");
	if (NULL == resource) {
		dev_alert(lockamp->dev, "Unable to get resource 'lockamp-fir' from the platform device.\n");
		return -ENOMEM;
	}
	lockamp->fir = devm_ioremap_resource(lockamp->dev, resource);
	if (IS_ERR(lockamp->control)) {
		dev_alert(lockamp->dev, "Unable to ioremap resource 'lockamp-fir'.\n");
		return PTR_ERR(lockamp->control);
	}
	return 0;
}

void lockamp_exit(struct lockamp *lockamp)
{
	/* struct platform_device *pdev = lockamp->dev->parent; */
}

int lockamp_set_decimation(struct lockamp *lockamp, u32 value)
{
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
	iowrite32(hb_filters, lockamp->control2 + 4);
	/* Set FIR cycles accordingly */
	fir_cycles = min(511, 341 * (1 << hb_filters) / 8 - 6);
	lockamp_set_fir_cycles(lockamp, fir_cycles);
	return 0;
}

u32 lockamp_get_decimation(struct lockamp *lockamp)
{
	u32 hb_filters = ioread32(lockamp->control2 + 4);
	/*
	 * 0 -> 1
	 * 1 -> 2
	 * 2 -> 4
	 * 3 -> 8
	 * 4 -> 16
	 */
	return 1 << hb_filters;
}

void lockamp_set_filter_coefficients(struct lockamp *lockamp, const s32 coefficients[512])
{
	size_t i;
	for (i = 0; 512 > i; ++i) {
		iowrite32(coefficients[i], lockamp->fir + i);
	}
}

void lockamp_get_adc_samples(struct lockamp *lockamp, s32 adc_samples[LOCKAMP_ADC_SAMPLES_SIZE_S32])
{
	int i;
	/* Reset ADC data acquisition */
	iowrite32(0, lockamp->control + 4);
	/* Read ADC data */
	for (i = 0; LOCKAMP_ADC_SAMPLES_SIZE_S32 > i; ++i) {
		adc_samples[i] = ioread32(lockamp->control + 4);
	}
}

size_t lockamp_fifo_move_to_sbuf(struct lockamp *lockamp)
{
	size_t i;
	ptrdiff_t head, tail;
	size_t bounded_size_n, signal_buf_space_n;
	struct circ_sample_buf *sbuf = &lockamp->signal_buf;
	size_t fifo_size_n = lockamp_fifo_size_n(lockamp);
	size_t sbuf_cap_minus_one = sbuf->capacity_n - 1;
	if (fifo_size_n > lockamp_fifo_capacity_n * 3 / 4) {
		/* Note that these 'print statements' are slow. May take 5-10 ms. */
		dev_warn_ratelimited(lockamp->dev, "FIFO is over 3/4 filled (%d/%d). Data loss may be imminent.\n",
									  fifo_size_n, lockamp_fifo_capacity_n);
	}

	head = sbuf->head;
	tail = READ_ONCE(sbuf->tail);
	signal_buf_space_n = CIRC_SPACE(head, tail, sbuf->capacity_n);

	if (0 == signal_buf_space_n) {
		atomic_inc(&lockamp->desyncs);
		/* Note that these 'print statements' are slow. May take 5-10 ms. */
		dev_warn_ratelimited(lockamp->dev, "Data loss. There is no more space in the signal buffer.\n");
	}

	bounded_size_n = min(signal_buf_space_n, fifo_size_n);
	for (i = 0; bounded_size_n != i; ++i) {
		lockamp_fifo_pop_sample(lockamp, &sbuf->buf[head]);
		head = (head + 1) & sbuf_cap_minus_one;
	}
	smp_store_release(&sbuf->head, head);

	return bounded_size_n;
}
