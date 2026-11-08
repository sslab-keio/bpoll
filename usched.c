#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <linux/futex.h>

#include <rte_epoll.h>

#include <rte_errno.h>
#include <rte_memory.h>

#include "fdpass.h"
#include "rdtsc.h"
#include "shmem.h"
#include "usched.h"
#include "trace.h"

#define SHM_USCHED "usched"
#define DEFAULT_IDLE_SPIN_US UINT64_C(1000000)
#define DOMAIN_READY_SPINS 20000

static struct usched_shared *shared;
static struct rxring_state *rxring_states;
static int this_cpu = -1;
static int worker_idx = IDX_UTHREAD_UNBOUND;
static int (*intr_enable_cb)(void *);
static int (*intr_disable_cb)(void *);
static void *intr_opaque;
static int poll_list[USCHED_MAX_WORKERS];
static pid_t mapped_tid[USCHED_MAX_WORKERS];
static int ring_fds[USCHED_MAX_WORKERS];
static int poll_list_count;
static uint64_t poll_list_generation = UINT64_MAX;
static int poll_list_self_boundary;
static struct rxring this_rxring;


static void admin_lock(void)
{
	bool expected;
	do {
		expected = false;
		if (!atomic_compare_exchange_weak_explicit(&shared->global.admin_lock, &expected, true,
				memory_order_acquire, memory_order_relaxed))
			sched_yield();
	} while (expected);
}

static void admin_unlock(void)
{
	atomic_store_explicit(&shared->global.admin_lock, false, memory_order_release);
}

static int set_priority(pid_t tid, int priority)
{
	struct sched_param p = { .sched_priority = priority };

	if (!sched_setparam(tid, &p))
		return 0;

	if (errno != ESRCH)
		fprintf(stderr, "usched: sched_setparam(%d, %d): %s\n",
			tid, priority, strerror(errno));

	return -errno;
}

static int reset_policy(pid_t tid)
{
	struct sched_param p = { .sched_priority = 0 };

	if (!sched_setscheduler(tid, SCHED_OTHER, &p))
		return 0;
	if (errno != ESRCH)
		fprintf(stderr, "usched: sched_setscheduler(%d, SCHED_OTHER): %s\n",
			tid, strerror(errno));
	return -errno;
}

static int allocate_worker_idx(void)
{
	for (int i = 0; i < USCHED_MAX_WORKERS; i++) {
		unsigned char expected = 0;
		if (atomic_compare_exchange_strong_explicit(&shared->global.slots[i], &expected, 1,
				memory_order_acq_rel, memory_order_relaxed)) {
			atomic_fetch_add_explicit(&shared->global.num_workers, 1, memory_order_relaxed);
			return i;
		}
	}
	return -ENOSPC;
}

static void release_worker_idx(int idx)
{
	struct worker *worker;

	if (idx < 0 || idx >= USCHED_MAX_WORKERS)
		return;

	worker = &shared->workers[idx];
	if (worker->role == WORKER_PREEMPTION && worker->cpu >= 0 &&
		worker->cpu < (int)shared->global.num_cpus) {
		int expected = idx;

		atomic_compare_exchange_strong_explicit(
			&shared->runqueues[worker->cpu].preemption_worker_idx,
			&expected,
			IDX_UTHREAD_UNBOUND,
			memory_order_acq_rel,
			memory_order_relaxed);
	}

	atomic_store_explicit(&worker->state, USCHED_WORKER_FREE, memory_order_release);
	if (atomic_exchange_explicit(&shared->global.slots[idx], 0, memory_order_acq_rel)) {
		unsigned int count = atomic_load_explicit(&shared->global.num_workers, memory_order_relaxed);
		if (count)
			atomic_fetch_sub_explicit(&shared->global.num_workers, 1, memory_order_relaxed);
	}
}

static void reclaim_worker_indices(int ncpus)
{
	admin_lock();
	for (int i = 0; i < USCHED_MAX_WORKERS; i++) {
		if (!atomic_load_explicit(&shared->global.slots[i], memory_order_acquire))
			continue;

		pid_t tid = shared->workers[i].tid;
		if (tid <= 0)
			continue;
		if (kill(tid, 0) && errno == ESRCH) {
			int c = shared->workers[i].cpu;
			release_worker_idx(i);
			if (c >= 0 && c < ncpus)
				atomic_fetch_add_explicit(&shared->runqueues[c].generation, 1,
					memory_order_release);
		}
	}
	admin_unlock();
}

