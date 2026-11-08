#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <rte_common.h>
#include <rte_ethdev.h>
#include <rte_epoll.h>

#include "usched.h"

int mlx5_export_rxring(void *rx_queue, struct rxring *rxring);
int memif_export_rxring(void *rx_queue, struct rxring *rxring);
int ixgbevf_export_rxring(void *rx_queue, struct rxring *rxring);
int iavf_export_rxring(void *rx_queue, struct rxring *rxring);
int bnxt_export_rxring(void *rx_queue, struct rxring *rxring);

#define MAX_IDLE_POLLS 1

struct queue_state {
	uint16_t port_id;
	uint16_t queue_id;
	uint64_t empty_polls;
	const struct rte_eth_rxtx_callback *rx_callback;
	int interrupt_registered;
	int initialized;
	int registered;
};

static struct queue_state hooked = {
	.port_id = UINT16_MAX,
	.queue_id = UINT16_MAX,
};
static uint16_t configured_port = UINT16_MAX;
static uint16_t target_port = UINT16_MAX;
static int target_port_initialized;

static int driver_export_rxring(const char *driver, void *rx_queue, struct rxring *rxring)
{
	static const struct {
		const char *name;
		int (*get)(void *, struct rxring *);
	} drivers[] = {
		{ "net_ixgbe", ixgbevf_export_rxring },
		{ "net_ixgbe_vf", ixgbevf_export_rxring },
		{ "mlx5_eth", mlx5_export_rxring },
		{ "net_mlx5", mlx5_export_rxring },
		{ "mlx5_pci", mlx5_export_rxring },
		{ "net_memif", memif_export_rxring },
		{ "net_iavf", iavf_export_rxring },
		{ "net_bnxt", bnxt_export_rxring },
	};

	for (size_t i = 0; i < RTE_DIM(drivers); i++) {
		if (!strcmp(driver, drivers[i].name))
			return drivers[i].get(rx_queue, rxring);
	}
	return -ENOTSUP;
}

static int enable_interrupt(void *opaque)
{
	struct queue_state *queue = opaque;

	return rte_eth_dev_rx_intr_enable(queue->port_id, queue->queue_id);
}

static int disable_interrupt(void *opaque)
{
	struct queue_state *queue = opaque;

	return rte_eth_dev_rx_intr_disable(queue->port_id, queue->queue_id);
}

static int select_target_port(uint16_t port_id)
{
	const char *value;
	char *end;
	unsigned long parsed;

	if (target_port_initialized)
		return 0;
	target_port_initialized = 1;
	value = getenv("TARGET_PORT_ID");
	if (!value || !*value) {
		target_port = port_id;
		return 0;
	}
	errno = 0;
	parsed = strtoul(value, &end, 10);
	if (errno || *end || parsed > UINT16_MAX) {
		fprintf(stderr, "hook: invalid TARGET_PORT_ID: %s\n", value);
		return -EINVAL;
	}
	target_port = (uint16_t)parsed;
	return 0;
}

static void *resolve_next(const char *symbol)
{
	void *fn;

	dlerror();
	fn = dlsym(RTLD_NEXT, symbol);
	if (!fn)
		rte_panic("hook: resolve %s: %s\n", symbol, dlerror());
	return fn;
}

static int join_usched(struct queue_state *queue)
{
	struct rte_eth_dev_info info;
	struct rte_eth_link link;
	struct rxring rxring;
	void *rxq;
	int ret;

	if (queue->initialized)
		return 0;
	if (rte_eth_link_get_nowait(queue->port_id, &link) || !link.link_status)
		return -EAGAIN;
	if ((ret = rte_eth_dev_info_get(queue->port_id, &info)))
		return ret;
	rxq = rte_eth_fp_ops[queue->port_id].rxq.data[queue->queue_id];
	if (!rxq)
		return -EAGAIN;
	memset(&rxring, 0, sizeof(rxring));
	rxring.fd = -1;
	ret = driver_export_rxring(info.driver_name, rxq, &rxring);
	if (ret)
		return ret;
	ret = usched_init(
		&rxring,
		rte_lcore_id(),
		enable_interrupt,
		disable_interrupt,
		queue);
	if (ret)
		return ret;
	queue->initialized = 1;
	ret = rte_eth_dev_rx_intr_ctl_q(
		queue->port_id,
		queue->queue_id,
		RTE_EPOLL_PER_THREAD,
		RTE_INTR_EVENT_ADD,
		NULL);
	if (!ret)
		queue->interrupt_registered = 1;
	if (ret && ret != -ENOTSUP && ret != -EPERM)
		return ret;
	fprintf(stderr, "hook: initialized port %u queue %u with driver %s\n",
		queue->port_id, queue->queue_id, info.driver_name);
	return 0;
}

