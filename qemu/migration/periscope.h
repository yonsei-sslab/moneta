#ifndef PERISCOPE_H
#define PERISCOPE_H

#include <stdint.h>
#include "qemu/thread.h"
#include "qom/object.h"
#include "sysemu/sysemu.h"
#include "sysemu/hostmem.h"
#include "hw/periscope/pci.h"
#include "migration/periscope-delta-snap.h"

// afl-2.52b/config.h
// #define MAX_FILE            (1 * 1024 * 1024)

//#define kMaxInput 1024 * 1024

// syzkaller
#define kMaxInput (16 << 20)
#define MaxCalls 3000

struct syzkaller_execute_req {
    uint64_t magic;
    uint64_t env_flags;
    uint64_t exec_flags;
    uint64_t pid;
    uint64_t fault_call;
    uint64_t mut_from_nth;//fault_nth;
    uint64_t prog_size;
};

#define MAX_INPUT_BYTES (kMaxInput + sizeof(struct syzkaller_execute_req))

uint32_t periscope_get_agent_id(void);
void periscope_configure_dev(const char *optarg);

bool periscope_snapshot_inited(void);
int periscope_total_execs(void);

int periscope_mmio_check(MemoryRegion *mr, uint32_t, uint8_t);
int periscope_mmio_read(void *, unsigned, uint64_t *);

int periscope_irq_check(void);
int periscope_maybe_raise_irq(PCIDevice *pci_dev);

enum PERISCOPE_CHKPT_POLICY {
    PERISCOPE_CHKPT_TIME_AND_COV, // conservative default
    PERISCOPE_CHKPT_TIME_ONLY,
    PERISCOPE_CHKPT_DISABLED,           // disable checkpoint
    PERISCOPE_CHKPT_TIME_ONLY_DISABLED_AFTER_NTH, // disable checkpoint after nth op
    PERISCOPE_CHKPT_MAX,
};

enum PERISCOPE_RESTORE_POLICY {
    PERISCOPE_RESTORE_LONGEST,
    PERISCOPE_RESTORE_ROOT,
};

int periscope_change_chkpt_policy(int, int);

void periscope_request_lock_init(void);
void periscope_request_lock(void);
void periscope_request_unlock(void);

int periscope_purge_and_checkpoint_request(void);
int periscope_maybe_checkpoint_request(void);
void periscope_checkpoint_request(void);
uint64_t periscope_checkpoint_requested(void);
int periscope_checkpoint(uint64_t);

void periscope_restore_request(void);
uint64_t periscope_restore_requested(void);
int periscope_restore(void);
int moneta_restore(void);

uint64_t periscope_get_estimated_restore_time_ms(void);

int periscope_system_shutdown_request(ShutdownCause reason);
int periscope_shutdown_requested(void);

void periscope_benchmark_hypercall(uint64_t);
void periscope_debug_hypercall(uint64_t, uint64_t, uint64_t);

// profiling
int periscope_snapshot_and_restore_baseline_requested(void);
void periscope_snapshot_and_restore_baseline(void);

void periscope_syzkaller_send_addr_offset(uint64_t);

#define DBG_RAM_STORED
#undef DBG_RAM_STORED
#define PERI_DEDUP
//#undef PERI_DEDUP
#define PERI_DEDUP_NOHASH
//#undef PERI_DEDUP_NOHASH

#ifdef PERI_DEDUP

#define FINE_CHUNKS
#undef FINE_CHUNKS
#ifdef FINE_CHUNKS
#define CHUNK_SHIFT 2
#else
#define CHUNK_SHIFT 0
#endif
#define CHUNK_DIV (1<<CHUNK_SHIFT)

#define N_CHECKS_REQ 2
#define N_R_BUF 32
// Structure holding information about each stored ramblock
// i.e. dirty bitmap and data to be restored
typedef struct periscope_ramblock {
    // RAMBlock->idstr (for matching)
    char idstr[256];
    int id;
    bool empty;
    bool store_done;
    bool dirty_done;
    // Dirty bitmap at time of checkpoint creation
    unsigned long *dirty;
#ifdef FINE_CHUNKS
    unsigned long *dirty_fine;
#endif
    unsigned long npages;
    unsigned long npages_dirty;
    // #Dirty pages / #Pages stored in *ram
    unsigned long npages_stored;
    unsigned long npages_hashes_added;
    unsigned long npages_zero;
    unsigned long npages_skipped;
    // RAMBlock offsets of stored pages (i.e. rb->host + offset)
    unsigned int *offsets;
    //unsigned int *offsets_zero;
    // zero pages bitmap
    unsigned long *zero_pages;
    // Dirty pages stored at checkpoint creation
    //void **ram_meta;
#ifdef PERI_DEDUP_NOHASH
    void *ram;
#else
    unsigned int *ram_idx;
#endif
#ifdef DBG_RAM_STORED
    // For debugging
    void *rambkp;
    unsigned long rambkp_size;
#endif
} periscope_ramblock;
#endif /* PERI_DEDUP */

