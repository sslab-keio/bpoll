OBJ := iavf.o
SRCS := iavf.c

DPDK_SRC ?= $(abspath ../../../dependency/dpdk)

IAVF_CFLAGS := \
	-I$(DPDK_SRC)/drivers/net/iavf \
	-I$(DPDK_SRC)/drivers/common/iavf \
	-I$(DPDK_SRC)/lib/ethdev \
	-I$(DPDK_SRC)/lib/eal/include \
	-I$(DPDK_SRC)/drivers/bus/pci

$(OBJ): $(SRCS)
	cc $(DRIVER_CFLAGS) $(IAVF_CFLAGS) -c -o $@ $^

.PHONY: clean
clean:
	rm -f $(OBJ)
