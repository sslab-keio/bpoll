#ifndef BPOLL_MLX5_H
#define BPOLL_MLX5_H

#include <stdint.h>

#include <usched.h>

struct mlx5_bpoll_cqe {
	uint8_t payload[62];
	uint8_t validity_iteration_count;
	uint8_t op_own;
};

int mlx5_export_rxring(void *rx_queue, struct rxring *rxring);

#endif
