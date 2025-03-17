#include "qemu/osdep.h"

#include "migration/ram.h"
#include "qapi/error.h"
#include "qom/cpu.h"
#include "sysemu/kvm.h"

#include <sys/shm.h>

#include "migration/periscope.h"

static int chkpt_disable_after_nth = 0; // one-indexed

#define CHKPT_TIME_THRESHOLD_MS 500
#define CHKPT_TIME_THRESHOLD_MULTIPLIER 2

static uint64_t chkpt_time_threshold_ms = CHKPT_TIME_THRESHOLD_MS;

#define CHKPT_COV_THRESHOLD 1500
//#define CHKPT_COV_THRESHOLD 500

static uint64_t last_chkpt_ms = 0;
static int chkpt_policy = PERISCOPE_CHKPT_TIME_ONLY; // default policy
static int restore_policy = PERISCOPE_RESTORE_LONGEST;

static uint8_t
    last_execute_req_input[sizeof(struct syzkaller_execute_req) + kMaxInput];

static uint64_t call_to_input_pos[MaxCalls+30];

uint64_t syzkaller_maybe_checkpoint(uint64_t input_pos, uint64_t cov_size)
{
    uint64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    uint64_t time_since_last_chkpt_ms = now - last_chkpt_ms;

    FuzzerState *fs = fuzzer_get_current();
    assert(fs);
    assert(fs->cur_cp);
    fs->cur_cp->exec_time.tv_sec = time_since_last_chkpt_ms / 1000UL;
    fs->cur_cp->exec_time.tv_usec =
        (time_since_last_chkpt_ms % 1000UL) * 1000UL;

//#define TRACE_CHKPT_POLICY
#ifdef TRACE_CHKPT_POLICY
    printf("periscope: call_num=%d input_pos=0x%lx cov_size=%lu "
           "time_since_last_chkpt=%lums\n", call_num,
           input_pos, cov_size, time_since_last_chkpt_ms);
#endif

    bool should_chkpt = true;
    int call_num = 0;

    switch (chkpt_policy) {
    case PERISCOPE_CHKPT_TIME_AND_COV:
        should_chkpt &= (cov_size > CHKPT_COV_THRESHOLD);
        should_chkpt &= (time_since_last_chkpt_ms > chkpt_time_threshold_ms);
        break;
    case PERISCOPE_CHKPT_TIME_ONLY:
        should_chkpt &= (time_since_last_chkpt_ms > chkpt_time_threshold_ms);
        break;
    case PERISCOPE_CHKPT_DISABLED:
        should_chkpt = false;
        break;
    case PERISCOPE_CHKPT_TIME_ONLY_DISABLED_AFTER_NTH:
        call_num = 0;

        for (int i=0; i<sizeof(call_to_input_pos)/sizeof(call_to_input_pos[0]); i++) {
            if (call_to_input_pos[i] == 0) {
                break;
            }

            if (input_pos == call_to_input_pos[i]) {
                printf("periscope: call_num=%d input_pos=0x%lx\n", i, call_to_input_pos[i]);
                call_num = i;
                break;
            }
        }

        if (chkpt_disable_after_nth < call_num) {
            printf("periscope: chkpt disabled for call %d (>%d)\n",
                   call_num, chkpt_disable_after_nth);
            should_chkpt = false;
        } else {
            should_chkpt &= (time_since_last_chkpt_ms > chkpt_time_threshold_ms);
        }
        break;
    default:
        printf("periscope: unknown checkpoint policy\n");
        break;
    }

    if (should_chkpt == true) {
        printf("periscope: policy %d requests checkpoint\n", chkpt_policy);

        FuzzerState *fs = fuzzer_get_current();
        assert(fs);

        uint32_t len = sizeof(struct syzkaller_execute_req) + input_pos;

        // used from full-length
        assert(fs->cur_input);
        fs->cur_input->used_len = len;

        // delta from base
        uint32_t len_base = 0;
        assert(fs->cur_cp);
        periscope_cp_desc *parent = fs->cur_cp->parent;
        while (parent != NULL) {
            len_base += parent->len;
            parent = parent->parent;
        }
        assert(len > len_base);
        fs->cur_cp->len = len - len_base;

        memcpy(fs->cur_cp->io, last_execute_req_input + len_base,
               fs->cur_cp->len);

        if (periscope_purge_and_checkpoint_request() == 0) {
            // reset chkpt counter
            last_chkpt_ms = now;
            // exponentially increasing intervals
            chkpt_time_threshold_ms *= CHKPT_TIME_THRESHOLD_MULTIPLIER;
            return 0; // SUCCESS
        }
    }

    return -1;
}

