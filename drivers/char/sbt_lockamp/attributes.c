#include <linux/device.h>

#include "lockin_amplifier.h"
#include "hw.h"

/* decimation_factor */
static ssize_t decimation_factor_show(
	struct device *device,
	struct device_attribute *attr,
	char *buf)
{
	struct lockamp *lockamp = dev_get_drvdata(device);
	return scnprintf(buf, PAGE_SIZE, "%d\n", lockamp_get_decimation(lockamp));
}
static ssize_t decimation_factor_store(
	struct device *device,
	struct device_attribute *attr,
	const char *buf,
	size_t count)
{
	struct lockamp *lockamp = dev_get_drvdata(device);
	u32 decimation_factor;
	int result = kstrtou32(buf, 0, &decimation_factor);
	if (0 != result) {
		return result;
	}
	result = lockamp_set_decimation(lockamp, decimation_factor);
	if (0 != result) {
		return result;
	}
	return count;
}
DEVICE_ATTR(decimation_factor, S_IRUGO | S_IWUSR, decimation_factor_show, decimation_factor_store);

/* time_step_ns */
static ssize_t time_step_ns_show(
	struct device *device,
	struct device_attribute *attr,
	char *buf)
{
	struct lockamp *lockamp = dev_get_drvdata(device);
	unsigned int time_step_ns = lockamp_get_time_step_ns(lockamp);
	return snprintf(buf, PAGE_SIZE, "%d\n", time_step_ns);
}
DEVICE_ATTR(time_step_ns, S_IRUGO, time_step_ns_show, NULL);

/* fir_cycles */
static ssize_t fir_cycles_show(
	struct device *device,
	struct device_attribute *attr,
	char *buf)
{
	struct lockamp *lockamp = dev_get_drvdata(device);
	u32 fir_cycles = lockamp_get_fir_cycles(lockamp);
	return snprintf(buf, PAGE_SIZE, "%d\n", fir_cycles);
}
DEVICE_ATTR(fir_cycles, S_IRUGO, fir_cycles_show, NULL);

/* signal_max_amplitude_e1 */
static int signal_max_amplitude_e1 = 1784331945;
DEVICE_INT_ATTR(signal_max_amplitude_e1, S_IRUGO, signal_max_amplitude_e1);

/* ma_time_step_ns */
DEVICE_INT_ATTR(ma_time_step_ns, S_IRUGO, lockamp_ma_time_step_ns);

/* signal_buf_capacity */
static ssize_t signal_buf_capacity_show(
	struct device *device,
	struct device_attribute *attr,
	char *buf)
{
	struct lockamp *lockamp = dev_get_drvdata(device);
	size_t capacity = sizeof(struct sample) * lockamp->signal_buf.capacity_n;
	return snprintf(buf, PAGE_SIZE, "%d\n", capacity);
}
DEVICE_ATTR(signal_buf_capacity, S_IRUGO, signal_buf_capacity_show, NULL);

/* generator_scale_min */
static ssize_t generator_scale_min_show(
	struct device *device,
	struct device_attribute *attr,
	char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", LOCKAMP_GENERATOR_SCALE_MIN);
}
DEVICE_ATTR(generator_scale_min, S_IRUGO, generator_scale_min_show, NULL);

/* generator_scale_max */
static ssize_t generator_scale_max_show(
	struct device *device,
	struct device_attribute *attr,
	char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", LOCKAMP_GENERATOR_SCALE_MAX);
}
DEVICE_ATTR(generator_scale_max, S_IRUGO, generator_scale_max_show, NULL);

/* generator1_scale */
static ssize_t generator1_scale_show(
	struct device *device,
	struct device_attribute *attr,
	char *buf)
{
	struct lockamp *lockamp = dev_get_drvdata(device);
	return scnprintf(buf, PAGE_SIZE, "%d\n", lockamp_get_generator1_scale(lockamp));
}
static ssize_t generator1_scale_store(
	struct device *device,
	struct device_attribute *attr,
	const char *buf,
	size_t count)
{
	struct lockamp *lockamp = dev_get_drvdata(device);
	s32 scale;
	int result = kstrtos32(buf, 0, &scale);
	if (0 != result)
		return result;
	result = lockamp_adjust_generator_scale(&scale);
	if (0 != result)
		return result;
	lockamp_set_generator1_scale(lockamp, scale);
	return count;
}
DEVICE_ATTR(generator1_scale, S_IRUGO | S_IWUSR, generator1_scale_show, generator1_scale_store);

/* generator2_scale */
static ssize_t generator2_scale_show(
	struct device *device,
	struct device_attribute *attr,
	char *buf)
{
	struct lockamp *lockamp = dev_get_drvdata(device);
	return scnprintf(buf, PAGE_SIZE, "%d\n", lockamp_get_generator2_scale(lockamp));
}
static ssize_t generator2_scale_store(
	struct device *device,
	struct device_attribute *attr,
	const char *buf,
	size_t count)
{
	struct lockamp *lockamp = dev_get_drvdata(device);
	s32 scale;
	int result = kstrtos32(buf, 0, &scale);
	if (0 != result)
		return result;
	result = lockamp_adjust_generator_scale(&scale);
	if (0 != result)
		return result;
	lockamp_set_generator2_scale(lockamp, scale);
	return count;
}
DEVICE_ATTR(generator2_scale, S_IRUGO | S_IWUSR, generator2_scale_show, generator2_scale_store);

