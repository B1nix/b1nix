#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include "wayland-os.h"

FILE *b1nix_wayland_open_memstream(char **buffer, size_t *size)
{
	(void)buffer;
	(void)size;
	errno = ENOSYS;
	return NULL;
}

static int cloexec(int fd)
{
	long flags;

	if (fd < 0)
		return -1;
	flags = fcntl(fd, F_GETFD);
	if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

int wl_os_socket_cloexec(int domain, int type, int protocol)
{
	return cloexec(socket(domain, type, protocol));
}

int wl_os_dupfd_cloexec(int fd, int minfd)
{
	return cloexec(fcntl(fd, F_DUPFD, minfd));
}

ssize_t wl_os_recvmsg_cloexec(int fd, struct msghdr *msg, int flags)
{
	ssize_t len = recvmsg(fd, msg, flags);
	struct cmsghdr *cmsg;

	if (len < 0)
		return -1;
	for (cmsg = CMSG_FIRSTHDR(msg); cmsg; cmsg = CMSG_NXTHDR(msg, cmsg)) {
		int *received;
		int *end;

		if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
			continue;
		received = (int *)CMSG_DATA(cmsg);
		end = (int *)((char *)cmsg + cmsg->cmsg_len);
		while (received < end) {
			*received = cloexec(*received);
			received++;
		}
	}
	return len;
}

int wl_os_socket_peercred(int fd, uid_t *uid, gid_t *gid, pid_t *pid)
{
	(void)fd;
	*uid = getuid();
	*gid = getgid();
	*pid = getpid();
	return 0;
}

int wl_os_epoll_create_cloexec(void)
{
	return epoll_create1(EPOLL_CLOEXEC);
}

int wl_os_accept_cloexec(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
	return cloexec(accept(fd, addr, addrlen));
}

void *wl_os_mremap_maymove(int fd, void *old_data, ssize_t *old_size,
			   ssize_t new_size, int prot, int flags)
{
	void *data = mmap(NULL, new_size, prot, flags, fd, 0);
	if (data == MAP_FAILED)
		return MAP_FAILED;
	if (munmap(old_data, *old_size) == 0)
		*old_size = 0;
	return data;
}