static bool reuse_worker_idx(struct runqueue *runqueue, int idx, int *pos_out)
{
	int count = atomic_load_explicit(&runqueue->num_workers, memory_order_acquire);

	for (int pos = 0; pos < count; pos++) {
		int old_idx = atomic_load_explicit(&runqueue->worker_indices[pos], memory_order_relaxed);

		if (old_idx < 0 || old_idx >= USCHED_MAX_WORKERS)
			continue;
		if (atomic_load_explicit(&shared->workers[old_idx].state, memory_order_acquire) !=
				USCHED_WORKER_FREE)
			continue;
		if (atomic_compare_exchange_strong_explicit(&runqueue->worker_indices[pos],
				&old_idx, idx, memory_order_acq_rel, memory_order_relaxed)) {
			*pos_out = pos;
			return true;
		}
	}
	return false;
}

static int export_rxring(const struct rxring *rxring, struct shared_rxring *shared)
{
	struct rte_memseg_list *msl;
	struct rte_memseg *ms;
	size_t object_size;
	size_t fd_offset;
	uintptr_t delta;
	int fd;

	if (!rxring->ring || !rxring->consumer_idx_word || !rxring->ring_len ||
		!rxring->layout.ring_size || !rxring->layout.descriptor_size)
		return -EINVAL;
	if (__builtin_mul_overflow((size_t)rxring->layout.ring_size,
			rxring->layout.descriptor_size, &object_size) || object_size > rxring->ring_len)
		return -ERANGE;

	if (rxring->fd >= 0) {
		if (!rxring->page_size)
			return -EINVAL;
		shared->ring_fd_offset = rxring->fd_offset;
		shared->ring_page_size = rxring->page_size;
		fd = rxring->fd;
	} else {
		msl = rte_mem_virt2memseg_list(rxring->ring);
		ms = msl ? rte_mem_virt2memseg(rxring->ring, msl) : NULL;
		if (!ms)
			return -ENODEV;
		delta = (uintptr_t)rxring->ring - (uintptr_t)ms->addr;
		if (delta > ms->len || object_size > ms->len - delta)
			return -ERANGE;
		if (rte_memseg_get_fd_offset(ms, &fd_offset) < 0 ||
			(fd = rte_memseg_get_fd(ms)) < 0)
			return rte_errno ? -rte_errno : -ENODEV;
		shared->ring_fd_offset = fd_offset + delta;
		shared->ring_page_size = ms->hugepage_sz;
	}

	shared->ring_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
	if (shared->ring_fd < 0)
		return -errno;
	shared->layout = rxring->layout;
	atomic_store_explicit(&shared->consumer_idx,
		(load_word_atomic(rxring->consumer_idx_word, rxring->layout.idx_width)
			& rxring->consumer_mask) >> rxring->consumer_shift,
		memory_order_relaxed);
	return 0;
}

static int map_rxring(
	const struct shared_rxring *shared,
	struct rxring_state *state,
	int ring_fd)
{
	size_t object_size;
	size_t map_len;
	off_t map_offset;
	void *mapping;

	if (ring_fd < 0 || !shared->ring_page_size || !shared->layout.ring_size ||
		!shared->layout.descriptor_size || shared->ring_page_size > SIZE_MAX)
		return -EINVAL;
	if (__builtin_mul_overflow((size_t)shared->layout.ring_size,
		shared->layout.descriptor_size, &object_size))
		return -EOVERFLOW;
	if (shared->layout.has_producer &&
		(shared->layout.producer_offset > object_size ||
		shared->layout.idx_width > object_size - shared->layout.producer_offset))
		return -EINVAL;
	if (object_size > SIZE_MAX - shared->ring_fd_offset % shared->ring_page_size)
		return -EOVERFLOW;

	map_len = object_size + shared->ring_fd_offset % shared->ring_page_size;
	if (map_len > SIZE_MAX - (size_t)shared->ring_page_size + 1)
		return -EOVERFLOW;
	map_len = (map_len + shared->ring_page_size - 1) /
		shared->ring_page_size * shared->ring_page_size;
	map_offset = shared->ring_fd_offset / shared->ring_page_size * shared->ring_page_size;
	mapping = mmap(NULL, map_len, PROT_READ, MAP_SHARED, ring_fd, map_offset);
	if (mapping == MAP_FAILED)
		return -errno;

	state->mapping = mapping;
	state->mapping_len = map_len;
	state->ring = (const char *)mapping + shared->ring_fd_offset % shared->ring_page_size;
	state->consumer_idx = &shared->consumer_idx;
	state->layout = shared->layout;
	if (shared->layout.has_producer)
		state->producer_idx = state->ring + shared->layout.producer_offset;
	return 0;
}

