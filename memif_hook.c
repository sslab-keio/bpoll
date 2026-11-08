#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <string.h>
#include <sys/epoll.h>

#include <rte_ethdev.h>
#include <rte_epoll.h>

#include "drivers/memif/memif.h"

static void *resolve_next(const char *symbol)
{
	void *fn;

	dlerror();
	fn = dlsym(RTLD_NEXT, symbol);
	if (!fn)
		return NULL;
	return fn;
}

static void *memif_rx_queue(uint16_t port_id, uint16_t queue_id)
{
	struct rte_eth_dev_info info;

	if (rte_eth_dev_info_get(port_id, &info) || strcmp(info.driver_name, "net_memif"))
		return NULL;
	if (port_id >= RTE_MAX_ETHPORTS || queue_id >= RTE_MAX_QUEUES_PER_PORT)
		return NULL;
	return rte_eth_fp_ops[port_id].rxq.data[queue_id];
}

int rte_eth_dev_rx_intr_enable(uint16_t port_id, uint16_t queue_id)
{
	typedef int (*fn_t)(uint16_t, uint16_t);
	static fn_t real_fn;
	void *queue = memif_rx_queue(port_id, queue_id);

	if (queue)
		return memif_enable_rx_interrupt(queue);
	if (!real_fn)
		real_fn = (fn_t)resolve_next("rte_eth_dev_rx_intr_enable");
	return real_fn ? real_fn(port_id, queue_id) : -ENOSYS;
}

int rte_eth_dev_rx_intr_disable(uint16_t port_id, uint16_t queue_id)
{
	typedef int (*fn_t)(uint16_t, uint16_t);
	static fn_t real_fn;
	void *queue = memif_rx_queue(port_id, queue_id);

	if (queue)
		return memif_disable_rx_interrupt(queue);
	if (!real_fn)
		real_fn = (fn_t)resolve_next("rte_eth_dev_rx_intr_disable");
	return real_fn ? real_fn(port_id, queue_id) : -ENOSYS;
}

int rte_eth_dev_rx_intr_ctl_q(
	uint16_t port_id,
	uint16_t queue_id,
	int epfd,
	int op,
	void *data)
{
	typedef int (*fn_t)(uint16_t, uint16_t, int, int, void *);
	static fn_t real_fn;
	void *queue = memif_rx_queue(port_id, queue_id);
	struct rte_epoll_event event = {
		.epdata = {
			.event = EPOLLIN,
			.data = data,
		},
	};
	int fd;

	if (queue) {
		fd = memif_rx_interrupt_fd(queue);
		if (fd < 0)
			return fd;
		if (op == RTE_INTR_EVENT_ADD)
			return rte_epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &event);
		if (op == RTE_INTR_EVENT_DEL)
			return rte_epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
		return -EINVAL;
	}
	if (!real_fn)
		real_fn = (fn_t)resolve_next("rte_eth_dev_rx_intr_ctl_q");
	return real_fn ? real_fn(port_id, queue_id, epfd, op, data) : -ENOSYS;
}