static bool read_virtual_memory(uint64_t address, uint8_t *data, uint32_t size,
                                CPUState *cpu)
{
    uint8_t tmp_buf[TARGET_PAGE_SIZE];
    MemTxAttrs attrs;
    hwaddr phys_addr;
    int asidx;
    uint64_t counter, l;
    int i = 0;

    counter = size;

    while (counter != 0) {
        l = TARGET_PAGE_SIZE;
        if (l > counter)
            l = counter;

        asidx = cpu_asidx_from_attrs(cpu, MEMTXATTRS_UNSPECIFIED);
        attrs = MEMTXATTRS_UNSPECIFIED;
        phys_addr = cpu_get_phys_page_attrs_debug(
            cpu, (address & TARGET_PAGE_MASK), &attrs);

        phys_addr += (address & ~TARGET_PAGE_MASK);

        printf("periscope: paddr=0x%lx for vaddr=0x%lx l=0x%lx\n", phys_addr,
               address, l);

        address_space_rw(cpu_get_address_space(cpu, asidx), phys_addr,
                         MEMTXATTRS_UNSPECIFIED, tmp_buf, l, false);

        memcpy(data + (i * TARGET_PAGE_SIZE), tmp_buf, l);

        i++;
        address += l;
        counter -= l;
    }

    return true;
}

static bool write_virtual_memory(uint64_t address, uint8_t *data, uint32_t size,
                                 CPUState *cpu)
{
    int asidx;
    MemTxAttrs attrs;
    hwaddr phys_addr;
    MemTxResult res;

    uint64_t counter, l, i;

    counter = size;
    while (counter != 0) {
        l = TARGET_PAGE_SIZE;
        if (l > counter)
            l = counter;

        asidx = cpu_asidx_from_attrs(cpu, MEMTXATTRS_UNSPECIFIED);
        attrs = MEMTXATTRS_UNSPECIFIED;
        phys_addr = cpu_get_phys_page_attrs_debug(
            cpu, (address & TARGET_PAGE_MASK), &attrs);

        if (phys_addr == -1) {
            printf("periscope: failed to get paddr for vaddr=0x%lx\n", address);
            return false;
        }

        phys_addr += (address & ~TARGET_PAGE_MASK);

        printf("periscope: paddr=0x%lx for vaddr=0x%lx l=0x%lx\n", phys_addr,
               address, l);

        res = address_space_rw(cpu_get_address_space(cpu, asidx), phys_addr,
                               MEMTXATTRS_UNSPECIFIED, data, l, true);
        if (res != MEMTX_OK) {
            printf("periscope: failed to write to vaddr=0x%lx paddr=0x%lx\n",
                   address, phys_addr);
            return false;
        }

        i++;
        data += l;
        address += l;
        counter -= l;
    }

    return true;
}

static int kInPipeFd = -1;
static int kOutPipeFd = -1;

#define kInMagic (uint64_t)0xbadc0ffeebadface
#define kOutMagic (uint32_t)0xbadf00d
#define kCrashMagic (uint32_t)0xbadcbadc

void syzkaller_reply_handshake(void)
{
    printf("periscope: replying handshake\n");

    struct syzkaller_handshake_reply reply = {};
    reply.magic = kOutMagic;

    if (write(kOutPipeFd, &reply, sizeof(reply)) != sizeof(reply))
        printf("periscope: syz-fuzzer control pipe write failed\n");
}

void syzkaller_reply_execute_crash(void)
{
    struct syzkaller_execute_reply repl = {};
    printf("periscope: syz-fuzzer reply execute crash size=%lu\n",
           sizeof(repl));

    repl.magic = kCrashMagic;
    repl.done = true;
    repl.status = 0;

    if (write(kOutPipeFd, &repl, sizeof(repl)) != sizeof(repl))
        printf("periscope: syz-fuzzer control pipe write failed\n");
}

