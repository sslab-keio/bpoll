#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_cycles.h>

struct control {
	_Atomic uint64_t start_ns;
};

struct options {
	const char *role;
	const char *control_name;
	uint32_t index;
	uint32_t active;
	uint32_t on_ms;
	uint32_t off_ms;
	uint32_t duration_s;
	uint32_t spin_us;
};

static uint64_t monotonic_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts))
		rte_exit(EXIT_FAILURE, "clock_gettime: %s\n", strerror(errno));
	return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
}

static struct control *open_control(const char *name, int create)
{
	int fd;
	struct control *control;

	fd = shm_open(name, O_RDWR | (create ? O_CREAT : 0), 0600);
	if (fd < 0)
		rte_exit(EXIT_FAILURE, "shm_open %s: %s\n", name, strerror(errno));
	if (create && ftruncate(fd, sizeof(*control)))
		rte_exit(EXIT_FAILURE, "ftruncate %s: %s\n", name, strerror(errno));
	control = mmap(NULL, sizeof(*control), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (control == MAP_FAILED)
		rte_exit(EXIT_FAILURE, "mmap %s: %s\n", name, strerror(errno));
	close(fd);
	return control;
}

static void control_command(int argc, char **argv)
{
	struct control *control;

	if (argc == 3 && !strcmp(argv[1], "--control-create")) {
		shm_unlink(argv[2]);
		control = open_control(argv[2], 1);
		atomic_store(&control->start_ns, 0);
		munmap(control, sizeof(*control));
		return;
	}
	if (argc == 4 && !strcmp(argv[1], "--control-arm")) {
		control = open_control(argv[2], 0);
		atomic_store(&control->start_ns,
			monotonic_ns() + strtoull(argv[3], NULL, 10) * UINT64_C(1000000));
		printf("%" PRIu64 "\n", atomic_load(&control->start_ns));
		munmap(control, sizeof(*control));
		return;
	}
	fprintf(stderr, "usage: %s --control-create NAME | --control-arm NAME DELAY_MS\n", argv[0]);
	exit(EXIT_FAILURE);
}

static void parse_options(int argc, char **argv, struct options *options)
{
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--role") && i + 1 < argc)
			options->role = argv[++i];
		else if (!strcmp(argv[i], "--control") && i + 1 < argc)
			options->control_name = argv[++i];
		else if (!strcmp(argv[i], "--index") && i + 1 < argc)
			options->index = strtoul(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "--active") && i + 1 < argc)
			options->active = strtoul(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "--on-ms") && i + 1 < argc)
			options->on_ms = strtoul(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "--off-ms") && i + 1 < argc)
			options->off_ms = strtoul(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "--duration") && i + 1 < argc)
			options->duration_s = strtoul(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "--spin-us") && i + 1 < argc)
			options->spin_us = strtoul(argv[++i], NULL, 10);
		else
			rte_exit(EXIT_FAILURE, "invalid option: %s\n", argv[i]);
	}
	if (!options->role || !options->control_name || options->index >= 12 ||
		(strcmp(options->role, "server") && strcmp(options->role, "client")))
		rte_exit(EXIT_FAILURE, "missing or invalid application options\n");
}

static struct rte_mempool *setup_port(void)
{
	struct rte_mempool *pool;
	struct rte_eth_conf conf = { 0 };

	pool = rte_pktmbuf_pool_create("m", 4096, 256, 0,
		RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
	if (!pool)
		rte_exit(EXIT_FAILURE, "rte_pktmbuf_pool_create: %s\n", rte_strerror(rte_errno));
	if (rte_eth_dev_count_avail() != 1)
		rte_exit(EXIT_FAILURE, "expected one memif port\n");
	if (rte_eth_dev_configure(0, 1, 1, &conf) ||
		rte_eth_rx_queue_setup(0, 0, 256, rte_socket_id(), NULL, pool) ||
		rte_eth_tx_queue_setup(0, 0, 256, rte_socket_id(), NULL) ||
		rte_eth_dev_start(0))
		rte_exit(EXIT_FAILURE, "memif port setup failed: %s\n", rte_strerror(rte_errno));
	return pool;
}

static int on(const struct options *options, uint64_t now, uint64_t start)
{
	uint64_t elapsed_ms;

	if (options->index >= options->active)
		return 0;
	if (!options->off_ms)
		return 1;
	elapsed_ms = (now - start) / UINT64_C(1000000);
	return elapsed_ms % (options->on_ms + options->off_ms) < options->on_ms;
}

static void server_loop(const struct options *options)
{
	struct control *control = open_control(options->control_name, 0);
	struct rte_mbuf *packets[32];
	struct timespec sleep = { .tv_nsec = 1000000 };
	uint64_t start;

	while (!(start = atomic_load(&control->start_ns))) {
		nanosleep(&sleep, NULL);
	}
	while (monotonic_ns() < start) {
		nanosleep(&sleep, NULL);
	}

	while (1) {
		uint16_t received = rte_eth_rx_burst(0, 0, packets, RTE_DIM(packets));
		uint16_t sent = 0;

		if (received && options->spin_us)
			rte_delay_us_block(options->spin_us);
		while (sent < received)
			sent += rte_eth_tx_burst(0, 0, &packets[sent], received - sent);
	}
}

static void client_loop(const struct options *options, struct rte_mempool *pool)
{
	struct control *control = open_control(options->control_name, 0);
	struct rte_mbuf *packets[1];
	uint64_t start;
	int inflight = 0;

	printf("ready %u\n", options->index);
	fflush(stdout);
	while (!(start = atomic_load(&control->start_ns)))
		rte_pause();
	while (monotonic_ns() < start)
		rte_pause();
	while (monotonic_ns() < start + (uint64_t)options->duration_s * UINT64_C(1000000000)) {
		if (rte_eth_rx_burst(0, 0, packets, 1)) {
			rte_pktmbuf_free(packets[0]);
			inflight = 0;
		}
		if (!inflight && on(options, monotonic_ns(), start)) {
			packets[0] = rte_pktmbuf_alloc(pool);
			if (!packets[0])
				continue;
			rte_pktmbuf_append(packets[0], 64);
			if (rte_eth_tx_burst(0, 0, packets, 1))
				inflight = 1;
			else
				rte_pktmbuf_free(packets[0]);
		}
	}
	munmap(control, sizeof(*control));
}

int main(int argc, char **argv)
{
	struct options options = { 0 };
	struct rte_mempool *pool;
	int eal_args;

	if (argc > 1 && (!strcmp(argv[1], "--control-create") ||
			 !strcmp(argv[1], "--control-arm")))
	{
		control_command(argc, argv);
		return EXIT_SUCCESS;
	}
	eal_args = rte_eal_init(argc, argv);
	if (eal_args < 0)
		rte_exit(EXIT_FAILURE, "rte_eal_init failed\n");
	parse_options(argc - eal_args, argv + eal_args, &options);
	pool = setup_port();
	printf("port ready %s %u pid %d\n", options.role, options.index,
		getpid());
	fflush(stdout);
	if (!strcmp(options.role, "server"))
		server_loop(&options);
	else
		client_loop(&options, pool);
	return 0;
}