static uint16_t cb_post_empty_poll(
	uint16_t port_id,
	uint16_t queue_id,
	struct rte_mbuf **pkts __rte_unused,
	uint16_t nb_pkts,
	uint16_t max_pkts __rte_unused,
	void *opaque)
{
	struct queue_state *q = opaque;
	int ret;

	if (port_id != q->port_id || queue_id != q->queue_id)
		return nb_pkts;
	if (!q->initialized) {
		ret = join_usched(q);
		if (ret == -EAGAIN) {
			nanosleep(&(struct timespec) { .tv_nsec = 100000000 }, NULL);
			return nb_pkts;
		}
		if (ret)
			rte_panic("hook: initialization failed for port %u: %s\n",
				q->port_id, rte_strerror(-ret));
	}
	usched_update_rx_tail();
	if (nb_pkts) {
		q->empty_polls = 0;
		usched_cancel_idle();
		return nb_pkts;
	}
	if (++q->empty_polls > MAX_IDLE_POLLS) {
		usched_poll_and_reschedule();
		q->empty_polls = 0;
	}
	return nb_pkts;
}

int rte_eth_dev_configure(
	uint16_t port_id,
	uint16_t nb_rx_queue,
	uint16_t nb_tx_queue,
	const struct rte_eth_conf *eth_conf)
{
	typedef int (*fn_t)(uint16_t, uint16_t, uint16_t,
		const struct rte_eth_conf *);
	static fn_t real_fn;
	struct rte_eth_conf conf;

	if (!eth_conf)
		return -EINVAL;
	if (!real_fn)
		real_fn = (fn_t)resolve_next("rte_eth_dev_configure");
	{
		int ret = select_target_port(port_id);
		if (ret)
			return ret;
	}
	if (port_id != target_port)
		return real_fn(port_id, nb_rx_queue, nb_tx_queue, eth_conf);
	if (nb_rx_queue != 1 || configured_port != UINT16_MAX) {
		fprintf(stderr, "hook: exactly one target port and one RX queue are supported (port=%u rx_queues=%u)\n",
			port_id, nb_rx_queue);
		return -ENOTSUP;
	}
	memcpy(&conf, eth_conf, sizeof(conf));
	conf.intr_conf.rxq = 1;
	{
		int ret = real_fn(port_id, nb_rx_queue, nb_tx_queue, &conf);
		if (ret)
			return ret;
		configured_port = port_id;
	}
	return 0;
}

static void unregister_hook(void)
{
	if (!hooked.registered && !hooked.initialized && !hooked.interrupt_registered)
		return;
	if (hooked.rx_callback) {
		int ret = rte_eth_remove_rx_callback(
			hooked.port_id,
			hooked.queue_id,
			hooked.rx_callback);
		if (ret)
			fprintf(stderr, "hook: remove RX callback failed: %s\n", rte_strerror(-ret));
	}
	if (hooked.initialized)
		usched_deinit();

	if (hooked.interrupt_registered) {
		(void)rte_eth_dev_rx_intr_ctl_q(
			hooked.port_id,
			hooked.queue_id,
			RTE_EPOLL_PER_THREAD,
			RTE_INTR_EVENT_DEL,
			NULL);
	}
	memset(&hooked, 0, sizeof(hooked));
	hooked.port_id = hooked.queue_id = UINT16_MAX;
}

int rte_eth_dev_start(uint16_t port_id)
{
	typedef int (*fn_t)(uint16_t);
	static fn_t real_fn;
	int ret;

	if (!real_fn)
		real_fn = (fn_t)resolve_next("rte_eth_dev_start");
	if (port_id != target_port)
		return real_fn(port_id);
	if (port_id != configured_port || hooked.rx_callback) {
		fprintf(stderr, "hook: unsupported additional/unconfigured port %u\n",
			port_id);
		return -ENOTSUP;
	}

	hooked.port_id = port_id;
	hooked.queue_id = 0;

	if ((ret = real_fn(port_id)))
		return ret;

	hooked.rx_callback = rte_eth_add_rx_callback(port_id, 0, cb_post_empty_poll, &hooked);
	if (!hooked.rx_callback) {
		ret = rte_errno ? -rte_errno : -EIO;
		goto fail;
	}

	hooked.registered = 1;
	ret = join_usched(&hooked);
	if (ret == -EAGAIN)
		return 0;
	if (ret)
		goto fail;

	return 0;

fail:
	fprintf(stderr, "hook: initialization failed for port %u: %s\n",
		port_id, rte_strerror(-ret));
	unregister_hook();
	(void)rte_eth_dev_stop(port_id);
	return ret ? ret : -EIO;
}

int rte_eth_dev_stop(uint16_t port_id)
{
	typedef int (*fn_t)(uint16_t);
	static fn_t real_fn;

	if (!real_fn)
		real_fn = (fn_t)resolve_next("rte_eth_dev_stop");
	if ((hooked.registered || hooked.initialized) && hooked.port_id == port_id)
		unregister_hook();
	return real_fn(port_id);
}

int rte_eth_dev_close(uint16_t port_id)
{
	typedef int (*fn_t)(uint16_t);
	static fn_t real_fn;

	if (!real_fn)
		real_fn = (fn_t)resolve_next("rte_eth_dev_close");
	if ((hooked.registered || hooked.initialized) && hooked.port_id == port_id)
		unregister_hook();
	{
		int ret = real_fn(port_id);
		if (ret)
			return ret;
	}
	if (configured_port == port_id)
		configured_port = UINT16_MAX;
	return 0;
}