typedef struct periscope_snapshot_desc {
    uint16_t id;
    int memfd;
#ifdef ENABLE_LW_CHKPT
#ifndef PERI_DEDUP
    int memfd_quick;
    uint8_t *buf_quick;
    size_t quick_sz;
    int memfd_ram;
    uint8_t *buf_ram;
    size_t ram_sz;
    unsigned long *dirty; // bitmap of pages stored in this snapshot
    unsigned long npages;
#ifdef DBG_RAM_STORED
    void *rambkp;
    unsigned long rambkp_size;
#endif
#else /* PERI_DEDUP */
    uint8_t *buf_dev;
    size_t dev_sz;
    unsigned int n_peri_rb;
    // Structure holding information about each stored ramblock
    // i.e. dirty bitmap and data to be restored
    periscope_ramblock *peri_rb;
    float uniqueness;
#endif /* PERI_DEDUP */
#else
    unsigned long dirty[]; // TODO: delta?
#endif

} periscope_snapshot_desc;

struct periscope_input_desc;
struct periscope_cp_desc;

#define MAX_CP_DESC_CHILDREN 1024

struct periscope_cp_desc {
    char io[MAX_INPUT_BYTES];
    uint32_t len;
    uint32_t n_children;
    bool closed;
    uint32_t num_restored;

    qemu_timeval last_restored;
    qemu_timeval exec_time; // since parent restore
    qemu_timeval closed_time;

    struct periscope_cp_desc *parent;
    // TODO: could be a dynamic structure (e.g., hash table of queues)
    struct periscope_cp_desc *children[MAX_CP_DESC_CHILDREN];

    periscope_snapshot_desc snapshot; // TODO: multiple snapshots?
};

struct periscope_input_desc {
    char *input;
    uint32_t len;
    uint32_t used_len; // 0 <= used_len <= len

    uint32_t num_irqs;
    uint32_t num_io_reads;

    struct periscope_cp_desc *restored_cp; // cp this input was initially based on
    struct periscope_cp_desc *base_cp; // cp this input is currently based on

    uint32_t num_pages_restored;
    uint32_t num_pages_dirtied;
};

typedef struct periscope_input_desc periscope_input_desc;
typedef struct periscope_cp_desc periscope_cp_desc;

void periscope_notify_boot(void);
void periscope_syzkaller_notify_boot(void);

// stats
uint32_t periscope_get_stat(int);

/*
 * Syzkaller
 */
struct syzkaller_handshake_req {
    uint64_t magic;
    uint64_t flags; // env flags
    uint64_t pid;
};

struct syzkaller_handshake_reply {
    uint32_t magic;
};

enum {
    /* aggregate stats */
    stat_time_saved_total = 0,
    stat_time_exec_total,
    stat_non_root_restores,
    stat_chkpts_created,
    stat_killed,
    stat_evict_policy1,
    stat_evict_policy2,
    stat_evict_policy3,
    stat_evict_policy4,
#ifdef PERI_DEDUP_STAT
    stat_evict_policy5,
#endif

    /* time stats */
    stat_exec_time,
    stat_saved_time,
    stat_actual_time,
#ifdef PERI_DEDUP_STAT
    stat_chkpt_time,
    stat_restore_time,
#endif

    /* dirty page stats */
    stat_restored,
    stat_dirtied,
#ifdef PERI_DEDUP_STAT
    stat_dedup,
    stat_hashed,
    stat_zero,
    stat_skipped,
#endif

    /* chkpt stats */
    stat_chkpt_size_0,
    stat_chkpt_size_1,
    stat_chkpt_size_2,
    stat_chkpt_size_3,
    stat_chkpt_size_4,
    stat_chkpt_size_5,
    stat_chkpt_size_6,
    stat_chkpt_size_7,
    stat_chkpt_size_8,
    stat_chkpt_size_9,
    stat_chkpt_size_max = stat_chkpt_size_9,

    /* add stats before this line */
    stat_count,
};

struct syzkaller_execute_reply {
    uint32_t magic;
    uint32_t done;
    uint32_t status;
    uint32_t stats[stat_count];
};

