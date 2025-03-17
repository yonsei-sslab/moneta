#include <limits.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <signal.h>
#include <unistd.h>
#include <linux/ioctl.h>
#include <sys/ioctl.h>
#include <sys/ptrace.h>
#include <elf.h>
#include <sys/user.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define _GNU_SOURCE
#include <sys/uio.h>

#include "defs.h"
#include "moneta.h"
#include "agamotto.h"
#include "print_utils.h"

#define ROUND_UP(x, multiple)   ( (((long)(x)) + multiple-1)  & (~(multiple-1)) )

int moneta_gpu_pid = 0;
int moneta_snapshot_ready = 0;
int moneta_syscall_count = -2;
int moneta_syscall_threshold = -1;
int moneta_timeout = -1;
int moneta_snapnum = -1;
int moneta_snapnum_count = 0;
int moneta_one_log = 0;
int moneta_lastdupfd = 0;
int moneta_bug_id = 0;
int moneta_target_driver = 0; // nvidia = 1, mali = 2, amdgpu = 3
int moneta_exec_tracee = 0;
int moneta_reopen_stdout = 0;

FILE* fp;
char sfd_tfd[50];


ssize_t
read_tracee_memory(pid_t source_pid, void* source_addr, long size, void* destination_addr)
{
    struct iovec tracer[1];
    struct iovec tracee[1];

    tracer[0].iov_base  = (void*)destination_addr;
    tracer[0].iov_len   = size;
    tracee[0].iov_base = (void*)source_addr;
    tracee[0].iov_len  = size;

    ssize_t nread = syscall(__NR_process_vm_readv, source_pid, tracer, 1, tracee, 1, 0);
    if (nread != size)
        error_func_msg("Reading memory failed. Tried to read %ld bytes - actually read %zd bytes - errno: %d (%s)\n",
                size, nread, errno, strerror(errno));

    return nread;
}


ssize_t
write_tracee_memory(pid_t target_pid, void* target_addr, long size, void* source_addr)
{
    struct iovec tracer[1];
    struct iovec tracee[1];

    tracer[0].iov_base  = (void*)source_addr;
    tracer[0].iov_len   = size;
    tracee[0].iov_base = (void*)target_addr;
    tracee[0].iov_len  = size;

    ssize_t nread = syscall(__NR_process_vm_writev, target_pid, tracer, 1, tracee, 1, 0);
    if (nread != size)
        error_func_msg("Writing memory failed (%p). Tried to write %ld bytes - actually wrote %zd bytes - errno: %d (%s)\n",
                target_addr, size, nread, errno, strerror(errno));

    return nread;
}


// TODO: there should be a better implementation here somewhere, but I cannot find it.
static void
print_buffer_hex(char* buffer, size_t buffer_size)
{
    char* buffer_out = (char*) malloc((buffer_size * 4) + 1);
    char* buffer_out_i = buffer_out;

    tprints("\"");
	for (unsigned int i = 0; i < buffer_size; i++){
        *buffer_out_i++ = '\\';
        *buffer_out_i++ = 'x';
        buffer_out_i = sprint_byte_hex(buffer_out_i, buffer[i]);
    }
    *buffer_out_i = 0;
		// tprintf("-%s%lx", buffer[i] > 0x0f ? "" : "0", ((unsigned long) buffer[i]) & 0xff);


    tprints(buffer_out);
    tprints("\"");
    free(buffer_out);
}


// based on https://github.com/NVIDIA/open-gpu-kernel-modules/blob/ebcc6656ff5535308bbb450487a9cbb89f7ddc7c/kernel-open/nvidia/nv.c#L2100
int
nvidia_ioctl(struct tcb *tcp, unsigned int cmd, unsigned long arg)
{
    size_t arg_size = _IOC_SIZE(cmd);
	void* arg_data  = malloc(arg_size);
	if (!arg_data)
		error_msg_and_die("Could not allocate data for Nvidia ioctl argument\n");
	read_tracee_memory(tcp->pid, (void*)arg, arg_size, arg_data);
    tprint_arg_next();
	print_buffer_hex(arg_data, arg_size);
	free(arg_data);
	return RVAL_IOCTL_DECODED;
}