static void release_rxring(struct rxring_state *state)
{
	if (state->mapping)
		munmap(state->mapping, state->mapping_len);
	memset(state, 0, sizeof(*state));
}

static __attribute__((noinline, cold)) int refresh_poll_list(void)
{
	struct runqueue *runqueue = &shared->runqueues[this_cpu];
	uint64_t generation = atomic_load_explicit(&runqueue->generation, memory_order_acquire);
	int count;
	int out = 0;
	int self_boundary = 0;

	if (generation == poll_list_generation)
		return 0;

	count = atomic_load_explicit(&runqueue->num_workers, memory_order_acquire);
	if (count < 0 || count > USCHED_MAX_WORKERS)
		return -EPROTO;

	for (int pos = 0; pos < count; pos++) {
		int idx = atomic_load_explicit(&runqueue->worker_indices[pos], memory_order_relaxed);
		bool is_self = idx == worker_idx;

		if (idx < 0 || idx >= USCHED_MAX_WORKERS)
			continue;
		if (atomic_load_explicit(&shared->workers[idx].state, memory_order_acquire) != USCHED_WORKER_ACTIVE ||
			shared->workers[idx].role != WORKER_APPLICATION)
			continue;

		if (mapped_tid[idx] && mapped_tid[idx] != shared->shared_rxrings[idx].tid) {
			release_rxring(&rxring_states[idx]);
			mapped_tid[idx] = 0;
			if (ring_fds[idx] >= 0) {
				close(ring_fds[idx]);
				ring_fds[idx] = -1;
			}
		}

		if (!mapped_tid[idx]) {
			if (ring_fds[idx] < 0) {
				ring_fds[idx] = is_self
					? fcntl(shared->shared_rxrings[idx].ring_fd, F_DUPFD_CLOEXEC, 0)
					: usched_dup_peer_fd(shared->shared_rxrings[idx].tid,
						shared->shared_rxrings[idx].ring_fd);
				if (ring_fds[idx] < 0)
					continue;
			}

			if (map_rxring(&shared->shared_rxrings[idx], &rxring_states[idx], ring_fds[idx])) {
				close(ring_fds[idx]);
				ring_fds[idx] = -1;
				continue;
			}
			mapped_tid[idx] = shared->shared_rxrings[idx].tid;
		}

		poll_list[out++] = idx;
		if (is_self)
			self_boundary = out;
	}

	poll_list_count = out;
	poll_list_generation = generation;
	poll_list_self_boundary = self_boundary;

	return 0;
}

static inline struct worker *poll_range(int begin, int end)
{
	for (int pos = begin; pos < end; pos++) {
		int idx = poll_list[pos];
		int has_pkt = descriptor_has_event(&rxring_states[idx]);
		TRACE_PICK_NEXT_CANDIDATE(gettid(), worker_idx,
			shared->workers[idx].tid, idx, has_pkt, has_pkt);
		if (has_pkt)
			return &shared->workers[idx];
	}
	return NULL;
}

static inline __attribute__((always_inline)) struct worker *pick_next_worker(void)
{
	struct runqueue *runqueue = &shared->runqueues[this_cpu];
	int current;
	int start = 0;
	struct worker *next;
	uint64_t generation;

	generation = atomic_load_explicit(&runqueue->generation, memory_order_acquire);
	if (generation != poll_list_generation && refresh_poll_list())
		return NULL;
	if (poll_list_count <= 0)
		return NULL;

	current = atomic_load_explicit(&runqueue->current_worker_idx, memory_order_relaxed);
	if (current == worker_idx) {
		start = poll_list_self_boundary;
	} else {
		for (int pos = 0; pos < poll_list_count; pos++) {
			if (poll_list[pos] == current) {
				start = pos + 1;
				break;
			}
		}
	}

	next = poll_range(start, poll_list_count);
	if (!next)
		next = poll_range(0, start);

	if (next)
		atomic_store_explicit(&runqueue->current_worker_idx, (int)(next - shared->workers),
			memory_order_release);

	return next;
}