/* generator1_step */
static ssize_t generator1_step_show(
	struct device *device,
	struct device_attribute *attr,
	char *buf)
{
	struct lockamp *lockamp = dev_get_drvdata(device);
	return scnprintf(buf, PAGE_SIZE, "%d\n", lockamp_get_generator1_step(lockamp));
}
static ssize_t generator1_step_store(
	struct device *device,
	struct device_attribute *attr,
	const char *buf,
	size_t count)
{
	struct lockamp *lockamp = dev_get_drvdata(device);
	s32 generator1_step;

	int result = kstrtou32(buf, 0, &generator1_step);
	if (0 != result)
		return result;

	lockamp_set_generator1_step(lockamp, generator1_step);
	return count;
}
DEVICE_ATTR(generator1_step, S_IRUGO | S_IWUSR, generator1_step_show, generator1_step_store);

/* generator2_step */
static ssize_t generator2_step_show(
	struct device *device,
	struct device_attribute *attr,
	char *buf)
{
	struct lockamp *lockamp = dev_get_drvdata(device);
	return scnprintf(buf, PAGE_SIZE, "%d\n", lockamp_get_generator2_step(lockamp));
}
static ssize_t generator2_step_store(
	struct device *device,
	struct device_attribute *attr,
	const char *buf,
	size_t count)
{
	struct lockamp *lockamp = dev_get_drvdata(device);
	s32 generator2_step;

	int result = kstrtou32(buf, 0, &generator2_step);
	if (0 != result)
		return result;

	lockamp_set_generator2_step(lockamp, generator2_step);
	return count;
}
DEVICE_ATTR(generator2_step, S_IRUGO | S_IWUSR, generator2_step_show, generator2_step_store);

/* dac_data_bits */
static ssize_t dac_data_bits_show(
	struct device *device,
	struct device_attribute *attr,
	char *buf)
{
	struct lockamp *lockamp = dev_get_drvdata(device);
	return scnprintf(buf, PAGE_SIZE, "%d\n", lockamp_get_dac_data_bits(lockamp));
}
static ssize_t dac_data_bits_store(
	struct device *device,
	struct device_attribute *attr,
	const char *buf,
	size_t count)
{
	struct lockamp *lockamp = dev_get_drvdata(device);
	s32 dac_data_bits;
	int result = kstrtou32(buf, 0, &dac_data_bits);
	if (0 != result)
		return result;
	if (31 < dac_data_bits)
		return -ERANGE;
	lockamp_set_dac_data_bits(lockamp, dac_data_bits);
	return count;
}
DEVICE_ATTR(dac_data_bits, S_IRUGO | S_IWUSR, dac_data_bits_show, dac_data_bits_store);

/* hw_debug1 */
static ssize_t hw_debug1_show(
	struct device *device,
	struct device_attribute *attr,
	char *buf)
{
	struct lockamp *lockamp = dev_get_drvdata(device);
	return scnprintf(buf, PAGE_SIZE, "%d\n", lockamp_get_debug1(lockamp));
}
static ssize_t hw_debug1_store(
	struct device *device,
	struct device_attribute *attr,
	const char *buf,
	size_t count)
{
	struct lockamp *lockamp = dev_get_drvdata(device);
	u32 hw_debug1;
	int result = kstrtou32(buf, 0, &hw_debug1);
	if (0 != result)
		return result;
	lockamp_set_debug1(lockamp, hw_debug1);
	return count;
}
DEVICE_ATTR(hw_debug1, S_IRUGO | S_IWUSR, hw_debug1_show, hw_debug1_store);

