#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <rte_common.h>
#include <rte_epoll.h>
#include <rte_ethdev.h>

#define MAX_IDLE_POLLS 1

struct queue_state {
	uint16_t port_id;
	uint64_t empty_polls;
	const struct rte_eth_rxtx_callback *rx_callback;
	int interrupt_registered;
};

static struct queue_state hooked = {
	.port_id = UINT16_MAX,
};
static uint16_t configured_port = UINT16_MAX;

static void *resolve_next(const char *symbol)
{
	void *fn;

	dlerror();
	fn = dlsym(RTLD_NEXT, symbol);
	if (!fn)
		rte_panic("hook-epoll: resolve %s: %s\n", symbol, dlerror());
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
	if (++queue->empty_polls <= MAX_IDLE_POLLS)
		return nb_pkts;

	queue->empty_polls = 0;
	if (rte_eth_dev_rx_intr_enable(queue->port_id, 0))
		return nb_pkts;

    {
        struct rte_epoll_event ev;
        int ret;

        ret = rte_epoll_wait_interruptible(RTE_EPOLL_PER_THREAD, &ev, 1, -1);
        if (ret < 0 && errno != EINTR) {
            rte_panic("hook-epoll: rte_epoll_wait: %s\n", strerror(errno));
        }
    }

	if (rte_eth_dev_rx_intr_disable(queue->port_id, 0))
		fprintf(stderr, "hook-epoll: RX interrupt disable failed\n");
	return nb_pkts;
}

static void unregister_hook(void)
{
	if (hooked.rx_callback) {
		if (rte_eth_remove_rx_callback(hooked.port_id, 0, hooked.rx_callback))
			fprintf(stderr, "hook-epoll: remove RX callback failed\n");
	}
	if (hooked.interrupt_registered) {
		(void)rte_eth_dev_rx_intr_ctl_q(
			hooked.port_id,
			0,
			RTE_EPOLL_PER_THREAD,
			RTE_INTR_EVENT_DEL,
			NULL);
	}
	memset(&hooked, 0, sizeof(hooked));
	hooked.port_id = UINT16_MAX;
}

int rte_eth_dev_configure(
	uint16_t port_id,
	uint16_t nb_rx_queue,
	uint16_t nb_tx_queue,
	const struct rte_eth_conf *eth_conf)
{
	typedef int (*fn_t)(uint16_t, uint16_t, uint16_t, const struct rte_eth_conf *);
	static fn_t real_fn;
	struct rte_eth_conf conf;
	int ret;

	if (!eth_conf)
		return -EINVAL;
	if (!real_fn)
		real_fn = (fn_t)resolve_next("rte_eth_dev_configure");
	if (configured_port != UINT16_MAX || nb_rx_queue != 1) {
		fprintf(stderr, "hook-epoll: one port and one RX queue are supported\n");
		return -ENOTSUP;
	}

	memcpy(&conf, eth_conf, sizeof(conf));
	conf.intr_conf.rxq = 1;
	ret = real_fn(port_id, nb_rx_queue, nb_tx_queue, &conf);
	if (ret)
		return ret;
	configured_port = port_id;
	return 0;
}

int rte_eth_dev_start(uint16_t port_id)
{
	typedef int (*fn_t)(uint16_t);
	static fn_t real_fn;
	int ret;

	if (!real_fn)
		real_fn = (fn_t)resolve_next("rte_eth_dev_start");
	if (port_id != configured_port || hooked.rx_callback) {
		fprintf(stderr, "hook-epoll: unsupported port %u\n", port_id);
		return -ENOTSUP;
	}
	if ((ret = real_fn(port_id)))
		return ret;

	hooked.port_id = port_id;
	hooked.rx_callback = rte_eth_add_rx_callback(port_id, 0, cb_post_empty_poll, &hooked);
	if (!hooked.rx_callback) {
		ret = rte_errno ? -rte_errno : -EIO;
		goto fail;
	}
	ret = rte_eth_dev_rx_intr_ctl_q(port_id, 0, RTE_EPOLL_PER_THREAD, RTE_INTR_EVENT_ADD, NULL);
	if (!ret) {
		hooked.interrupt_registered = 1;
		return 0;
	}

fail:
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
	if (configured_port == port_id)
		configured_port = UINT16_MAX;
	return real_fn(port_id);
}