void syzkaller_reply_execute(uint32_t status)
{
    struct syzkaller_execute_reply repl = {};
    printf("periscope: syz-fuzzer reply execute status=%d size=%lu\n", status,
           sizeof(repl));

    bool killed = false;
    if (status == 1) {
        killed = true;
        status = 0;
    }

    repl.magic = kOutMagic;
    repl.done = true;
    repl.status = status;

    FuzzerState *fs = fuzzer_get_current();
    assert(fs);
    assert(fs->cur_cp);
    uint64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    qemu_timeval elapsed;
    elapsed.tv_sec = (now - last_chkpt_ms) / 1000;
    elapsed.tv_usec = ((now - last_chkpt_ms) % 1000) * 1000;
    memcpy(&fs->cur_cp->exec_time, &elapsed, sizeof(qemu_timeval));

    for (int stat = 0; stat < stat_count; stat++) {
        if (stat == stat_killed) {
            repl.stats[stat] = killed ? 1 : 0;
            continue;
        }
        uint32_t v = periscope_get_stat(stat);
        repl.stats[stat] = v;
    }

    if (write(kOutPipeFd, &repl, sizeof(repl)) != sizeof(repl))
        printf("periscope: syz-fuzzer control pipe write failed\n");
}

void syzkaller_receive_handshake(CPUState *cpu, uint64_t addr)
{
    // printf("periscope: syz-fuzzer receive handshake kInPipe=%d,
    // addr=0x%lx\n", kInPipeFd, addr);

    struct syzkaller_handshake_req req = {};

    if (read_virtual_memory(addr, (uint8_t *)&req, sizeof(req), cpu) == false) {
        printf("periscope: mem tx failed\n");
        return;
    }

    for (unsigned i = 0; i < sizeof(req); i++) {
        if (*((uint8_t *)&req + i) != 0xdc) {
            printf("periscope: buffer sanity check failed\n");
            exit(1);
        }
    }

    int n = read(kInPipeFd, &req, sizeof(req));
    if (n != sizeof(req)) {
        printf("periscope: handshake read failed: %d\n", n);
        return;
    }

    printf("periscope: syz-fuzzer receive handshake (magic=0x%lx)\n",
           req.magic);

#if 1
    bool res = write_virtual_memory(addr, (uint8_t *)&req, sizeof(req), cpu);
    if (res == false) {
        printf("periscope: mem tx failed %d\n", res);
        return;
    }
#else
    RAMBlock *block = qemu_ram_block_by_name("pc.ram");
    if (!block) {
        printf("periscope: could not find ramblock\n");
        return;
    }
    struct syzkaller_handshake_req *dst =
        host_from_ram_block_offset(block, addr);
    if (!dst) {
        printf("periscope: could not find offset=0x%lx in ramblock", addr);
        return;
    }
    memcpy(dst, &req, sizeof(req));
#endif

    printf("periscope: syz-fuzzer receive handshake (size=%lu)\n", sizeof(req));
}

/*
 * executor input buffer parsing logic
 */
typedef unsigned char uint8;
typedef unsigned long long uint64;

static char *input_data;

#define kMaxArgs 9
#define kFailStatus 67

static void fail(const char *msg, ...)
{
    int e = errno;
    va_list args;
    va_start(args, msg);
#if 0
	printk(msg, args);
#else
    vfprintf(stderr, msg, args);
#endif
    va_end(args);
    fprintf(stderr, " (errno %d)\n", e);
    exit(kFailStatus);
}

static unsigned long long procid;

#define kMaxCommands 1000

#define instr_eof -1
#define instr_copyin -2
#define instr_copyout -3
#define instr_nextcall -4

#define arg_const 0
#define arg_result 1
#define arg_data 2
#define arg_csum 3

#define binary_format_native 0
#define binary_format_bigendian 1
#define binary_format_strdec 2
#define binary_format_strhex 3
#define binary_format_stroct 4

struct res_t {
    bool executed;
    uint64 val;
};

static uint64 read_input(uint64 **input_posp /*, bool peek*/)
{
    uint64 *input_pos = *input_posp;
    if ((char *)input_pos >= input_data + kMaxInput)
        fail("input command overflows input %p: [%p:%p)", input_pos, input_data,
             input_data + kMaxInput);
    // if (!peek)
    *input_posp = input_pos + 1;
    return *input_pos;
}