/* fir_filter */
enum fir_filter {
	NONE,
	GROENNING,
	A1,
	A2,
	B1,
	B2,
	C1,
	C2,
};
const s32 lockamp_fir_coefs[LOCKAMP_FIR_FILTER_COUNT][512] = {
	{
		16777215 /* remaining 511 values are 0 */
	},
	{
		162, 206, 212, 150, 0, -258, -628, -1094,
		-1620, -2146, -2586, -2838, -2794, -2350, -1432, 0,
		1924, 4250, 6804, 9338, 11544, 13066, 13544, 12652,
		10146, 5908, 0, -7324, -15590, -24118, -32058, -38446,
		-42294, -42680, -38872, -30438, -17336, 0, 20626, 43098,
		65534, 85720, 101290, 109918, 109558, 98684, 76518, 43216,
		0, -50790, -105732, -160464, -209910, -248604, -271064, -272226,
		-247866, -195004, -112240, 0, 139346, 301492, 480366, 668426,
		857076, 1037190, 1199684, 1336108, 1439204, 1503404, 1525202, 1503404,
		1439204, 1336108, 1199684, 1037190, 857076, 668426, 480366, 301492,
		139346, 0, -112240, -195004, -247866, -272226, -271064, -248604,
		-209910, -160464, -105732, -50790, 0, 43216, 76518, 98684,
		109558, 109918, 101290, 85720, 65534, 43098, 20626, 0,
		-17336, -30438, -38872, -42680, -42294, -38446, -32058, -24118,
		-15590, -7324, 0, 5908, 10146, 12652, 13544, 13066,
		11544, 9338, 6804, 4250, 1924, 0, -1432, -2350,
		-2794, -2838, -2586, -2146, -1620, -1094, -628, -258,
		0, 150, 212, 206, 162
	},
	{
		0, 0, -1, -1, -1, -1, 0, 0, 1, 3, 4, 6, 8, 9, 10, 10, 10, 8, 5, 1, -5, -11, -18, -24, -29, -32, -33, -31, -25, -16, -3, 13, 30, 48, 65, 79, 88, 90, 84, 69, 44, 11, -30, -75, -122, -165, -201, -224, -229, -210, -164, -87, 22, 166, 342, 549, 782, 1036, 1304, 1580, 1857, 2126, 2381, 2615, 2825, 3005, 3155, 3274, 3362, 3421, 3456, 3470, 3468, 3455, 3434, 3411, 3387, 3367, 3352, 3342, 3338, 3339, 3343, 3350, 3356, 3360, 3361, 3357, 3347, 3330, 3305, 3273, 3233, 3186, 3133, 3074, 3009, 2940, 2867, 2789, 2708, 2623, 2533, 2439, 2340, 2236, 2127, 2012, 1891, 1764, 1630, 1491, 1344, 1192, 1032, 867, 694, 515, 330, 138, -61, -267, -479, -699, -926, -1160, -1400, -1649, -1904, -2167, -2437, -2715, -3000, -3292, -3592, -3900, -4215, -4538, -4868, -5206, -5552, -5905, -6267, -6635, -7012, -7397, -7789, -8189, -8596, -9012, -9435, -9865, -10304, -10750, -11204, -11665, -12134, -12610, -13094, -13586, -14085, -14591, -15104, -15625, -16153, -16688, -17230, -17779, -18335, -18898, -19468, -20044, -20627, -21216, -21812, -22414, -23022, -23636, -24257, -24883, -25515, -26152, -26795, -27444, -28097, -28756, -29420, -30089, -30762, -31440, -32123, -32809, -33500, -34195, -34894, -35596, -36301, -37011, -37723, -38438, -39156, -39876, -40599, -41325, -42052, -42781, -43512, -44245, -44979, -45714, -46450, -47186, -47924, -48661, -49399, -50137, -50874, -51611, -52347, -53082, -53817, -54550, -55281, -56011, -56739, -57465, -58188, -58909, -59627, -60342, -61053, -61762, -62466, -63167, -63864, -64557, -65245, -65928, -66607, -67280, -67948, -68610, -69267, -69918, -71502, -72309, -72925, -73261, -73241, -72825, -72024, -70914, -69645, -68438, -67574, -67368, -68137, -70157, -73613, -78555, -84859, -92201, -100051, -107690, -114257, -118821, -120468, -118416, -112127, -101412, -86524, -68211, -47726, -26790, -7490, 7866, 16952, 17653, 8314, -12026, -43421, -84870, -134238, -188277, -242738, -292597, -332363, -356464, -359677, -337580, -286969, -206223, -95571, 42763, 204563, 383770, 572778, 762868, 944734, 1109080, 1247234, 1351735, 1416849, 1438964, 1416849, 1351735, 1247234, 1109080, 944734, 762868, 572778, 383770, 204563, 42763, -95571, -206223, -286969, -337580, -359677, -356464, -332363, -292597, -242738, -188277, -134238, -84870, -43421, -12026, 8314, 17653, 16952, 7866, -7490, -26790, -47726, -68211, -86524, -101412, -112127, -118416, -120468, -118821, -114257, -107690, -100051, -92201, -84859, -78555, -73613, -70157, -68137, -67368, -67574, -68438, -69645, -70914, -72024, -72825, -73241, -73261, -72925, -72309, -71502, -69918, -69267, -68610, -67948, -67280, -66607, -65928, -65245, -64557, -63864, -63167, -62466, -61762, -61053, -60342, -59627, -58909, -58188, -57465, -56739, -56011, -55281, -54550, -53817, -53082, -52347, -51611, -50874, -50137, -49399, -48661, -47924, -47186, -46450, -45714, -44979, -44245, -43512, -42781, -42052, -41325, -40599, -39876, -39156, -38438, -37723, -37011, -36301, -35596, -34894, -34195, -33500, -32809, -32123, -31440, -30762, -30089, -29420, -28756, -28097, -27444, -26795, -26152, -25515, -24883, -24257, -23636, -23022, -22414, -21812, -21216, -20627, -20044, -19468, -18898, -18335, -17779, -17230, -16688, -16153, -15625, -15104, -14591, -14085, -13586, -13094, -12610, -12134, -11665, -11204, -10750, -10304, -9865, -9435, -9012, -8596, -8189, -7789, -7397, -7012, -6635, -6267, -5905, -5552, -5206, -4868, -4538, -4215, -3900, -3592, -3292, -3000, -2715, -2437, -2167, -1904, -1649, -1400, -1160, -926, -699, -479, -267, -61, 138, 330
	},
	{
		0, 0, -1, -1, -1, -1, 0, 1, 2, 3, 5, 7, 9, 11, 12, 12, 11, 9, 5, 0, -6, -13, -21, -29, -35, -39, -40, -38, -31, -20, -5, 13, 34, 55, 75, 92, 102, 105, 98, 80, 52, 13, -35, -88, -143, -194, -237, -265, -271, -250, -197, -108, 19, 187, 392, 634, 906, 1202, 1514, 1835, 2154, 2463, 2753, 3016, 3245, 3436, 3586, 3693, 3758, 3783, 3770, 3725, 3651, 3555, 3441, 3313, 3176, 3032, 2883, 2731, 2575, 2414, 2247, 2073, 1889, 1693, 1484, 1259, 1018, 760, 486, 194, -114, -438, -775, -1125, -1487, -1860, -2241, -2630, -3027, -3430, -3839, -4252, -4670, -5091, -5516, -5942, -6370, -6799, -7227, -7653, -8076, -8495, -8909, -9316, -9715, -10104, -10483, -10850, -11203, -11542, -11865, -12171, -12458, -12725, -12971, -13194, -13394, -13569, -13717, -13839, -13931, -13994, -14027, -14027, -13995, -13929, -13828, -13692, -13520, -13311, -13064, -12779, -12456, -12094, -11692, -11251, -10770, -10250, -9689, -9090, -8450, -7772, -7055, -6300, -5508, -4678, -3813, -2913, -1979, -1012, -13, 1016, 2074, 3158, 4269, 5403, 6559, 7734, 8928, 10137, 11360, 12593, 13835, 15083, 16335, 17587, 18837, 20082, 21319, 22546, 23758, 24954, 26130, 27283, 28409, 29505, 30569, 31596, 32584, 33530, 34429, 35278, 36075, 36817, 37498, 38118, 38672, 39157, 39570, 39909, 40170, 40350, 40446, 40456, 40378, 40208, 39944, 39584, 39125, 38566, 37905, 37139, 36267, 35288, 34200, 33001, 31691, 30269, 28735, 27086, 25324, 23447, 21456, 19350, 17131, 14799, 12353, 9796, 7128, 4350, 1464, -1529, -4626, -7826, -11126, -14525, -18020, -21607, -25284, -29048, -32896, -36824, -41768, -46014, -50146, -54067, -57701, -61002, -63977, -66699, -69311, -72031, -75134, -78931, -83735, -89814, -97351, -106389, -116799, -128252, -140212, -151956, -162617, -171257, -176958, -178933, -176636, -169876, -158898, -144446, -127767, -110576, -94957, -83211, -77659, -80411, -93118, -116736, -151314, -195846, -248194, -305104, -362325, -414827, -457115, -483614, -489099, -469143, -420539, -341664, -232743, -95999, 64356, 242264, 430122, 619212, 800228, 963878, 1101489, 1205602, 1270483, 1292520, 1270483, 1205602, 1101489, 963878, 800228, 619212, 430122, 242264, 64356, -95999, -232743, -341664, -420539, -469143, -489099, -483614, -457115, -414827, -362325, -305104, -248194, -195846, -151314, -116736, -93118, -80411, -77659, -83211, -94957, -110576, -127767, -144446, -158898, -169876, -176636, -178933, -176958, -171257, -162617, -151956, -140212, -128252, -116799, -106389, -97351, -89814, -83735, -78931, -75134, -72031, -69311, -66699, -63977, -61002, -57701, -54067, -50146, -46014, -41768, -36824, -32896, -29048, -25284, -21607, -18020, -14525, -11126, -7826, -4626, -1529, 1464, 4350, 7128, 9796, 12353, 14799, 17131, 19350, 21456, 23447, 25324, 27086, 28735, 30269, 31691, 33001, 34200, 35288, 36267, 37139, 37905, 38566, 39125, 39584, 39944, 40208, 40378, 40456, 40446, 40350, 40170, 39909, 39570, 39157, 38672, 38118, 37498, 36817, 36075, 35278, 34429, 33530, 32584, 31596, 30569, 29505, 28409, 27283, 26130, 24954, 23758, 22546, 21319, 20082, 18837, 17587, 16335, 15083, 13835, 12593, 11360, 10137, 8928, 7734, 6559, 5403, 4269, 3158, 2074, 1016, -13, -1012, -1979, -2913, -3813, -4678, -5508, -6300, -7055, -7772, -8450, -9090, -9689, -10250, -10770, -11251, -11692, -12094, -12456, -12779, -13064, -13311, -13520, -13692, -13828, -13929, -13995, -14027, -14027, -13994, -13931, -13839, -13717, -13569, -13394, -13194, -12971, -12725, -12458, -12171, -11865, -11542, -11203, -10850, -10483
	},
	{
		0, 0, 1, 1, 0, 0, -1, -2, -2, -2, -1, 1, 3, 4, 5, 3, 1, -3, -7, -9, -9, -5, 1, 9, 15, 17, 14, 5, -7, -19, -27, -27, -19, -2, 19, 37, 45, 40, 20, -10, -43, -67, -73, -55, -14, 40, 91, 121, 115, 66, -21, -124, -212, -248, -195, -26, 266, 671, 1155, 1674, 2175, 2609, 2943, 3160, 3264, 3278, 3235, 3172, 3121, 3103, 3124, 3180, 3255, 3333, 3397, 3437, 3451, 3443, 3422, 3400, 3384, 3379, 3385, 3397, 3409, 3414, 3408, 3389, 3358, 3319, 3276, 3231, 3188, 3145, 3102, 3055, 3003, 2944, 2877, 2802, 2720, 2633, 2542, 2447, 2347, 2244, 2135, 2020, 1899, 1771, 1637, 1496, 1348, 1195, 1036, 870, 698, 520, 335, 143, -56, -261, -474, -693, -920, -1154, -1395, -1643, -1898, -2160, -2430, -2708, -2993, -3285, -3585, -3892, -4207, -4530, -4860, -5198, -5544, -5897, -6258, -6627, -7004, -7388, -7780, -8180, -8587, -9002, -9425, -9856, -10294, -10740, -11194, -11655, -12124, -12600, -13084, -13575, -14074, -14580, -15093, -15613, -16141, -16676, -17218, -17767, -18323, -18885, -19455, -20031, -20614, -21203, -21798, -22400, -23008, -23622, -24242, -24868, -25500, -26138, -26780, -27429, -28082, -28741, -29405, -30073, -30746, -31424, -32106, -32793, -33484, -34178, -34877, -35579, -36284, -36993, -37705, -38420, -39138, -39858, -40581, -41306, -42034, -42763, -43494, -44226, -44960, -45694, -46430, -47167, -47904, -48641, -49379, -50116, -50853, -51590, -52326, -53062, -53796, -54529, -55260, -55989, -56717, -57443, -58166, -58886, -59604, -60319, -61031, -61739, -62444, -63144, -63841, -64533, -65221, -65905, -66583, -67256, -67924, -68586, -69243, -69893, -69536, -69921, -70831, -72452, -74682, -77051, -78804, -79141, -77571, -74246, -70125, -66834, -66204, -69575, -77109, -87375, -97473, -103758, -103054, -93989, -77957, -59267, -44218, -39233, -48528, -72095, -104808, -137207, -157993, -157691, -132357, -86034, -30853, 15690, 35824, 17598, -39948, -124483, -212219, -273953, -283982, -229365, -116540, 27335, 159583, 233128, 210951, 79887, -140548, -397650, -614623, -707864, -608515, -282752, 254845, 939051, 1664333, 2305903, 2746684, 2903612, 2746684, 2305903, 1664333, 939051, 254845, -282752, -608515, -707864, -614623, -397650, -140548, 79887, 210951, 233128, 159583, 27335, -116540, -229365, -283982, -273953, -212219, -124483, -39948, 17598, 35824, 15690, -30853, -86034, -132357, -157691, -157993, -137207, -104808, -72095, -48528, -39233, -44218, -59267, -77957, -93989, -103054, -103758, -97473, -87375, -77109, -69575, -66204, -66834, -70125, -74246, -77571, -79141, -78804, -77051, -74682, -72452, -70831, -69921, -69536, -69893, -69243, -68586, -67924, -67256, -66583, -65905, -65221, -64533, -63841, -63144, -62444, -61739, -61031, -60319, -59604, -58886, -58166, -57443, -56717, -55989, -55260, -54529, -53796, -53062, -52326, -51590, -50853, -50116, -49379, -48641, -47904, -47167, -46430, -45694, -44960, -44226, -43494, -42763, -42034, -41306, -40581, -39858, -39138, -38420, -37705, -36993, -36284, -35579, -34877, -34178, -33484, -32793, -32106, -31424, -30746, -30073, -29405, -28741, -28082, -27429, -26780, -26138, -25500, -24868, -24242, -23622, -23008, -22400, -21798, -21203, -20614, -20031, -19455, -18885, -18323, -17767, -17218, -16676, -16141, -15613, -15093, -14580, -14074, -13575, -13084, -12600, -12124, -11655, -11194, -10740, -10294, -9856, -9425, -9002, -8587, -8180, -7780, -7388, -7004, -6627, -6258, -5897, -5544, -5198, -4860, -4530, -4207, -3892, -3585, -3285, -2993, -2708, -2430, -2160, -1898, -1643, -1395, -1154, -920, -693, -474, -261, -56, 143, 335
	},
	{
		0, 0, 1, 1, 0, 0, -1, -2, -2, -2, -1, 1, 3, 5, 5, 4, 1, -4, -8, -11, -10, -6, 1, 10, 17, 19, 16, 6, -8, -22, -31, -32, -22, -2, 21, 42, 53, 47, 24, -11, -49, -77, -84, -63, -17, 45, 105, 140, 133, 77, -22, -141, -244, -287, -226, -33, 305, 772, 1333, 1935, 2515, 3019, 3404, 3651, 3762, 3763, 3691, 3589, 3491, 3422, 3391, 3390, 3404, 3412, 3395, 3341, 3247, 3117, 2963, 2797, 2628, 2461, 2296, 2129, 1951, 1756, 1539, 1297, 1032, 749, 452, 146, -167, -487, -813, -1150, -1498, -1859, -2233, -2620, -3017, -3422, -3833, -4248, -4667, -5089, -5513, -5939, -6368, -6798, -7227, -7655, -8080, -8500, -8915, -9322, -9721, -10110, -10489, -10856, -11209, -11549, -11872, -12178, -12465, -12732, -12978, -13202, -13402, -13577, -13726, -13847, -13940, -14003, -14036, -14036, -14004, -13938, -13837, -13701, -13529, -13320, -13073, -12788, -12465, -12103, -11701, -11260, -10779, -10258, -9698, -9098, -8458, -7780, -7062, -6307, -5514, -4684, -3819, -2918, -1983, -1016, -16, 1013, 2071, 3157, 4268, 5402, 6559, 7735, 8930, 10140, 11363, 12597, 13840, 15089, 16341, 17594, 18845, 20091, 21329, 22557, 23770, 24967, 26144, 27297, 28425, 29522, 30587, 31615, 32604, 33550, 34451, 35301, 36099, 36841, 37524, 38145, 38700, 39186, 39600, 39940, 40201, 40382, 40479, 40491, 40413, 40244, 39981, 39621, 39164, 38605, 37945, 37179, 36308, 35330, 34242, 33044, 31735, 30314, 28779, 27131, 25369, 23493, 21502, 19397, 17178, 14846, 12400, 9843, 7175, 4397, 1512, -1481, -4578, -7778, -11079, -14478, -17973, -21560, -25238, -29002, -32850, -36779, -39782, -43607, -48032, -53240, -59124, -65211, -70741, -74910, -77222, -77824, -77671, -78385, -81789, -89221, -100836, -115200, -129404, -139801, -143209, -138249, -126312, -111698, -100705, -99747, -113036, -140558, -177183, -213443, -238036, -241481, -219827, -177115, -125469, -82381, -65616, -87120, -147850, -235470, -326186, -390792, -403580, -351607, -241306, -99830, 30146, 101550, 77365, -55570, -277736, -536428, -754847, -849388, -751189, -426426, 110321, 793830, 1518569, 2159751, 2600299, 2757149, 2600299, 2159751, 1518569, 793830, 110321, -426426, -751189, -849388, -754847, -536428, -277736, -55570, 77365, 101550, 30146, -99830, -241306, -351607, -403580, -390792, -326186, -235470, -147850, -87120, -65616, -82381, -125469, -177115, -219827, -241481, -238036, -213443, -177183, -140558, -113036, -99747, -100705, -111698, -126312, -138249, -143209, -139801, -129404, -115200, -100836, -89221, -81789, -78385, -77671, -77824, -77222, -74910, -70741, -65211, -59124, -53240, -48032, -43607, -39782, -36779, -32850, -29002, -25238, -21560, -17973, -14478, -11079, -7778, -4578, -1481, 1512, 4397, 7175, 9843, 12400, 14846, 17178, 19397, 21502, 23493, 25369, 27131, 28779, 30314, 31735, 33044, 34242, 35330, 36308, 37179, 37945, 38605, 39164, 39621, 39981, 40244, 40413, 40491, 40479, 40382, 40201, 39940, 39600, 39186, 38700, 38145, 37524, 36841, 36099, 35301, 34451, 33550, 32604, 31615, 30587, 29522, 28425, 27297, 26144, 24967, 23770, 22557, 21329, 20091, 18845, 17594, 16341, 15089, 13840, 12597, 11363, 10140, 8930, 7735, 6559, 5402, 4268, 3157, 2071, 1013, -16, -1016, -1983, -2918, -3819, -4684, -5514, -6307, -7062, -7780, -8458, -9098, -9698, -10258, -10779, -11260, -11701, -12103, -12465, -12788, -13073, -13320, -13529, -13701, -13837, -13938, -14004, -14036, -14036, -14003, -13940, -13847, -13726, -13577, -13402, -13202, -12978, -12732, -12465, -12178, -11872, -11549, -11209, -10856, -10489
	},
	{
		-2,-4,-6,-7,-5,2,13,25,33,31,16,-13,-49,-80,-90,-61,22,167,370,618,893,1171,1433,1667,1869,2043,2201,2356,2519,2698,2896,3110,3336,3570,3809,4050,4293,4539,4790,5045,5304,5566,5830,6096,6362,6628,6893,7155,7416,7672,7924,8170,8409,8640,8862,9074,9275,9463,9638,9797,9940,10065,10171,10257,10321,10362,10378,10369,10332,10266,10170,10042,9882,9687,9456,9188,8881,8535,8147,7717,7243,6724,6159,5546,4885,4175,3414,2601,1737,819,-153,-1180,-2263,-3401,-4596,-5847,-7156,-8523,-9948,-11430,-12971,-14569,-16226,-17940,-19711,-21539,-23424,-25364,-27360,-29409,-31513,-33668,-35875,-38133,-40439,-42793,-45193,-47637,-50125,-52654,-55222,-57827,-60468,-63143,-65849,-68583,-71345,-74130,-76938,-79765,-82609,-85466,-88335,-91213,-94096,-96983,-99869,-102752,-105630,-108499,-111355,-114197,-117021,-119823,-122602,-125353,-128073,-130761,-133412,-136023,-138592,-141116,-143591,-146014,-148384,-150696,-152949,-155139,-157264,-159322,-161309,-163224,-187094,-198773,-196885,-172368,-124585,-66306,-23521,-28138,-104605,-254704,-446907,-616281,-677884,-551570,-191081,392646,1123399,1871852,2486168,2832452,2832452,2486168,1871852,1123399,392646,-191081,-551570,-677884,-616281,-446907,-254704,-104605,-28138,-23521,-66306,-124585,-172368,-196885,-198773,-187094,-163224,-161309,-159322,-157264,-155139,-152949,-150696,-148384,-146014,-143591,-141116,-138592,-136023,-133412,-130761,-128073,-125353,-122602,-119823,-117021,-114197,-111355,-108499,-105630,-102752,-99869,-96983,-94096,-91213,-88335,-85466,-82609,-79765,-76938,-74130,-71345,-68583,-65849,-63143,-60468,-57827,-55222,-52654,-50125,-47637,-45193,-42793,-40439,-38133,-35875,-33668,-31513,-29409,-27360,-25364,-23424,-21539,-19711,-17940,-16226,-14569,-12971,-11430,-9948,-8523,-7156,-5847,-4596,-3401,-2263,-1180,-153,819,1737,2601,3414,4175,4885,5546,6159,6724,7243,7717,8147,8535,8881,9188,9456,9687,9882,10042,10170,10266,10332,10369,10378,10362,10321,10257,10171,10065,9940,9797,9638,9463,9275,9074,8862,8640,8409,8170,7924,7672,7416,7155,6893,6628,6362,6096,5830,5566,5304,5045
	},
	{
		-3, -6, -9, -12, -14, -12, -8, 2, 15, 31, 49, 65, 76, 79, 71, 49, 13, -35, -91, -148, -198, -229, -233, -198, -115, 19, 206, 443, 723, 1033, 1358, 1681, 1984, 2251, 2468, 2623, 2712, 2731, 2685, 2579, 2423, 2229, 2007, 1767, 1519, 1268, 1018, 771, 525, 277, 24, -238, -513, -804, -1112, -1438, -1784, -2147, -2528, -2925, -3337, -3764, -4205, -4658, -5127, -5611, -6111, -6626, -7157, -7703, -8265, -8843, -9436, -10045, -10670, -11311, -11967, -12639, -13326, -14029, -14748, -15482, -16231, -16995, -17775, -18570, -19379, -20203, -21042, -21895, -22762, -23643, -24539, -25447, -26369, -27304, -28252, -29213, -30186, -31171, -32168, -33176, -34195, -35226, -36266, -37317, -38378, -39448, -40527, -41615, -42711, -43815, -44927, -46045, -47170, -48301, -49438, -50580, -51728, -52879, -54034, -55193, -56354, -57518, -58684, -59851, -61019, -62188, -63356, -64524, -65690, -66855, -68017, -69177, -70333, -71486, -72633, -73777, -74914, -76045, -77170, -78288, -79398, -80500, -81592, -82676, -83750, -84813, -85865, -86906, -87935, -88951, -89955, -90945, -91920, -92881, -93827, -94758, -95672, -96570, -97451, -98314, -99160, -99987, -100795, -101584, -116358, -121221, -123568, -121712, -114337, -100926, -82126, -59965, -37841, -20237, -12148, -18273, -42075, -84835, -144876, -217119, -293087, -361442, -409045, -422455, -389689, -302037, -155645, 47351, 298368, 582702, 880655, 1169371, 1425168, 1626097, 1754389, 1798489, 1754389, 1626097, 1425168, 1169371, 880655, 582702, 298368, 47351, -155645, -302037, -389689, -422455, -409045, -361442, -293087, -217119, -144876, -84835, -42075, -18273, -12148, -20237, -37841, -59965, -82126, -100926, -114337, -121712, -123568, -121221, -116358, -101584, -100795, -99987, -99160, -98314, -97451, -96570, -95672, -94758, -93827, -92881, -91920, -90945, -89955, -88951, -87935, -86906, -85865, -84813, -83750, -82676, -81592, -80500, -79398, -78288, -77170, -76045, -74914, -73777, -72633, -71486, -70333, -69177, -68017, -66855, -65690, -64524, -63356, -62188, -61019, -59851, -58684, -57518, -56354, -55193, -54034, -52879, -51728, -50580, -49438, -48301, -47170, -46045, -44927, -43815, -42711, -41615, -40527, -39448, -38378, -37317, -36266, -35226, -34195, -33176, -32168, -31171, -30186, -29213, -28252, -27304, -26369, -25447, -24539, -23643, -22762, -21895, -21042, -20203, -19379, -18570, -17775, -16995, -16231, -15482, -14748, -14029, -13326, -12639, -11967, -11311, -10670, -10045, -9436, -8843, -8265, -7703, -7157, -6626, -6111, -5611, -5127, -4658, -4205
	},
};
const char *fir_filter_strings[] = {
	"none",
	"groenning",
	"a1",
	"a2",
	"b1",
	"b2",
	"c1",
	"c2",
};
static const char *fir_filter_to_str( enum fir_filter fir_filter )
{
	return fir_filter_strings[fir_filter];
}
static int strn_to_fir_filter( const char *s, size_t n, enum fir_filter *firf )
{
	size_t i;
	for (i = 0; ARRAY_SIZE(fir_filter_strings) > i; ++i)
		if (0 == strncmp(s, fir_filter_strings[i], n)) {
			*firf = i;
			return 0;
		}
	return -EINVAL;
}

