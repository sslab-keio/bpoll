OBJ := mlx5.o
SRCS := mlx5.c

DPDK_SRC ?= $(abspath ../../../dependency/dpdk)
MLX5_DPDK_BUILD ?= $(DPDK_SRC)/build

MLX5_CFLAGS := \
	-I$(MLX5_DPDK_BUILD) \
	-I$(MLX5_DPDK_BUILD)/drivers/common/mlx5 \
	-I$(DPDK_SRC)/lib/ethdev \
	-I$(DPDK_SRC)/lib/eal/include \
	-I$(DPDK_SRC)/lib/eal/linux/include \
	-I$(DPDK_SRC)/lib/eal/x86/include \
	-I$(DPDK_SRC)/lib/eal/common \
	-I$(DPDK_SRC)/lib/eal \
	-I$(DPDK_SRC)/lib/kvargs \
	-I$(DPDK_SRC)/lib/metrics \
	-I$(DPDK_SRC)/lib/telemetry \
	-I$(DPDK_SRC)/lib/net \
	-I$(DPDK_SRC)/lib/mbuf \
	-I$(DPDK_SRC)/lib/mempool \
	-I$(DPDK_SRC)/lib/ring \
	-I$(DPDK_SRC)/lib/meter \
	-I$(DPDK_SRC)/lib/pci \
	-I$(DPDK_SRC)/lib/hash \
	-I$(DPDK_SRC)/lib/rcu \
	-I$(DPDK_SRC)/drivers/bus/pci \
	-I$(DPDK_SRC)/drivers/bus/pci/linux \
	-I$(DPDK_SRC)/drivers/bus/vdev \
	-I$(DPDK_SRC)/drivers/bus/auxiliary \
	-I$(DPDK_SRC)/drivers/common/mlx5 \
	-I$(DPDK_SRC)/drivers/common/mlx5/linux \
	-I$(DPDK_SRC)/drivers/net/mlx5 \
	-I$(DPDK_SRC)/drivers/net/mlx5/linux \
	-I$(DPDK_SRC)/drivers/net/mlx5/hws \
	$(shell pkg-config --cflags libnl-3.0 2>/dev/null) \
	-Wno-error=packed-not-aligned -Wno-error=address-of-packed-member

$(OBJ): $(SRCS)
	cc $(DRIVER_CFLAGS) $(MLX5_CFLAGS) -c -o $@ $^

.PHONY: clean
clean:
	rm -f $(OBJ)