#ifdef DEBUG_CALL
static void debug_call(uint64 call_num, uint64 *args, int num_args)
{
    printf("periscope: syscall #%lld (", call_num);
    for (int i = 0; i < num_args; i++) {
        if (i != 0)
            printf(", ");
        printf("0x%llx", (uint64)args[i]);
    }
    printf(")\n");
}
#endif

static void read_execute_req(uint8 *out_buf, uint64 *out_len,
                             uint32_t *out_num_calls)
{
    struct syzkaller_execute_req *req = (struct syzkaller_execute_req *)out_buf;
    assert(req != NULL);

    uint8 *input = out_buf + sizeof(struct syzkaller_execute_req);
    assert(input != NULL);

    // printf("periscope: read execute req\n");

    int n = read(kInPipeFd, req, sizeof(struct syzkaller_execute_req));
    if (n != sizeof(struct syzkaller_execute_req)) {
        printf("periscope: read execute req failed: %d\n", n);
        return;
    }

    if (req->magic != kInMagic) {
        printf("periscope: read execute req magic value incorrect 0x%lx\n",
               req->magic);
    }

    procid = req->pid;

    // printf("periscope: syz-fuzzer receive execute (magic=0x%lx)\n",
    // req.magic);

    // checkpoint and restore policy determination before execution based on
    // the hints provided by syz-fuzzer
    bool flag_triage = req->exec_flags & (1 << 6);
    bool flag_minimize = req->exec_flags & (1 << 7);
    bool flag_minimize_retry = req->exec_flags & (1 << 8);
    bool flag_fuzz = req->exec_flags & (1 << 9);
    bool flag_generate = req->exec_flags & (1 << 10);
    bool flag_smash = req->exec_flags & (1 << 11);

    uint64_t mutated_from_nth = req->mut_from_nth;

    // cleanup any auxiliary flags (e.g., hint flags) not affecting test case
    // execution by any means
    req->exec_flags &= ((uint64)(1 << 6) - 1);
    req->mut_from_nth = 0;

    assert(req->prog_size == 0);

    printf("periscope: t=%d m=%d mr=%d, f=%d g=%d s=%d exec_flags=0x%lx\n",
           flag_triage, flag_minimize, flag_minimize_retry, flag_fuzz,
           flag_generate, flag_smash, req->exec_flags);

    chkpt_disable_after_nth = 0;

    if (flag_triage) {
        chkpt_policy = PERISCOPE_CHKPT_TIME_ONLY;

        // restore to root to help determine flaky coverage signals; this is to
        // preserve intended functionality of the triaging process
        restore_policy = PERISCOPE_RESTORE_LONGEST;
    } else if (flag_minimize) {
        if (flag_minimize_retry) {
            chkpt_policy = PERISCOPE_CHKPT_TIME_ONLY;
            restore_policy = PERISCOPE_RESTORE_LONGEST;
        } else {
            chkpt_policy = PERISCOPE_CHKPT_TIME_ONLY;
            restore_policy = PERISCOPE_RESTORE_LONGEST;
        }
    } else if (flag_fuzz || flag_smash) {
        if (mutated_from_nth > 0) {
            chkpt_policy = PERISCOPE_CHKPT_TIME_ONLY_DISABLED_AFTER_NTH;
            chkpt_disable_after_nth = mutated_from_nth;
            restore_policy = PERISCOPE_RESTORE_LONGEST;
            printf("periscope: disable chkpt after %dth\n",
                   chkpt_disable_after_nth);
        } else {
            chkpt_policy = PERISCOPE_CHKPT_DISABLED;
            restore_policy = PERISCOPE_RESTORE_LONGEST;
            printf("periscope: disable chkpt\n");
        }
    } else if (flag_generate) {
        chkpt_policy = PERISCOPE_CHKPT_DISABLED;
        restore_policy = PERISCOPE_RESTORE_LONGEST;
    } else {
        // conservative default
        chkpt_policy = PERISCOPE_CHKPT_DISABLED;
        restore_policy = PERISCOPE_RESTORE_ROOT;
    }

    FuzzerState *fs = fuzzer_get_current();
    assert(fs);

    MemoryRegion *mr = fs->mr[0];
    assert(mr);

    // determine program size for chkpt search & potential chkpt
    input_data = memory_region_get_ram_ptr(
        mr); // ensure nobody else is modifying this region concurrently.
    if (input_data) {
        uint64 *input_pos = (uint64 *)input_data;
        *out_num_calls = 0;

        for (;;) {
            uint64 call_num = read_input(&input_pos);
            // printf("periscope: pos=0x%lx call_num=0x%llx\n",
            //        (char *)input_pos - input_data, call_num);

            if (call_num == instr_nextcall) {
                uint64 next_call = read_input(&input_pos);
                // printf("periscope: pos=0x%lx next_call=0x%llx\n",
                //        (char *)input_pos - input_data, next_call*8);
                call_to_input_pos[*out_num_calls] = (uint64_t)input_pos - (uint64_t)input_data;
                call_to_input_pos[*out_num_calls+1] = 0;

                input_pos += next_call;
                if (next_call > 0)
                    *out_num_calls += 1;
                continue;
            }

            if (call_num == instr_eof) {
                uint64 len = (char *)input_pos - input_data;
                printf("periscope: %llu bytes (instr_eof after %d calls)\n",
                       len, *out_num_calls);

                memcpy(input, input_data, len);
                *out_len = sizeof(struct syzkaller_execute_req) + len;
                break;
            }

            // must be unreachable
            assert(false);
        }
    }
}

