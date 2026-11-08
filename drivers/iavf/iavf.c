#include <errno.h>
#include <rte_byteorder.h>
#include <iavf.h>
#include <iavf_rxtx.h>
#include <usched.h>

_Static_assert(sizeof(union iavf_rx_flex_desc) == sizeof(union iavf_rx_desc),
        "IAVF legacy and flex RX descriptor sizes differ");
_Static_assert(offsetof(union iavf_rx_desc, wb.qword1.status_error_len) + sizeof(((union iavf_rx_desc *)0)->wb.qword1.status_error_len) <= sizeof(union iavf_rx_desc),
        "IAVF legacy DD field is outside the RX descriptor");
_Static_assert(offsetof(union iavf_rx_flex_desc, wb.status_error0) + sizeof(((union iavf_rx_flex_desc *)0)->wb.status_error0) <= sizeof(union iavf_rx_flex_desc),
        "IAVF flex DD field is outside the RX descriptor");

int iavf_export_rxring(void *rx_queue, struct rxring *rxring)
{
	struct iavf_rx_queue *rxq = rx_queue;

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
			.idx_width = sizeof(rxq->rx_tail),
		},
	};

	if (rxq->rxdid <= IAVF_RXDID_LEGACY_1) {
		rxring->layout.event_source_offset = offsetof(union iavf_rx_desc, wb.qword1.status_error_len);
		rxring->layout.event_source_width  = sizeof(rxq->rx_ring->wb.qword1.status_error_len);
		rxring->layout.event_mask          = rte_cpu_to_le_64(UINT64_C(1) << IAVF_RX_DESC_STATUS_DD_SHIFT);
		rxring->layout.event_expected      = rxring->layout.event_mask;
	} else {
		rxring->layout.event_source_offset = offsetof(union iavf_rx_flex_desc, wb.status_error0);
		rxring->layout.event_source_width  = sizeof(((union iavf_rx_flex_desc *)0)->wb.status_error0);
		rxring->layout.event_mask          = rte_cpu_to_le_16(UINT16_C(1) << IAVF_RX_FLEX_DESC_STATUS0_DD_S);
		rxring->layout.event_expected      = rxring->layout.event_mask;
	}

	return 0;
}
