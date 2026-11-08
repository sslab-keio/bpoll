OBJ := bnxt.o
SRCS := bnxt.c

DPDK_SRC ?= $(abspath ../../../dependency/dpdk)

BNXT_CFLAGS := \
	-I$(DPDK_SRC)/drivers/net/bnxt \
	-I$(DPDK_SRC)/drivers/net/bnxt/tf_core \
	-I$(DPDK_SRC)/drivers/net/bnxt/tf_ulp \
	-I$(DPDK_SRC)/drivers/net/bnxt/tf_ulp/generic_templates \
	-I$(DPDK_SRC)/drivers/net/bnxt/hcapi \
	-I$(DPDK_SRC)/drivers/net/bnxt/hcapi/cfa \
	-I$(DPDK_SRC)/lib/ethdev \
	-I$(DPDK_SRC)/lib/eal/include \
	-I$(DPDK_SRC)/drivers/bus/pci

$(OBJ): $(SRCS)
	cc $(DRIVER_CFLAGS) $(BNXT_CFLAGS) -c -o $@ $^

.PHONY: clean
clean:
	rm -f $(OBJ)