enum fir_filter fir_filter = A1;
static ssize_t fir_filter_show(
	struct device *device,
	struct device_attribute *attr,
	char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", fir_filter_to_str(fir_filter));
}
static ssize_t fir_filter_store(
	struct device *device,
	struct device_attribute *attr,
	const char *buf,
	size_t count)
{
	struct lockamp *lockamp = dev_get_drvdata(device);
	int result;
	int content_length = count;

	/* truncate the newline away */
	if (0 < content_length && '\n' == buf[content_length - 1])
		content_length -= 1;

	result = strn_to_fir_filter(buf, content_length, &fir_filter);
	if (0 > result)
		return result;
	lockamp_set_filter_coefficients(lockamp, lockamp_fir_coefs[fir_filter]);
	return count;
}
DEVICE_ATTR(fir_filter, S_IRUGO | S_IWUSR, fir_filter_show, fir_filter_store);

static ssize_t sample_multiplier_show(
	struct device *device,
	struct device_attribute *attr,
	char *buf)
{
	struct lockamp *lockamp = dev_get_drvdata(device);
	return scnprintf(buf, PAGE_SIZE, "%d\n", lockamp->sample_multiplier);
}
static ssize_t sample_multiplier_store(
	struct device *device,
	struct device_attribute *attr,
	const char *buf,
	size_t count)
{
	struct lockamp *lockamp = dev_get_drvdata(device);
	int value;
	int result = kstrtoint(buf, 0, &value);
	if (0 != result)
		return result;
	mutex_lock(&lockamp->signal_buf_m);
	lockamp->sample_multiplier = value;
	mutex_unlock(&lockamp->signal_buf_m);
	return count;
}
DEVICE_ATTR(sample_multiplier, S_IRUGO | S_IWUSR, sample_multiplier_show,
                                                 sample_multiplier_store);