static int init_shmem(int cpu, int ncpus)
{
	bool created;
	const char *tsc_source = NULL;
	uint64_t tsc_hz;
	uint64_t tsc_mhz;
	uint64_t idle_spin_us;
	int tsc_error = 0;

	idle_spin_us = DEFAULT_IDLE_SPIN_US;
	if (getenv("USCHED_IDLE_SPIN_US")) {
		char *end;
		unsigned long long value;

		errno = 0;
		value = strtoull(getenv("USCHED_IDLE_SPIN_US"), &end, 10);
		if (errno || *end)
			return -EINVAL;
		idle_spin_us = value;
	}

	tsc_hz = detect_tsc_hz(cpu, &tsc_source, &tsc_error);

	if (!tsc_is_invariant())
		fprintf(stderr, "usched: WARNING: CPUID does not report Invariant TSC\n");

	if (!tsc_hz) {
		uint32_t denominator, numerator, crystal_hz, reserved;
		bool supported = read_cpuid15(&denominator, &numerator, &crystal_hz, &reserved);
		fprintf(stderr, "usched: cannot determine TSC frequency from CPUID.15H or "
			"MSR_PLATFORM_INFO "
			"(supported=%s EAX=%" PRIu32 " EBX=%" PRIu32
			" ECX=%" PRIu32 " EDX=%" PRIu32 ", MSR CPU=%d: %s); "
			"refusing to start\n",
			supported ? "yes" : "no", denominator, numerator, crystal_hz, reserved,
			cpu, tsc_error ? strerror(-tsc_error) : "invalid frequency");
		return -ENOTSUP;
	}

	tsc_mhz = tsc_hz / UINT64_C(1000000);
	if (!tsc_mhz) {
		fprintf(stderr, "usched: TSC frequency is below 1 MHz; refusing to start\n");
		return -ENOTSUP;
	}
	if (tsc_hz % UINT64_C(1000000)) {
		fprintf(stderr, "usched: WARNING: truncating TSC frequency from %" PRIu64
			" Hz to %" PRIu64 " MHz (discarding %" PRIu64 " Hz)\n",
			tsc_hz, tsc_mhz, tsc_hz % UINT64_C(1000000));
	}

	printf("[usched @C%d] TSC: %.3f MHz via %s%s; idle spin %" PRIu64 " us\n",
		cpu, tsc_hz / 1e6, tsc_source,
		tsc_is_invariant() ? ", invariant" : ", NOT invariant", idle_spin_us);

	shared = attach_or_create_shmem(
		SHM_USCHED,
		sizeof(*shared) + (size_t)ncpus * sizeof(*shared->runqueues),
		&created);

	if (created) {
		shared->global.num_cpus = ncpus;
		shared->global.tsc_hz = tsc_hz;
		shared->global.idle_spin_us = idle_spin_us;
		if (idle_spin_us > UINT64_MAX / tsc_mhz) {
			fprintf(stderr, "usched: WARNING: idle spin %" PRIu64
				" us overflows at %" PRIu64 " Hz (%" PRIu64
				" MHz); saturating idle spin ticks to UINT64_MAX\n",
				idle_spin_us, tsc_hz, tsc_mhz);
			shared->global.idle_spin_ticks = UINT64_MAX;
		} else {
			shared->global.idle_spin_ticks = idle_spin_us * tsc_mhz;
		}
		for (int i = 0; i < ncpus; i++) {
			atomic_store_explicit(&shared->runqueues[i].current_worker_idx,
				IDX_UTHREAD_UNBOUND,
				memory_order_relaxed);
			atomic_store_explicit(&shared->runqueues[i].preemption_worker_idx,
				IDX_UTHREAD_UNBOUND,
				memory_order_relaxed);
		}

		atomic_store_explicit(&shared->global.magic, USCHED_MAGIC, memory_order_release);
	} else {
		int spins = 0;
		uint32_t magic;

		while ((magic = atomic_load_explicit(&shared->global.magic, memory_order_acquire)) == 0) {
			if (++spins > DOMAIN_READY_SPINS) {
				fprintf(stderr, "usched: timed out waiting for %s to finish initializing\n",
					SHM_USCHED);
				return -ETIMEDOUT;
			}
			if (spins < 1000)
				sched_yield();
			else
				usleep(100);
		}

		if (magic != USCHED_MAGIC
				|| shared->global.num_cpus != (uint32_t)ncpus
				|| shared->global.tsc_hz != tsc_hz
				|| shared->global.idle_spin_us != idle_spin_us) {
			fprintf(stderr, "usched: incompatible stale shared memory; remove /dev/shm/%s\n",
				SHM_USCHED);
			return -EPROTO;
		}
	}

	if (!rxring_states) {
		rxring_states = calloc(USCHED_MAX_WORKERS, sizeof(*rxring_states));
		if (!rxring_states)
			return -ENOMEM;
	}

	for (int i = 0; i < USCHED_MAX_WORKERS; i++)
		ring_fds[i] = -1;

	return 0;
}

