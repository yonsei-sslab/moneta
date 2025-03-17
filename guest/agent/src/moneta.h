#ifndef MONETA_H
#define MONETA_H

#define VECTORADD_PATH "/usr/local/cuda-12.1/extras/demo_suite/vectorAdd"
#define EXECUTOR_PATH "/" // make sure this ends with a slash
#define EXECUTOR_BASENAME "syz-executor.debug"
#define CHROME_PATH "/usr/bin/chromium"

typedef unsigned long long uint64;

extern int moneta_gpu_pid;
extern int moneta_snapshot_ready;
extern int moneta_syscall_count;
extern int moneta_syscall_threshold;
extern int moneta_timeout;
extern int moneta_snapnum;
extern int moneta_snapnum_count;
extern int moneta_one_log;
extern int moneta_bug_id;
extern int moneta_target_driver;
extern int moneta_exec_tracee;
extern int moneta_reopen_stdout;

#define MONETA_SHOULD_SNAPSHOT tcp->pid == moneta_gpu_pid && (moneta_snapshot_ready && (moneta_syscall_count >= moneta_syscall_threshold)) && (moneta_snapnum > moneta_snapnum_count)


#ifndef __NR_pidfd_getfd
#define __NR_pidfd_getfd 438
#endif


ssize_t read_tracee_memory(pid_t source_pid, void* source_addr, long size, void* destination_addr);
ssize_t write_tracee_memory(pid_t source_pid, void* source_addr, long size, void* destination_addr);

int nvidia_ioctl(struct tcb *tcp, unsigned int cmd, unsigned long arg);
int mali_ioctl(struct tcb *tcp, unsigned int cmd, unsigned long arg);
int amdgpu_ioctl(struct tcb *tcp, unsigned int cmd, unsigned long arg);

#if 0
int handle_moneta_open(struct tcb *const tcp, int offset);
#else
int handle_moneta_open(struct tcb *const tcp);
#endif
int moneta_arm_hypercall(uint64_t c0, uint64_t a0, uint64_t a1, uint64_t a2);
void moneta_snapshot(void);
int kill_all_tracees(bool but_one);
void open_one_log(struct tcb *const);

void moneta_exec(void);
#endif