#include "mali_ioctl.h"

int
mali_ioctl(struct tcb *tcp, unsigned int code, unsigned long arg)
{

    size_t arg_size = _IOC_SIZE(code);
	void* arg_data  = malloc(arg_size);
	if (!arg_data)
		error_msg_and_die("Could not allocate data for Nvidia ioctl argument\n");
	read_tracee_memory(tcp->pid, (void*)arg, arg_size, arg_data);
    tprint_arg_next();
	print_buffer_hex(arg_data, arg_size);
    
	free(arg_data);
	return RVAL_IOCTL_DECODED;
}

#include "amdgpu_drm.h"

int
amdgpu_ioctl(struct tcb *tcp, unsigned int cmd, unsigned long arg)
{
    size_t arg_size = _IOC_SIZE(cmd);
	void* arg_data  = malloc(arg_size);
	if (!arg_data)
		error_msg_and_die("Could not allocate data for amdgpu ioctl argument\n");
	read_tracee_memory(tcp->pid, (void*)arg, arg_size, arg_data);
    tprint_arg_next();
	print_buffer_hex(arg_data, arg_size);
	free(arg_data);
	return RVAL_IOCTL_DECODED;
}

int
handle_moneta_open(struct tcb *const tcp)
{
    if (moneta_syscall_count >= 0) {
        if (tcp->pid == moneta_gpu_pid)
            moneta_syscall_count++;
        return 0;
    }

    moneta_gpu_pid = tcp->pid;
    moneta_snapshot_ready = 1;
    if (moneta_syscall_count < 0 && moneta_syscall_threshold > 0){
        error_msg("> Found gpu process <%d>", tcp->pid);
        moneta_syscall_count = 0;
    }
    return 0;
}

// By default, fd returned from pidfd_getfd is set FD_CLOEXEC,
// Making the dupfd close on exec.
static void
moneta_revert_fdflags(int fd) 
{
    int flags = fcntl(fd, F_GETFD);
    flags &= ~FD_CLOEXEC;
    fcntl(fd, F_SETFD, flags);
}

static int dummyfd = 2;

static int
moneta_dupfd(pid_t pid, int tfd, char* pname)
{
    int pidfd;
    int fd;
    char opt;

    // TODO this kill seems dangerous, might be better to ensure this function is only run at syscall entrance or exit
    if (kill(pid, 0) == 0) {
        pidfd = syscall(__NR_pidfd_open, pid, 0);
        if (pidfd == -1) {
            error_msg("pidfd_open for %d error: %s, pidfd is %d\n", pid, strerror(errno), pidfd);
            return -1;
        }
        else { 
            fd = syscall(__NR_pidfd_getfd, pidfd, tfd, 0);
            if (fd == -1) {
                error_msg("pidfd_getfd for PID %d, FD %d, error: %s, fd is %d\n", pid, tfd, strerror(errno), fd);
                return -1;
            }
            moneta_lastdupfd = fd;
            close(pidfd);
            moneta_revert_fdflags(fd);

            fp = fopen(sfd_tfd, "a");
            if (fp == NULL) {
                error_msg_and_die("Error opening sfd_tfd %s", sfd_tfd);
            }

            if (!strncmp(pname, "/dev/nvidia", 11)) {
                opt = 'n'; // generate syz_get_snapfd$nvidia
            } else if (!strncmp(pname, "/dev/mali", 9)) {
                opt = 'm'; // generate syz_get_snapfd$mali
            } else if (!strncmp(pname, "/dev/dri/renderD128", strlen("/dev/dri/renderD128"))) { // amdgpu
                opt = 'a'; // generate syz_get_snapfd$mali
            } else {
                opt = 'd'; // generate syz_get_snapfd
            }

            if (moneta_exec_tracee){
                while (tfd > dummyfd+1) {
                    dummyfd++;
                    fprintf(fp, "%d %d %s\n", dummyfd, dummyfd, "d");
                }
                fprintf(fp, "%d %d %c\n", tfd, tfd, opt);
                dummyfd = tfd;
            }
            else
                fprintf(fp, "%d %d %c\n", fd, tfd, opt);
            fclose(fp);

            return fd;
        }
    }
    else {
        error_msg("PID %d is invalid!\n", pid);
        return -1;
    }
}