int usched_init(
	const struct rxring *rxring,
	int cpu,
	int (*enable_cb)(void *),
	int (*disable_cb)(void *),
	void *enable_opaque)
{
	struct runqueue *runqueue;
	int ncpus = get_nprocs();
	int idx, pos;
	int ret;

	if (worker_idx != IDX_UTHREAD_UNBOUND)
		return -EALREADY;
	this_cpu = cpu;

	ret = init_shmem(cpu, ncpus);
	if (ret)
		return ret;

	idx = allocate_worker_idx();
	if (idx == -ENOSPC) {
		reclaim_worker_indices(ncpus);
		idx = allocate_worker_idx();
	}
	if (idx < 0)
		return idx;

	shared->workers[idx].tid = gettid();
	shared->workers[idx].cpu = cpu;
	shared->workers[idx].role = WORKER_APPLICATION;
	atomic_store_explicit(&shared->workers[idx].state, USCHED_WORKER_STARTING, memory_order_release);

	memset(&shared->shared_rxrings[idx], 0, sizeof(shared->shared_rxrings[idx]));
	shared->shared_rxrings[idx].tid = shared->workers[idx].tid;
	shared->shared_rxrings[idx].ring_fd = -1;

	ret = export_rxring(rxring, &shared->shared_rxrings[idx]);
	if (ret) {
		release_worker_idx(idx);
		return ret;
	}
	runqueue = &shared->runqueues[this_cpu];

	if (!reuse_worker_idx(runqueue, idx, &pos)) {
		reclaim_worker_indices(ncpus);
		if (!reuse_worker_idx(runqueue, idx, &pos)) {
			pos = atomic_fetch_add_explicit(&runqueue->num_workers, 1, memory_order_acq_rel);
			if (pos >= USCHED_MAX_WORKERS) {
				atomic_fetch_sub_explicit(&runqueue->num_workers, 1, memory_order_relaxed);
				close(shared->shared_rxrings[idx].ring_fd);
				release_worker_idx(idx);
				return -ENOSPC;
			}
			atomic_store_explicit(&runqueue->worker_indices[pos], idx, memory_order_release);
		}
	}
	{
		int expected = IDX_UTHREAD_UNBOUND;
		atomic_compare_exchange_strong_explicit(&runqueue->current_worker_idx, &expected, idx,
			memory_order_acq_rel, memory_order_relaxed);
	}

	atomic_store_explicit(&shared->workers[idx].state, USCHED_WORKER_ACTIVE, memory_order_release);
	atomic_fetch_add_explicit(&runqueue->generation, 1, memory_order_release);

	worker_idx = idx;
	this_rxring = *rxring;

	intr_enable_cb = enable_cb;
	intr_disable_cb = disable_cb;
	intr_opaque = enable_opaque;

	(void)set_priority(0, PRIO_WORKER_IDLE);

	return 0;
}

