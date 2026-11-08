#ifndef __RDTSC_H__
#define __RDTSC_H__

#include <cpuid.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

#define MSR_PLATFORM_INFO 0xCE
#define PLATFORM_INFO_MAX_NON_TURBO_RATIO_SHIFT 8
#define PLATFORM_INFO_MAX_NON_TURBO_RATIO_MASK UINT64_C(0xff)

static inline uint64_t rdtsc(void)
{
	uint32_t lo, hi;
	__asm__ __volatile__("mfence\nlfence\nrdtsc\nlfence\n"
		: "=a" (lo), "=d" (hi) :: "memory");
	return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t rdtsc_relaxed(void)
{
	uint32_t lo, hi;
	__asm__ __volatile__("lfence\nrdtsc\n" : "=a" (lo), "=d" (hi) :: "memory");
	return ((uint64_t)hi << 32) | lo;
}

static inline bool tsc_is_invariant(void)
{
	unsigned int a, b, c, d;
	unsigned int max = __get_cpuid_max(0x80000000, NULL);

	if (max < 0x80000007)
		return false;

	__cpuid(0x80000007, a, b, c, d);

	return (d & (1U << 8)) != 0;
}

static inline bool read_cpuid15(uint32_t *denominator, uint32_t *numerator,
	uint32_t *crystal_hz, uint32_t *reserved)
{
	unsigned int a, b, c, d;

	*denominator = *numerator = *crystal_hz = *reserved = 0;

	if (__get_cpuid_max(0, NULL) < 0x15)
		return false;

	__cpuid_count(0x15, 0, a, b, c, d);

	*denominator = a;
	*numerator = b;
	*crystal_hz = c;
	*reserved = d;

	return true;
}

static inline uint64_t tsc_hz_from_ratio(uint32_t denominator,
	uint32_t numerator, uint32_t crystal_hz)
{
	if (!denominator || !numerator || !crystal_hz)
		return 0;
	return (uint64_t)(((__uint128_t)crystal_hz * numerator) / denominator);
}

static inline uint64_t tsc_hz_from_cpuid15(void)
{
	uint32_t denominator, numerator, crystal_hz, reserved;
	if (!read_cpuid15(&denominator, &numerator, &crystal_hz, &reserved))
		return 0;
	return tsc_hz_from_ratio(denominator, numerator, crystal_hz);
}

static inline unsigned int intel_cpu_model(uint32_t family_model_stepping)
{
	unsigned int family = (family_model_stepping >> 8) & 0xf;
	unsigned int model = (family_model_stepping >> 4) & 0xf;
	if (family == 6 || family == 15)
		model += ((family_model_stepping >> 16) & 0xf) << 4;
	return model;
}

static inline uint64_t tsc_msr_base_hz(unsigned int model, uint32_t features_ecx)
{
	switch (model) {
	case 0x1a: /* Nehalem */
	case 0x1e:
	case 0x1f:
	case 0x25: /* Westmere */
	case 0x2c:
	case 0x2e:
	case 0x2f:
		return UINT64_C(133000000);
	case 0x5c: /* Goldmont */
	case 0x5f: /* Denverton */
		return UINT64_C(100000000);
	default:
		/* DPDK uses AVX as the boundary for Intel models with a 100 MHz
		 * maximum-non-turbo ratio in MSR_PLATFORM_INFO. */
		return (features_ecx & bit_AVX) ? UINT64_C(100000000) : 0;
	}
}

static inline uint64_t tsc_hz_from_platform_info(uint64_t platform_info,
	uint64_t base_hz)
{
	uint64_t ratio = (platform_info >> PLATFORM_INFO_MAX_NON_TURBO_RATIO_SHIFT) &
		PLATFORM_INFO_MAX_NON_TURBO_RATIO_MASK;
	if (!ratio || !base_hz || ratio > UINT64_MAX / base_hz)
		return 0;
	return ratio * base_hz;
}

static inline int read_platform_info_msr_file(const char *path, uint64_t *value)
{
	ssize_t bytes;
	int fd, saved_errno;

	if (!path || !value)
		return -EINVAL;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;

	bytes = pread(fd, value, sizeof(*value), MSR_PLATFORM_INFO);
	saved_errno = errno;
	close(fd);

	if (bytes < 0)
		return -saved_errno;
	if (bytes != (ssize_t)sizeof(*value))
		return -EIO;

	return 0;
}

static inline int read_platform_info_msr(int cpu, uint64_t *value)
{
	char path[64];

	if (cpu < 0 || !value)
		return -EINVAL;
	if (snprintf(path, sizeof(path), "/dev/cpu/%d/msr", cpu) >= (int)sizeof(path))
		return -ENAMETOOLONG;

	return read_platform_info_msr_file(path, value);
}

static inline uint64_t tsc_hz_from_msr(int cpu, int *error)
{
	uint32_t a, b, c, d;
	uint64_t platform_info, base_hz, hz;

	__cpuid(0, a, b, c, d);
	if (b != UINT32_C(0x756e6547) || d != UINT32_C(0x49656e69) ||
	    c != UINT32_C(0x6c65746e)) {
		*error = -ENOTSUP;
		return 0;
	}
	__cpuid(1, a, b, c, d);
	base_hz = tsc_msr_base_hz(intel_cpu_model(a), c);
	if (!base_hz) {
		*error = -ENOTSUP;
		return 0;
	}

	{
		int ret;
		ret = read_platform_info_msr(cpu, &platform_info);
		if (ret) {
			*error = ret;
			return 0;
		}
	}

	hz = tsc_hz_from_platform_info(platform_info, base_hz);
	if (!hz) {
		*error = -ERANGE;
		return 0;
	}
	
	*error = 0;

	return hz;
}

static inline uint64_t detect_tsc_hz(int cpu, const char **source, int *error)
{
	uint64_t hz;

	*error = 0;

	hz = tsc_hz_from_cpuid15();
	if (hz) {
		*source = "CPUID.15H";
		return hz;
	}

	hz = tsc_hz_from_msr(cpu, error);
	if (hz)
		*source = "MSR_PLATFORM_INFO";

	return hz;
}

#endif