#if 1
static void 
moneta_print_ofd(pid_t pid)
{
    char procPath[256];
    snprintf(procPath, sizeof(procPath), "/proc/%d/fd", pid);

    DIR* dir = opendir(procPath);
    if (dir == NULL) {
        error_msg("opendir error");
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
                error_msg("For PID %d, File descriptor %s -> %s", pid, entry->d_name, linkTarget);
            }
        }
    }
    error_msg("\n");

    closedir(dir);
}
#endif

static int
moneta_find_fd(pid_t pid)
{
    char procPath[256];
    snprintf(procPath, sizeof(procPath), "/proc/%d/fd", pid);

    DIR* dir = opendir(procPath);
    if (dir == NULL) {
        error_msg("opendir error");
        return -1;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            char fdPath[512];
            snprintf(fdPath, sizeof(fdPath), "%s/%s", procPath, entry->d_name);

            char linkTarget[256];
            ssize_t bytesRead = readlink(fdPath, linkTarget, sizeof(linkTarget) - 1);
            if (bytesRead != -1 && moneta_target_driver == 1 && !strncmp(linkTarget, "/dev/nvidiactl", strlen("/dev/nvidiactl")))
                return atoi(entry->d_name);
            else if (bytesRead != -1 && moneta_target_driver == 2 && !strncmp(linkTarget, "/dev/mali", strlen("/dev/mali")))
                return atoi(entry->d_name);
            else if (bytesRead != -1 && moneta_target_driver == 3 && !strncmp(linkTarget, "/dev/dri/renderD128", strlen("/dev/dri/renderD128")))
                return atoi(entry->d_name);
                // error_msg("For PID %d, File descriptor %s -> %s", pid, entry->d_name, linkTarget);
            
        }
    }
    error_msg("\n");

    closedir(dir);
    return 0;
}

static void
moneta_duplicate_tfd(pid_t pid) 
{
    char procPath[20];
    snprintf(procPath, sizeof(procPath), "/proc/%d/fd", pid);

    DIR *dir = opendir(procPath);
    if (dir == NULL) {
        error_msg("opendir");
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0 &&
        strcmp(entry->d_name, "0") != 0 &&
        strcmp(entry->d_name, "1") != 0 &&
        strcmp(entry->d_name, "2") != 0) {
            char fdPath[276];
            snprintf(fdPath, sizeof(fdPath), "%s/%s", procPath, entry->d_name);

            char linkTarget[256];
            ssize_t bytesRead = readlink(fdPath, linkTarget, sizeof(linkTarget) - 1);
            if (bytesRead != -1) {
                linkTarget[bytesRead] = '\0';
            }

            if(strcmp(linkTarget, "/dev/ttyS0") != 0 && strcmp(linkTarget, "/root/tmp_kInFd") != 0 && strcmp(linkTarget, "/root/tmp_kOutFd") != 0 && strcmp(linkTarget, "/dev/null") != 0)
                moneta_dupfd(pid, atoi(entry->d_name), linkTarget);
        }
    }
    closedir(dir);
}

static void
moneta_close_dfd(pid_t pid, int fsnapPoint) 
{
    if (moneta_lastdupfd) {
        int i = moneta_lastdupfd;
        while (i >= fsnapPoint) {
            error_msg("Closing FD %d, snappoint %d", i, fsnapPoint);
            close(i);
            i--;
        }
        moneta_lastdupfd = -1;
    }
}

static void
moneta_reproduce_bug(void) {
    int bug_fd;

    bug_fd = moneta_find_fd(getpid());

    error_msg("Trying to reproduce bug %d, nvidiactl fd: %d\n", moneta_bug_id, bug_fd);
    switch (moneta_bug_id) {
        default:
            error_msg("No cases were given! given %d, nvidiactl fd: %d\n", moneta_bug_id, bug_fd);
            break;
    }
    return;
}