void usched_deinit(void)
{
	if (rxring_states) {
		for (int i = 0; i < USCHED_MAX_WORKERS; i++) {
			if (mapped_tid[i]) {
				release_rxring(&rxring_states[i]);
			}
		}
		free(rxring_states);
		rxring_states = NULL;
	}

	for (int i = 0; i < USCHED_MAX_WORKERS; i++) {
		if (ring_fds[i] >= 0)
			close(ring_fds[i]);
		ring_fds[i] = -1;
	}

	if (worker_idx != IDX_UTHREAD_UNBOUND
            && shared->workers[worker_idx].role == WORKER_APPLICATION
            && shared->shared_rxrings[worker_idx].ring_fd >= 0)
		close(shared->shared_rxrings[worker_idx].ring_fd);

	memset(mapped_tid, 0, sizeof(mapped_tid));

	poll_list_count = 0;
	poll_list_generation = UINT64_MAX;

	if (worker_idx != IDX_UTHREAD_UNBOUND && shared) {
		(void)reset_policy(0);

		admin_lock();
		release_worker_idx(worker_idx);
		atomic_fetch_add_explicit(&shared->runqueues[this_cpu].generation, 1, memory_order_release);
		if (!atomic_load_explicit(&shared->global.num_workers, memory_order_acquire)
				&& shm_unlink(SHM_USCHED) && errno != ENOENT)
			fprintf(stderr, "usched: shm_unlink(%s): %s\n", SHM_USCHED, strerror(errno));
		admin_unlock();
	}

	worker_idx = IDX_UTHREAD_UNBOUND;
}

void usched_update_rx_tail(void)
{
	if (worker_idx >= 0) {
        const void *loc = this_rxring.consumer_idx_word;
        uint8_t width = shared->shared_rxrings[worker_idx].layout.idx_width;
        uint64_t mask = this_rxring.consumer_mask;
        uint8_t shift = this_rxring.consumer_shift;

        uint64_t val = (load_word_atomic(loc, width) & mask) >> shift;

        atomic_store_explicit(&shared->shared_rxrings[worker_idx].consumer_idx, val, memory_order_relaxed);
    }
}

void usched_cancel_idle(void)
{
	if (worker_idx >= 0) {
		struct runqueue *runqueue = &shared->runqueues[this_cpu];

		if (atomic_exchange_explicit(&runqueue->idle_started_at, 0, memory_order_acq_rel)) {
			atomic_fetch_add_explicit(&runqueue->worker_active, 1, memory_order_release);
			(void)syscall(SYS_futex, &runqueue->worker_active, FUTEX_WAKE, 1, NULL, NULL, 0);
		}
	}
}

void usched_poll_and_reschedule(void)
{
	struct worker *self;
	struct runqueue *runqueue;
	uint64_t started_at;

	if (worker_idx < 0 || !shared)
		return;

	self = &shared->workers[worker_idx];
	runqueue = &shared->runqueues[this_cpu];

	while (1) {
		struct worker *next = pick_next_worker();

		if (!next)
			break;
		atomic_store_explicit(&runqueue->idle_started_at, 0, memory_order_release);
		if (next == self)
			return;
		{
			int idx = (int)(next - shared->workers);
			int ret;

			TRACE_PRE_PRIORITY_SWITCH(gettid(), worker_idx, next->tid, idx);
			ret = set_priority(next->tid, PRIO_WORKER_ACTIVE);
			TRACE_POST_PRIORITY_SWITCH(gettid(), worker_idx);

			if (ret == -ESRCH) {
				release_worker_idx(idx);
				atomic_fetch_add_explicit(&runqueue->generation, 1, memory_order_release);
				continue;
			}

			(void)set_priority(0, PRIO_WORKER_IDLE);
			TRACE_POST_PRIORITY_SWITCH(gettid(), worker_idx);
		}
		return;
	}
	started_at = atomic_load_explicit(&runqueue->idle_started_at, memory_order_acquire);
	if (!started_at) {
		uint64_t expected = 0;

		started_at = rdtsc_relaxed();
		if (!atomic_compare_exchange_strong_explicit(&runqueue->idle_started_at, &expected,
				started_at, memory_order_acq_rel, memory_order_acquire))
			started_at = expected;
	}
	if (rdtsc_relaxed() - started_at < shared->global.idle_spin_ticks
			|| !intr_enable_cb || intr_enable_cb(intr_opaque))
		return;

    {
        struct rte_epoll_event ev;
        int ret;

        ret = rte_epoll_wait_interruptible(RTE_EPOLL_PER_THREAD, &ev, 1, -1);
        if (ret < 0 && errno != EINTR) {
            fprintf(stderr, "usched: worker rte_epoll_wait: %s\n", strerror(errno));
            exit(1);
        }
    }

	if (intr_disable_cb && intr_disable_cb(intr_opaque))
		fprintf(stderr, "usched: worker interrupt disable failed\n");
}

