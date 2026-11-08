#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <inttypes.h>

void *attach_or_create_shmem(const char *name, size_t size, bool *created)
{
	int fd;
	void *addr;

	*created = false;

	if ((fd = shm_open(name, O_RDWR, 0)) < 0) {
		fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0660);
		if (fd >= 0)
			*created = true;
		else if (errno == EEXIST)
			fd = shm_open(name, O_RDWR, 0);
		if (fd < 0) {
			perror("shm_open create");
			exit(errno);
		}
	}

	if (*created && ftruncate(fd, size)) {
		perror("ftruncate shared memory");
		close(fd);
		exit(errno);
	}
	
	{
		struct stat st;
		int ret = fstat(fd, &st);
		if (ret || st.st_size != (off_t)size) {
			fprintf(stderr, "shared memory %s has size %jd, expected %zu\n",
				name, (intmax_t)(ret ? -1 : st.st_size), size);
			close(fd);
			exit(EPROTO);
		}
	}

	addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (addr == MAP_FAILED) {
		perror("mmap shared memory");
		close(fd);
		exit(errno);
	}

	if (*created)
		memset(addr, 0, size);

	close(fd);

	return addr;
}