int
moneta_arm_hypercall(uint64_t c0, uint64_t a0, uint64_t a1, uint64_t a2)
{
    int fd;
    unsigned long ret;
    struct agamotto_ioctl arg_struct;

    // Open the device file
    fd = open(DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        perror("Failed to open the device file");
        return EXIT_FAILURE;
    }

	arg_struct.c0 = c0;
	arg_struct.a0 = a0;
	arg_struct.a1 = a1;
	arg_struct.a2 = a2;

    ret = ioctl(fd, AGAMOTTO_IOC_HVC, &arg_struct);
    if (ret == (unsigned long)-1) {
        perror("ioctl");
        return EXIT_FAILURE;
    }
    close(fd);
    return ret;
}

void
moneta_snapshot(void)
{
    //moneta_exec();moneta_gpu_pid = 0; return;
    int ret = -1;
    int fsnapPoint = openat(AT_FDCWD, "snapshot_point", O_RDONLY|O_CREAT, S_IRWXU | S_IRWXG | S_IRWXO);

    moneta_snapnum_count++;
    sprintf(sfd_tfd, "sfd_tfd_%d", moneta_snapnum_count);

    moneta_print_ofd(moneta_gpu_pid);
    moneta_duplicate_tfd(moneta_gpu_pid);
    moneta_print_ofd(getpid());

    error_msg("Entered moneta_snapshot @ %d syscalls", moneta_syscall_count);

    /* 
        snapshot point
    */

    error_msg("Taking a snapshot %d/%d now", moneta_snapnum_count, moneta_snapnum);
    if (moneta_bug_id)
        moneta_reproduce_bug();
    else {
        sleep(5);
#ifdef X86_64
        ret = agamotto_kvm_hypercall2(HC_AGAMOTTO_DEBUG, MONETA_DEBUG_HC_SAVEVM);
        if (ret) {
                error_msg("ret: %d for SAVEVM - something wrong!", ret);
                exit(0);
            }
#elif defined(AARCH64)
        ret = moneta_arm_hypercall(HC_AGAMOTTO_DEBUG, MONETA_DEBUG_HC_SAVEVM, 0, 0);
#endif
    }

    if (moneta_syscall_count >= moneta_syscall_threshold)
        moneta_syscall_count = 0;

    if (moneta_timeout > 0)
    {
        pid_t ppid = getpid();
        if (!fork())
        {
            error_msg("waiting to kill parent");
            sleep(moneta_timeout);
            error_msg("killing");
            kill(ppid, SIGKILL);
        }
        moneta_timeout = 0;
    }

    ret = -1473;
    error_msg("Determining current stage...");
#ifdef X86_64
    ret = agamotto_kvm_hypercall2(HC_AGAMOTTO_DEBUG, MONETA_DEBUG_HC_CHKCHKPT);
#elif defined(AARCH64)
    ret = moneta_arm_hypercall(HC_AGAMOTTO_DEBUG, MONETA_DEBUG_HC_CHKCHKPT, 0, 0);
#endif
    error_msg("RET from MONETA_DEBUG_HC_CHKCHKPT: %d", ret);

    if (ret == 1) {
        error_msg("Fuzzing stage");
        if (moneta_exec_tracee)
            moneta_exec();
        kill_all_tracees(false);
        execl("/syz-executor.debug", "syz-executor.debug", NULL);
        error_msg("syz-executor exec fail!\n");
        exit(0);
    }
    else if (ret == 0) {
        error_msg("Still in Snapshot stage");
        moneta_close_dfd(getpid(), fsnapPoint);

        if (moneta_snapnum > 0 && moneta_snapnum > moneta_snapnum_count)
        {
            error_msg("Continue tracee run, snapshots are left");
        }
        else {
            error_msg("No more snapshots");
        }
        // TODO stop after some time, otherwise just go business as usual
    }
    // This is just her for debug for now
    else
    {
        error_msg("Something weird is going on");
    }
}

extern char **environ;