int usched_preemption_loop(int cpu, uint64_t quantum_ns)
{
	struct runqueue *runqueue;
	struct sched_param priority;
	struct timespec sleep;
	int ncpus = get_nprocs();
	int idx;
	int pos;
	int ret;

	if (worker_idx != IDX_UTHREAD_UNBOUND || cpu < 0 || cpu >= ncpus || !quantum_ns)
		return -EINVAL;
	if (sched_getparam(0, &priority) ||
		(sched_getscheduler(0) != SCHED_RR && sched_getscheduler(0) != SCHED_FIFO) ||
		priority.sched_priority < PRIO_JOIN)
		return -EPERM;

	this_cpu = cpu;
	ret = init_shmem(cpu, ncpus);
	if (ret)
		return ret;

	idx = allocate_worker_idx();
	if (idx == -ENOSPC) {
		reclaim_worker_indices(ncpus);
		idx = allocate_worker_idx();
	}
	if (idx < 0)
		return idx;

	shared->workers[idx].tid = gettid();
	shared->workers[idx].cpu = cpu;
	shared->workers[idx].role = WORKER_PREEMPTION;
	atomic_store_explicit(&shared->workers[idx].state, USCHED_WORKER_STARTING, memory_order_release);

	runqueue = &shared->runqueues[cpu];
	{
		int expected = IDX_UTHREAD_UNBOUND;

		if (!atomic_compare_exchange_strong_explicit(
			&runqueue->preemption_worker_idx,
			&expected,
			idx,
			memory_order_acq_rel,
			memory_order_relaxed)) {
			release_worker_idx(idx);
			return -EEXIST;
		}
	}

	if (!reuse_worker_idx(runqueue, idx, &pos)) {
		reclaim_worker_indices(ncpus);
		if (!reuse_worker_idx(runqueue, idx, &pos)) {
			pos = atomic_fetch_add_explicit(&runqueue->num_workers, 1, memory_order_acq_rel);
			if (pos >= USCHED_MAX_WORKERS) {
				atomic_fetch_sub_explicit(&runqueue->num_workers, 1, memory_order_relaxed);
				release_worker_idx(idx);
				return -ENOSPC;
			}
			atomic_store_explicit(&runqueue->worker_indices[pos], idx, memory_order_release);
		}
	}

	atomic_store_explicit(&shared->workers[idx].state, USCHED_WORKER_ACTIVE, memory_order_release);
	atomic_fetch_add_explicit(&runqueue->generation, 1, memory_order_release);
	worker_idx = idx;

	ret = set_priority(0, PRIO_PREEMPT);
	if (ret) {
		usched_deinit();
		return ret;
	}

	sleep.tv_sec = quantum_ns / UINT64_C(1000000000);
	sleep.tv_nsec = quantum_ns % UINT64_C(1000000000);
	while (1) {
		uint32_t active = atomic_load_explicit(&runqueue->worker_active, memory_order_acquire);
		struct worker *next = pick_next_worker();

		if (!next) {
			if (syscall(SYS_futex, &runqueue->worker_active, FUTEX_WAIT,
				active, NULL, NULL, 0) < 0 && errno != EAGAIN && errno != EINTR) {
				ret = -errno;
				break;
			}
			continue;
		}

		ret = set_priority(next->tid, PRIO_WORKER_ACTIVE);
		if (ret == -ESRCH) {
			release_worker_idx((int)(next - shared->workers));
			atomic_fetch_add_explicit(&runqueue->generation, 1, memory_order_release);
			continue;
		}
		if (ret)
			break;
		if (nanosleep(&sleep, NULL) && errno != EINTR) {
			ret = -errno;
			break;
		}
	}

	usched_deinit();
	return ret;
}
