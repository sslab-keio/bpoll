#include <errno.h>

#include <rte_byteorder.h>

#include <bnxt.h>
#include <bnxt_cpr.h>
#include <bnxt_ring.h>
#include <bnxt_rxq.h>

#include <usched.h>

int bnxt_export_rxring(void *rx_queue, struct rxring *rxring)
{
	struct bnxt_rx_queue *rxq = rx_queue;
	struct bnxt_cp_ring_info *cpr;

	if (!rxq || !rxq->cp_ring)
		return -EINVAL;

	cpr = rxq->cp_ring;
	if (!cpr->cp_desc_ring || !cpr->cp_ring_struct || !cpr->cp_ring_struct->ring_size)
		return -EINVAL;

	*rxring = (struct rxring) {
		.ring = cpr->cp_desc_ring,
		.consumer_idx_word = &cpr->cp_raw_cons,
		.consumer_mask = UINT32_MAX,
		.ring_len = (size_t)cpr->cp_ring_struct->ring_size * sizeof(*cpr->cp_desc_ring),
		.fd = -1,
		.layout = {
			.ring_size = cpr->cp_ring_struct->ring_size,
			.descriptor_size = sizeof(*cpr->cp_desc_ring),
			.event_source_offset = offsetof(struct cmpl_base, info3_v),
			.idx_width = sizeof(cpr->cp_raw_cons),
			.event_source_width = sizeof(cpr->cp_desc_ring->info3_v),
			.event_mask = rte_cpu_to_le_32(CMPL_BASE_V),
			.event_expected = rte_cpu_to_le_32(CMPL_BASE_V),
			.phase_mask = rte_cpu_to_le_32(CMPL_BASE_V),
		},
	};

	return 0;
}
