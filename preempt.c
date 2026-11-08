#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "usched.h"

int main(int argc, char **argv)
{
	char *end;
	uint64_t quantum_ns;
	int ret;

	if (argc != 3 || strcmp(argv[1], "-n")) {
		fprintf(stderr, "usage: %s -n QUANTUM_NS\n", argv[0]);
		return EXIT_FAILURE;
	}

	errno = 0;
	quantum_ns = strtoull(argv[2], &end, 10);
	if (errno || *end || !quantum_ns) {
		fprintf(stderr, "invalid quantum: %s\n", argv[2]);
		return EXIT_FAILURE;
	}

	ret = usched_preemption_loop(sched_getcpu(), quantum_ns);
	if (ret) {
		fprintf(stderr, "preempt: %s\n", strerror(-ret));
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