void syzkaller_receive_execute(CPUState *cpu, uint64_t addr)
{
    printf("periscope: syz-fuzzer receive execute (addr=0x%lx)\n", addr);

    struct syzkaller_execute_req req = {};
    if (read_virtual_memory(addr, (uint8_t *)&req, sizeof(req), cpu) == false) {
        printf("periscope: mem tx failed\n");
        return;
    }

    if (req.magic != 0xfefefefefefefefe) {
        printf("periscope: wrong execute_req.magic=0x%lx\n", req.magic);
        exit(1);
    }

#if 1
    bool res = write_virtual_memory(addr, last_execute_req_input,
                                    sizeof(struct syzkaller_execute_req), cpu);

    if (res == false) {
        printf("periscope: syz-fuzzer receive exeucte mem tx failed %d\n", res);
    }
#else
    RAMBlock *block = qemu_ram_block_by_name("pc.ram");
    if (!block) {
        printf("periscope: could not find ramblock\n");
        return;
    }

    struct syzkaller_execute_req *dst = host_from_ram_block_offset(block, addr);
    if (!dst) {
        printf("periscope: could not find offset=0x%lx in ramblock", addr);
        return;
    }
    memcpy(dst, &req, sizeof(req));
#endif

    // printf("periscope: syz-fuzzer receive execute (size=%lu)\n",
    // sizeof(req));
}

struct syzkaller_forksrv_ctx {
    uint32_t magic; // expected to be kOutMagic
    uint64_t start;
    uint64_t last_executed;

    // TODO
};

static uint64_t forksrv_ctx_ptr = 0UL;

void syzkaller_submit_forkserver_context(uint64_t ptr)
{
    printf("periscope: submit forkserver context ptr=0x%lx\n", ptr);

    forksrv_ctx_ptr = ptr;
}

#if 0
static void update_forkserver_context(struct syzkaller_forksrv_ctx *ctx,
                                      CPUState *cpu)
{
    if (forksrv_ctx_ptr == 0UL) {
        printf("periscope: no fork server context submitted\n");
        return;
    }

    // TODO: check magic byte is equal to kOutMagic

    write_virtual_memory(forksrv_ctx_ptr, (uint8_t *)ctx,
                         sizeof(struct syzkaller_forksrv_ctx), cpu);
}
#endif

static int mmio_read(unsigned size, uint64_t *out) { return -1; }

static void restored(void)
{
    // reset timer
    last_chkpt_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);

    FuzzerState *fs = fuzzer_get_current();
    assert(fs);
    assert(fs->cur_input);
    assert(fs->cur_input->restored_cp);
    int level = 0;
    periscope_cp_desc *cp = fs->cur_input->restored_cp;
    while (cp != NULL) {
        level++;
        cp = cp->parent;
    }

    // reset chkpt time threshold
    chkpt_time_threshold_ms = CHKPT_TIME_THRESHOLD_MS;
    level--;
    while (level > 0) {
        chkpt_time_threshold_ms *= CHKPT_TIME_THRESHOLD_MULTIPLIER;
        level--;
    }
}

