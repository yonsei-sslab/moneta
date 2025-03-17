#ifdef __aarch64__

#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <stdio.h>

#define DEVICE_PATH "/dev/monetahvc"
#define AGAMOTTO_IOC_BASE 0x33
#define AGAMOTTO_IOC_HVC _IOW(AGAMOTTO_IOC_BASE, 0, struct agamotto_ioctl)

typedef unsigned long long uint64;

struct agamotto_ioctl {
	unsigned long c0;
	unsigned long a0;
	unsigned long a1;
	unsigned long a2;
};

uint64
moneta_arm_hypercall(uint64 c0, uint64 a0, uint64 a1, uint64 a2)
{
	uint64 ret;
	struct agamotto_ioctl arg_struct;
	int fd;

	// Open the device file
	fd = open(DEVICE_PATH, O_RDWR);
	if (fd < 0) {
		perror("Failed to open the moneta device file");
		return EXIT_FAILURE;
	}

	arg_struct.c0 = c0;
	arg_struct.a0 = a0;
	arg_struct.a1 = a1;
	arg_struct.a2 = a2;

	ret = ioctl(fd, AGAMOTTO_IOC_HVC, &arg_struct);

	close(fd);

	return ret;
}

#endif // __aarch64__