static unsigned long
get_process_stack_base(pid_t pid)
{
	char tmp[4096];
	char cmd[128];

	sprintf(cmd, "cat /proc/%d/maps | grep \\\\[stack | cut -d' ' -f1 | cut -d'-' -f2", pid);

	FILE* fp = popen(cmd, "r");

	if (!fp || feof(fp))
	{
		error_msg_and_die("Couldn't calculate stack base for gpu proc");
		exit(-1);
		return 0;
	}

	int ret = fread(tmp, 1, 4096, fp);
	if (ret <= 0)
	{
		error_msg_and_die("Couldn't calculate stack base for gpu proc");
		exit(-1);
		return 0;
	}

	pclose(fp);
	return strtoul(tmp, NULL, 16);	
}

static void
moneta_exec_syscall(struct user_regs_struct regs, int check)
{
    int status;
    #ifdef __x86_64
    unsigned long long syscall_no = regs.rax;
    #elif defined(__aarch64__)
    unsigned long long syscall_no = regs.regs[8];
    struct iovec registers = { &regs, sizeof(regs) };
    #else
    #error what?
    #endif

    // at syscall entrance
    #ifdef __x86_64
    if (ptrace(PTRACE_SETREGS, moneta_gpu_pid, NULL, &regs) < 0)
        error_msg_and_die("Error while writing regs for injected syscall %lld - errno: %d", syscall_no, errno);
    #elif defined(__aarch64__)
    struct iovec syscall_iov = { &regs.regs[8], sizeof(unsigned long) };
    if (ptrace(PTRACE_SETREGSET, moneta_gpu_pid, NT_ARM_SYSTEM_CALL, &syscall_iov) < 0)
        error_msg_and_die("Error while writing syscall number %lld - errno: %d", syscall_no, errno);
    if (ptrace(PTRACE_SETREGSET, moneta_gpu_pid, NT_PRSTATUS, &registers) < 0)
        error_msg_and_die("Error while writing regs for injected syscall %lld - errno: %d", syscall_no, errno);
    #else
    #error what?
    #endif

    // executin the syscall
    if (ptrace(PTRACE_SYSCALL, moneta_gpu_pid, NULL, NULL) < 0)
        error_msg_and_die("Error while going to exit for injected syscall %lld", syscall_no);
    if (waitpid(moneta_gpu_pid, &status, 0) != moneta_gpu_pid)
        error_msg_and_die("Error while waiting on exit for injected syscall %lld", syscall_no);

    printf(" > first status: %d - ", status);
    if (WIFEXITED(status))
        printf("WIFEXITED (%d)\n", WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        printf("WIFSIGNALED (%d)\n", WTERMSIG(status));
    else if (WCOREDUMP(status))
        printf("WCOREDUMP\n");
    else if (WIFSTOPPED(status))
    {
        printf("WIFSTOPPED - ");
        siginfo_t sig_info;
        if (ptrace(PTRACE_GETSIGINFO, moneta_gpu_pid, NULL, &sig_info) < 0)
            error_msg_and_die("Error while writing regs for injected syscall %lld - errno: %d", syscall_no, errno);
        printf("si_signo: %d", sig_info.si_signo);
        printf(" | si_errno: %d", sig_info.si_errno);
        printf(" | si_code: %d\n", sig_info.si_code);
    }
    else if (WSTOPSIG(status))
        printf("WSTOPSIG\n");
    else if (WIFCONTINUED(status))
        printf("WIFCONTINUED\n");
    else
        printf("what?\n");

    // at syscall exit, going back to syscall entrance
    #ifdef __x86_64
    if (ptrace(PTRACE_SETREGS, moneta_gpu_pid, NULL, &regs) < 0)
        error_msg_and_die("Error while writing regs to go back to start for injected syscall %lld.", syscall_no);
    #elif defined(__aarch64__)
    if (ptrace(PTRACE_SETREGSET, moneta_gpu_pid, NT_PRSTATUS, &registers) < 0)
        error_msg_and_die("Error while writing regs to go back to start for injected syscall %lld.", syscall_no);
    #else
    #error what
    #endif
    if (ptrace(PTRACE_SYSCALL, moneta_gpu_pid, NULL, NULL) < 0)
        error_msg_and_die("Error while going back to start after injected syscall %lld", syscall_no);
    if (waitpid(moneta_gpu_pid, &status, 0) != moneta_gpu_pid)
        error_msg_and_die("Error while waiting for start after injected syscall %lld", syscall_no);

    printf(" > second status: %d - ", status);
    if (WIFEXITED(status))
        printf("WIFEXITED (%d)\n", WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        printf("WIFSIGNALED (%d)\n", WTERMSIG(status));
    else if (WCOREDUMP(status))
        printf("WCOREDUMP\n");
    else if (WIFSTOPPED(status))
    {
        printf("WIFSTOPPED - ");
        siginfo_t sig_info;
        if (ptrace(PTRACE_GETSIGINFO, moneta_gpu_pid, NULL, &sig_info) < 0)
            error_msg_and_die("Error while writing regs for injected syscall %lld - errno: %d", syscall_no, errno);
        printf("si_signo: %d", sig_info.si_signo);
        printf(" | si_errno: %d", sig_info.si_errno);
        printf(" | si_code: %d\n", sig_info.si_code);
    }
    else if (WSTOPSIG(status))
        printf("WSTOPSIG\n");
    else if (WIFCONTINUED(status))
        printf("WIFCONTINUED\n");
    else
        printf("what?\n");
    // back at syscall entrance
}

void
moneta_exec(void)
{
	printf("moneta_exec: start\n");
    kill_all_tracees(true);

    struct user_regs_struct regs;
    #ifdef __x86_64
    if (ptrace(PTRACE_GETREGS, moneta_gpu_pid, (void*)NULL, (void*)&regs) == -1)
    {
        printf("%d\n", errno);
        error_msg_and_die("Something went horribly wrong...");
    }
	{
		printf("moneta_exec: read registers\n");
	}
    #elif defined(__aarch64__)
    struct iovec registers = { &regs, sizeof(regs) };
    if (ptrace(PTRACE_GETREGSET, moneta_gpu_pid, (void*)NT_PRSTATUS, (void*)&registers) == -1)
    {
        printf("%d\n", errno);
        error_msg_and_die("Something went horribly wrong...");
    }
	{
		printf("moneta_exec: read registers (%ld bytes)\n", registers.iov_len);
	}
	#else
    #error not done.
    #endif

	// stijn: not safe. Assumes rbp is a valid frame pointer. It might not be
//    unsigned long args_pt = regs.rbp;

	// stijn: this is better
	unsigned long args_pt = get_process_stack_base(moneta_gpu_pid);
	printf("moneta_exec: found stack top @ %016lx\n", args_pt);
	
	printf("moneta_exec: analyzing env vars\n");
    int env_i = 0; // number of env vars
    void* next = 0;
    int non_contigous = 0; // set to 1 if there's a gap in the environment strings region
    int env_data_size = 0;
    do
    {
        int start = env_data_size;
		env_data_size += strlen(environ[env_i]) + 1;
		
        if (next && (next != environ[env_i]))
            non_contigous = 1;

		// remember where envp[i] ended so we can detect gaps
        next = (char*)(environ[env_i] + (env_data_size - start));
    } while (environ[++env_i]);

    if (non_contigous)
	{
        error_func_msg_and_die("FIXME: non-contigous envp");
	}

	printf("moneta_exec: found %d env vars - total string size: %d\n", env_i, env_data_size);

	//
	// Build stack layout as follows:
	//
	// OFFSET             | DESCRIPTION                      | SIZE                             | ALIGNMENT
	// -------------------+----------------------------------+----------------------------------+-------------------------
	// ...                | envp[env_i-1] string             | ^                                |
	// ...                | ...                              | | env_data_size                  |
	// envp_string_start  | envp[0] string                   | V                                | 1
	// ...                | envp[env_i] pointer (== NULL)    | ^                                | 
	// ...                | ...                              | | (env_i+1) * sizeof(void*)      | 
	// envp_pointer_start | envp[0] pointer                  | V                                | 16
	// argv_string_start  | argv[0] string                   | > basename_size                  | 1
	//                    | argv[1] pointer (== NULL)        | ^                                |
	// argv_pointer_start | argv[0] pointer                  | V 2 * sizeof(void*) 	            | 16
	// full_path_start    | full binary path string          | full_path_size                   | 16 (start of the region)
	//

	// sizes
	int basename_size      = strlen(EXECUTOR_BASENAME) + 1;
	int full_path_size     = strlen(EXECUTOR_PATH) + strlen(EXECUTOR_BASENAME) + 1;
	
	// offsets - calculate them relative to the stack top, then turn them around
	int envp_string_start  = env_data_size;
	int envp_pointer_start = ROUND_UP(envp_string_start + (env_i + 1) * sizeof(void*), 16);
	int argv_string_start  = envp_pointer_start + basename_size;
	int argv_pointer_start = ROUND_UP(argv_string_start + 2 * sizeof(void*), 16);
	int full_path_start    = ROUND_UP(argv_pointer_start + full_path_size, 16);
	int args_size          = full_path_start;

	// convert offsets into bottom-relative numbers
	full_path_start    = 0;
	argv_pointer_start = args_size - argv_pointer_start;
	argv_string_start  = args_size - argv_string_start;
	envp_pointer_start = args_size - envp_pointer_start;
	envp_string_start  = args_size - envp_string_start;

	printf("moneta_exec: calculated execve stack layout - full path @ 0, argv pointers @ %08x, argv strings @ %08x, envp pointers @ %08x, envp strings @ %08x\n", argv_pointer_start, argv_string_start, envp_pointer_start, envp_string_start);
	
    char* buffer = (char*) malloc(args_size);

	// adjust args_pt so we definitely have room to write all of our argv/envp data
	// remember: the stack growns down
	args_pt -= args_size;

	// write full name
	sprintf(buffer, "%s%s", EXECUTOR_PATH, EXECUTOR_BASENAME);
	printf("moneta_exec: writing execve path = %s @ %016lx\n", buffer, args_pt);
	
	// write basename
	sprintf(buffer + argv_string_start, "%s", EXECUTOR_BASENAME);

	// write argv[0] pointer
    void* buffer_i = (void*)(buffer + argv_pointer_start);	
    *(void**)(buffer_i) = (void*)args_pt + argv_string_start;

	printf("moneta_exec: writing argv[0] = %016lx (%s) @ %016lx\n",
		   args_pt + argv_string_start,
		   buffer + argv_string_start,
		   args_pt + (char*)buffer_i - buffer);	
	
    buffer_i += sizeof(void*);

	// write argv[1] pointer
    *(void**)(buffer_i) = (void*) NULL;
	printf("moneta_exec: writing argv[1] = NULL @ %016lx\n",
		   args_pt + (char*)buffer_i - buffer);		
    buffer_i += sizeof(void*);

	printf("moneta_exec: copying environment vars\n");
	buffer_i = (void*)(buffer + envp_pointer_start);
    for (int env_i_copy = 0; env_i_copy < env_i; env_i_copy++)
	{
        ((void**)buffer_i)[env_i_copy] = (void*) (args_pt + envp_string_start + (environ[env_i_copy] - environ[0]));
		printf("moneta_exec: writing envp[%d] = %016lx (%s) @ %016lx\n",
			   env_i_copy,
			   args_pt + envp_string_start + (environ[env_i_copy] - environ[0]),
			   environ[env_i_copy],			   
			   args_pt + (char*)buffer_i + env_i_copy * sizeof(void*) - buffer);				
	}
    ((void**)buffer_i)[env_i] = NULL;
    buffer_i += (env_i * sizeof(void*)) + sizeof(void*);

	// copy env var strings as-is	
    memcpy(buffer + envp_string_start, environ[0], env_data_size);

	printf("moneta_exec: copying execve arguments to %016lx\n", args_pt);
    if (write_tracee_memory(moneta_gpu_pid, (void*)args_pt, args_size, (void*)buffer) != args_size)
	{
        die();
	}
    free(buffer);

    unsigned long path_pt = args_pt - 256;
    char fd_path[256];
    if (moneta_reopen_stdout)
    {
        char proc_fd_path[100];
        snprintf(proc_fd_path, sizeof(proc_fd_path), "/proc/%d/fd/0", getpid());
        ssize_t bytes_read = readlink(proc_fd_path, fd_path, sizeof(fd_path) - 1);
        if (bytes_read != -1) {
            fd_path[bytes_read] = '\0';
        }
        printf("moneta_exec: copying open path %s to %016lx\n", fd_path, path_pt);
        if (write_tracee_memory(moneta_gpu_pid, (void*)path_pt, bytes_read + 1, (void*)fd_path) != bytes_read + 1)
        {
            die();
        }
    }
	
    #ifdef __x86_64
    if (moneta_reopen_stdout)
    {
        printf("moneta_exec: injecting open(%s)\n", fd_path);
        regs.rax = __NR_open;
        regs.orig_rax = __NR_open;
        regs.rdi = path_pt;
        regs.rsi = O_RDWR;
        regs.rdx = 0;
        moneta_exec_syscall(regs, 0);

        if (ptrace(PTRACE_GETREGS, moneta_gpu_pid, (void*)NULL, (void*)&regs) == -1)
        {
            printf("%d\n", errno);
            error_msg_and_die("Something went horribly wrong...");
        }

        int newfd = regs.rax;
        regs.rax = regs.orig_rax = __NR_dup2;
        regs.rdi = newfd;
        regs.rsi = 1;
        regs.rdx = 0;
        moneta_exec_syscall(regs, 0);
    }

	printf("moneta_exec: injecting execve(%s%s)\n", EXECUTOR_PATH, EXECUTOR_BASENAME);
	regs.rax = __NR_execve;
	regs.orig_rax = __NR_execve;
    regs.rdi = args_pt; // full path name
    regs.rsi = args_pt + argv_pointer_start;
    regs.rdx = args_pt + envp_pointer_start;

    if (ptrace(PTRACE_SETREGS, moneta_gpu_pid, (void*)NULL, (void*)&regs) < 0)
    {
        error_msg("Could not write ptrace regs %d", errno);
    }

	printf("moneta_exec: detaching\n");
    ptrace(PTRACE_DETACH, moneta_gpu_pid, NULL, NULL);

    #elif defined(__aarch64__)
    if (moneta_reopen_stdout)
    {
        printf("moneta_exec: injecting open(%s)\n", fd_path);
        regs.regs[8] = __NR_openat;
        regs.regs[0] = -1;
        regs.regs[1] = path_pt;
        regs.regs[2] = O_RDWR;
        regs.regs[3] = 0;
        moneta_exec_syscall(regs, 0);

        if (ptrace(PTRACE_GETREGSET, moneta_gpu_pid, (void*)NT_PRSTATUS, (void*)&registers) == -1)
        {
            printf("%d\n", errno);
            error_msg_and_die("Something went horribly wrong...");
        }

        regs.regs[8] = __NR_dup3;
        regs.regs[0] = 1;
        regs.regs[1] = 0;
        regs.regs[2] = 0;
        moneta_exec_syscall(regs, 0);
    }

	printf("moneta_exec: injecting execve(%s%s)\n", EXECUTOR_PATH, EXECUTOR_BASENAME);
	regs.regs[8] = __NR_execve;
    regs.regs[0] = args_pt; // full path name
    regs.regs[1] = args_pt + argv_pointer_start;
    regs.regs[2] = args_pt + envp_pointer_start;

    struct iovec syscall_no = { &regs.regs[8], sizeof(unsigned long) };
    ptrace(PTRACE_SETREGSET, moneta_gpu_pid, NT_ARM_SYSTEM_CALL, &syscall_no);
    if (ptrace(PTRACE_SETREGSET, moneta_gpu_pid, (void*)NT_PRSTATUS, (void*)&registers) < 0)
    {
        error_msg("Could not write ptrace regs %d", errno);
    }

	printf("moneta_exec: detaching\n");
    ptrace(PTRACE_DETACH, moneta_gpu_pid, NULL, NULL);
    #else
    #error what?
    #endif

    exit(0);
    error_msg_and_die("tracer did not exit");
    return;
}