// return buffer should be prefixed by `syzkaller_execute_req` to make sure that
// all environment/configuration vars are taken into account when performing a
// prefix match.
static char *fetch_next(uint32_t *len)
{
    // printf("periscope: syz fetch next\n");

    uint64 buf_len = 0;
    uint32_t num_calls = 0;
    read_execute_req(last_execute_req_input, &buf_len, &num_calls);

    if (restore_policy == PERISCOPE_RESTORE_ROOT) {
        *len = sizeof(struct syzkaller_execute_req);
        return (char *)last_execute_req_input;
    }

    *len = buf_len;

    return (char *)last_execute_req_input;
}

static int mgr_pipe_st;
static int mgr_pipe_ctl;

static bool should_restore(void)
{
    int nread;
    int res;
    if (mgr_pipe_ctl < -1) {
        printf("periscope: syz-manager no ctl pipe found\n");
        return false;
    }

#if 0
    if (mgr_pipe_st < -1) {
        printf("periscope: syz-manager no st pipe found\n");
        return false;
    }

    int val = 0xbadf00d;
    if ((res = write(mgr_pipe_st, &val, 4)) != 4) {
        printf("periscope: syz-manager pipe write failed\n");
    }
#endif

    nread = read(mgr_pipe_ctl, &res, 4);

    int try = 0;

    while (nread == -1) {
        printf("periscope: syz-manager did not request restore yet %d\n", try);
        if (try > 3){
            return false;
        }
        sleep(1);
        nread = read(mgr_pipe_ctl, &res, 4);
        try++;
    }
    if (nread != 4) {
        printf("periscope: syz-manager sent wrong # of bytes\n");
        return false;
    }

    if (res != 0xdeadf00d) {
        return false;
    }

    printf("periscope: syz-manager requested restore\n");

    return true;
}

static int st_pipe;
static int ctl_pipe;
static char *shm_ptr = NULL;
static int shm_id;

void periscope_syzkaller_send_addr_offset(uint64_t offset)
{
    // TODO overflow check
    uint32_t val = (uint32_t)offset;
    int res;

    printf("periscope: writing to status pipe\n");
    return;

    if ((res = write(st_pipe, &val, 4)) != 4) {
        printf("periscope: write to status pipe failed\n");
    }

    // printf("periscope: reading from control pipe\n");

    if ((res = read(ctl_pipe, &val, 4)) != 4) {
        printf("periscope: read from control pipe failed\n");
    }

    if (val > 0) {
        printf("periscope: val=0x%x\n", val);
    }
}

void periscope_syzkaller_notify_boot(void)
{
    uint32_t res, val;
    val = 0xbeeff00d;
    // notify manager
    if (mgr_pipe_st > 0) {
        if ((res = write(mgr_pipe_st, &val, 4)) != 4) {
            printf("periscope: syzkaller notify boot failed\n");
        }
    }
    // notify fuzzer
    if (kOutPipeFd > 0) {
        if ((res = write(kOutPipeFd, &val, 4)) != 4) {
            printf("periscope: syzkaller notify boot failed\n");
        }
    }
    printf("periscope: syzkaller notify boot\n");
}

static int guest_crashed(void)
{
    int res;
    int val = 0xdeadf00d;

    // suspend VM for restore
    bool should_suspend_on_crash = periscope_snapshot_inited();

    printf("periscope: guest crashed %d %d\n", periscope_snapshot_inited(),
           periscope_total_execs());

    if (should_suspend_on_crash) {
        val = 0xbadf00d;
    }

    // inform manager about suspension
    if (mgr_pipe_st < -1) {
        printf("periscope: syz-manager no st pipe found\n");
        return 0;
    }

    if ((res = write(mgr_pipe_st, &val, 4)) != 4) {
        printf("periscope: syz-manager crash signalling failed\n");
        return 0;
    }

    if (should_suspend_on_crash) {
        // let fuzzer know
        syzkaller_reply_execute_crash();

        return PERISCOPE_GUEST_SUSPEND;
    }

    // otherwise, restart VM
    return 0;
}