/* latest_adc_samples
 *
 * TODO: This attribute has a race condition. If two processes start reading simultaneously,
 * then lockamp_get_adc_samples will be called twice. This will modify the single
 * lockamp->adc_buffer that both processes try to copy from.
 *
 * A quick fix would be to not allow partial reads (so that another process can't modify
 * lockamp->adc_buffer while said buffer is being read). A binary attribute, however, can
 * only by read in PAGE_SIZE chunks (e.g., 4096 bytes). Since the ADC buffer is a lot larger than
 * that, partial reads must be used.
 *
 * The real solution is to expose the ADC buffer in a different way. Possibly as its own character
 * device.
 *
 * For now, we ignore the race condition here.
 * */
static ssize_t latest_adc_samples_read(
	struct file *file,
	struct kobject *kobj,
	struct bin_attribute *bin_attr,
	char *buf,
	loff_t offset,
	size_t count)
{
	int result;
	struct device *dev = container_of(kobj, struct device, kobj);
	struct lockamp *lockamp = dev_get_drvdata(dev);
	mutex_lock(&lockamp->adc_buf_m);
	/* Buffer samples at start of request */
	if (0 == offset) {
		lockamp_get_adc_samples(lockamp, (s32*)lockamp->adc_buffer);
	}
	result = memory_read_from_buffer(buf, count, &offset,
	                                 lockamp->adc_buffer,
                                     LOCKAMP_ADC_SAMPLES_SIZE);
	mutex_unlock(&lockamp->adc_buf_m);
	return result;
}
BIN_ATTR_RO(latest_adc_samples, LOCKAMP_ADC_SAMPLES_SIZE);

