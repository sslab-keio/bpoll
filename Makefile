WORKDIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
DPDK_SRC ?= $(WORKDIR)../dependency/dpdk

LIBS := libbpoll.so libusched-memif.so libepoll.so libyield.so
TEST := tests/test_core
MEMIF_SCHED := tests/memif_sched
PREEMPT := preempt

SRCS := usched.c shmem.c
OBJS := $(SRCS:.c=.o)

CC := cc
USCHED_MAGIC := 0x$(shell od -An -N4 -tx4 /dev/urandom | tr -d '[:space:]' | sed 's/.$$/1/')U

ifneq ($(MAKECMDGOALS),clean)
ifneq ($(shell pkg-config --exists libdpdk && echo 0),0)
$(error "no installation of DPDK found")
endif
DPDK_CFLAGS := $(shell pkg-config --cflags libdpdk)
DPDK_LIBS := $(shell pkg-config --libs libdpdk)
else
DPDK_CFLAGS :=
DPDK_LIBS :=
endif

CFLAGS := -g -Werror -Wall -Wunused-function -Wextra -O3
CFLAGS += -I$(abspath .)
CFLAGS += $(DPDK_CFLAGS)
CFLAGS += -I$(DPDK_SRC)/drivers/net/ixgbe
CFLAGS += -I$(DPDK_SRC)/drivers/net/ixgbe/base
CFLAGS += -DALLOW_EXPERIMENTAL_API
CFLAGS += -DUSCHED_MAGIC=$(USCHED_MAGIC)
CFLAGS += -fPIC

ifeq ($(TRACE),1)
CFLAGS += -DTRACE
endif
DRIVER_DIRS := drivers/ixgbevf drivers/mlx5 drivers/memif drivers/iavf drivers/bnxt
DRIVERS := $(foreach dir,$(DRIVER_DIRS),$(dir)/$(notdir $(dir)).o)

.PHONY: all
all: $(LIBS)

libbpoll.so: hook.o $(DRIVERS) $(OBJS)
	$(CC) $(CFLAGS) -fPIC -shared -Wl,--no-undefined -o $@ $^ \
		$(DPDK_LIBS) -ldl

libusched-memif.so: memif_hook.o libbpoll.so
	$(CC) $(CFLAGS) -fPIC -shared -Wl,--no-undefined -Wl,-rpath,'$$ORIGIN' -o $@ memif_hook.o \
		-L. -lbpoll $(DPDK_LIBS) -ldl

libepoll.so: other-hooks/hook-epoll.o
	$(CC) $(CFLAGS) -fPIC -shared -Wl,--no-undefined -o $@ $^ $(DPDK_LIBS) -ldl

libyield.so: other-hooks/hook-yield.o
	$(CC) $(CFLAGS) -fPIC -shared -Wl,--no-undefined -o $@ $^ $(DPDK_LIBS) -ldl

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: FORCE
FORCE:

usched.o: FORCE

.PHONY: $(DRIVERS)
$(DRIVERS):
	$(MAKE) DRIVER_CFLAGS='$(CFLAGS)' -C $(dir $@)

.PHONY: driver_clean
driver_clean:
	@for driver_dir in $(DRIVER_DIRS); do \
		$(MAKE) -C $$driver_dir clean || exit $$?; \
	done

$(TEST): tests/test_core.c
	$(CC) $(CFLAGS) -o $@ $<

$(MEMIF_SCHED): tests/memif_sched.c
	$(CC) $(CFLAGS) -o $@ $< $(DPDK_LIBS)

$(PREEMPT): preempt.c libbpoll.so
	$(CC) $(CFLAGS) -Wl,-rpath,'$$ORIGIN' -L. -o $@ $< -lbpoll $(DPDK_LIBS) -ldl

.PHONY: test
test: $(TEST)
	./$(TEST)

.PHONY: test-e2e
test-e2e: all test

.PHONY: quick-mlx5
quick-mlx5: all
	sh tests/quick_mlx5.sh

.PHONY: quick-mlx5-12
quick-mlx5-12: all
	USCHED_NIC=mlx5 sh tests/quick_many.sh 12


.PHONY: quick-mlx5-24
quick-mlx5-24: all
	USCHED_NIC=mlx5 sh tests/quick_many.sh 24

.PHONY: quick-ixgbe-24
quick-ixgbe-24: all
	USCHED_NIC=ixgbe sh tests/quick_many.sh 24

.PHONY: quick-hooks-mlx5-24
quick-hooks-mlx5-24: all
	sh tests/quick_hooks_mlx5_24.sh

.PHONY: pollings-mlx5-12
pollings-mlx5-12: all
	sh tests/pollings_mlx5_12.sh

.PHONY: local-memif
local-memif: all
	sh tests/local_memif.sh

.PHONY: memif-sched
memif-sched: all $(MEMIF_SCHED)
	sh tests/memif_sched.sh $(MEMIF_SCHED) tests/plot_memif_sched.py

.PHONY: memif-preemption
memif-preemption: all $(MEMIF_SCHED) $(PREEMPT)
	sh tests/memif_sched.sh $(MEMIF_SCHED) tests/plot_memif_sched.py preempt

.PHONY: preemption
preemption: all $(PREEMPT)
	sh tests/preemption.sh

.PHONY: quick-mlx5-24-policies
quick-mlx5-24-policies: all
	sh tests/quick_mlx5_policies.sh

.PHONY: clean
clean: driver_clean
	rm -f $(OBJS) hook.o memif_hook.o other-hooks/*.o $(LIBS) $(TEST) $(MEMIF_SCHED) $(PREEMPT)
