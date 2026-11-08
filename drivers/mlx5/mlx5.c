#include <errno.h>
#include <rte_byteorder.h>

#include "mlx5_rx.h"

#include "mlx5.h"

_Static_assert(sizeof(struct mlx5_bpoll_cqe) == 64,
	"mlx5 completion queue entries must be 64 bytes");
_Static_assert(offsetof(struct mlx5_rxq_data, rq_db) == 8,
	"mlx5 RX queue consumer word changed");

int mlx5_export_rxring(void *rx_queue, struct rxring *rxring)
{
	struct mlx5_rxq_data *rxq = rx_queue;

	if (rxq == NULL || rxq->cqes == NULL)
		return -EINVAL;
	if (rxq->cqe_n == 0 || rxq->cqe_n >= 31)
		return -ENOTSUP;
	if (rxq->cqe_comp_layout) {
		fprintf(stderr, "mlx5 CQ layout=compressed is unsupported\n");
		return -ENOTSUP;
	}

	*rxring = (struct rxring) {
		.ring = (const void *)(uintptr_t)rxq->cqes,
		.consumer_idx_word = (const char *)rxq + sizeof(uint32_t),
		.consumer_mask = UINT32_C(0x7fffff80),
		.consumer_shift = 7,
		.ring_len = ((size_t)1 << rxq->cqe_n) * sizeof(struct mlx5_bpoll_cqe),
		.fd = -1,
		.layout = {
			.ring_size = UINT32_C(1) << rxq->cqe_n,
			.descriptor_size = sizeof(struct mlx5_bpoll_cqe),
			.event_source_offset = offsetof(struct mlx5_bpoll_cqe, op_own),
			.idx_width = sizeof(uint32_t),
			.event_source_width = sizeof(((struct mlx5_bpoll_cqe *)0)->op_own),
			.event_mask = 1,
			.phase_mask = 1,
		},
	};
	return 0;
}
