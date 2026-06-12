#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define SOCK_PATH "/tmp/m48-smoke.sock"
#define MEM_SIZE 4096
#define MAGIC 0x4d343846u

static void marker(const char *s) {
	write(1, s, strlen(s));
}

static void test_shared_fork_cow(void) {
	int memfd = memfd_create("m48-cow-test", MFD_CLOEXEC);
	if (memfd < 0 || ftruncate(memfd, MEM_SIZE) < 0) {
		marker("M48-FDPASS: fail memfd_create/ftruncate\n");
		_exit(10);
	}
	uint32_t *map = mmap(0, MEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
	if (map == MAP_FAILED) {
		marker("M48-FDPASS: fail mmap MAP_SHARED\n");
		_exit(11);
	}

	// 1. Write to the mapping BEFORE fork to populate the page table entry
	map[0] = 0x11111111u;

	int pid = fork();
	if (pid < 0) {
		marker("M48-FDPASS: fail fork\n");
		_exit(12);
	}

	if (pid == 0) {
		// Child: write new value
		map[0] = 0x22222222u;
		munmap(map, MEM_SIZE);
		close(memfd);
		_exit(0);
	} else {
		// Parent: wait for child
		int status = 0;
		waitpid(pid, &status, 0);
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			marker("M48-FDPASS: fail child exit status\n");
			_exit(13);
		}

		// Parent should see the child's write because it is MAP_SHARED
		if (map[0] != 0x22222222u) {
			marker("M48-FDPASS: fail CoW shared mapping de-shared!\n");
			_exit(14);
		}

		munmap(map, MEM_SIZE);
		close(memfd);
	}
	marker("M48-FDPASS: ok shared-fork-cow\n");
}

int main(void) {
	test_shared_fork_cow();
	unlink(SOCK_PATH);
	int listener = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, SOCK_PATH);
	if (listener < 0 ||
	    bind(listener, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    listen(listener, 1) < 0)
		return 1;

	int child = fork();
	if (child == 0) {
		int fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
			_exit(2);
		int memfd = memfd_create("m48-smoke", MFD_CLOEXEC);
		if (memfd < 0 || ftruncate(memfd, MEM_SIZE) < 0)
			_exit(3);
		uint32_t *map = mmap(0, MEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		                     memfd, 0);
		if (map == MAP_FAILED)
			_exit(4);
		map[0] = MAGIC;

		char byte = 'F';
		struct iovec iov = {&byte, 1};
		char control[CMSG_SPACE(sizeof(int)) + CMSG_SPACE(sizeof(struct ucred))];
		memset(control, 0, sizeof(control));
		struct msghdr msg;
		memset(&msg, 0, sizeof(msg));
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = control;
		msg.msg_controllen = sizeof(control);
		struct cmsghdr *rights = (struct cmsghdr *)control;
		rights->cmsg_level = SOL_SOCKET;
		rights->cmsg_type = SCM_RIGHTS;
		rights->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(rights), &memfd, sizeof(memfd));
		struct cmsghdr *credentials =
		    (struct cmsghdr *)(control + CMSG_SPACE(sizeof(int)));
		credentials->cmsg_level = SOL_SOCKET;
		credentials->cmsg_type = SCM_CREDENTIALS;
		credentials->cmsg_len = CMSG_LEN(sizeof(struct ucred));
		struct ucred supplied = {getpid(), 0, 0};
		memcpy(CMSG_DATA(credentials), &supplied, sizeof(supplied));
		if (sendmsg(fd, &msg, 0) != 1)
			_exit(5);

		munmap(map, MEM_SIZE);
		close(memfd);
		close(fd);
		_exit(0);
	}

	int peer = accept(listener, 0, 0);
	if (peer < 0)
		return 1;
	char byte = 0;
	struct iovec iov = {&byte, 1};
	char control[CMSG_SPACE(sizeof(int)) + CMSG_SPACE(sizeof(struct ucred))];
	memset(control, 0, sizeof(control));
	struct msghdr msg;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control;
	msg.msg_controllen = sizeof(control);
	if (recvmsg(peer, &msg, 0) != 1 || byte != 'F')
		return 1;

	int received_fd = -1;
	struct ucred received_cred = {0, 0, 0};
	for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c;
	     c = CMSG_NXTHDR(&msg, c)) {
		if (c->cmsg_level != SOL_SOCKET)
			continue;
		if (c->cmsg_type == SCM_RIGHTS)
			memcpy(&received_fd, CMSG_DATA(c), sizeof(received_fd));
		if (c->cmsg_type == SCM_CREDENTIALS)
			memcpy(&received_cred, CMSG_DATA(c), sizeof(received_cred));
	}
	if (received_fd < 0)
		return 1;
	marker("M48-FDPASS: ok scm-rights\n");

	uint32_t *map = mmap(0, MEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
	                     received_fd, 0);
	if (map == MAP_FAILED || map[0] != MAGIC)
		return 1;
	marker("M48-FDPASS: ok scm-refcount-close\n");
	marker("M48-FDPASS: ok memfd\n");
	if (received_cred.pid != child)
		return 1;

	munmap(map, MEM_SIZE);
	close(received_fd);
	close(peer);
	close(listener);
	unlink(SOCK_PATH);
	int status = 0;
	waitpid(child, &status, 0);
	return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
}
