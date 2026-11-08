#define _GNU_SOURCE
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rdtsc.h"
#include "drivers/mlx5/mlx5.h"

#define CHECK(expr) do { \
	if (!(expr)) { \
		fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
		return 1; \
	} \
} while (0)

int main(void)
{
	const char *source = NULL;
	uint64_t hz;
	uint64_t fake_msr = UINT64_C(0x15) << 8;
	uint64_t read_msr = 0;
	int tsc_error;
	uint32_t denominator, numerator, crystal_hz, reserved;
	struct mlx5_bpoll_cqe mlx5_cqes[2] = {
		{ .op_own = 0xf0 },
		{ .op_own = 0xf1 },
	};
	_Atomic uint32_t mlx5_ci = 0;
	uint32_t bnxt_cqes[2] = { 0, 0 };
	_Atomic uint32_t bnxt_ci = 0;
	struct {
		uint64_t qword0;
		uint64_t status_error_len;
		uint8_t reserved[16];
	} iavf_legacy_descs[2] = { 0 };
	struct {
		uint64_t qword0;
		uint16_t status_error0;
		uint8_t reserved[22];
	} iavf_flex_descs[2] = { 0 };
	_Atomic uint16_t iavf_legacy_tail = 0;
	_Atomic uint16_t iavf_flex_tail = 0;
	struct rxring_state iavf_legacy_state = {
		.ring = iavf_legacy_descs,
		.consumer_idx = &iavf_legacy_tail,
		.layout = {
			.ring_size = 2,
			.descriptor_size = sizeof(iavf_legacy_descs[0]),
			.event_source_offset = offsetof(typeof(iavf_legacy_descs[0]),
				status_error_len),
			.idx_width = sizeof(iavf_legacy_tail),
			.event_source_width = sizeof(iavf_legacy_descs[0].status_error_len),
			.event_mask = 1,
			.event_expected = 1,
		},
	};
	struct rxring_state iavf_flex_state = {
		.ring = iavf_flex_descs,
		.consumer_idx = &iavf_flex_tail,
		.layout = {
			.ring_size = 2,
			.descriptor_size = sizeof(iavf_flex_descs[0]),
			.event_source_offset = offsetof(typeof(iavf_flex_descs[0]), status_error0),
			.idx_width = sizeof(iavf_flex_tail),
			.event_source_width = sizeof(iavf_flex_descs[0].status_error0),
			.event_mask = 1,
			.event_expected = 1,
		},
	};
	struct rxring_state bnxt_state = {
		.ring = bnxt_cqes,
		.consumer_idx = &bnxt_ci,
		.layout = {
			.ring_size = 2,
			.descriptor_size = sizeof(bnxt_cqes[0]),
			.idx_width = sizeof(bnxt_ci),
			.event_source_width = sizeof(bnxt_cqes[0]),
			.event_mask = 1,
			.event_expected = 1,
			.phase_mask = 1,
		},
	};
	struct rxring_state mlx5_state = {
		.ring = mlx5_cqes,
		.consumer_idx = &mlx5_ci,
		.layout = {
			.ring_size = 2,
			.descriptor_size = sizeof(mlx5_cqes[0]),
			.event_source_offset = offsetof(struct mlx5_bpoll_cqe, op_own),
			.idx_width = sizeof(mlx5_ci),
			.event_source_width = sizeof(mlx5_cqes[0].op_own),
			.event_mask = 1,
			.phase_mask = 1,
		},
	};
	_Atomic uint32_t producer = 2;
	struct rxring_state index_state = {
		.consumer_idx = &mlx5_ci,
		.producer_idx = &producer,
		.layout.idx_width = sizeof(producer),
	};
	_Atomic uint16_t memif_consumer = 7;
	_Atomic uint16_t memif_producer = 7;
	struct rxring_state memif_state = {
		.consumer_idx = &memif_consumer,
		.producer_idx = &memif_producer,
		.layout.idx_width = sizeof(memif_consumer),
	};
	int fake_msr_fd;
	char fake_msr_path[] = "/tmp/usched-msr-test-XXXXXX";

	CHECK(tsc_hz_from_ratio(2, 175, 24000000) == UINT64_C(2100000000));
	CHECK(tsc_hz_from_ratio(0, 175, 24000000) == 0);
	CHECK(tsc_hz_from_ratio(2, 0, 24000000) == 0);
	CHECK(tsc_hz_from_ratio(2, 175, 0) == 0);
	CHECK(intel_cpu_model(UINT32_C(0x000506c0)) == 0x5c);
	CHECK(tsc_msr_base_hz(0x1a, 0) == UINT64_C(133000000));
	CHECK(tsc_msr_base_hz(0x5c, 0) == UINT64_C(100000000));
	CHECK(tsc_msr_base_hz(0x6a, bit_AVX) == UINT64_C(100000000));
	CHECK(tsc_msr_base_hz(0x01, 0) == 0);
	CHECK(tsc_hz_from_platform_info(UINT64_C(0x15) << 8,
		UINT64_C(100000000)) == UINT64_C(2100000000));
	CHECK(tsc_hz_from_platform_info(0, UINT64_C(100000000)) == 0);
	CHECK(tsc_hz_from_platform_info(UINT64_C(0x15) << 8, 0) == 0);
	CHECK(read_platform_info_msr_file("/no/such/usched-msr", &read_msr) == -ENOENT);
	CHECK((fake_msr_fd = mkstemp(fake_msr_path)) >= 0);
	CHECK(read_platform_info_msr_file(fake_msr_path, &read_msr) == -EIO);
	CHECK(pwrite(fake_msr_fd, &fake_msr, sizeof(fake_msr), MSR_PLATFORM_INFO) ==
		(ssize_t)sizeof(fake_msr));
	CHECK(read_platform_info_msr_file(fake_msr_path, &read_msr) == 0);
	CHECK(read_msr == fake_msr);
	close(fake_msr_fd);
	unlink(fake_msr_path);
	CHECK(rdtsc_relaxed() > 0);
	mlx5_cqes[0].op_own = 1;
	CHECK(!descriptor_has_event(&mlx5_state));
	mlx5_cqes[0].op_own = 0x00;
	CHECK(descriptor_has_event(&mlx5_state));
	atomic_store(&mlx5_ci, 1);
	CHECK(!descriptor_has_event(&mlx5_state));
	mlx5_cqes[1].op_own = 0x00;
	CHECK(descriptor_has_event(&mlx5_state));
	atomic_store(&mlx5_ci, 2);
	CHECK(!descriptor_has_event(&mlx5_state));
	mlx5_cqes[0].op_own = 0x01;
	CHECK(descriptor_has_event(&mlx5_state));
	mlx5_cqes[0].op_own = 0xf1;
	CHECK(descriptor_has_event(&mlx5_state));
	CHECK(!descriptor_has_event(&bnxt_state));
	CHECK(!descriptor_has_event(&iavf_legacy_state));
	iavf_legacy_descs[0].status_error_len = 1;
	CHECK(descriptor_has_event(&iavf_legacy_state));
	atomic_store(&iavf_legacy_tail, 1);
	CHECK(!descriptor_has_event(&iavf_legacy_state));
	iavf_legacy_descs[1].status_error_len = 1;
	CHECK(descriptor_has_event(&iavf_legacy_state));
	atomic_store(&iavf_legacy_tail, 2);
	CHECK(descriptor_has_event(&iavf_legacy_state));
	CHECK(!descriptor_has_event(&iavf_flex_state));
	iavf_flex_descs[0].status_error0 = 1;
	CHECK(descriptor_has_event(&iavf_flex_state));
	atomic_store(&iavf_flex_tail, 1);
	CHECK(!descriptor_has_event(&iavf_flex_state));
	iavf_flex_descs[1].status_error0 = 1;
	CHECK(descriptor_has_event(&iavf_flex_state));
	atomic_store(&iavf_flex_tail, 2);
	CHECK(descriptor_has_event(&iavf_flex_state));
	bnxt_cqes[0] = 1;
	CHECK(descriptor_has_event(&bnxt_state));
	atomic_store(&bnxt_ci, 1);
	CHECK(!descriptor_has_event(&bnxt_state));
	bnxt_cqes[1] = 1;
	CHECK(descriptor_has_event(&bnxt_state));
	atomic_store(&bnxt_ci, 2);
	CHECK(!descriptor_has_event(&bnxt_state));
	CHECK(!descriptor_has_event(&index_state));
	atomic_store(&producer, 3);
	CHECK(descriptor_has_event(&index_state));
	CHECK(!descriptor_has_event(&memif_state));
	atomic_store(&memif_producer, 8);
	CHECK(descriptor_has_event(&memif_state));
	hz = detect_tsc_hz(sched_getcpu(), &source, &tsc_error);
	CHECK(read_cpuid15(&denominator, &numerator, &crystal_hz, &reserved));
	if (hz)
		printf("TSC: %.3f MHz via %s; invariant=%s\n", hz / 1e6, source,
			tsc_is_invariant() ? "yes" : "no");
	else
		printf("CPUID.15H/MSR cannot provide TSC Hz: EAX=%u EBX=%u ECX=%u "
			"EDX=%u, MSR: %s\n", denominator, numerator, crystal_hz, reserved,
			tsc_error ? strerror(-tsc_error) : "invalid frequency");
	return 0;
}