/* fifo_read_duration_us */
static ssize_t fifo_read_duration_us_show(
	struct device *device,
	struct device_attribute *attr,
	char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", (u32)lockamp_fifo_read_duration / 1000);
}
DEVICE_ATTR(fifo_read_duration_us, S_IRUGO, fifo_read_duration_us_show, NULL);

/* fifo_read_delay_us */
static ssize_t fifo_read_delay_us_show(
	struct device *device,
	struct device_attribute *attr,
	char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", (u32)lockamp_fifo_read_delay / 1000);
}
DEVICE_ATTR(fifo_read_delay_us, S_IRUGO, fifo_read_delay_us_show, NULL);

DEVICE_INT_ATTR(debug1, S_IRUGO, lockamp_debug1);
DEVICE_INT_ATTR(debug2, S_IRUGO, lockamp_debug2);
DEVICE_INT_ATTR(debug3, S_IRUGO, lockamp_debug3);

/* attribute group */
static struct attribute *attrs[] = {
	&dev_attr_decimation_factor.attr,
	&dev_attr_time_step_ns.attr,
	&dev_attr_fir_cycles.attr,
	&dev_attr_signal_buf_capacity.attr,
	&dev_attr_signal_max_amplitude_e1.attr.attr,
	&dev_attr_ma_time_step_ns.attr.attr,
	&dev_attr_generator_scale_min.attr,
	&dev_attr_generator_scale_max.attr,
	&dev_attr_generator1_scale.attr,
	&dev_attr_generator2_scale.attr,
	&dev_attr_generator1_step.attr,
	&dev_attr_generator2_step.attr,
	&dev_attr_dac_data_bits.attr,
	&dev_attr_hw_debug1.attr,
	&dev_attr_fir_filter.attr,
	&dev_attr_sample_multiplier.attr,
	&dev_attr_fifo_read_duration_us.attr,
	&dev_attr_fifo_read_delay_us.attr,
	&dev_attr_debug1.attr.attr,
	&dev_attr_debug2.attr.attr,
	&dev_attr_debug3.attr.attr,
	NULL
};
static struct bin_attribute *bin_attrs[] = {
	&bin_attr_latest_adc_samples,
	NULL
};
static struct attribute_group attr_group = {
	.attrs = attrs,
	.bin_attrs = bin_attrs
};
const struct attribute_group *lockamp_attr_groups[] = {
	&attr_group,
	NULL
};
