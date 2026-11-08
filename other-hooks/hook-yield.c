#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sched.h>
#include <rte_common.h>
#include <rte_ethdev.h>

#define MAX_IDLE_POLLS 1

struct queue_state {
	uint16_t port_id;
	uint64_t empty_polls;
	const struct rte_eth_rxtx_callback *rx_callback;
};

static struct queue_state hooked = {
	.port_id = UINT16_MAX,
};

static void *resolve_next(const char *symbol)
{
	void *fn;

	dlerror();
	fn = dlsym(RTLD_NEXT, symbol);
	if (!fn)
		rte_panic("hook-yield: resolve %s: %s\n", symbol, dlerror());
	return fn;
}

static uint16_t cb_post_empty_poll(
	uint16_t port_id,
	uint16_t queue_id __rte_unused,
	struct rte_mbuf **pkts __rte_unused,
	uint16_t nb_pkts,
	uint16_t max_pkts __rte_unused,
	void *opaque)
{
	struct queue_state *queue = opaque;

	if (port_id != queue->port_id)
		return nb_pkts;
	if (nb_pkts) {
		queue->empty_polls = 0;
		return nb_pkts;
	}
	if (++queue->empty_polls > MAX_IDLE_POLLS) {
		sched_yield();
		queue->empty_polls = 0;
	}
	return nb_pkts;
}

static void unregister_hook(void)
{
	if (!hooked.rx_callback)
		return;
	if (rte_eth_remove_rx_callback(hooked.port_id, 0, hooked.rx_callback))
		fprintf(stderr, "hook-yield: remove RX callback failed\n");
	memset(&hooked, 0, sizeof(hooked));
	hooked.port_id = UINT16_MAX;
}

int rte_eth_dev_start(uint16_t port_id)
{
	typedef int (*fn_t)(uint16_t);
	static fn_t real_fn;
	int ret;

	if (!real_fn)
		real_fn = (fn_t)resolve_next("rte_eth_dev_start");
	if (hooked.port_id != UINT16_MAX) {
		fprintf(stderr, "hook-yield: one port is supported\n");
		return -ENOTSUP;
	}
	if ((ret = real_fn(port_id)))
		return ret;

	hooked.port_id = port_id;
	hooked.rx_callback = rte_eth_add_rx_callback(port_id, 0, cb_post_empty_poll, &hooked);
	if (hooked.rx_callback)
		return 0;

	ret = rte_errno ? -rte_errno : -EIO;
	unregister_hook();
	(void)rte_eth_dev_stop(port_id);
	return ret;
}

int rte_eth_dev_stop(uint16_t port_id)
{
	typedef int (*fn_t)(uint16_t);
	static fn_t real_fn;

	if (!real_fn)
		real_fn = (fn_t)resolve_next("rte_eth_dev_stop");
	if (hooked.port_id == port_id)
		unregister_hook();
	return real_fn(port_id);
}

int rte_eth_dev_close(uint16_t port_id)
{
	typedef int (*fn_t)(uint16_t);
	static fn_t real_fn;

	if (!real_fn)
		real_fn = (fn_t)resolve_next("rte_eth_dev_close");
	if (hooked.port_id == port_id)
		unregister_hook();
	return real_fn(port_id);
}
