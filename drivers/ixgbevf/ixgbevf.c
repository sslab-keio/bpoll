#include <errno.h>

#include <usched.h>

#include "ixgbevf.h"

int ixgbevf_export_rxring(void *rx_queue, struct rxring *rxring)
{
	struct ixgbe_rx_queue *rxq = rx_queue;

	if (!rxq || !rxq->rx_ring || !rxq->nb_rx_desc)
		return -EINVAL;

	*rxring = (struct rxring) {
		.ring = (const void *)rxq->rx_ring,
		.consumer_idx_word = &rxq->rx_tail,
		.consumer_mask = UINT16_MAX,
		.ring_len = (size_t)rxq->nb_rx_desc * sizeof(*rxq->rx_ring),
		.fd = -1,
		.layout = {
			.ring_size = rxq->nb_rx_desc,
			.descriptor_size = sizeof(*rxq->rx_ring),
			.event_source_offset = offsetof(union ixgbe_adv_rx_desc, wb.upper.status_error),
			.idx_width = sizeof(rxq->rx_tail),
			.event_source_width = sizeof(rxq->rx_ring->wb.upper.status_error),
			.event_mask = rte_cpu_to_le_32(IXGBE_RXDADV_STAT_DD),
			.event_expected = rte_cpu_to_le_32(IXGBE_RXDADV_STAT_DD),
		},
	};

	return 0;
}