enum {
    SYZKALLER_HC_ROOT_CHKPT          = 5,
    SYZKALLER_HC_RECV_EXEC,         // 6
    SYZKALLER_HC_REPLY_EXEC,        // 7
    SYZKALLER_HC_RECV_HANDSHAKE,    // 8
    SYZKALLER_HC_REPLY_HANDSHAKE,   // 9
    SYZKALLER_HC_MAYBE_CHKPT,       // 10
    SYZKALLER_HC_FORKSRV_CTX,       // 11
    AGAMOTTO_DEBUG_HC_BENCHMARK,    // 12
    AGAMOTTO_DEBUG_HC_NEXT,         // 13
    AGAMOTTO_DEBUG_HC_VMSYNC,       // 14
    AGAMOTTO_DEBUG_HC_CLOCK_SCALE,	// 15
	MONETA_DEBUG_HC_SAVEVM,        // 16
	MONETA_DEBUG_HC_CHKCHKPT,      // 17
	MONETA_DEBUG_HC_GPA2HPA,       // 18
};

void syzkaller_reply_handshake(void);
void syzkaller_reply_execute_crash(void);
void syzkaller_reply_execute(uint32_t);
void syzkaller_receive_handshake(CPUState *, uint64_t);
void syzkaller_receive_execute(CPUState *, uint64_t);

uint64_t syzkaller_maybe_checkpoint(uint64_t, uint64_t);

void syzkaller_submit_forkserver_context(uint64_t);

#define PERISCOPE_INPUT_FOREACH(list, var)          \
        for ((var) = (list);                        \
            (var);                                  \
            (var) = ((var)->next))

#define TYPE_FUZZER "fuzzer"

#define FUZZER_CLASS(klass) \
    OBJECT_CLASS_CHECK(FuzzerClass, (klass), TYPE_FUZZER)
#define FUZZER_OBJ(obj) \
    OBJECT_CHECK(FuzzerState, (obj), TYPE_FUZZER)
#define FUZZER_GET_CLASS(obj) \
    OBJECT_GET_CLASS(FuzzerClass, (obj), TYPE_FUZZER)

typedef struct FuzzerClass {
    /*< private >*/
    DeviceClass parent_class;
    /*< public >*/
} FuzzerClass;

enum {
    PERISCOPE_MODE_EXEC = 0,
    PERISCOPE_MODE_COVERAGE,
    PERISCOPE_MODE_AFL,
    PERISCOPE_MODE_SYZKALLER_USBFUZZER,
    PERISCOPE_MODE_PROFILER,
};

typedef struct FuzzerState {
    /*< private >*/
    DeviceState parent_obj;
    /*< public >*/
    char *uri;
    int st_pipe, ctl_pipe, shm_id;
    char *mgr_pipe;
    int chkpt_pool_size;
    int fuzzer_id;
#if 1
    HostMemoryBackend *hostmem[2];
#else
    IVShmemState *shmem[2];
#endif
    MemoryRegion *mr[2];

    QEMUTimer *timer;
    QemuThread thread;

    periscope_input_desc *root;
    periscope_input_desc *input_queue;

    QCAState *dev;

    // candidate
    periscope_input_desc *cur_input;
    periscope_cp_desc *cur_cp;
    bool irq_pending;

    int mode;

    // fuzzer backend must initialize the following fptrs
    int (*mmio_read)(unsigned, uint64_t *);
    char *(*fetch_next)(uint32_t *);
    char *(*get_cur)(uint32_t *);
    void (*cur_executed)(uint8_t *, uint32_t, uint64_t, bool);
    void (*restored)(void);
    bool (*should_restore)(void);
    int (*guest_crashed)(void);
    bool (*get_stat)(int, uint32_t *);

    uint8_t (*get_queue_cur_info)(void);
} FuzzerState;

FuzzerState *fuzzer_get_current(void);
bool should_restore_at_agent_exit(uint64_t);
bool should_shutdown_at_agent_exit(uint64_t);
bool periscope_should_suspend_on_shutdown(void);

extern bool snapshot_inited;

#define PERISCOPE_GUEST_SUSPEND -1
int periscope_guest_crashed(void);

void periscope_start_fuzzer(const char *uri, Error **errp);

void start_syzkaller_fuzzer(const char *, int, int, const char *, int, Error **);

void start_profiler(int, Error **);

void start_afl_fuzzer(const char *pipes, int, int, int, Error **errp);

void start_input_executor(const char *uri, Error **errp);

void start_coverage_collector(const char *args, Error **errp);

void periscope_fetch_next_input(void);
#endif
