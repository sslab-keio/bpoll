#ifndef USCHED_H
#define USCHED_H

#include <inttypes.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <sys/types.h>

#define USCHED_MAX_WORKERS 128
#define IDX_UTHREAD_UNBOUND (-1)

#define PRIO_WORKER_IDLE 1
#define PRIO_WORKER_ACTIVE 2
#define PRIO_PREEMPT 3
#define PRIO_JOIN 4

enum usched_worker_state {
	USCHED_WORKER_FREE = 0,
	USCHED_WORKER_STARTING,
	USCHED_WORKER_ACTIVE,
};

enum worker_role {
	WORKER_APPLICATION,
	WORKER_PREEMPTION,
};

struct worker {
	_Atomic int state;
	pid_t tid;
	int cpu;
	enum worker_role role;
};

struct runqueue {
	_Atomic int num_workers;
	_Atomic int current_worker_idx;
	_Atomic uint64_t generation;
	_Atomic uint64_t idle_started_at;
	_Atomic uint32_t worker_active;
	_Atomic int preemption_worker_idx;
	_Atomic int worker_indices[USCHED_MAX_WORKERS];
};

struct usched_global {
	_Atomic uint32_t magic;
	uint32_t num_cpus;
	uint64_t tsc_hz;
	uint64_t idle_spin_us;
	uint64_t idle_spin_ticks;
	_Atomic bool admin_lock;
	_Atomic unsigned int num_workers;
	_Atomic unsigned char slots[USCHED_MAX_WORKERS];
};

struct descriptor_layout {
	uint32_t ring_size;
	uint16_t descriptor_size;
	uint16_t event_source_offset;
	uint16_t producer_offset;
	uint8_t idx_width;
	uint8_t event_source_width;
	uint8_t has_producer;
	uint64_t event_mask;
	uint64_t event_expected;
	uint64_t phase_mask;
};

struct rxring_state {
	const void *ring;
	const void *consumer_idx;
	const void *producer_idx;
	struct descriptor_layout layout;
	void *mapping;
	size_t mapping_len;
};

static inline uint64_t load_word_atomic(const void *address, uint8_t width)
{
	switch (width) {
		case 1:
			return __atomic_load_n((const uint8_t *)address, __ATOMIC_RELAXED);
		case 2:
			return __atomic_load_n((const uint16_t *)address, __ATOMIC_RELAXED);
		case 4:
			return __atomic_load_n((const uint32_t *)address, __ATOMIC_RELAXED);
		case 8:
			return __atomic_load_n((const uint64_t *)address, __ATOMIC_RELAXED);
		default:
			return 0;
	}
}

static inline bool descriptor_has_event(const struct rxring_state *state)
{
	const uint8_t *entry;
	uint64_t consumer;
	uint64_t event;
	uint64_t expected;

	consumer = load_word_atomic(state->consumer_idx, state->layout.idx_width);

	if (state->producer_idx)
		return load_word_atomic(state->producer_idx, state->layout.idx_width) != consumer;

	entry = state->ring + consumer % state->layout.ring_size * state->layout.descriptor_size;
	event = load_word_atomic(entry + state->layout.event_source_offset,
		state->layout.event_source_width);

	expected = state->layout.event_expected;
	if ((consumer / state->layout.ring_size) & 1)
		expected ^= state->layout.phase_mask;

	return (event & state->layout.event_mask) == expected;
}

struct shared_rxring {
	pid_t tid;
	int ring_fd;
	uint64_t ring_fd_offset;
	uint64_t ring_page_size;
	_Atomic uint32_t consumer_idx;
	struct descriptor_layout layout;
};

struct usched_shared {
	struct usched_global global;
	struct worker workers[USCHED_MAX_WORKERS];
	struct shared_rxring shared_rxrings[USCHED_MAX_WORKERS];
	struct runqueue runqueues[];
};

struct rxring {
	const void *ring;
	const void *consumer_idx_word;
	uint32_t consumer_mask;
	uint8_t consumer_shift;
	size_t ring_len;
	int fd;
	uint64_t fd_offset;
	uint64_t page_size;
	struct descriptor_layout layout;
};

int usched_init(
	const struct rxring *rxring,
	int cpu,
	int (*cb_intr_enable)(void *),
	int (*cb_intr_disable)(void *),
	void *cb_opaque);
void usched_deinit(void);
void usched_update_rx_tail(void);
void usched_cancel_idle(void);
void usched_poll_and_reschedule(void);
int usched_preemption_loop(int cpu, uint64_t quantum_ns);

#endif
