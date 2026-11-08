OBJ := memif.o
SRCS := memif.c

DPDK_SRC ?= $(abspath ../../../dependency/dpdk)

$(OBJ): $(SRCS)
	cc $(DRIVER_CFLAGS) \
		-I$(DPDK_SRC)/lib/ethdev \
		-I$(DPDK_SRC)/lib/eal/include \
		-I$(DPDK_SRC)/drivers/net/memif \
		-c -o $@ $^

.PHONY: clean
clean:
	rm -f $(OBJ)