static bool get_stat(int stat, uint32_t *statVal)
{
    FuzzerState *fs = fuzzer_get_current();

    if (!fs || !fs->cur_input) {
        return false;
    }

    return false;
}

void start_syzkaller_fuzzer(const char *uri, int in_st_pipe, int in_ctl_pipe,
                            const char *in_mgr_pipe, int in_shm_id,
                            Error **errp)
{
    printf("periscope: initializing syz-fuzzer io channels\n");

    char tmp[1024];
    if (uri) {
        strncpy(tmp, uri, strlen(uri));

        char *tok;
        tok = strtok(tmp, ",");
        if (!tok)
            return;

        st_pipe = strtol(tok, NULL, 0);

        tok = strtok(NULL, ",");
        if (!tok)
            return;

        ctl_pipe = strtol(tok, NULL, 0);

        tok = strtok(NULL, ",");
        if (!tok)
            return;

        shm_id = strtol(tok, NULL, 0);
    } else {
        st_pipe = in_st_pipe;
        ctl_pipe = in_ctl_pipe;
        shm_id = in_shm_id;

        if (in_mgr_pipe) {
            memcpy(tmp, in_mgr_pipe, strlen(in_mgr_pipe) + 1);
            tmp[strlen(in_mgr_pipe) - 1] = '0';
            mgr_pipe_st = open(tmp, O_WRONLY);
            if (mgr_pipe_st < 0) {
                printf("periscope: mgr_pipe %s open failed\n", tmp);
            }
            tmp[strlen(in_mgr_pipe) - 1] = '1';
            mgr_pipe_ctl = open(tmp, O_RDONLY);
            if (mgr_pipe_ctl < 0) {
                printf("periscope: mgr_pipe %s open failed\n", tmp);
            }
        }
    }

    if (st_pipe == -1 || ctl_pipe == -1)
        return;

    if (shm_id > -1) {
        int kCoverSize = 256 << 10;
        shm_ptr = mmap(NULL, kCoverSize * sizeof(uintptr_t) * 2,
                       PROT_READ | PROT_WRITE, MAP_SHARED, shm_id, 0);
        if (shm_ptr != MAP_FAILED) {
            printf("periscope: syz-fuzzer shm (id=%d, ptr=%p) initialized\n",
                   shm_id, shm_ptr);
        } else {
            printf("periscope: syz-fuzzer mmap failed\n");
        }
    }

    // int res;
    // int magic_value;
    // if ((res = read(ctl_pipe, &magic_value, 4)) != 4) {
    //     printf("periscope: syz-fuzzer magic value=0x%x\n", magic_value);
    // }

    FuzzerState *s = fuzzer_get_current();
    assert(s != NULL);

    s->mode = PERISCOPE_MODE_SYZKALLER_USBFUZZER;
    s->mmio_read = mmio_read;
    s->fetch_next = fetch_next;
    s->get_cur = NULL;
    s->cur_executed = NULL;
    s->restored = restored;
    s->should_restore = should_restore;
    s->guest_crashed = guest_crashed;
    s->get_stat = get_stat;

    // pipe sanity check
    uint32_t res, val;
    if ((res = read(ctl_pipe, &val, 4)) != 4) {
        printf("periscope: syz-fuzzer read from control pipe failed\n");
    }

    if (val == 0xdeadf00d) {
        // if (shm_ptr != MAP_FAILED && *((uint32_t *)shm_ptr) == 0xbeefbeef)
        printf("periscope: syz-fuzzer handshake successful\n");
    } else {
        printf("periscope: syz-fuzzer handshake failed 0x%x 0x%x\n", val,
               *shm_ptr);

        // TODO: exit?
    }

    // TODO: probably create another set of pipes for in/out pipes?
    kInPipeFd = ctl_pipe;
    kOutPipeFd = st_pipe;

#if 0
    val = 0xbeeff00d;
    if ((res = write(kOutPipeFd, &val, 4)) != 4) {
        printf("periscope: syz-fuzzer write to out pipe failed\n");
    }
#endif

    printf("periscope: syz-fuzzer io channels (st=%d, ctl=%d) initialized\n",
           st_pipe, ctl_pipe);
}
