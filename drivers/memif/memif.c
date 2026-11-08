#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include "rte_eth_memif.h"

#include "memif.h"

static memif_ring_t *memif_ring(struct memif_queue *queue)
{
	struct pmd_process_private *private;
	struct memif_region *region;

	if (!queue || queue->region >= ETH_MEMIF_MAX_REGION_NUM)
		return NULL;

	private = rte_eth_devices[queue->in_port].process_private;
	if (!private)
		return NULL;

	region = private->regions[queue->region];
	if (!region
        || !region->addr
        || queue->ring_offset > region->region_size
        || sizeof(memif_ring_t) > region->region_size - queue->ring_offset)
		return NULL;

	return (memif_ring_t *)((uint8_t *)region->addr + queue->ring_offset);
}

int memif_rx_interrupt_fd(void *rx_queue)
{
	struct memif_queue *queue = rx_queue;

	if (!queue || queue->type != MEMIF_RING_C2S || !queue->intr_handle)
		return -ENOTSUP;
	if (rte_intr_fd_get(queue->intr_handle) < 0)
		return -ENOTSUP;

	return rte_intr_fd_get(queue->intr_handle);
}

int memif_enable_rx_interrupt(void *rx_queue)
{
	struct memif_queue *queue = rx_queue;
	memif_ring_t *ring = memif_ring(queue);

	if (memif_rx_interrupt_fd(queue) < 0 || !ring)
		return -ENOTSUP;

	__atomic_fetch_and(&ring->flags, (uint16_t)~MEMIF_RING_FLAG_MASK_INT, __ATOMIC_RELEASE);

	if (__atomic_load_n(&ring->head, __ATOMIC_ACQUIRE) == queue->last_head)
		return 0;

	__atomic_fetch_or(&ring->flags, MEMIF_RING_FLAG_MASK_INT, __ATOMIC_RELEASE);

	return -EAGAIN;
}

int memif_disable_rx_interrupt(void *rx_queue)
{
	struct memif_queue *queue = rx_queue;
	memif_ring_t *ring = memif_ring(queue);
	uint64_t value;
	ssize_t size;

	if (memif_rx_interrupt_fd(queue) < 0 || !ring)
		return -ENOTSUP;

	__atomic_fetch_or(&ring->flags, MEMIF_RING_FLAG_MASK_INT, __ATOMIC_RELEASE);

	size = read(memif_rx_interrupt_fd(queue), &value, sizeof(value));
	if (size < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
		return -errno;

	return 0;
}

int memif_export_rxring(void *rx_queue, struct rxring *rxring)
{
	struct memif_queue *queue = rx_queue;
	struct pmd_process_private *private;
	struct memif_region *region;
	long page_size;

	if (!queue)
		return -EINVAL;
	private = rte_eth_devices[queue->in_port].process_private;
	if (!private || queue->region >= ETH_MEMIF_MAX_REGION_NUM)
		return -EAGAIN;
	region = private->regions[queue->region];
	if (!region || !region->addr || region->fd < 0 || !queue->log2_ring_size)
		return -EAGAIN;
	if (queue->ring_offset > region->region_size ||
		sizeof(memif_ring_t) > region->region_size - queue->ring_offset)
		return -ERANGE;
	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0)
		return -errno;
	if (queue->type == MEMIF_RING_C2S) {
		*rxring = (struct rxring) {
			.ring = memif_ring(queue),
			.consumer_idx_word = &queue->last_head,
			.consumer_mask = UINT16_MAX,
			.ring_len = sizeof(memif_ring_t),
			.fd = region->fd,
			.fd_offset = queue->ring_offset,
			.page_size = page_size,
			.layout = {
				.ring_size = 1,
				.descriptor_size = sizeof(memif_ring_t),
				.producer_offset = offsetof(memif_ring_t, head),
				.idx_width = sizeof(queue->last_head),
				.has_producer = 1,
			},
		};
		return rxring->ring ? 0 : -ERANGE;
	}
	if (queue->type != MEMIF_RING_S2C)
		return -EINVAL;

	*rxring = (struct rxring) {
		.ring = memif_ring(queue),
		.consumer_idx_word = &queue->last_tail,
		.consumer_mask = UINT16_MAX,
		.ring_len = sizeof(memif_ring_t),
		.fd = region->fd,
		.fd_offset = queue->ring_offset,
		.page_size = page_size,
		.layout = {
			.ring_size = 1,
			.descriptor_size = sizeof(memif_ring_t),
			.producer_offset = offsetof(memif_ring_t, tail),
			.idx_width = sizeof(queue->last_tail),
			.has_producer = 1,
		},
	};
	if (!rxring->ring)
		return -ERANGE;
	return 0;
}
