#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/prctl.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <agamotto.h>

#include "fuzzer.h"

static int st_pipe[2], ctl_pipe[2];

int periscope_init_syz_fuzzer(char **qemu_argv, char *syz_fuzzer_path,
                              char *syz_fuzzer_argv, char *syz_fuzzer_executor,
                              char *syz_fuzzer_index, int *out_st_pipe,
                              int *out_ctl_pipe, int *out_shm_id,
                              char *fifo_out) {
    if (pipe(st_pipe) || pipe(ctl_pipe)) {
        return -1;
    }

    *out_st_pipe = st_pipe[1];
    *out_ctl_pipe = ctl_pipe[0];

#ifdef OPEN_SHM
#if 0
    *out_shm_id = open("/dev/shm/syzkaller", O_CREAT | O_RDWR | O_TRUNC, 0600);
#else
    char *shm_id_str = "/syzkaller";
    *out_shm_id = shm_open(shm_id_str, O_CREAT | O_RDWR | O_TRUNC, 0600);
#endif
    int kCoverSize = 256 << 10;
    ftruncate(*out_shm_id, (kCoverSize * sizeof(uintptr_t)) * 2);
#endif

    if (fork() == 0) {
        int pid = getpid();
        printf("periscope: child pid=%d syz-fuzzer=%s %s syz-executor=%s...\n",
               pid, syz_fuzzer_path, syz_fuzzer_argv, syz_fuzzer_executor);

        prctl(PR_SET_PDEATHSIG, SIGHUP);

#ifdef OPEN_IN_PIPE
        int in_pipe = open("/tmp/serial0.in", O_WRONLY | O_SYNC);
#endif
#ifdef OPEN_OUT_PIPE
        int out_pipe = open("/tmp/serial1.out", O_RDONLY);
#endif
        int err_pipe = 0;
        if (fifo_out) {
            err_pipe = open(fifo_out, O_RDONLY);
        }
#ifdef OPEN_IN_PIPE
        char in_pipe_str[10 + 1];
        sprintf(in_pipe_str, "%d", in_pipe);
#endif
#ifdef OPEN_OUT_PIPE
        char out_pipe_str[10 + 1];
        sprintf(out_pipe_str, "%d", out_pipe);
#endif
        char err_pipe_str[10 + 1];
        sprintf(err_pipe_str, "%d", err_pipe);

        char st_pipe_str[10 + 1];
        char ctl_pipe_str[10 + 1];
        sprintf(st_pipe_str, "%d", st_pipe[0]);
        sprintf(ctl_pipe_str, "%d", ctl_pipe[1]);

        if (execl(syz_fuzzer_path, "syz-fuzzer", "-executor",
                  syz_fuzzer_executor, "-args", syz_fuzzer_argv, "-st_pipe",
                  st_pipe_str, "-ctl_pipe", ctl_pipe_str,
#ifdef OPEN_IN_PIPE
                  "-in_pipe", in_pipe_str,
#endif
#ifdef OPEN_OUT_PIPE
                  "-out_pipe", out_pipe_str,
#endif
                  "-err_pipe", err_pipe_str,
#ifdef OPEN_SHM
                  "-shm_id", shm_id_str,
#endif
                  "-index", syz_fuzzer_index, NULL) == -1) {
            printf("periscope-child: execv errno=%d\n", errno);
            exit(1);
        }
    } else {
        // parent
    }

    return 0;
}

static void (*qemu_sigint_handler)(int);

static void vmfuzzer_sigint_handler(int signo) {
    printf("received SIGINT\n");
    qemu_sigint_handler(signo);
}

static void initialize_signal_handler(void) {
    struct sigaction sa;
    struct sigaction old;

    printf("periscope: initializing signal handler\n");

    sigaction(SIGINT, NULL, &old);
    if (old.sa_handler) {
        printf("No existing signal handler\n");
    }
    qemu_sigint_handler = old.sa_handler;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = vmfuzzer_sigint_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
}

void periscope_post_qemu_init(void) {
    printf("periscope: post init...\n");

    initialize_signal_handler();
}

int periscope_mmio_read(unsigned size, uint64_t *out) {
    //
    return 0;
}