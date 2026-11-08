#ifndef USCHED_FDPASS_H
#define USCHED_FDPASS_H

#include <errno.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

static inline int usched_dup_peer_fd(pid_t tid, int remote_fd)
{
	int pidfd;
	int local_fd;

	pidfd = syscall(SYS_pidfd_open, tid, 0);
	if (pidfd < 0)
		return -errno;

	local_fd = syscall(SYS_pidfd_getfd, pidfd, remote_fd, 0);
	close(pidfd);

	return local_fd < 0 ? -errno : local_fd;
}
#endif
