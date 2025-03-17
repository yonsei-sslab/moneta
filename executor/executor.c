#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>


#include "agamotto.h"
#ifdef __aarch64__
#include "moneta.h"
#endif // !__aarch64__

#define EXEC_BUFFER_SIZE (2 << 20)
#define OUTPUT_SIZE (16 << 20)

const int kInFd = 3;
const int kOutFd = 4;

static void printk(const char* fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	//char msg[1024];
	//snprintf(msg, sizeof(msg), fmt, args);

	int kmsg_fd = open("/dev/kmsg", O_WRONLY);
	if (kmsg_fd > 0) {
		//printf("printk fd=%d msg=%s", kmsg_fd, msg);
		//write(kmsg_fd, msg, strlen(msg) + 1);
		dprintf(kmsg_fd, fmt, args);
		close(kmsg_fd);
	} else {
		printf("open /dev/kmsg failed with errno=%d\n", errno);
	}

	va_end(args);
}

#define OPEN_IN_PIPE
#undef OPEN_IN_PIPE
#define OPEN_OUT_PIPE
#undef OPEN_OUT_PIPE

static int create_virtio_pipes(int* in_fd, int* out_fd, int* err_fd)
{
#ifdef OPEN_IN_PIPE
	char* in_file = "/dev/virtio-ports/serial0";
#endif
#ifdef OPEN_OUT_PIPE
	char* out_file = "/dev/virtio-ports/serial1";
#endif
	char* err_file = "/dev/vport0p1";
	char* err2_file = "/dev/vport1p1";
	int fd;

#ifdef OPEN_IN_PIPE
	fd = open(in_file, O_RDONLY);
	if (fd <= 0) {
		printf("opening stdin failed\n");
		return -1;
	}

	if (dup2(fd, 0) < 0) {
		printf("dup to stdin failed\n");
		return -1;
	}

	close(fd);
#endif

	*in_fd = 0;

#ifdef OPEN_OUT_PIPE
	fd = open(out_file, O_WRONLY | O_SYNC);
	if (fd <= 0) {
		printf("opening stdout failed\n");
		return -1;
	}

	if (dup2(fd, 1) < 0) {
		printf("dup to stdout failed\n");
		return -1;
	}

	close(fd);
#endif

	*out_fd = 1;

	fd = open(err_file, O_WRONLY | O_SYNC);
	if (fd <= 0) {
		fd = open(err2_file, O_WRONLY | O_SYNC);
		if (fd <= 0) {
			printf("opening stderr failed\n");
			return -1;
		}
	}

	// make stderr go to err_file
	if (dup2(fd, 2) < 0) {
		printf("dup to stderr failed\n");
		return -1;
	}

	close(fd);

	*err_fd = 2;

	return 0;
}

#define NOSHMEM 0

static int make_env()
{
	int in_fd, out_fd, err_fd;

#if !NOSHMEM
	int in_shm_fd = open("/dev/uio0", O_RDWR | O_SYNC); // in
	int out_shm_fd = open("/dev/uio1", O_RDWR | O_SYNC); // out

	if (dup2(in_shm_fd, 230) < 0) {
		printf("dup to in_shm_fd failed\n");
		return -1;
	}

	if (dup2(out_shm_fd, 231) < 0) {
		printf("dup to out_shm_fd failed\n");
		return -1;
	}

	close(in_shm_fd);
	close(out_shm_fd);

#endif

	if (create_virtio_pipes(&in_fd, &out_fd, &err_fd) != 0) {
		printf("create_virtio_pipes failed\n");
		return -1;
	}

	if (in_fd != 0 || out_fd != 1 || err_fd != 2) {
		perror("unexpected file descriptors 2\n");
		return -1;
	}

	return 0;
}

#include <dirent.h>
static void 
moneta_print_ofd(pid_t pid)
{
    char procPath[256];
    snprintf(procPath, sizeof(procPath), "/proc/%d/fd", pid);

    DIR* dir = opendir(procPath);
    if (dir == NULL) {
        printf("opendir error");
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            char fdPath[512];
            snprintf(fdPath, sizeof(fdPath), "%s/%s", procPath, entry->d_name);

            char linkTarget[256];
            ssize_t bytesRead = readlink(fdPath, linkTarget, sizeof(linkTarget) - 1);
            if (bytesRead != -1) {
                linkTarget[bytesRead] = '\0';
                printf("For PID %d, File descriptor %s -> %s\n", pid, entry->d_name, linkTarget);
            }
        }
    }
    printf("\n");

    closedir(dir);
}

int main(int argc, char** argv)
{
	// TODO: support simple executor command, e.g., revision

	printf("syz-executor.debug boot\n");

	// syzkaller notify boot
#ifdef __x86_64__
	agamotto_kvm_hypercall(HC_AGAMOTTO_GET_PROG);
#elif defined(__aarch64__)
	moneta_arm_hypercall(HC_AGAMOTTO_GET_PROG, 0, 0, 0);
#endif

	if (make_env() == 0) {
		moneta_print_ofd(getpid());
		printf("executing syz-executor...\n");
		printk("### executing syz-executor...\n");

		argv[0] = "syz-executor";
		execv("/syz-executor", argv);

		printf("execv exited with error %d\n", errno);
	} else {
		printf("syz-executor env setup failed.\n");
		printk("### syz-executor env setup failed.\n");
	}

	return 0;
}
