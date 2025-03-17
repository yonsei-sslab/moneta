#include "qemu/osdep.h"

#include "hw/boards.h"
#include "qemu/cutils.h"
#include "qapi/error.h"
#include "qemu/error-report.h"

#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>

#include "hw/pci/pci.h"
#include "qemu-common.h"
#include "block/snapshot.h"
#include "migration/snapshot.h"
#include "migration/global_state.h"
#include "hw/hw.h"
#include "sysemu/cpus.h"
#include "sysemu/sysemu.h"
#include "qemu/rcu.h"
#include "qemu-file-channel.h"
#include "io/channel-file.h"
#include "io/channel-buffer.h"
#include "io/channel-util.h"
#include "migration/migration.h"
#include "migration/savevm.h"
#include "migration/qemu-file.h"
#include "migration/ram.h"

#include "trace.h"
#include "periscope.h"
#include "hw/periscope/kcov_vdev.h"
#include "hw/periscope/pci.h"

#include "cpu.h"
#include "exec/ram_addr.h"
#include "exec/address-spaces.h"
#include "migration/periscope_perf_switches.h"
#include "migration/periscope-timers.h"
#include "migration/periscope-delta-snap.h"

#include "migration/moneta.h"

static QemuMutex agamotto_lock;

// see comments in snaphost
// #include "migration/periscope_perf_switches.h"
bool quick_snapshot;
bool quick_reset_devs;
bool quick_reset_ram;

// This will disable storing of all ram sections
// not individual pages but the whole section
// it is only checked in migration/savevm.c
bool dev_only_snapshot = false;

bool periscope_no_loadvm_state_setup;
bool periscope_no_loadvm_state_cleanup;

bool snapshot_inited = false;
// enable only creation of root checkpoint
static bool root_only_chkpt = false;
static bool no_restore = false;

static bool single_exec = false;

static uint32_t max_used_len = 0;
static uint32_t max_irqs = 0;
static uint32_t max_io_reads = 0;
static qemu_timeval max_exec_time = {0, 0};
//static qemu_timeval max_restore_time = {0, 0};
static qemu_timeval last_restore_time = {0, 0};

static int total_execs = 0;
int periscope_total_execs(void)
{
    return total_execs;
}

static int total_non_root_restores = 0;
static int total_chkpts = 0;

static qemu_timeval total_exec_time = {0, 0};
static qemu_timeval total_restore_time = {0, 0};
static qemu_timeval total_chkpt_time = {0, 0};
static qemu_timeval total_chkpt_saving_time = {0, 0};

static void print_time_consumption_statistics(void) {
    uint64_t exec_ms = total_exec_time.tv_sec * 1000L + total_exec_time.tv_usec / 1000L;
    uint64_t restore_ms = total_restore_time.tv_sec * 1000L + total_restore_time.tv_usec / 1000L;
    uint64_t chkpt_ms = total_chkpt_time.tv_sec * 1000L + total_chkpt_time.tv_usec / 1000L;
    uint64_t sum_ms = exec_ms + restore_ms + chkpt_ms;
    uint64_t chkpt_saving_ms = total_chkpt_saving_time.tv_sec * 1000L + total_chkpt_saving_time.tv_usec / 1000L;

    printf("periscope: %d execs %lu ms (%.1f%%), %d restores (%d non-root) %lu ms (%.1f%%), %d chkpts %lu ms (%.1f%%) to save %lu ms\n",
           total_execs, exec_ms, (float)exec_ms/sum_ms*100,
           total_execs, total_non_root_restores, restore_ms, (float)restore_ms/sum_ms*100,
           total_chkpts, chkpt_ms, (float)chkpt_ms/sum_ms*100, chkpt_saving_ms);
}

#define SNAPSHOT_INVALID 0
#define SNAPSHOT_ROOT_ID 1
#define MAX_SNAPSHOT_ID INT_MAX

// TODO: Exponential/Linear/Adaptive back-off to handle
// drivers putting itself on a sleep?
#define MMIO_RESPONSE_TIMEOUT 100 // ms

// We want to give the guest OS some time to schedule
// whatever bottom-half handler the driver uses to process
// an interrupt in full.
#define INTERRUPT_RESPONSE_TIMEOUT 100 // ms

enum TimeoutReason {
    TIMEOUT_UNKNOWN = -1,
    TIMEOUT_RESTORE,
    TIMEOUT_CHECKPOINT,
    TIMEOUT_MMIO,
    TIMEOUT_INTERRUPT,
};

static int timed_out = 0;

static int timeout_reason = TIMEOUT_UNKNOWN;

static int next_snapshot_id = SNAPSHOT_ROOT_ID;

static periscope_cp_desc cp_root = {
    .len = 0,
    .n_children = 0,
    .parent = NULL, // root does not have a parent
    .closed = false, // root will be closed at the first checkpoint
    .snapshot = {
        .id = SNAPSHOT_ROOT_ID,
#ifndef PERI_DEDUP
        .memfd = -1,
        .memfd_quick = -1,
        .memfd_ram = -1,
        .buf_quick = NULL,
        .buf_ram = NULL,
        .dirty = NULL,
        .npages = 0,
#else
        .buf_dev = NULL,
        .dev_sz = 0,
        .peri_rb = NULL,
        .n_peri_rb = 0,
#endif

    }
};

static periscope_input_desc root = {
    .input = NULL,
    .len = 0,
    .used_len = 0,
    .num_irqs = 0,
    .num_io_reads = 0,
    .base_cp = NULL,
};

static int __periscope_purge_and_checkpoint_request(bool do_request);
#ifdef ENABLE_LW_CHKPT
static int close_cp_desc(periscope_cp_desc *cpd, uint16_t snapid, int memfd, int memfd_quick, unsigned long dirty[], unsigned long npages);
#else
static int close_cp_desc(periscope_cp_desc *cpd, uint16_t snapid, int memfd, int memfd_quick);
#endif
static int delete_cp_desc(periscope_cp_desc *cpd);
static periscope_cp_desc *create_cp_desc(periscope_cp_desc *parent);

static uint32_t mlen(char* a, char *b, uint32_t a_len, uint32_t b_len) {
    uint32_t i;
    for(i=0; i<a_len && i<b_len; ++i) {
        if(a[i] != b[i])
            return i;
    }
    return i;
}

static periscope_cp_desc *get_longest_match(char *ref, uint32_t reflen,
                                            uint32_t *matched_len,
                                            periscope_cp_desc *node)
{
    uint32_t ml_node = mlen(node->io, ref, node->len, reflen);

    // if there is no full match, no need to look at children
    if (ml_node != node->len) {
        *matched_len = 0;
        return NULL;
    }

    // if there are no children, return the full match
    if (node->n_children == 0) {
        *matched_len = ml_node;
        return node;
    }

    // search children
    uint32_t ml_child;
    periscope_cp_desc *child;
    periscope_cp_desc *match;
    for (int i = 0; i < node->n_children; ++i) {
        child = node->children[i];
        assert(child != NULL);

        match = get_longest_match(&ref[ml_node], reflen - ml_node, &ml_child,
                                  child);
        if (match != NULL) {
            break;
        }
    }

    // no match found, return current node
    if (match == NULL) {
        *matched_len = ml_node;
        return node;
    }

    *matched_len = ml_node + ml_child;

    return match;
}

//#define FULL_DELTA_RESTORE
//#define FULL_SNAPSHOT_RESTORE

static periscope_cp_desc *periscope_find_closest_snapshot(
    char *next, uint32_t next_len, periscope_input_desc *cur, uint32_t *matched_len) {

    assert(next != NULL);
    assert(cur != NULL);

    FuzzerState *s = fuzzer_get_current();
    assert(s != NULL);
    assert(s->root != NULL);

    periscope_cp_desc* max_cp = get_longest_match(next, next_len, matched_len, &cp_root);

#define TRACE_SNAPSHOT_SEARCH
#undef TRACE_SNAPSHOT_SEARCH

#ifdef TRACE_SNAPSHOT_SEARCH
    printf("Best match %p snap id %d, len %d\n", max_cp, max_cp->snapshot.id, *matched_len);
#endif

    assert(max_cp->snapshot.id > 0);

    // Mark this checkpoint and its parents used.
    periscope_cp_desc* cp = max_cp;
    while (cp != NULL) {
        cp->num_restored++;
        qemu_gettimeofday(&cp->last_restored);
        cp = cp->parent;
    }

#ifdef FULL_SNAPSHOT_RESTORE
    return &cp_root;
#else
    return max_cp;
#endif
}

#if 0
#ifdef ENABLE_LW_CHKPT
static int periscope_ram_checkpoint(char *name, size_t *sz) {
    int ret;
    Error *err = NULL;
    int memfd = memfd_create(name, 0);
    int dupfd = dup(memfd);
    QIOChannelFile *iochannel = qio_channel_file_new_fd(dupfd);
    QEMUFile *file = qemu_fopen_channel_output(QIO_CHANNEL(iochannel));
    object_unref(OBJECT(iochannel));

    periscope_save_ram_only = true;
    ret = qemu_savevm_state(file, &err);
    periscope_save_ram_only = false;

    if(ret != 0) {
      printf("%s: ERR %d\n", __FUNCTION__, ret);
      return -1;
    }
    qemu_fflush(file);
    if (sz) {
        *sz = qemu_ftell(file);
        printf("periscope: ram chkpt sz=%lu\n", *sz);
    }
    qemu_fclose(file);
    return memfd;
}
#endif

static int periscope_quick_checkpoint(char *name, size_t *sz) {
    int ret;
    Error *err = NULL;
    int memfd = memfd_create(name, 0);
    int dupfd = dup(memfd);
    QIOChannelFile *iochannel = qio_channel_file_new_fd(dupfd);
    QEMUFile *file = qemu_fopen_channel_output(QIO_CHANNEL(iochannel));
    object_unref(OBJECT(iochannel));

    global_state_store_running();
    quick_snapshot = true;
    ret = qemu_savevm_state(file, &err);
    quick_snapshot = false;

    if(ret != 0) {
      printf("%s: ERR %d\n", __FUNCTION__, ret);
      return -1;
    }
    qemu_fflush(file);
    if (sz) {
        *sz = qemu_ftell(file);
        printf("periscope: dev chkpt sz=%lu\n", *sz);
    }
    qemu_fclose(file);
    return memfd;
}
#endif

#ifdef PERI_DEDUP
static uint8_t *periscope_quick_checkpoint(char *name, size_t *sz) {
    int ret;
    Error *err = NULL;
    QIOChannelBuffer *iochannel_quick = qio_channel_buffer_new(32*1024*1024);
    assert(iochannel_quick);
    QEMUFile *file_quick = qemu_fopen_channel_output(QIO_CHANNEL(iochannel_quick));
    assert(file_quick);
    object_unref(OBJECT(iochannel_quick));

    //int memfd_ram = -1;
    //QIOChannelBuffer *iochannel_ram = qio_channel_buffer_new(8000*TARGET_PAGE_SIZE+32*1024*1024);
    //QEMUFile *file_ram = qemu_fopen_channel_output(QIO_CHANNEL(iochannel_ram));
    //object_unref(OBJECT(iochannel_ram));


    global_state_store_running();
    // This will disable storing of all ram sections
    // not individual pages but the whole section
    // it is only checked in migration/savevm.c
    dev_only_snapshot = true;
    //ret = qemu_savevm_state(file_quick, &err);
    ret = periscope_qemu_savevm_state(NULL, file_quick, &err);
    dev_only_snapshot = false;

    if(ret != 0) {
      printf("%s: ERR %d\n", __FUNCTION__, ret);
      return NULL;
    }

    qemu_fflush(file_quick);
    size_t file_quick_sz = qemu_ftell(file_quick);
    printf("periscope: dev checkpoint %lu KiB\n", file_quick_sz/1024UL);
    if (sz) {
        *sz = file_quick_sz;
    }

    uint8_t *buf_quick = g_realloc(qio_channel_buffer_close_without_free(iochannel_quick), file_quick_sz);
    iochannel_quick = NULL;
    qemu_fclose(file_quick);
    file_quick = NULL;

    return buf_quick;
}
#endif


static uint64_t last_chkpt_fixed_sz = 0;
static uint64_t chkpt_memory_used = 0UL;

static uint64_t compute_memory_cost(periscope_cp_desc *cp)
{
    assert(cp != NULL);
    uint64_t cost_kb = cp->len/1024UL;
#ifdef PERI_DEDUP
    cost_kb += cp->snapshot.dev_sz/1024UL;
    cost_kb += compute_prb_cost(cp->snapshot.peri_rb, cp->snapshot.n_peri_rb)/1024UL;
#else /* PERI_DEDUP */
    cost_kb += cp->snapshot.quick_sz/1024UL;
    cost_kb += cp->snapshot.ram_sz/1024UL;
#ifdef DBG_RAM_STORED // set in periscope.h
    cost_kb += (cp->snapshot.rambkp_size / 1024UL);
#endif
#endif
    return cost_kb;
}

#ifdef DBG_RAM_STORED // set in periscope.h
static unsigned long copy_rambkp(void **rambkp) {
   rcu_read_lock();
   RAMBlock *rb = qemu_ram_block_by_name("pc.ram");
   assert(rb);
   unsigned long rambkp_size = rb->max_length;
   if(*rambkp == NULL) {
      *rambkp = g_malloc(rambkp_size);
   }
   assert(*rambkp);
   memcpy(*rambkp, rb->host, rambkp_size);
   rcu_read_unlock();
   return rambkp_size;
}

static bool compare_ram_pages(void* ram0, void *ram1,
      unsigned long mem_size,
      unsigned long* dirty,
      bool dirty_only) {
    //bool no_diff = true;
    unsigned long n_diff = 0;
    rcu_read_lock();
    if(ram1 == NULL || ram0 == NULL) {
       RAMBlock *rb = qemu_ram_block_by_name("pc.ram");
       assert(rb);
       if(ram1 == NULL) ram1 = rb->host;
       else if(ram0 == NULL) ram0 = rb->host;
       assert(mem_size == rb->max_length);
    }
    assert(ram1);
    assert(ram0);

    for(unsigned long addr=0; addr < mem_size; addr+=TARGET_PAGE_SIZE) {
       bool is_dirty = dirty != NULL && test_bit(addr >> TARGET_PAGE_BITS, dirty) != 0;
       if(dirty_only && !is_dirty) continue;
       if(memcmp(ram0 + addr, ram1 + addr, TARGET_PAGE_SIZE) != 0) {
          printf("DIFF %d %lx\n", is_dirty, addr);
          //no_diff = false;
          n_diff++;
       }
    }
    rcu_read_unlock();
    //return no_diff;
    printf("#diff pages %ld\n", n_diff);
    // There are always changes due to wall clock
    return n_diff < 4;
}
#endif

int periscope_checkpoint(uint64_t id) {
    int ret;

    char pt_mark_buf[256];

    snprintf(pt_mark_buf, 256, "## %s\n", __FUNCTION__);
    pt_mark_all(pt_mark_buf);

    if (id >= MAX_SNAPSHOT_ID) {
        printf("periscope: too many snapshots > %d\n", MAX_SNAPSHOT_ID);
        return -1;
    }

    FuzzerState *s = fuzzer_get_current();
    assert(s != NULL);

#ifdef DBG_RAM_STORED // set in periscope.h
    if(s->cur_cp->parent) {
       unsigned long *dirtytmp;
       unsigned long npagestmp = get_current_delta_bm(&dirtytmp);
       unsigned long *inv_dirty = bitmap_new(npagestmp);
       bitmap_complement(inv_dirty, dirtytmp, npagestmp);
       printf("---------- comparing all non dirty pages (%ld) pc.ram to last restored %d\n",
             bitmap_count_one(inv_dirty, npagestmp), s->cur_cp->parent->snapshot.id);
       assert(compare_ram_pages(s->cur_cp->parent->snapshot.rambkp, NULL,
                s->cur_cp->parent->snapshot.rambkp_size, inv_dirty, true));
       g_free(inv_dirty);
       g_free(dirtytmp);
    }
#endif

    char name[50];
    char qname[50];
    char ramname[50];
    switch (s->mode) {
    case PERISCOPE_MODE_AFL:
        sprintf(name, "periscope-%ld", id);
        sprintf(qname, "periscope-q-%ld", id);
        sprintf(ramname, "periscope-r-%ld", id);
        break;
    case PERISCOPE_MODE_COVERAGE:
        sprintf(name, "periscope-cov");
        sprintf(qname, "periscope-cov-q");
        sprintf(ramname, "periscope-cov-r");
        break;
    case PERISCOPE_MODE_SYZKALLER_USBFUZZER:
        sprintf(name, "moneta");
        sprintf(qname, "periscope-syz-q-%ld", id);
        sprintf(ramname, "periscope-syz-r-%ld", id);
        break;
    default:
        printf("periscope: unknown mode\n");
        sprintf(name, "periscope-unknown");
        break;
    }

    qemu_timeval chkpt_begin, chkpt_end;
    qemu_gettimeofday(&chkpt_begin);

#ifdef ENABLE_LW_CHKPT
    unsigned long *dirty = NULL;
    unsigned long npages = 0;
#endif

    if (id == SNAPSHOT_ROOT_ID) {
        BlockDriverState *bs;
        printf("periscope: searching for snapshot %s\n", name);
        ret = bdrv_all_find_snapshot(name, &bs);

        if (ret == 0) {
//            bdrv_all_delete_snapshot(name, &bs, &err);
        }

        //if (ret < 0) {
            printf("periscope: checkpointing %s (bdrv)...\n", name);

            // qemu_timeval tv1, tv2;
            // qemu_gettimeofday(&tv1);

            // ret = save_snapshot(name, &err);
            // if (ret < 0) {
            //     printf("periscope: save snapshot failed\n");
            // }

            // qemu_gettimeofday(&tv2);
            // tv2.tv_sec -= tv1.tv_sec;
            // tv2.tv_usec -= tv1.tv_usec;
        //}

#ifdef ENABLE_LW_CHKPT
#ifndef PERI_DEDUP
        MachineState *machine = MACHINE(qdev_get_machine());
        assert(machine != NULL);
        npages = machine->ram_size / TARGET_PAGE_SIZE;
        dirty = bitmap_new(npages);
        bitmap_fill(dirty, npages); // root snapshot has to be reset completely
        update_and_clear_delta_snap_bm(NULL);
    } else {
        //npages = get_current_delta_bm(&dirty);
        npages = update_and_clear_delta_snap_bm(&dirty);
#endif /* PERI_DEDUP */
#endif /* ENABLE_LW_CHKPT */
    }

#ifndef ENABLE_LW_CHKPT
    peri_timer *pt = NULL;
    pt = start_interval("periscope_checkpoint.timer");
    ret = qemu_savevm_state(file, &err);
    stop_interval(pt);
    //ret = qemu_savevm_state_live(file, &err);
    qemu_fflush(file);
    qemu_fclose(file);
    file = NULL;

#else /* ENABLE_LW_CHKPT */
#ifdef PERI_DEDUP
    int memfd_ram = -1;
    int memfd_quick = -1;
    unsigned long num_dirty_pages = 0;
    size_t dev_sz = 0;

    s->cur_cp->snapshot.n_peri_rb = create_prb_and_fill(&s->cur_cp->snapshot.peri_rb, &num_dirty_pages, id, true);

    uint8_t *buf_dev = periscope_quick_checkpoint(qname, &dev_sz);
    assert(buf_dev);
    assert(dev_sz > 0);
    s->cur_cp->snapshot.buf_dev = buf_dev;
    s->cur_cp->snapshot.dev_sz = dev_sz;
#endif /* ENABLE_LW_CHKPT */
#endif /* PERI_DEDUP */

    qemu_gettimeofday(&chkpt_end);

    qemu_timeval chkpt_time;
    timersub(&chkpt_end, &chkpt_begin, &chkpt_time);
    // stat
    timeradd(&chkpt_time, &total_chkpt_time, &total_chkpt_time);

    total_chkpts += 1;

    if (ret < 0) {
        printf("periscope: save vm state failed ret=%d\n", ret);
        return ret;
    }

    if (id == SNAPSHOT_ROOT_ID) {
        snapshot_inited = true;
    }

#ifdef ENABLE_LW_CHKPT
    if (close_cp_desc(s->cur_cp, id, memfd_ram, memfd_quick, dirty, npages) != 0) {
#endif
        printf("periscope: failed to close checkpoint\n");
        return -1;
    }

    // update checkpoint memory usage
    chkpt_memory_used += compute_memory_cost(s->cur_cp);

    // approx. size of fixed memory usage
    last_chkpt_fixed_sz = 0;
    unsigned long prb_cost = compute_prb_cost(s->cur_cp->snapshot.peri_rb, s->cur_cp->snapshot.n_peri_rb);
    uint64_t dirty_cost = compute_dirty_cost(num_dirty_pages);
    if (prb_cost > dirty_cost) {
        last_chkpt_fixed_sz += prb_cost - dirty_cost;
    }
    last_chkpt_fixed_sz += dev_sz;

    // logging
    printf("periscope: checkpoint created (%lu ms): ",
           chkpt_time.tv_sec * 1000L + chkpt_time.tv_usec / 1000L);

    periscope_cp_desc *cp = s->cur_cp;
    while (cp != NULL) {
        printf("[#%u, %u B, %lu MiB, %lu ms, %u chld] ",
               cp->snapshot.id, cp->len, compute_memory_cost(cp) / 1024UL,
               cp->exec_time.tv_sec * 1000L + cp->exec_time.tv_usec / 1000L,
               cp->n_children);
        if (cp != &cp_root)
            printf("<- ");
        cp = cp->parent;
    }
    printf("\n");

    print_time_consumption_statistics();

    s->cur_input->base_cp = s->cur_cp;
    s->cur_cp = create_cp_desc(s->cur_cp);

    if (!periscope_restore_requested())
        vm_start();

    return 0;
}

void periscope_request_lock_init(void) {
    qemu_mutex_init(&agamotto_lock);
}

void periscope_request_lock(void) {
    qemu_mutex_lock(&agamotto_lock);
}

void periscope_request_unlock(void) {
    qemu_mutex_unlock(&agamotto_lock);
}

static int checkpoint_request = 0;

#define CHKPT_TESTING
#undef CHKPT_TESTING

static uint64_t calc_total_time_saved(periscope_cp_desc *cpd) {
    uint64_t exec_time_ms = 0;

    periscope_cp_desc *cp = cpd;
    while (cp != NULL) {
        exec_time_ms +=
            cp->exec_time.tv_sec * 1000L + cp->exec_time.tv_usec / 1000L;
        cp = NULL;
        // cp = cp->parent;
    }

    return cpd->num_restored * exec_time_ms;
}

static uint64_t get_exec_time_ms(periscope_cp_desc *cpd) {
    uint64_t exec_time_ms = 0;

    while (cpd != NULL) {
        exec_time_ms +=
            cpd->exec_time.tv_sec * 1000L + cpd->exec_time.tv_usec / 1000L;
        cpd = cpd->parent;
    }

    return exec_time_ms;
}

#define MAX_CANDIDATES 1024

typedef struct periscope_cp_candidates {
    periscope_cp_desc *cps[MAX_CANDIDATES];
    int num_cps;
} periscope_cp_candidates;

typedef struct periscope_cp_policy {
    const char *name;
    int (*find_replacement_candidates)(periscope_cp_candidates *);
} periscope_cp_policy;

#if 0
static int cp_compare_always_replace(periscope_cp_desc *old_cp,
                                     periscope_cp_desc *new_cp)
{
    return -1;
}

static periscope_cp_desc *cp_find_lowest_time_saver(periscope_cp_desc *cpd) {
    assert(cpd != NULL);

    if (cpd->n_children == 0) {
        return cpd;
    }

    uint64_t lowest_time_saved = UINT64_MAX;
    periscope_cp_desc *lowest_time_saver = NULL;

    // Lowest among children, which recursively return the lowest of their children
    for (int i=0; i<cpd->n_children; ++i) {
        periscope_cp_desc *child = cpd->children[i];
        child = cp_find_lowest_time_saver(child);
        uint64_t time_saved = calc_total_time_saved(child);
        if (time_saved <= lowest_time_saved) {
            lowest_time_saved = time_saved;
            lowest_time_saver = child;
        }
    }

    return lowest_time_saver;
}
#endif

static int cp_find_non_active_nodes(periscope_cp_candidates *candidates)
{
    assert(candidates);
    assert(candidates->num_cps > 0);

    FuzzerState *fs = fuzzer_get_current();
    assert(fs);
    assert(fs->cur_input);

    periscope_cp_desc *active = fs->cur_input->base_cp;
    assert(active);

    for (int i=0; i<candidates->num_cps; i++) {
        const periscope_cp_desc *cur = candidates->cps[i];
        assert(cur != NULL);

        if (cur == active) {
            candidates->cps[i] = candidates->cps[candidates->num_cps-1];
            candidates->num_cps--;
            candidates->cps[candidates->num_cps] = NULL;
            break;
        }
    }

    return candidates->num_cps;
}

static int cp_level(const periscope_cp_desc *cpd)
{
    int level = 0;
    while (cpd != &cp_root) {
        level++;
        cpd = cpd->parent;
    }
    return level;
}

static int cp_cmp_level(const void *a, const void *b)
{
    const periscope_cp_desc *const *cp_a = a;
    const periscope_cp_desc *const *cp_b = b;

    return cp_level(*cp_b) - cp_level(*cp_a);
}

static int cp_find_last_level_leafs(periscope_cp_candidates *candidates)
{
    if (candidates->num_cps > 1)
        qsort(candidates->cps, candidates->num_cps, sizeof(candidates->cps[0]),
              cp_cmp_level);

    for (int i=0; i<candidates->num_cps - 1; i++) {
        const periscope_cp_desc *cur = candidates->cps[i];
        assert(cur != NULL);
        const periscope_cp_desc *next = candidates->cps[i+1];
        assert(next != NULL);

        if (cp_level(cur) == cp_level(next)) {
            continue;
        }

        candidates->num_cps = i + 1;
    }

    return candidates->num_cps;
}

#ifdef EVICT_LRU_LAST_RESTORED
static int cp_cmp_last_restored(const void *a, const void *b)
{
    const periscope_cp_desc *const *cp_a = a;
    const periscope_cp_desc *const *cp_b = b;

    if (timercmp(&(*cp_a)->last_restored, &(*cp_b)->last_restored, <)) {
        return -1;
    }
    else if (timercmp(&(*cp_a)->last_restored, &(*cp_b)->last_restored, >)) {
        return 1;
    }
    return 0;
}
#else
static int cp_cmp_last_used(const void *a, const void *b)
{
    const periscope_cp_desc *const *cp_a = a;
    const periscope_cp_desc *const *cp_b = b;

    const qemu_timeval *last_used_a = &(*cp_a)->closed_time;
    if ( (*cp_a)->num_restored > 0 ) {
        last_used_a = &(*cp_a)->last_restored;
    }
    const qemu_timeval *last_used_b = &(*cp_b)->closed_time;
    if ( (*cp_a)->num_restored > 0 ) {
        last_used_b = &(*cp_b)->last_restored;
    }

    if (timercmp(last_used_a, last_used_b, <)) {
        return -1;
    }
    else if (timercmp(last_used_a, last_used_b, >)) {
        return 1;
    }
    return 0;
}
#endif

static int cp_find_least_recently_used(periscope_cp_candidates *candidates)
{
    assert(candidates);
    assert(candidates->num_cps > 0);

    if (candidates->num_cps > 1)
        qsort(candidates->cps, candidates->num_cps, sizeof(candidates->cps[0]),
#ifdef EVICT_LRU_LAST_RESTORED
              cp_cmp_last_restored
#else
              cp_cmp_last_used
#endif
        );

#if 0
    printf("periscope: LRU [#%d]", candidates->cps[0]->snapshot.id);
    for (int i=0; i<candidates->num_cps - 1; i++) {
        const periscope_cp_desc *cur = candidates->cps[i];
        assert(cur != NULL);
        const periscope_cp_desc *next = candidates->cps[i+1];
        assert(next != NULL);

        qemu_timeval sub;
        timersub(&next->last_restored, &cur->last_restored, &sub);
        printf(" [#%d] (%lu ms)", next->snapshot.id,
               sub.tv_sec * 1000UL + sub.tv_usec / 1000UL);

    }
    printf("\n");
#endif

    for (int i=0; i<candidates->num_cps - 1; i++) {
        const periscope_cp_desc *cur = candidates->cps[i];
        assert(cur != NULL);
        const periscope_cp_desc *next = candidates->cps[i+1];
        assert(next != NULL);

#ifdef EVICT_LRU_LAST_RESTORED
        if (timercmp(&cur->last_restored, &next->last_restored, ==)) {
#else
        const qemu_timeval *last_used_a = &cur->closed_time;
        if ( cur->num_restored > 0 ) {
            last_used_a = &cur->last_restored;
        }
        const qemu_timeval *last_used_b = &next->closed_time;
        if ( next->num_restored > 0 ) {
            last_used_b = &next->last_restored;
        }
        if (timercmp(last_used_a, last_used_b, ==)) {
#endif
            continue;
        }

        candidates->num_cps = i + 1;
    }

    return candidates->num_cps;
}

#ifdef EVICT_FIFO
static int cp_cmp_closed_time(const void *a, const void *b)
{
    const periscope_cp_desc *const *cp_a = a;
    const periscope_cp_desc *const *cp_b = b;

    if (timercmp(&(*cp_a)->closed_time, &(*cp_b)->closed_time, <)) {
        return -1;
    }
    else if (timercmp(&(*cp_a)->closed_time, &(*cp_b)->closed_time, >)) {
        return 1;
    }
    return 0;
}

static int cp_find_least_recently_created(periscope_cp_candidates *candidates)
{
    assert(candidates);
    assert(candidates->num_cps > 0);

    if (candidates->num_cps > 1)
        qsort(candidates->cps, candidates->num_cps, sizeof(candidates->cps[0]),
              cp_cmp_closed_time);

    for (int i=0; i<candidates->num_cps - 1; i++) {
        const periscope_cp_desc *cur = candidates->cps[i];
        assert(cur != NULL);
        const periscope_cp_desc *next = candidates->cps[i+1];
        assert(next != NULL);

        if (timercmp(&cur->closed_time, &next->closed_time, ==)) {
            continue;
        }

        candidates->num_cps = i + 1;

    }

    return candidates->num_cps;
}
#endif

#if defined(PERI_DEDUP) && !defined(PERI_DEDUP_NOHASH)
#define UNIQUENESS_PERC 0.85f
static int cmp_uniqueness(const void *a, const void *b) {
    const periscope_cp_desc *const *cp_a = a;
    const periscope_cp_desc *const *cp_b = b;
    //unsigned int ua = compute_prb_freed((*cp_a)->snapshot.peri_rb);
    //unsigned int ub = compute_prb_freed((*cp_b)->snapshot.peri_rb);
    //return ub - ua;
    //unsigned int ua = compute_uniqueness((*cp_a)->snapshot.peri_rb, (*cp_a)->snapshot.n_peri_rb);
    //unsigned int ub = compute_uniqueness((*cp_b)->snapshot.peri_rb, (*cp_b)->snapshot.n_peri_rb);
    float ua = (*cp_a)->snapshot.uniqueness;
    float ub = (*cp_b)->snapshot.uniqueness;
    return ub - ua;
}
static int cp_find_most_unique(periscope_cp_candidates *candidates)
{
    assert(candidates);
    assert(candidates->num_cps > 0);

    unsigned int min_c = 4;
    if(candidates->num_cps < min_c) return candidates->num_cps;
    //float *u = g_malloc(candidates->num_cps * sizeof(float));
    float min_u = 10000000.0f;
    for (int i=0; i<candidates->num_cps; i++) {
       float u = compute_uniqueness(candidates->cps[i]->snapshot.peri_rb, candidates->cps[i]->snapshot.n_peri_rb);
       candidates->cps[i]->snapshot.uniqueness = u;
       if(u < min_u) min_u = u;
    }

    qsort(candidates->cps, candidates->num_cps, sizeof(candidates->cps[0]),
          cmp_uniqueness);
    for (int i=0; i<candidates->num_cps; i++) {
        const periscope_cp_desc *cur = candidates->cps[i];
        assert(cur != NULL);
        if (candidates->cps[i]->snapshot.uniqueness > min_u * UNIQUENESS_PERC) {
            candidates->cps[i] = candidates->cps[candidates->num_cps-1];
            candidates->num_cps--;
            candidates->cps[candidates->num_cps] = NULL;
            if(candidates->num_cps < min_c) break;
        }
    }

    //for (int i=0; i<candidates->num_cps; i++) {
    //   printf("%s u[%d] = %f\n", __FUNCTION__, i, candidates->cps[i]->snapshot.uniqueness);
    //}
    return candidates->num_cps;
}
#endif

static periscope_cp_policy cp_policy_nac = {
    .name = "NAC",
    .find_replacement_candidates = cp_find_non_active_nodes,
};

static periscope_cp_policy cp_policy_lll = {
    .name = "LLL",
    .find_replacement_candidates = cp_find_last_level_leafs,
};

static periscope_cp_policy cp_policy_lru = {
    .name = "LRU",
    .find_replacement_candidates = cp_find_least_recently_used,
};

#ifdef EVICT_FIFO
static periscope_cp_policy cp_policy_fifo = {
    .name = "FIFO",
    .find_replacement_candidates = cp_find_least_recently_created,
};
#endif

#if defined(PERI_DEDUP) && !defined(PERI_DEDUP_NOHASH)
static periscope_cp_policy cp_policy_dedup = {
    .name = "DEDUP",
    .find_replacement_candidates = cp_find_most_unique,
};
#endif

static periscope_cp_policy *cp_policy_chain[] = {
#if defined(PERI_DEDUP) && !defined(PERI_DEDUP_NOHASH)
    &cp_policy_dedup,
#endif
    &cp_policy_nac,
    &cp_policy_lll,
    &cp_policy_lru,
#ifdef EVICT_FIFO
    &cp_policy_fifo,
#endif
    NULL,
};

static int num_evicted[sizeof(cp_policy_chain)/sizeof(cp_policy_chain[0])];

static void collect_all_leaf_cps(periscope_cp_desc *cpd,
                                 periscope_cp_candidates *candidates)
{
    assert(cpd != NULL);
    assert(cpd->closed);
    assert(candidates != NULL);

    // base case
    if (cpd->n_children == 0) {
        if (candidates->num_cps >= MAX_CANDIDATES) {
            printf("periscope: MAX_CANDIDATES too small\n");
            return;
        }
        candidates->cps[candidates->num_cps++] = cpd;
        return;
    }

    for (int i = 0; i < cpd->n_children; ++i) {
        periscope_cp_desc *child = cpd->children[i];
        assert(child != NULL);
        collect_all_leaf_cps(child, candidates);
    }
}

static int periscope_maybe_purge_checkpoints(periscope_cp_desc *cp_to_store,
                                             uint64_t cost)
{
    FuzzerState *fs = fuzzer_get_current();
    assert(fs);

    if (chkpt_memory_used + cost < fs->chkpt_pool_size * 1024) {
        return 0;
    }

    periscope_cp_candidates candidates;
    memset(&candidates, 0x0, sizeof(candidates));
    collect_all_leaf_cps(&cp_root, &candidates);
    printf("periscope: # of (initial) candidates = %d\n", candidates.num_cps);

    int i = 0;
    periscope_cp_desc* cp_to_purge = NULL;
    periscope_cp_policy *cp_policy = cp_policy_chain[i];

    while (candidates.num_cps > 0) {
        assert(cp_policy->find_replacement_candidates);

        if (cp_policy->find_replacement_candidates(&candidates) == 1) {
            // we found a single candidate
            printf("periscope: # of candidates = %d\n", candidates.num_cps);
            cp_to_purge = candidates.cps[0];
            num_evicted[i]++;
            break;
        }

        printf("periscope: # of candidates = %d\n", candidates.num_cps);

        if (cp_policy_chain[i+1] == NULL) {
            // no more policy exists.
            break;
        }

        i++;
        cp_policy = cp_policy_chain[i];
    }

    if (cp_to_purge == NULL) {
        printf("periscope: no policy finds good replacement candidate.\n");
        return -1;
    }

    assert(cp_policy != NULL);
    printf("periscope: policy-%s decides to replace checkpoint %d\n",
           cp_policy->name, cp_to_purge->snapshot.id);

    if (&cp_root == cp_to_purge) {
        printf("periscope: root checkpoint cannot be replaced.\n");
        return -2;
    }

    // TODO: probably lift this constraint later
    if (cp_to_purge->n_children > 0) {
        printf("periscope: checkpoint having any child cannot be replaced.\n");
        return -3;
    }

    // TODO: the following only works in conjunction with children number check
    // up there; we should later generalize this
    if (cp_to_store->parent == cp_to_purge) {
#if 1
        // let's not specialize the checkpoint, and leave it more general and
        // potentially more useful.
        printf("periscope: replacement candidate is new checkpoint's parent.\n");
        return -4;
#else
        printf("periscope: purging the parent of a new checkpoint\n");

        memcpy(cp_to_store->io + cp_to_purge->len, cp_to_store,
               cp_to_purge->len);
        memcpy(cp_to_store->io, cp_to_purge->io, cp_to_purge->len);
        cp_to_store->len += cp_to_purge->len;

        timeradd(&cp_to_purge->exec_time, &cp_to_store->exec_time,
                 &cp_to_store->exec_time);

        cp_to_store->parent = cp_to_purge->parent;
#endif
    }

    qemu_timeval now;
    qemu_timeval time_since_last_restore;
    qemu_gettimeofday(&now);
    qemu_timersub(&now, &cp_to_purge->last_restored, &time_since_last_restore);
    printf("periscope: purging checkpoint periscope-%u (%lu ms saved for %d "
           "restores, last restore %ld s ago)\n",
           cp_to_purge->snapshot.id, calc_total_time_saved(cp_to_purge),
           cp_to_purge->num_restored, time_since_last_restore.tv_sec);

    // Detach from the parent with lots of checks
    periscope_cp_desc *parent = cp_to_purge->parent;
    assert(parent != NULL);
    assert(parent->n_children > 0);
    bool found = false;
    for (int i = 0; i < parent->n_children; i++) {
        if (parent->children[i] == cp_to_purge) {
            parent->children[i] = parent->children[parent->n_children - 1];
            found = true;
            break;
        }
    }
    assert(found);
    parent->n_children--;
    cp_to_purge->parent = NULL;

    chkpt_memory_used -= compute_memory_cost(cp_to_purge);

    // Close all the associated file
#ifndef ENABLE_LW_CHKPT
    assert(cp_to_purge->snapshot.memfd > -1);
    close(cp_to_purge->snapshot.memfd);
    cp_to_purge->snapshot.memfd = -1;
#else /* ENABLE_LW_CHKPT */
#ifndef PERI_DEDUP
    if (cp_to_purge->snapshot.memfd_quick > -1) {
        close(cp_to_purge->snapshot.memfd_quick);
        cp_to_purge->snapshot.memfd_quick = -1;
    }

    if (cp_to_purge->snapshot.memfd_ram > -1) {
        close(cp_to_purge->snapshot.memfd_ram);
        cp_to_purge->snapshot.memfd_ram = -1;
    }

    if (cp_to_purge->snapshot.buf_quick != NULL) {
        g_free(cp_to_purge->snapshot.buf_quick);
        cp_to_purge->snapshot.buf_quick = NULL;
    }

    if (cp_to_purge->snapshot.buf_ram != NULL) {
        g_free(cp_to_purge->snapshot.buf_ram);
        cp_to_purge->snapshot.buf_ram = NULL;
    }

    if (cp_to_purge->snapshot.dirty) {
        g_free(cp_to_purge->snapshot.dirty);
        cp_to_purge->snapshot.dirty = NULL;
    }
#ifdef DBG_RAM_STORED
    if (cp_to_purge->snapshot.rambkp) {
        g_free(cp_to_purge->snapshot.rambkp);
        cp_to_purge->snapshot.rambkp = NULL;
    }
    cp_to_purge->snapshot.rambkp_size = 0;
#endif
#else /*PERI_DEDUP */
    if(cp_to_purge->snapshot.peri_rb) {
       delete_peri_rbs(
             cp_to_purge->snapshot.peri_rb,
             cp_to_purge->snapshot.n_peri_rb,
             parent->snapshot.peri_rb);
       g_free(cp_to_purge->snapshot.peri_rb);
       cp_to_purge->snapshot.peri_rb = NULL;
       cp_to_purge->snapshot.n_peri_rb = 0;
    }
    if (cp_to_purge->snapshot.buf_dev != NULL) {
        g_free(cp_to_purge->snapshot.buf_dev);
        cp_to_purge->snapshot.buf_dev = NULL;
        cp_to_purge->snapshot.dev_sz = 0;
    }
#endif /*PERI_DEDUP */
#endif /*ENABLE_LW_CHKPT*/
    cp_to_purge->snapshot.id = SNAPSHOT_INVALID;

    // Delete the descriptor itself
    free(cp_to_purge);
    cp_to_purge = NULL;

    return 0;
}

void periscope_checkpoint_request(void) {
    vm_stop(RUN_STATE_SAVE_VM);

    FuzzerState *s = fuzzer_get_current();
    assert(s != NULL);

    // printf("periscope: checkpoint request\n");

    // timeout for checkpoint
    timer_mod(s->timer, qemu_clock_get_ms(QEMU_CLOCK_HOST) + 5000000);
    timeout_reason = TIMEOUT_CHECKPOINT;

    checkpoint_request = next_snapshot_id++;
}

static qemu_timeval tv_restore_end = {0, 0};

bool periscope_snapshot_inited(void) {
    return snapshot_inited;
}

static int __periscope_purge_and_checkpoint_request(bool do_request) {
    // approx. memory cost in KiB
    FuzzerState *s = fuzzer_get_current();
    uint64_t input_cost = (s->cur_input->used_len + 1024U) / 1024U;
#ifndef PERI_DEDUP
    unsigned long *dirty = NULL;
    unsigned long num_pages = get_current_delta_bm(&dirty);
    assert(dirty != NULL);
    unsigned long num_dirty_pages = bitmap_count_one(dirty, num_pages);
    g_free(dirty);

    uint64_t dirty_cost = num_dirty_pages * TARGET_PAGE_SIZE / 1024U;
    uint64_t fixed_cost = last_chkpt_fixed_sz / 1024U;
#else /*PERI_DEDUP */
    unsigned long num_dirty_pages = count_dirty_pages();
    uint64_t dirty_cost = compute_dirty_cost(num_dirty_pages) / 1024U;
    uint64_t fixed_cost = last_chkpt_fixed_sz / 1024U;
#ifndef PERI_DEDUP_NOHASH
    unsigned long num_unique_pages = get_max_new_pages();
    fixed_cost += (get_ram_page_pool_size() +
          get_rpp_hashmap_size()) / 1024U;
#endif
#endif

    uint64_t mem_cost = input_cost + dirty_cost + fixed_cost;

    // TODO: needed for profiling!
    while (chkpt_memory_used + mem_cost > s->chkpt_pool_size * 1024) {
    int reject_reason = periscope_maybe_purge_checkpoints(s->cur_cp, mem_cost);
    if (reject_reason != 0) {
#ifndef CHKPT_TESTING
       printf("periscope: checkpoint request (%lu+%lu+%lu=%lu KiB) rejected for reason %d.\n",
             input_cost, dirty_cost, fixed_cost, mem_cost,
             reject_reason);
#endif
       return -1;
    }
    }

#if defined(PERI_DEDUP) && !defined(PERI_DEDUP_NOHASH)
    long freed = 0;
    while(get_free_pages() < num_unique_pages || !do_request) {
       long free = ((long)s->chkpt_pool_size * 1024L - ((long)chkpt_memory_used + mem_cost)) * 1024L - freed;
       if(free > TARGET_PAGE_SIZE * 512) {
          if(grow_ram_page_pool(free/8) == 0) {
             freed += free/8;
          } else {
             freed = (long)s->chkpt_pool_size * 1024L; // to terminate
          }
          if(!do_request) break;
          continue;
       }
       int reject_reason = periscope_maybe_purge_checkpoints(s->cur_cp, s->chkpt_pool_size * 1024);
       if (reject_reason != 0) {
          printf("periscope: checkpoint request (%lu+%lu+%lu=%lu KiB) rejected for reason %d.\n",
                input_cost, dirty_cost, fixed_cost, mem_cost,
                reject_reason);
          return -1;
       }
       // TODO
       if(!do_request) break;
    }
#endif


    if(do_request) {
       printf("periscope: requesting checkpoint (approx. %lu+%lu+%lu=%lu KiB)...\n",
             input_cost, dirty_cost, fixed_cost, mem_cost);

       periscope_checkpoint_request();
    }

    return 0;
}
int periscope_purge_and_checkpoint_request(void) {
   return __periscope_purge_and_checkpoint_request(true);
}

static int chkpt_disable_after_nth = 0; // zero-indexed

#define CHKPT_TIME_THRESHOLD_MS 50
#define CHKPT_TIME_THRESHOLD_MULTIPLIER 2

static uint64_t chkpt_time_threshold_ms = CHKPT_TIME_THRESHOLD_MS;

static uint64_t last_chkpt_ms = 0;
static int chkpt_policy = PERISCOPE_CHKPT_TIME_ONLY; // default policy

int periscope_change_chkpt_policy(int pol, int param1)
{
    if (pol < 0 || pol >= PERISCOPE_CHKPT_MAX) {
        return -1;
    }

    if (chkpt_policy == PERISCOPE_CHKPT_TIME_ONLY_DISABLED_AFTER_NTH) {
        if (param1 < 0)
            return -1;
        chkpt_disable_after_nth = param1;
    }

    chkpt_policy = pol;

    return 0;
}

int periscope_maybe_checkpoint_request(void) {
    if (unlikely(!snapshot_inited)) {
        periscope_checkpoint_request();
        snapshot_inited = true;
        return 0;
    }
    if (next_snapshot_id != SNAPSHOT_ROOT_ID && root_only_chkpt) {
      printf("periscope: only root checkpoints allowed\n");
      return -1;
    }

    FuzzerState *s = fuzzer_get_current();
    assert(s != NULL);
    assert(s->cur_input != NULL);
    assert(s->cur_cp != NULL);

    if (s->cur_cp->len == 0) {
        return -1;
    }

    uint64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    uint64_t time_since_last_chkpt_ms = now - last_chkpt_ms;

    printf(
        "periscope: chkpt requested after %ld ms (used=%d, threshold=%ld ms)\n",
        time_since_last_chkpt_ms, s->cur_input->used_len,
        chkpt_time_threshold_ms);

    s->cur_cp->exec_time.tv_sec = time_since_last_chkpt_ms / 1000UL;
    s->cur_cp->exec_time.tv_usec =
        (time_since_last_chkpt_ms % 1000UL) * 1000UL;

    qemu_timeval elapsed; // in vm time
    memcpy(&elapsed, &s->cur_cp->exec_time, sizeof(qemu_timeval));
    timeradd(&elapsed, &total_exec_time, &total_exec_time);
    if (timercmp(&elapsed, &max_exec_time, >)) {
        printf("periscope: new max exec time %lu ms (%u/%u bytes, %u irqs, %u io reads)\n",
               elapsed.tv_sec * 1000L + elapsed.tv_usec / 1000L,
               s->cur_input->used_len,
               s->cur_input->len,
               s->cur_input->num_irqs,
               s->cur_input->num_io_reads);
        memcpy(&max_exec_time, &elapsed, sizeof(qemu_timeval));

        print_time_consumption_statistics();
    }

    bool should_chkpt = true;

    switch (chkpt_policy) {
    case PERISCOPE_CHKPT_TIME_ONLY:
        should_chkpt &= (time_since_last_chkpt_ms > chkpt_time_threshold_ms);
        break;
    case PERISCOPE_CHKPT_DISABLED:
        should_chkpt = false;
        break;
    case PERISCOPE_CHKPT_TIME_ONLY_DISABLED_AFTER_NTH:
        if (chkpt_disable_after_nth + 1 >= s->cur_input->used_len) {
            should_chkpt &= (time_since_last_chkpt_ms > chkpt_time_threshold_ms);
        } else {
            printf("periscope: chkpt disabled at %d > %d\n",
                   s->cur_input->used_len - 1, chkpt_disable_after_nth);
            should_chkpt = false;
        }
        break;
    default:
        printf("periscope: unknown checkpoint policy\n");
        break;
    }

    if (should_chkpt == true) {
        printf("periscope: policy %d requests checkpoint\n", chkpt_policy);

        if (periscope_purge_and_checkpoint_request() == 0) {
#ifndef CHKPT_TESTING
        printf("periscope: last input (%u/%u B, %u irqs, %u io reads) took too much time (%lu ms).\n",
                s->cur_input->used_len,
                s->cur_input->len,
                s->cur_input->num_irqs,
                s->cur_input->num_io_reads,
                elapsed.tv_sec * 1000L + elapsed.tv_usec / 1000L);
#endif

            // reset chkpt counter
            last_chkpt_ms = now;

            chkpt_time_threshold_ms *= CHKPT_TIME_THRESHOLD_MULTIPLIER;
        }
    }
    else {
#ifdef TRACE_CHECKPOINT_POLICY
        printf("periscope: elapsed.tv_usec %8ld < %8ld\n",
               elapsed.tv_usec,
               CHKPT_TIME_THRESHOLD);
#endif
    }

    return -1;
}

static int restore_request = 0;

static periscope_input_desc *periscope_next_input(void);

#define TRACE_LCA
#undef TRACE_LCA
static periscope_cp_desc *find_lowest_common_ancestor(periscope_cp_desc *a,
                                                      periscope_cp_desc *b)
{
    assert(a != NULL);
    assert(b != NULL);

    int depth_a = 0;
    periscope_cp_desc *tmp = a;
    while (tmp->parent) {
        tmp = tmp->parent;
        depth_a++;
    }

    int depth_b = 0;
    tmp = b;
    while (tmp->parent) {
        tmp = tmp->parent;
        depth_b++;
    }

    while (depth_a > depth_b) {
        depth_a--;
        a = a->parent;
    }
    while (depth_b > depth_a) {
        depth_b--;
        b = b->parent;
    }

    while (a != b) {
        b = b->parent;
        a = a->parent;
    }

    assert(a != NULL);
    assert(b != NULL);
    assert(a == b);

#ifdef TRACE_LCA
    if (a != &cp_root) {
        printf("periscope: non-root lowest common ancestor id=%d\n", a->snapshot.id);
    }
#endif

    return a;
}

#ifdef ENABLE_LW_CHKPT
#define TRACE_DELTA_RESTORE
//#undef TRACE_DELTA_RESTORE

//static bool first_restore = true;

static bool suspended = false;

int periscope_guest_crashed(void)
{
    FuzzerState *fs = fuzzer_get_current();
    if (fs && fs->guest_crashed) {
        if (fs->guest_crashed() == PERISCOPE_GUEST_SUSPEND) {
            atomic_set(&suspended, true);
            return PERISCOPE_GUEST_SUSPEND;
        }
    }

    return 0;
}

struct DirtyBitmapSnapshot {
    ram_addr_t start;
    ram_addr_t end;
    unsigned long dirty[];
};

#ifdef PERI_DEDUP

/* Should be holding either ram_list.mutex, or the RCU lock. */
#define  INTERNAL_RAMBLOCK_FOREACH(block)  \
    QLIST_FOREACH_RCU(block, &ram_list.blocks, next)
/* Never use the INTERNAL_ version except for defining other macros */
#define RAMBLOCK_FOREACH(block) INTERNAL_RAMBLOCK_FOREACH(block)

/* Should be holding either ram_list.mutex, or the RCU lock. */
#define RAMBLOCK_FOREACH_NOT_IGNORED(block)            \
    INTERNAL_RAMBLOCK_FOREACH(block)                   \
        if (ramblock_is_ignored(block)) {} else

#define RAMBLOCK_FOREACH_MIGRATABLE(block)             \
    INTERNAL_RAMBLOCK_FOREACH(block)                   \
        if (!qemu_ram_is_migratable(block)) {} else



#define TRACE_PERI_DEVRAM_RESTORE
//#undef TRACE_PERI_DEVRAM_RESTORE
static void get_restore_bitmap(
      periscope_cp_desc *cp_last,
      periscope_cp_desc *cp_next,
      periscope_cp_desc *cp_lca,
      unsigned long *dirty,
      //unsigned long *dirty_fine,
      unsigned long npages_snap,
      const char *name,
      unsigned long *src_dirty_cnt,
      unsigned long *dst_dirty_cnt) {
    /*
     * concatenate all the pages that have been dirtied
     * in between the last checkpoint and the checkpoint to be restored.
     * we want to go from
     *       cp_last ---- HERE
     *     /
     *  cp_lca
     *     \
     *       cp_next - TO HERE
     * -> Pages dirties by A have to be restored as well
     */
    periscope_cp_desc *cp_tmp = cp_last;
    periscope_snapshot_desc *snap = NULL;

    while (cp_tmp != cp_lca) {
        snap = &cp_tmp->snapshot;
        cp_tmp = cp_tmp->parent;
        assert(cp_tmp != NULL);
        assert(snap->id != SNAPSHOT_INVALID);
        periscope_ramblock *prb  = get_ramblock(snap->peri_rb, snap->n_peri_rb, name);
        assert(prb);
        if(prb->empty) {
           printf("%s: prb %s empty -> skip\n", __FUNCTION__, prb->idstr);
           continue;
        }

#ifdef FINE_CHUNKS
        unsigned long* prb_dirty = prb->dirty_fine;
#else
        unsigned long* prb_dirty = prb->dirty;
#endif
        if (npages_snap != prb->npages * CHUNK_DIV) {
#ifdef TRACE_PERI_DEVRAM_RESTORE
           printf("(now)%ld -- %ld(%d)\n", npages_snap, prb->npages, snap->id);
#endif
        }

        //assert(npages_snap == prb->npages * CHUNK_DIV);

        bitmap_or(dirty, dirty, prb_dirty, npages_snap);
#ifdef TRACE_PERI_DEVRAM_RESTORE
        printf("#pages after adding chkpt %d -> %ld\n", snap->id, bitmap_count_one(dirty, npages_snap));
#endif
    }
    if(src_dirty_cnt) *src_dirty_cnt = bitmap_count_one(dirty, npages_snap) / CHUNK_DIV;
    cp_tmp = cp_next;
    while (cp_tmp != cp_lca) {
        snap = &cp_tmp->snapshot;
        cp_tmp = cp_tmp->parent;
        assert(cp_tmp != NULL);
        assert(snap->id != SNAPSHOT_INVALID);
        periscope_ramblock *prb  = get_ramblock(snap->peri_rb, snap->n_peri_rb, name);
        assert(prb);
        if(prb->empty) {
           printf("%s: prb %s empty -> skip\n", __FUNCTION__, prb->idstr);
           continue;
        }
#ifdef FINE_CHUNKS
        unsigned long* prb_dirty = prb->dirty_fine;
#else
        unsigned long* prb_dirty = prb->dirty;
#endif
#ifdef TRACE_PERI_DEVRAM_RESTORE
        if (npages_snap != prb->npages * CHUNK_DIV) {
           printf("(now)%ld -- %ld(%d)\n", npages_snap, prb->npages, snap->id);
        }
#endif

        //assert(npages_snap == prb->npages * CHUNK_DIV);

        bitmap_or(dirty, dirty, prb_dirty, npages_snap);
#ifdef TRACE_PERI_DEVRAM_RESTORE
        printf("#pages after adding chkpt %d -> %ld\n", snap->id, bitmap_count_one(dirty, npages_snap));
#endif
    }
    if(dst_dirty_cnt) *dst_dirty_cnt = bitmap_count_one(dirty, npages_snap) / CHUNK_DIV;
}

static void restore_branch(
      RAMBlock *rb,
      periscope_cp_desc *cp_last,
      periscope_cp_desc *cp_dest,
      unsigned long *dirty,
      unsigned long npages_snap) {
    periscope_snapshot_desc *snap = NULL;
    periscope_cp_desc *cp_next = cp_dest;

    unsigned long* bm_and = bitmap_new(npages_snap);
    bitmap_zero(bm_and, npages_snap);

    // go through all parents of the snapshot to be restored
    while (cp_next != NULL) {
        snap = &cp_next->snapshot;
        cp_next = cp_next->parent;
        assert(snap->id != SNAPSHOT_INVALID);
        periscope_ramblock *prb  = get_ramblock(snap->peri_rb, snap->n_peri_rb, rb->idstr);
        assert(prb);
        if(prb->empty) {
           //printf("%s: prb %s empty -> skip\n", __FUNCTION__, prb->idstr);
           continue;
        }
#ifdef FINE_CHUNKS
        unsigned long* prb_dirty = prb->dirty_fine;
#else
        unsigned long* prb_dirty = prb->dirty;
#endif
        if(npages_snap > prb->npages * CHUNK_DIV) {
           assert(false);
           printf("Reallocating %s %ld ->  %ld\n", rb->idstr, prb->npages, npages_snap);
           unsigned long *bm_new = bitmap_new(npages_snap);
           bitmap_copy(bm_new, prb->dirty, prb->npages);
           g_free(prb->dirty);
           prb->dirty = bm_new;
           prb->npages = npages_snap;
           printf("%ld %ld\n", npages_snap, prb->npages);
        }
        //assert(npages_snap == prb->npages * CHUNK_DIV);
        // get inersection of pages stored in this specific snapshot and all the
        // pages currently dirty (which have to be restored)
        bitmap_and(bm_and, dirty, prb_dirty, npages_snap);

        // unless the intersection is empty ...
        if (!bitmap_empty(bm_and, npages_snap)) {
#ifdef TRACE_PERI_DEVRAM_RESTORE
           printf(
                 "periscope: restoring %lu/%lu dirty pages from snapshot %d\n",
                 bitmap_count_one(bm_and, npages_snap),
                 bitmap_count_one(prb_dirty, npages_snap), snap->id);
#endif
           restore_ram_pages(rb, prb, bm_and);
        } else {
           //printf("%s 0 overlap -> skip\n", rb->idstr);
        }


        // remove the pages that we have already restored fromt the bitmap
        // indicating which pages still have to be restored
        bitmap_andnot(dirty, dirty, bm_and, npages_snap);

        //periscope_no_loadvm_state_setup = true;
        //if(bitmap_empty(dirty, npages_snap)) break;
    }

#ifdef DBG_RAM_STORED // set in periscope.h
    periscope_ramblock *prb  = get_ramblock(cp_dest->snapshot.peri_rb, cp_dest->snapshot.n_peri_rb, rb->idstr);
    printf("---------- comparing restored chktp %d to %s\n", snap->id, rb->idstr);
    assert(compare_ram_pages(prb->rambkp, NULL, prb->rambkp_size, prb->dirty, false, rb->idstr));
#endif

    if (bm_and) {
        g_free(bm_and);
        bm_and = NULL;
    }
}
#endif /* PERI_DEDUP */


int periscope_restore(void) {
    vm_stop(RUN_STATE_RESTORE_VM);

    int ret = -1; // will be updated as the return value of loadvm
    rcu_read_lock();

    periscope_cp_desc *cp_base = NULL; // base snapshot
    periscope_cp_desc *cp_next = NULL; // to be restored
    periscope_snapshot_desc *snap = NULL;

    FuzzerState *fs = fuzzer_get_current();
    assert(fs);
    assert(fs->cur_input);

    if (fs->cur_input == &root) {
       cp_base = &cp_root;
    }
    else {
       assert(fs->cur_input);
       cp_base = fs->cur_input->base_cp;
    }

    periscope_next_input(); // this function changes fs->cur_input

    static qemu_timeval tv_restore_begin;
    qemu_gettimeofday(&tv_restore_begin);

    if (fs->cur_input == &root) {
        cp_next = &cp_root;
    }
    else {
        assert(fs->cur_input->base_cp);
        cp_next = fs->cur_input->base_cp;
    }

    atomic_set(&restore_request, 0);

#ifndef PERI_DEDUP
    // get current dirty page bitmap -> all the pages we have to restore
    // from all the upstream snapshots
    unsigned long *dirty = NULL;
    //unsigned long npages_snap = get_current_delta_bm(&dirty);
    unsigned long npages_snap = update_and_clear_delta_snap_bm(&dirty);
    assert(dirty != NULL);

    unsigned long current_dirty_cnt = bitmap_count_one(dirty, npages_snap);
    unsigned long src_dirty_cnt = 0;
    unsigned long dst_dirty_cnt = 0;

    printf("periscope: #dirty before restore %ld\n", bitmap_count_one(dirty, npages_snap));
    /*
     * concatenate all the pages that have been dirtied
     * in between the last checkpoint and the checkpoint to be restored.
     * we want to go from
     *       cp_base ---- HERE
     *     /
     *  cp_lca
     *     \
     *       cp_next - TO HERE
     * -> Pages dirties by A have to be restored as well
     */
    periscope_cp_desc *cp_lca = find_lowest_common_ancestor(cp_base, cp_next);
    assert(cp_lca);
    printf("periscope: base checkpoint %d, lca %d, dest %d\n",
          cp_base->snapshot.id, cp_lca->snapshot.id, cp_next->snapshot.id);

    struct periscope_cp_desc *cp_tmp = cp_base;
    while (cp_tmp != cp_lca) {
        snap = &cp_tmp->snapshot;
        assert(snap->id != SNAPSHOT_INVALID);

        if (npages_snap != snap->npages) {
            printf("(now)%ld -- %ld(%d)\n", npages_snap, snap->npages, snap->id);
        }

        assert(npages_snap == snap->npages);

        bitmap_or(dirty, dirty, snap->dirty, npages_snap);

        cp_tmp = cp_tmp->parent;
        assert(cp_tmp != NULL);
    }

    src_dirty_cnt = bitmap_count_one(dirty, npages_snap) - current_dirty_cnt;

    cp_tmp = cp_next;
    while (cp_tmp != cp_lca) {
        snap = &cp_tmp->snapshot;
        assert(snap->id != SNAPSHOT_INVALID);

        if (npages_snap != snap->npages) {
            printf("(now)%ld -- %ld(%d)\n", npages_snap, snap->npages, snap->id);
        }

        assert(npages_snap == snap->npages);

        bitmap_or(dirty, dirty, snap->dirty, npages_snap);

        cp_tmp = cp_tmp->parent;
        assert(cp_tmp != NULL);
    }

    dst_dirty_cnt = bitmap_count_one(dirty, npages_snap) - current_dirty_cnt - src_dirty_cnt;

    fs->cur_input->num_pages_restored = current_dirty_cnt + src_dirty_cnt + dst_dirty_cnt;

    snap = &cp_next->snapshot;

    periscope_cp_desc *cp_dest = cp_next;

    unsigned long* bm_and = bitmap_new(npages_snap);

#ifdef FULL_SNAPSHOT_RESTORE
    ret = load_snapshot("periscope-syz-1", NULL);
#else

#ifdef FULL_DELTA_RESTORE
    bitmap_fill(dirty, npages_snap);
#endif

    // TODO:
    // sometimes on first restore, we only have 1 or 2 pages in the bitmap
    // this fixes that problem, but we should really only match on the first restore
    //if (first_restore)
    //   bitmap_fill(dirty, npages_snap);
    //first_restore = false;

    //QIOChannelFile *iochannel;
    QIOChannelBuffer *iochannel;
    QEMUFile *file;
    MigrationIncomingState* mis;

    //qemu_system_reset(SHUTDOWN_CAUSE_NONE); // TODO: can we avoid this?

    periscope_no_loadvm_state_setup = false;
    periscope_no_loadvm_state_cleanup = true;

    // go through all parents of the snapshot to be restored
    while (cp_next != NULL) {
        snap = &cp_next->snapshot;
        assert(npages_snap == snap->npages);

        // get inersection of pages stored in this specific snapshot and all the
        // pages currently dirty (which have to be restored)
        bitmap_and(bm_and, dirty, snap->dirty, npages_snap);

        // unless the intersection is empty ...
        if (!bitmap_empty(bm_and, npages_snap) || cp_next==cp_dest) {
#ifdef TRACE_DELTA_RESTORE
            printf(
                "periscope: restoring %lu/%lu dirty pages from snapshot %d\n",
                bitmap_count_one(bm_and, npages_snap),
                bitmap_count_one(snap->dirty, npages_snap), snap->id);
#endif

            // copy intersection bitmap to ramblock
            // this bitmap will determine which pages are skipped in ram_load
            RAMBlock *rb;
            INTERNAL_RAMBLOCK_FOREACH(rb)
            { // TODO: get_system_ram returns mr->ram_block empty??
                if (rb->bmap_delta_restore == NULL) {
                    // if there is none yet, create one (hope that npages is
                    // fixed)
                    unsigned long npages = rb->max_length >> TARGET_PAGE_BITS;
                    rb->bmap_delta_restore = bitmap_new(npages);
                }
                assert(rb->bmap_delta_restore != NULL);
                if (strcmp(rb->idstr, "pc.ram") != 0) {
                    if (periscope_no_loadvm_state_setup == false) {
                        bitmap_set(rb->bmap_delta_restore, 0, rb->max_length >> TARGET_PAGE_BITS);
                    }
                    else {
                        bitmap_clear(rb->bmap_delta_restore, 0, rb->max_length >> TARGET_PAGE_BITS);
                    }
                }
                else {
                    assert(rb->max_length>>TARGET_PAGE_BITS == npages_snap);
                    bitmap_copy(rb->bmap_delta_restore, bm_and, npages_snap);
                }
            }

            // setup the restore
            //lseek(snap->memfd_ram, 0, SEEK_SET);
            //iochannel = qio_channel_file_new_fd(dup(snap->memfd_ram));
            iochannel = qio_channel_buffer_new_with_existing_data(snap->buf_ram, snap->ram_sz);
            file = qemu_fopen_channel_input(QIO_CHANNEL(iochannel));
            mis = migration_incoming_get_current();
            mis->from_src_file = file;
            ret = qemu_loadvm_state(file);
            mis->from_src_file = NULL;
            //migration_incoming_state_destroy();
            qio_channel_buffer_close_without_free(iochannel);
            object_unref(OBJECT(iochannel));
            qemu_fclose(file);
            file = NULL;
        }

        // remove the pages that we have already restored fromt the bitmap
        // indicating which pages still have to be restored
        bitmap_andnot(dirty, dirty, bm_and, npages_snap);

        periscope_no_loadvm_state_setup = true;
        cp_next = cp_next->parent;
    }

    periscope_no_loadvm_state_cleanup = false;
#ifdef TRACE_DELTA_RESTORE
    printf("periscope: restoring device state\n");
#endif
    snap = &cp_dest->snapshot;
    iochannel = qio_channel_buffer_new_with_existing_data(snap->buf_quick, snap->quick_sz);
    file = qemu_fopen_channel_input(QIO_CHANNEL(iochannel));
    mis = migration_incoming_get_current();
    mis->from_src_file = file;
    ret = qemu_loadvm_state(file);
    mis->from_src_file = NULL;
    migration_incoming_state_destroy();
    qio_channel_buffer_close_without_free(iochannel);
    object_unref(OBJECT(iochannel));
    qemu_fclose(file);
    file = NULL;
#endif /*FULL_DELTA_RESTORE*/

#ifdef ENABLE_LW_CHKPT
    if (bm_and) {
        g_free(bm_and);
        bm_and = NULL;
    }
    if (dirty) {
        g_free(dirty);
        dirty = NULL;
    }
#endif
#else /*PERI_DEDUP*/
    periscope_cp_desc *cp_dest = cp_next;
    periscope_cp_desc *cp_lca = find_lowest_common_ancestor(cp_base, cp_next);
    assert(cp_lca);
#ifdef TRACE_DELTA_RESTORE
    printf("last snap %d, lca snap %d\n", cp_base->snapshot.id, cp_lca->snapshot.id);
#endif
    unsigned long total_dirty_cnt = 0;
    unsigned long current_dirty_cnt = 0;
    unsigned long src_dirty_cnt = 0;
    unsigned long dst_dirty_cnt = 0;
    unsigned long npages_snap = 0;

    //qemu_system_reset(SHUTDOWN_CAUSE_NONE); // TODO: can we avoid this?
    RAMBlock *rb;
    qemu_mutex_lock_ramlist();
    rcu_read_lock(); // see comment on INTERNAL_RAMBLOCK_FOREACH
    RAMBLOCK_FOREACH_MIGRATABLE(rb) {
       struct DirtyBitmapSnapshot *db_snap = memory_region_snapshot_and_clear_dirty(
             rb->mr,
             0, rb->max_length,
             DIRTY_MEMORY_DELTA // TODO: do we need a custom flag?
             );
#ifdef TRACE_DELTA_DEDUP_RESTORE
       printf("%s: update %s %lx - %lx\n", __FUNCTION__, rb->idstr, db_snap->start, db_snap->end);
#endif
       npages_snap = (db_snap->end - db_snap->start) >> TARGET_PAGE_BITS;
       assert(db_snap->dirty != NULL);
#ifdef FINE_CHUNKS
       unsigned long *dirty_fine = copy_fine_bitmap(db_snap->dirty, npages_snap);
       unsigned long *dirty = dirty_fine;
       npages_snap *= CHUNK_DIV;
#else
       unsigned long *dirty = db_snap->dirty;
#endif
       //unsigned long *dirty = bitmap_new(npages_snap);
       //bitmap_copy(dirty, db_snap->dirty, npages_snap);
       //assert(dirty != NULL);
       unsigned long rb_current_dirty_cnt = 0;
       unsigned long rb_src_dirty_cnt = 0;
       unsigned long rb_dst_dirty_cnt = 0;
       rb_current_dirty_cnt = bitmap_count_one(dirty, npages_snap) / CHUNK_DIV;
       current_dirty_cnt += rb_current_dirty_cnt;
#ifdef TRACE_DELTA_DEDUP_RESTORE
       printf("%s #dirty pages %ld\n", rb->idstr, rb_current_dirty_cnt);
#endif
       // go through all parents of last restored snapshot to collect dirty bits
       get_restore_bitmap(cp_base, cp_next, cp_lca, dirty, npages_snap, rb->idstr,
             &rb_src_dirty_cnt, &rb_dst_dirty_cnt);
       src_dirty_cnt += (rb_src_dirty_cnt - rb_current_dirty_cnt);
       dst_dirty_cnt += (rb_dst_dirty_cnt - rb_src_dirty_cnt - rb_current_dirty_cnt);
       total_dirty_cnt += bitmap_count_one(dirty, npages_snap) / CHUNK_DIV;
       // restore all dirty memory pages along the branch stating from the destination checkpoint
       restore_branch(rb, cp_base, cp_next, dirty, npages_snap);
       g_free(db_snap);
#ifdef FINE_CHUNKS
       g_free(dirty_fine);
#endif
    }
    fs->cur_input->num_pages_restored = total_dirty_cnt; //current_dirty_cnt + src_dirty_cnt + dst_dirty_cnt;
    rcu_read_unlock();
    qemu_mutex_unlock_ramlist();

    QIOChannelBuffer *iochannel;
    QEMUFile *file;
    MigrationIncomingState* mis;


    periscope_no_loadvm_state_cleanup = true;
    periscope_no_loadvm_state_setup = true;
    snap = &cp_dest->snapshot;
    iochannel = qio_channel_buffer_new_with_existing_data(snap->buf_dev, snap->dev_sz);
    file = qemu_fopen_channel_input(QIO_CHANNEL(iochannel));
    mis = migration_incoming_get_current();
    mis->from_src_file = file;
    //quick_reset_devs = true;
    ret = qemu_loadvm_state(file);
    mis->from_src_file = NULL;
    migration_incoming_state_destroy();
    qio_channel_buffer_close_without_free(iochannel);
    object_unref(OBJECT(iochannel));
    qemu_fclose(file);
    file = NULL;

//    //quick_reset_devs = true;
//    qemu_timeval tt0, tt1, tte, ttx0, ttx1, ttxe;
//    qemu_gettimeofday(&tt0);
//    //periscope_no_loadvm_state_cleanup = false;
//    //periscope_no_loadvm_state_setup = false;
//    periscope_no_loadvm_state_cleanup = false;
//    periscope_no_loadvm_state_setup = true;
//    snap = &cp_dest->snapshot;
//    assert(snap->buf_dev != NULL);
//    assert(snap->dev_sz != 0);
//    assert(snap != NULL);
//    iochannel = qio_channel_buffer_new_with_existing_data(snap->buf_dev, snap->dev_sz);
//    file = qemu_fopen_channel_input(QIO_CHANNEL(iochannel));
//    mis = migration_incoming_get_current();
//    mis->from_src_file = file;
//    ret = qemu_loadvm_state(file);
//    mis->from_src_file = NULL;
//    migration_incoming_state_destroy();
//    qio_channel_buffer_close_without_free(iochannel);
//    object_unref(OBJECT(iochannel));
//    qemu_fclose(file);
//    file = NULL;
//    qemu_gettimeofday(&tt1);
//    timersub(&tt1, &tt0, &tte);
//    unsigned long ttt = tte.tv_sec * 1000L *1000L + tte.tv_usec;
//    //quick_reset_devs = false;
//    printf("periscope: dev restore %ld\n", ttt/1000L);
#endif /*PERI_DEDUP */

    if (ret < 0) {
        printf("periscope: restoring periscope-%u failed\n", snap->id);
        return -1;
    }

    qemu_gettimeofday(&tv_restore_end);

    qemu_timeval restore_elapsed;
    timersub(&tv_restore_end, &tv_restore_begin, &restore_elapsed);

    last_restore_time.tv_sec =  restore_elapsed.tv_sec;
    last_restore_time.tv_usec = restore_elapsed.tv_usec;
#ifdef TRACE_DELTA_RESTORE
    printf("periscope: restored snapshot %d (%lu ms) %u times (%ld+%ld+%ld=%ld/%ld dirty pages)\n",
           snap->id, restore_elapsed.tv_usec / 1000L, cp_dest->num_restored,
           current_dirty_cnt, src_dirty_cnt, dst_dirty_cnt,
           current_dirty_cnt+src_dirty_cnt+dst_dirty_cnt, npages_snap);
#endif

    last_chkpt_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);

    int level = 0;
    periscope_cp_desc *cp = fs->cur_input->restored_cp;
    while (cp != NULL) {
        level++;
        cp = cp->parent;
    }

    chkpt_time_threshold_ms = CHKPT_TIME_THRESHOLD_MS;
    level--;
    while (level > 0) {
        chkpt_time_threshold_ms *= CHKPT_TIME_THRESHOLD_MULTIPLIER;
        level--;
    }

    if (fs->restored) {
        fs->restored();

        memset(num_evicted, 0, sizeof(num_evicted));
    }
    atomic_set(&suspended, false);

    assert(fs->cur_cp);

    if (!fs->cur_cp->closed) {
        delete_cp_desc(fs->cur_cp);
        fs->cur_cp = NULL;
    }

    // base a new cp on the base snapshot of the new input.
    fs->cur_cp = create_cp_desc(cp_dest);

    if (snap->id != SNAPSHOT_ROOT_ID) {
        total_non_root_restores += 1;
    }

#if 0 // loadvm will change pc.ram, so we can not clear the bitmap here
    // clear the bitmap
    unsigned long *dirty_new;
    update_and_clear_delta_snap_bm(&dirty_new);
    unsigned int n_dirty_new = bitmap_count_one(dirty_new, npages_snap);
    printf("%d new bits after restore\n", n_dirty_new);
    g_free(dirty_new);
    assert(n_dirty_new == 0);
#endif

    rcu_read_unlock();

    cpu_synchronize_all_post_init();

#ifdef DBG_RAM_STORED // set in periscope.h
    printf("---------- comparing restored chktp %d to pc.ram\n", cp_dest->snapshot.id);
    assert(compare_ram_pages(cp_dest->snapshot.rambkp, NULL,
          cp_dest->snapshot.rambkp_size, NULL, false));
#endif

    // vm_start();

    return 0;
}
#else  /* !ENABLE_LWPCHKPT */
int periscope_restore(void) {
    int ret;
    int last_id = -1;
    int id = 0;

    char pt_mark_buf[256];
    snprintf(pt_mark_buf, 256, "## %s\n", __FUNCTION__);
    pt_mark_all(pt_mark_buf);

    FuzzerState *fs = fuzzer_get_current();
    assert(fs);
    assert(fs->cur_input);

    if (fs->cur_input != &root) {
        assert(fs->cur_input->base_cp);
        last_id = fs->cur_input->base_cp->snapshot.id;
    }

    if (fs->fetch_next) {
        periscope_next_input(); // this function changes fs->cur_input
    }

#if 1
    periscope_cp_desc *cp;
    periscope_snapshot_desc *s;

    if (fs->cur_input == &root) {
        cp = &cp_root;
    }
    else {
        assert(fs->cur_input->base_cp);
        cp = fs->cur_input->base_cp;
    }
    s = &cp->snapshot;
    id = s->id;

    //printf("periscope: restore %d (old %d)\n", id, last_id);
    // stat
    timeradd(&cp->exec_time, &total_chkpt_saving_time,
             &total_chkpt_saving_time);

    restore_request = 0;
#else
    assert(s->cur_input->parent);

    // FIXME: parent is always root only for now.
    assert(s->cur_input->parent == &root);

    restore_request = &s->cur_input->parent->snapshot;

#if 0
    printf("as=0x%p as->root=0x%p as->root->ram_block=0x%p\n",
            as, as->root, as->root->ram_block);
#endif
#endif

    // printf("periscope: sync dirty log start\n");
    // memory_global_dirty_log_sync();
    // printf("perscope: sync dirty log end\n");

#ifdef ENABLE_DELTA_CHECKPOINT
    check_memory_regions();
#endif

#if 0
    // snapshot dirty log
    MemoryRegion *mr = get_system_memory();
    DirtyBitmapSnapshot *snap = memory_region_snapshot_and_clear_dirty(
        mr,
        mr->addr,
        mr->size,
        DIRTY_CLIENTS_ALL
    );

    // TODO
    (void)snap;
#endif

#if 0
    if (restore_request == NULL) {
        printf("periscope: restore not requested\n");
        return -1;
    }

    periscope_snapshot_desc *s = restore_request;
    restore_request = NULL;

    rcu_read_lock();
    // TODO
    rcu_read_unlock();
#endif

    static qemu_timeval tv_restore_begin;
    qemu_gettimeofday(&tv_restore_begin);

    vm_stop(RUN_STATE_RESTORE_VM);

#if 1

    qemu_gettimeofday(&tv_restore_end);

    qemu_timeval reset_elapsed;
    timersub(&tv_restore_end, &tv_restore_begin, &reset_elapsed);
    // printf("periscope: system reset time %lu ms\n",
    //        reset_elapsed.tv_usec / 1000L);

    //lseek(s->memfd, 0, SEEK_SET);
    QIOChannelFile *iochannel;// = qio_channel_file_new_fd(dup(s->memfd));
    QEMUFile *file;// = qemu_fopen_channel_input(QIO_CHANNEL(iochannel));
    MigrationIncomingState* mis;// = migration_incoming_get_current();
    //mis->from_src_file = file;

    peri_timer *pt = NULL;
    if(last_id != id || last_id < 0) {
      pt = start_interval("periscope_restore.timer");
      lseek(s->memfd, 0, SEEK_SET);
      iochannel = qio_channel_file_new_fd(dup(s->memfd));
      file = qemu_fopen_channel_input(QIO_CHANNEL(iochannel));
      mis = migration_incoming_get_current();
      mis->from_src_file = file;
      qemu_system_reset(SHUTDOWN_CAUSE_NONE); // TODO: can we avoid this?
      ret = qemu_loadvm_state(file);
      stop_interval(pt);
    } else {
      pt = start_interval("periscope_quick_restore.timer");
      lseek(s->memfd_quick, 0, SEEK_SET);
      iochannel = qio_channel_file_new_fd(dup(s->memfd_quick));
      file = qemu_fopen_channel_input(QIO_CHANNEL(iochannel));
      mis = migration_incoming_get_current();
      mis->from_src_file = file;

      set_quick_reset_devs();
      qemu_system_reset(SHUTDOWN_CAUSE_NONE); // TODO: can we avoid this?

      set_quick_reset_ram();
      ret = qemu_loadvm_state(file);
      unset_quick_reset_ram();

      unset_quick_reset_devs();
      stop_interval(pt);
    }

    mis->from_src_file = NULL;
    migration_incoming_state_destroy();
    object_unref(OBJECT(iochannel));
    qemu_fclose(file);
    file = NULL;
#else
    Error *err = NULL;
    if (load_snapshot_via_rollback(name, &err) == 0 && saved_vm_running) {
        vm_start();
    }
    vm_start();
#endif

    qemu_gettimeofday(&tv_restore_end);

    qemu_timeval elapsed;
    timersub(&tv_restore_end, &tv_restore_begin, &elapsed);

    timeradd(&elapsed, &total_restore_time, &total_restore_time);
    if (timercmp(&elapsed, &max_restore_time, >)) {
        printf("periscope: new max restore time %lu ms\n",
               elapsed.tv_sec * 1000L + elapsed.tv_usec / 1000L);
        memcpy(&max_restore_time, &elapsed, sizeof(qemu_timeval));
    }
    //printf("periscope: restore time %lu ms\n",
    //      elapsed.tv_sec * 1000L + elapsed.tv_usec / 1000L);

    assert(fs->cur_cp);

    if (!fs->cur_cp->closed) {
        delete_cp_desc(fs->cur_cp);
        fs->cur_cp = NULL;
    }

    // base a new cp on the base snapshot of the new input.
    fs->cur_cp = create_cp_desc(cp);

    if (ret < 0) {
        printf("periscope: restoring periscope-%u failed\n",
               s->id);
        return -1;
    }

#define TRACE_CHKPT_RESTORE
//#undef TRACE_CHKPT_RESTORE
#ifdef TRACE_CHKPT_RESTORE
    static int last_restore_id = SNAPSHOT_ROOT_ID;
    if (s->id != last_restore_id) {
        printf("periscope: restoring periscope-%d (%u/%u bytes, saved %lu ms %u times) took %lu ms\n",
                s->id,
                fs->cur_input->used_len,
                fs->cur_input->len,
                calc_total_time_saved(fs->cur_input->base_cp),
                fs->cur_input->base_cp->num_restored,
                elapsed.tv_sec * 1000L + elapsed.tv_usec / 1000L);
        last_restore_id = s->id;
    }
#endif
    if (s->id != SNAPSHOT_ROOT_ID) {
        total_non_root_restores += 1;
    }

    // vm_start();

    return 0;
}
#endif

#ifdef TARGET_AARCH64
int moneta_restore(void) {
    FuzzerState *fs = fuzzer_get_current();
    assert(fs);
    assert(fs->cur_input);

    periscope_next_input(); // this function changes fs->cur_input

    static qemu_timeval tv_restore_begin;
    qemu_gettimeofday(&tv_restore_begin);

    atomic_set(&restore_request, 0);

    load_snapshot("post-rehost", 0);
    atomic_set(&suspended, false);

    return 0;
}
#endif

uint64_t periscope_get_estimated_restore_time_ms(void) {
    // TODO: take into account the number of dirty pages
    return 100;//FIXME
}

#define TRACE_NUM_DIRTY_PAGES
#undef TRACE_NUM_DIRTY_PAGES

#ifdef TRACE_NUM_DIRTY_PAGES
static uint64_t get_num_dirty_pages(DirtyBitmapSnapshot *snap) {
    uint64_t num_dirty_pages = 0;
    unsigned long page, end;

    ram_addr_t start = snap->start;
    ram_addr_t length = snap->end;

    end = TARGET_PAGE_ALIGN(start + length - snap->start) >> TARGET_PAGE_BITS;
    page = (start - snap->start) >> TARGET_PAGE_BITS;

    while (page < end) {
        if (test_bit(page, snap->dirty)) {
            num_dirty_pages++;
        }
        page++;
    }
    return num_dirty_pages;
}
#endif

#ifdef ENABLE_DELTA_CHECKPOINT
static void check_memory_regions(void) {
#ifdef TRACE_NUM_DIRTY_PAGES
    uint64_t total_dirty_pages = 0;
#endif

    //if (mr->ram_block) {
    RAMBlock *rb;
    INTERNAL_RAMBLOCK_FOREACH(rb) {
        if (strcmp(rb->idstr, "pc.ram") != 0) {
            continue;
        }

        //printf("ram_block=0x%p, size=0x%lx\n", rb->mr->ram_block, (uint64_t)rb->mr->size);

        struct DirtyBitmapSnapshot *snap = memory_region_snapshot_and_clear_dirty(
            rb->mr,
            rb->mr->addr,
            rb->mr->size,
            DIRTY_MEMORY_MIGRATION // TODO: do we need a custom flag?
        );

#ifdef TRACE_NUM_DIRTY_PAGES
        uint64_t num_dirty_pages = get_num_dirty_pages(snap);
        if (strcmp(rb->idstr, "pc.ram") == 0) {
            printf("periscope: pc.ram has %lu dirty pages\n", num_dirty_pages);
        }
        else {
            printf("periscope: %s has %lu dirty pages\n", rb->idstr, num_dirty_pages);
        }
        total_dirty_pages += num_dirty_pages;
#endif

        //printf("snap=0x%p\n", snap);

        //for (unsigned i = 0; i < rb->mr->size / 0x4000; i++) {
        //    bool dirty = memory_region_snapshot_get_dirty(
        //        rb->mr,
        //        snap,
        //        rb->mr->addr + 0x4000 * i,
        //        0x4000
        //    );
        //    if (dirty) {
        //        printf("0x%lx: dirty\n", rb->mr->addr + 0x4000 * i);
        //    }
        //}

        if (rb->mr->ram_block->bmap2 == NULL) {
           unsigned long npages = rb->mr->ram_block->max_length >> TARGET_PAGE_BITS;
           rb->mr->ram_block->bmap2 = bitmap_new(npages);
           printf("periscope: new dirty page bitmap created\n");
        }

        unsigned long npages_snap = (snap->end - snap->start) >> TARGET_PAGE_BITS;
        //printf("%s bmap2 %p\n", rb->mr->ram_block->idstr, rb->mr->ram_block->bmap2);
        memcpy(rb->mr->ram_block->bmap2, snap->dirty, BITS_TO_LONGS(npages_snap) * sizeof(unsigned long));
        g_free(snap);
    }

#ifdef TRACE_NUM_DIRTY_PAGES
    printf("periscope: %lu total dirty pages found\n", total_dirty_pages);
#endif

    //MemoryRegion *subregion;
    //QTAILQ_FOREACH(subregion, &mr->subregions, subregions_link) {
    //    check_memory_regions(subregion);
    //}
}
#endif

static periscope_input_desc *periscope_create_input_desc(char *input, uint32_t len) {
    periscope_input_desc *new = malloc(sizeof(periscope_input_desc));
    memset(new, 0, sizeof(periscope_input_desc));
    new->input = (char *)malloc(len);
    memcpy(new->input, input, len);
    new->len = len;

    return new;
}

static void periscope_delete_input_desc(periscope_input_desc *input_desc) {
    if (input_desc->input) {
        free(input_desc->input);
        input_desc->input = NULL;
    }
    free(input_desc);
}

#define PRINT_THROUGHPUT
#undef PRINT_THROUGHPUT

void periscope_fetch_next_input(void) {
    periscope_next_input();
    return;

}

static peri_timer *fuzz_iteration_pt = NULL;
static periscope_input_desc *periscope_next_input(void) {
#ifndef TARGET_AARCH64
    if(fuzz_iteration_pt)
       stop_interval(fuzz_iteration_pt);
    fuzz_iteration_pt = start_interval("periscope_fuzz_interation.timer");
#endif
    char pt_mark_buf[256];
    snprintf(pt_mark_buf, 256, "## %s\n", __FUNCTION__);
    pt_mark_all(pt_mark_buf);

#ifdef PRINT_THROUGHPUT
    static qemu_timeval tv_prev;
    static int total_prev = 0;

    if (total_execs % 100 == 0) {
        if (total_execs > 1) {
            qemu_timeval tv, sub;
            qemu_gettimeofday(&tv);
            timersub(&tv, &tv_prev, &sub);

            printf("periscope: total inputs=%d, throughput=%.2f for the last %d inputs\n",
                total_execs,
                (float)(total_execs - total_prev) * 1000
                / (sub.tv_sec * 1000 + sub.tv_usec / 1000),
                total_execs - total_prev);

            total_prev = total_execs;
        }
        qemu_gettimeofday(&tv_prev);
    }
#endif

    trace_periscope_next_input(total_execs);
    total_execs++;

    FuzzerState *s = fuzzer_get_current();
    assert(s != NULL);

    periscope_input_desc *cur = s->cur_input;
    assert(cur != NULL);

#define AFL_COVERAGE_SUPPORT 1
#if AFL_COVERAGE_SUPPORT
    if (cur != &root && s->cur_executed) {
        uint8_t *kcov_area = kcov_get_area();
        if (kcov_area == NULL) {
            printf("periscope: kcov_area not initialized\n");
        }
        else {
            uint64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
            s->cur_executed(kcov_area, cur->used_len, now - last_chkpt_ms, timed_out);
        }
    }
#endif

    // update useful stats
    if (cur->num_irqs > max_irqs) {
        max_irqs = cur->num_irqs;
        printf("periscope: new max irqs %u\n", max_irqs);
    }

    if (cur->num_io_reads > max_io_reads) {
        max_io_reads = cur->num_io_reads;
        printf("periscope: new max io reads %u\n", max_io_reads);
    }

    if (cur->used_len > max_used_len) {
        max_used_len = cur->used_len;
        printf("periscope: new max used len %u bytes\n", max_used_len);
    }

    // reset timeout flag
    timed_out = 0;

    uint32_t next_len;
    uint32_t matched_len = 0;
    char *next = NULL;

    if (s->fetch_next) {
        next = s->fetch_next(&next_len);
    }

    if (next == NULL) {
        // printf("periscope: no next input exists.\n");
        return NULL;
    }

    // printf("periscope: input (%u B) fetched \n", next_len);

    periscope_cp_desc *closest = &cp_root;
    if(!no_restore) {
       closest =
          periscope_find_closest_snapshot(next, next_len, cur, &matched_len);

       static int last_restore_id = -1;
       if (closest->snapshot.id != last_restore_id) {
          printf("periscope: closest snapshot %d, exec time %lu + (%lu) ms, len %d, match %d\n",
                closest->snapshot.id,
                closest->exec_time.tv_sec * 1000L + closest->exec_time.tv_usec / 1000L,
                get_exec_time_ms(closest->parent),
                next_len, matched_len);
          last_restore_id = closest->snapshot.id;
       }
    }

    periscope_input_desc *new = periscope_create_input_desc(next, next_len);
    new->restored_cp = closest;
    new->num_pages_restored = 0;
    new->base_cp = closest;
    new->used_len = matched_len;

    assert(new->len >= new->used_len);

    if (s->cur_input != &root) {
        periscope_delete_input_desc(s->cur_input);
    }
    s->cur_input = new;

    return 0;
}

void periscope_restore_request(void) {
    if (restore_request) {
        printf("periscope: restore already requested.\n");
        return;
    }
    if (no_restore) return;

    if (get_pcidev_by_name("kcov_vdev")) {
        PCIDevice *p = get_pcidev_by_name("kcov_vdev");
        KCovState *state = DO_UPCAST(KCovState, parent_obj, p);
        if (state) {
            kcov_flush_area(true, state->fd);
        }
    }

    atomic_xchg(&restore_request, 1);

    vm_stop(RUN_STATE_RESTORE_VM);

    FuzzerState *s = fuzzer_get_current();
    assert(s != NULL);

    printf("periscope: restore request\n");

    // timeout for restore
    timer_mod(s->timer, qemu_clock_get_ms(QEMU_CLOCK_HOST) + 5000000);
    timeout_reason = TIMEOUT_RESTORE;
#ifdef TARGET_AARCH64
    post_rehost_tick = true;
#endif
}

int periscope_mmio_check(MemoryRegion *mr, uint32_t len, uint8_t is_write) {
    if (no_restore)
       return 0;

    if (is_write)
        return 0;

    if (single_exec)
        return 0;

    if (!fuzzer_get_current())
        return 0;

    periscope_input_desc *cur = fuzzer_get_current()->cur_input;
    if (!cur)
        return 0;

    if (!mr)
        return 0;

    if (!mr->name)
        return 0;

    if (!strstart(mr->name, "periscope-", NULL))
        return 0;

    if (cur->len - cur->used_len < len) {
        //printf("input consumed %d\n", cur->used_len);
        return -1;
    }

    return 0; // OK
}

static uint64_t seed = 0xDEAD;

#define TRACE_IRQ_ASSERT

int periscope_maybe_raise_irq(PCIDevice *pci_dev) {
    FuzzerState *s = fuzzer_get_current();

    if (!s)
       return 0;

    if (s->irq_pending) {
#ifdef TRACE_IRQ_ASSERT
        printf("periscope: pci_irq_assert\n");
#endif
        pci_irq_assert(pci_dev);
        s->irq_pending = false;
    }

    return 0;
}

#define PERI_KEY_IRQ_CHECK 1
#if PERI_KEY_IRQ_CHECK == 1

#define PERISCOPE_IRQ_TOKEN (uint32_t)0xaddeadde

static int lookahead_tokens(periscope_input_desc* cur, uint32_t token) {
    int num_tokens = 0;

    while (cur->used_len + sizeof(uint32_t) <= cur->len) {
        if (*((uint32_t*)&cur->input[cur->used_len]) != token) {
            break;
        }
        cur->used_len += sizeof(uint32_t);
        num_tokens++;
    }

    return num_tokens;
}
#endif

int periscope_mmio_read(void* opaque, unsigned size, uint64_t *out) {
    int ret = 0;

    FuzzerState *s = fuzzer_get_current();
    if (!s || !s->cur_input) {
        // just return some value for the sake of performance profiling
        *out = seed++;
        return 0;
    }

    s->cur_input->num_io_reads++;

    if (s->dev == NULL) {
        s->dev = opaque;
    }

    if (s->cur_input->len - s->cur_input->used_len < size) {
        if (single_exec || no_restore) {
            *out = 0xbabababababababa;
            printf("periscope: no input left. returning 0x%lx\n", *out);
            return 0;
        }

        printf("periscope: no input left!\n");
        return -1;
    }

    if (s->mmio_read) {
        ret = s->mmio_read(size, out);
    }
    else {
        *out = 0x0;
        s->cur_input->used_len += size;
    }

    if(no_restore) {
      return 0;
    }
    if (s->cur_cp->len + size < MAX_INPUT_BYTES) {
        switch (size) {
        case 1:
            *(uint8_t*)&s->cur_cp->io[s->cur_cp->len] = (uint8_t)*out;
            break;
        case 2:
            *(uint16_t*)&s->cur_cp->io[s->cur_cp->len] = (uint16_t)*out;
            break;
        case 4:
            *(uint32_t*)&s->cur_cp->io[s->cur_cp->len] = (uint32_t)*out;
            break;
        case 8:
            *(uint64_t*)&s->cur_cp->io[s->cur_cp->len] = (uint64_t)*out;
            break;
        default:
            printf("periscope: unexpected size!\n");
        }
        s->cur_cp->len += size;
    }
    else {
        printf("periscope: input length cannot exceed %ld!\n", MAX_INPUT_BYTES);
    }

#if PERI_KEY_IRQ_CHECK == 1
    int num_irq_tokens = lookahead_tokens(s->cur_input, PERISCOPE_IRQ_TOKEN);
    if (num_irq_tokens > 0) {
#ifdef TRACE_IRQ_ASSERT
        printf("periscope: %d irq tokens ahead consumed\n", num_irq_tokens);
#endif
        s->cur_input->num_irqs += num_irq_tokens;
        s->irq_pending = true;

        while (num_irq_tokens > 0) {
            if (s->cur_cp->len + sizeof(uint32_t) > MAX_INPUT_BYTES) {
                printf("periscope: not enough chkpt storage\n");
                break;
            }

            *(uint32_t*)&s->cur_cp->io[s->cur_cp->len] = PERISCOPE_IRQ_TOKEN;
            s->cur_cp->len += sizeof(uint32_t);
            num_irq_tokens--;
        }
    }
#endif

    if (!single_exec) {
        timer_mod(s->timer, qemu_clock_get_ms(QEMU_CLOCK_HOST) + MMIO_RESPONSE_TIMEOUT);
        timeout_reason = TIMEOUT_MMIO;
    }

    return ret;
}

uint32_t periscope_get_stat(int stat)
{
    uint32_t statVal;

    FuzzerState *fs = fuzzer_get_current();
    if (!fs || !fs->cur_input)
        return 0;

    if (fs->get_stat && fs->get_stat(stat, &statVal)) {
        return statVal;
    }

    if (stat == stat_saved_time || stat == stat_time_saved_total) {
        periscope_cp_desc *cp = fs->cur_input->restored_cp;
        uint32_t exec_time_ms = 0;
        while (cp != &cp_root) {
            exec_time_ms +=
                cp->exec_time.tv_sec * 1000L + cp->exec_time.tv_usec / 1000L;
            cp = cp->parent;
        }
        return exec_time_ms;
    }
    else if (stat == stat_exec_time || stat == stat_time_exec_total) {
        periscope_cp_desc *cp = fs->cur_cp;
        uint32_t exec_time_ms = 0;
        while (cp != &cp_root) {
            exec_time_ms +=
                cp->exec_time.tv_sec * 1000L + cp->exec_time.tv_usec / 1000L;
            cp = cp->parent;
        }
        return exec_time_ms;
    }
#ifdef PERI_DEDUP_STAT
    else if (stat == stat_chkpt_time) {
       return (uint32_t)(last_chkpt_time.tv_sec * 1000L * 1000L + last_chkpt_time.tv_usec) / 10; // 10us
    }
    else if (stat == stat_restore_time) {
       return (uint32_t)(last_restore_time.tv_sec * 1000L * 1000L + last_restore_time.tv_usec) / 10; // 10us
    }
#endif
    else if (stat == stat_actual_time) {
        periscope_cp_desc *cp = fs->cur_cp;
        uint32_t exec_time_ms = 0;
        while (cp != fs->cur_input->restored_cp) {
            exec_time_ms +=
                cp->exec_time.tv_sec * 1000L + cp->exec_time.tv_usec / 1000L;
            cp = cp->parent;
        }
        return exec_time_ms;
    }
    else if (stat == stat_non_root_restores) {
        if (fs->cur_input->restored_cp != &cp_root) {
            return 1;
        }
        return 0;
    }
    else if (stat == stat_chkpts_created) {
        int num_chkpts_created = 0;
        periscope_cp_desc *cp = fs->cur_input->base_cp;
        while (cp != fs->cur_input->restored_cp) {
            num_chkpts_created++;
            cp = cp->parent;
        }
        return num_chkpts_created;
    }
    else if (stat >= stat_evict_policy1 && stat <= stat_evict_policy4) {
        int policy = stat - stat_evict_policy1;
        if (policy < sizeof(num_evicted)/sizeof(num_evicted[0])) {
            return num_evicted[policy];
        }
        return 0;
    }
#ifdef PERI_DEDUP_STAT
    else if (stat == stat_zero) {
#if !defined(PERI_DEDUP)  || defined(PERI_DEDUP_NOHASH)
       return 0;
#else
       unsigned long num_zero_pages = 0;
       periscope_cp_desc *cp = fs->cur_input->base_cp;
       while (cp != fs->cur_input->restored_cp) {
          num_zero_pages += count_zero_pages_prbs(cp->snapshot.peri_rb, cp->snapshot.n_peri_rb);
          cp = cp->parent;
       }
       return num_zero_pages;
#endif
    }
    else if (stat == stat_skipped) {
#if !defined(PERI_DEDUP)  || defined(PERI_DEDUP_NOHASH)
       return 0;
#else
       unsigned long num_skipped_pages = 0;
       periscope_cp_desc *cp = fs->cur_input->base_cp;
       while (cp != fs->cur_input->restored_cp) {
          num_skipped_pages += count_skipped_pages_prbs(cp->snapshot.peri_rb, cp->snapshot.n_peri_rb);
          cp = cp->parent;
       }
       return num_skipped_pages;
#endif
    }


    else if (stat == stat_dedup) {
#if !defined(PERI_DEDUP)  || defined(PERI_DEDUP_NOHASH)
       return 0;
#else
       unsigned long num_unique_pages = 0;
       unsigned long num_stored_pages = 0;
       periscope_cp_desc *cp = fs->cur_input->base_cp;
       while (cp != fs->cur_input->restored_cp) {
          num_unique_pages += count_hashed_pages_prbs(cp->snapshot.peri_rb, cp->snapshot.n_peri_rb);
          num_stored_pages += count_stored_pages_prbs(cp->snapshot.peri_rb, cp->snapshot.n_peri_rb);
          cp = cp->parent;
       }
       return num_stored_pages - num_unique_pages;
#endif
    }
    else if (stat == stat_hashed) {
#if !defined(PERI_DEDUP)  || defined(PERI_DEDUP_NOHASH)
       return 0;
#else
       unsigned long num_unique_pages = 0;
       periscope_cp_desc *cp = fs->cur_input->base_cp;
       while (cp != fs->cur_input->restored_cp) {
          num_unique_pages += count_hashed_pages_prbs(cp->snapshot.peri_rb, cp->snapshot.n_peri_rb);
          cp = cp->parent;
       }
       return num_unique_pages;
#endif
    }
#endif /* PERI_DEDUP_STAT */
    else if (stat == stat_restored) {
        return fs->cur_input->num_pages_restored;
    }
    else if (stat == stat_dirtied) {
#ifndef PERI_DEDUP
        unsigned long *dirty = NULL;
        unsigned long num_pages = get_current_delta_bm(&dirty);
        assert(dirty != NULL);

        periscope_cp_desc *cp = fs->cur_input->base_cp;
        while (cp != fs->cur_input->restored_cp) {
            bitmap_or(dirty, dirty, cp->snapshot.dirty, num_pages);
            cp = cp->parent;
        }

        unsigned long num_dirty_pages = bitmap_count_one(dirty, num_pages);
        g_free(dirty);
        fs->cur_input->num_pages_dirtied = num_dirty_pages;
        return fs->cur_input->num_pages_dirtied;
#else
        unsigned long num_dirty_pages = 0;
        RAMBlock *rb = NULL;
        rcu_read_lock(); // see comment on INTERNAL_RAMBLOCK_FOREACH
        RAMBLOCK_FOREACH_MIGRATABLE(rb) {
           struct DirtyBitmapSnapshot *db_snap = memory_region_snapshot_and_get_dirty(
                 rb->mr,
                 0, rb->max_length,
                 DIRTY_MEMORY_DELTA // TODO: do we need a custom flag?
                 );
           assert(db_snap);
           assert(db_snap->dirty != NULL);
           unsigned long npages = (db_snap->end - db_snap->start) >> TARGET_PAGE_BITS;
           unsigned long *dirty = db_snap->dirty;
           periscope_cp_desc *cp = fs->cur_input->base_cp;
           while (cp != fs->cur_input->restored_cp) {
              periscope_snapshot_desc *snap = &cp->snapshot;
              cp = cp->parent;
              periscope_ramblock *prb  = get_ramblock(snap->peri_rb, snap->n_peri_rb, rb->idstr);
              assert(prb);
              if(prb->empty) continue;
              assert(prb != NULL);
              assert(prb->dirty != NULL);
              assert(npages == prb->npages);
              bitmap_or(dirty, dirty, prb->dirty, npages);
           }
           num_dirty_pages += bitmap_count_one(dirty, npages);
           g_free(db_snap);
        }
        rcu_read_unlock(); // see comment on INTERNAL_RAMBLOCK_FOREACH
        fs->cur_input->num_pages_dirtied = num_dirty_pages;
        return num_dirty_pages;
#endif
    }
    else if (stat >= stat_chkpt_size_0 && stat <= stat_chkpt_size_max) {
        int idx = 0;

        periscope_cp_desc *cp = fs->cur_input->base_cp;
        while (cp != fs->cur_input->restored_cp) {
            if (idx == stat - stat_chkpt_size_0) {
#if defined(PERI_DEDUP) && !defined(PERI_DEDUP_NOHASH)
                return compute_memory_cost(cp) +
                   compute_hash_cost(cp->snapshot.peri_rb, cp->snapshot.n_peri_rb)/1024U;
#else
                return compute_memory_cost(cp);
#endif
            }
            idx++;
            cp = cp->parent;
        }

        return 0;
    }
    // TODO: more stats

    return 0;
}

#define TRACE_CP_DESC
#undef TRACE_CP_DESC

#ifdef ENABLE_LW_CHKPT
static int close_cp_desc(periscope_cp_desc *cpd, uint16_t snapid, int memfd_ram, int memfd_quick, unsigned long *dirty, unsigned long npages) {
#else
static int close_cp_desc(periscope_cp_desc *cpd, uint16_t snapid, int memfd, int memfd_quick) {
#endif
    if (cpd->closed) {
        printf("periscope: chkpt already closed!\n");
        return 0;
    }

    if (cpd != &cp_root && cpd->len == 0) {
        printf("periscope: chkpt requested with an empty io history\n");
        return -1;
    }

    cpd->snapshot.id = snapid;
#ifndef ENABLE_LW_CHKPT
    cpd->snapshot.memfd = memfd;
#else
#ifndef PERI_DEDUP
    cpd->snapshot.memfd_quick = memfd_quick;
    cpd->snapshot.memfd_ram = memfd_ram;
    // TODO: maybe copy and free in parent?
    //cpd->snapshot.dirty = dirty;
    cpd->snapshot.dirty = bitmap_new(npages);
    bitmap_copy(cpd->snapshot.dirty, dirty, npages);
    cpd->snapshot.npages = npages;
#endif
#endif
    cpd->closed = true;
    qemu_gettimeofday(&cpd->closed_time);

    if (cpd->parent) {
        cpd->parent->children[cpd->parent->n_children] = cpd;
        cpd->parent->n_children++;
    }

#ifdef TRACE_CP_DESC
    printf("closed new cp desc %p with id %d\n", cpd, snapid);
#endif

    return 0;
}

static int delete_cp_desc(periscope_cp_desc *cpd) {
    if (cpd && cpd != &cp_root) {
#ifdef ENABLE_LW_CHKPT
#ifndef PERI_DEDUP
        if (cpd->snapshot.dirty) {
            g_free(cpd->snapshot.dirty);
            cpd->snapshot.dirty = NULL;
        }
#else
        //delete_peri_rbs(cpd->snapshot.peri_rb, cpd->snapshot.n_peri_rb);
        //cpd->snapshot.peri_rb = NULL;
        //cpd->snapshot.n_peri_rb = 0;
#endif
#endif
        free(cpd);
        return 0;
    }
    return -1;
}

static periscope_cp_desc *create_cp_desc(periscope_cp_desc *parent) {
    assert(parent);

#ifdef TRACE_CP_DESC
    printf("Adding new cp desc to parent snapshot id %d\n", parent->snapshot.id);
#endif

    if (parent->n_children > MAX_CP_DESC_CHILDREN - 1) {
        printf("periscope: too many children!\n");
        return NULL;
    }

    periscope_cp_desc *new_cp = (periscope_cp_desc *)malloc(sizeof(periscope_cp_desc));
    new_cp->len = 0;

    new_cp->n_children = 0;
    new_cp->parent = parent;

    new_cp->closed = false;

    new_cp->num_restored = 0;

    new_cp->last_restored.tv_sec = 0;
    new_cp->last_restored.tv_usec = 0;

    new_cp->exec_time.tv_sec = 0;
    new_cp->exec_time.tv_usec = 0;

    new_cp->closed_time.tv_sec = 0;
    new_cp->closed_time.tv_usec = 0;

    new_cp->snapshot.id = SNAPSHOT_INVALID;
#ifndef PERI_DEDUP
    new_cp->snapshot.memfd = -1;
    new_cp->snapshot.memfd_quick = -1;
    new_cp->snapshot.memfd_ram = -1;
    new_cp->snapshot.buf_quick = NULL;
    new_cp->snapshot.buf_ram = NULL;
#ifdef ENABLE_LW_CHKPT
    new_cp->snapshot.dirty = NULL;
#ifdef DBG_RAM_STORED // set in periscope.h
    new_cp->snapshot.rambkp = NULL;
    new_cp->snapshot.rambkp_size = 0;
#endif
#endif
#else
    new_cp->snapshot.buf_dev = NULL;
    new_cp->snapshot.dev_sz = 0;
    new_cp->snapshot.peri_rb = NULL;
    new_cp->snapshot.n_peri_rb = 0;
#endif

    return new_cp;
}

int periscope_irq_check(void) {
    static qemu_timeval cur;

    qemu_timeval prev = cur;
    qemu_gettimeofday(&cur);

    qemu_timeval sub;
    timersub(&cur, &prev, &sub);

#if 0
    printf("periscope: irq check - %lu (s) + %lu (us) elapsed since last check\n",
           sub.tv_sec, sub.tv_usec);
#endif

    return 0;
}

uint64_t periscope_checkpoint_requested(void) {
    int r = checkpoint_request;
    checkpoint_request = 0;
    return r;
}

uint64_t periscope_restore_requested(void) {
    if (!snapshot_inited)
        return 0;

    FuzzerState *fs = fuzzer_get_current();
    if (!restore_request && fs && fs->should_restore) {
        if (suspended && fs->should_restore())
            restore_request = 1;
    }

    if (restore_request) {
        return 1;
    }
    return 0;
}

static ShutdownCause shutdown_requested;

int periscope_system_shutdown_request(ShutdownCause reason) {
    if (reason == SHUTDOWN_CAUSE_GUEST_PANIC) {
        shutdown_requested = reason;
        return 0;
    }
    return -1;
}

int periscope_shutdown_requested(void) {
    return shutdown_requested;
}

#define TRACE_TIMEOUT
#undef TRACE_TIMEOUT

static void periscope_on_timeout(void *opaque) {
#ifdef TRACE_TIMEOUT
    printf("periscope: timeout reason=%s\n",
        timeout_reason == TIMEOUT_RESTORE ? "RESTORE" :
        timeout_reason == TIMEOUT_CHECKPOINT ? "CHECKPOINT" :
        timeout_reason == TIMEOUT_MMIO ? "MMIO" :
        timeout_reason == TIMEOUT_INTERRUPT ? "INTERRUPT" :
        "UNKNOWN");
#endif

    FuzzerState *s = opaque;
    assert(s);

    // raise a pending irq, if any, and delay the timeout for another interval
    if (s->irq_pending) {
        printf("periscope: draining pending irqs...\n");
        periscope_maybe_raise_irq(&s->dev->parent_obj);

        timer_mod(s->timer, qemu_clock_get_ms(QEMU_CLOCK_HOST) + INTERRUPT_RESPONSE_TIMEOUT);
        timeout_reason = TIMEOUT_INTERRUPT;
        return;
    }

    timed_out = 1;

    //timer_del(s->timer);
    //timer_mod(s->timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1);

    // TODO: shutdown guest
    periscope_restore_request();
    vm_stop(RUN_STATE_SAVE_VM);
}

static FuzzerState *current_fuzzer = NULL;

static uint8_t get_queue_cur_info_default(void) {
    printf("periscope: default cur info\n");
    return 0;
}

static void fuzzer_object_init(FuzzerState *fuzzer) {
    fuzzer->dev = NULL;

    fuzzer->irq_pending = false;

    fuzzer->mmio_read = NULL;
    fuzzer->fetch_next = NULL;
    fuzzer->get_cur = NULL;
    fuzzer->cur_executed = NULL;
    fuzzer->restored = NULL;
    fuzzer->should_restore = NULL;
    fuzzer->guest_crashed = NULL;
    fuzzer->get_stat = NULL;
    fuzzer->get_queue_cur_info = get_queue_cur_info_default;

    fuzzer->root = &root;
    fuzzer->cur_input = &root;
    fuzzer->cur_cp = &cp_root;

#ifdef ENABLE_LW_CHKPT
#ifndef PERI_DEDUP
    MachineState *machine = MACHINE(qdev_get_machine());
    assert(machine != NULL);
    cp_root.snapshot.npages = machine->ram_size / TARGET_PAGE_SIZE;
    cp_root.snapshot.dirty = bitmap_new(cp_root.snapshot.npages);
    bitmap_fill(cp_root.snapshot.dirty, cp_root.snapshot.npages);
    //memset(cp_root.snapshot.dirty, 0xff, cp_root.snapshot.npages / 8);
#endif
#endif

    fuzzer->timer =
        timer_new_ms(QEMU_CLOCK_HOST, periscope_on_timeout, fuzzer);

    char *root_only = getenv("__PERISCOPE_ROOT_ONLY_CHKPT");
    root_only_chkpt = root_only != NULL && strcmp(root_only, "root_only") == 0;
    char *no_restore_str = getenv("__PERISCOPE_NO_RESTORE");
    no_restore = no_restore_str != NULL && strcmp(no_restore_str, "no_restore") == 0;
}

static void fuzzer_object_finalize(void)
{
    // TODO: iterate through input and snapshot descriptors to deallocate them.
    printf("periscope: deleting fuzzer object\n");
    object_unref(OBJECT(current_fuzzer));
    current_fuzzer = NULL;
}

FuzzerState *fuzzer_get_current(void)
{
    return current_fuzzer;
}

// TODO
void periscope_debug_hypercall(uint64_t a0, uint64_t a1, uint64_t a2) {
    printf("periscope: debug hypercall 0x%lx 0x%lx 0x%lx\n", a0, a1, a2);

    RAMBlock *block = qemu_ram_block_by_name("pc.ram");
    if(!block) {
        printf("periscope: could not find ramblock\n");
        return;
    }

    uint64_t *addr = host_from_ram_block_offset(block, a1);
    switch (a0) {
    case 3:
        if (addr) {
            printf("*inmem=0x%lx\n", *addr);
        }
        periscope_syzkaller_send_addr_offset(a1);
        break;
    case 4:
        if (addr) {
            printf("*outmem=0x%lx\n", *addr);
        }
        periscope_syzkaller_send_addr_offset(a1);
        break;
    default:
        printf("periscope: unknown parameters for debug hypercall a0=0x%lx a1=0x%lx\n", a0, a1);
        break;
    }
}

bool should_restore_at_agent_exit(uint64_t code)
{
    if (!current_fuzzer) return false;

    if (code == 0) {
        //printf("periscope: guest agent exited successfully\n");
    } else
        printf("periscope: guest agent exited with code %ld\n",
               (int64_t)code);

    switch (current_fuzzer->mode) {
    case PERISCOPE_MODE_AFL:
    case PERISCOPE_MODE_COVERAGE:
        return true;
    }

    return false;
}

bool should_shutdown_at_agent_exit(uint64_t code)
{
    if ((code & 0xffff0000) == 0xdead0000) {
        printf("periscope: guest agent requested shutdown\n");
        if(get_pcidev_by_name("kcov_vdev")){
            PCIDevice *p = get_pcidev_by_name("kcov_vdev");
            KCovState *state = DO_UPCAST(KCovState, parent_obj, p);
            kcov_flush_area(false, state->fd);
        }
        return true;
    }
    printf("periscope: guest agent did not request shutdown %lu\n", code);
    return false;
}

void periscope_notify_boot(void) {
    FuzzerState *fs = fuzzer_get_current();

    if (fs && fs->mode == PERISCOPE_MODE_SYZKALLER_USBFUZZER) {
        periscope_syzkaller_notify_boot();
    }
}

static uint32_t agent_id = 0xdeadbeef;

uint32_t periscope_get_agent_id(void) {
    printf("periscope: returning agent id %d\n", agent_id);

    return agent_id;
}

static const char *parse_agent_id(const char *arg) {
    char str[256];
    char *tok;

    strncpy(str, arg, sizeof(str));

    tok = strtok(str, ",");
    if (!tok) return NULL;

    agent_id = strtol(tok, NULL, 0);

    printf("periscope: agent id %d\n", agent_id);

    tok = strtok(NULL, ",");
    if (!tok) return NULL;

    return arg + (tok - str);
}

// legacy
void periscope_start_fuzzer(const char *uri, Error **errp)
{
    const char *p;

    assert(!current_fuzzer);
    current_fuzzer = FUZZER_OBJ(object_new(TYPE_FUZZER));

    fuzzer_object_init(current_fuzzer);
    if (strstart(uri, "syzkaller:", &p)) {
        p = parse_agent_id(p);
        start_syzkaller_fuzzer(p, -1, -1, NULL, -1, errp);
    }
    else if (strstart(uri, "kcov:", &p)) {
        p = parse_agent_id(p);
        start_coverage_collector(p, errp);
    }
    else if (strstart(uri, "exec:", &p)) {
        single_exec = true;
        p = parse_agent_id(p);
        start_input_executor(p, errp);
    }
    else if (strstart(uri, "dummy:", &p)) {
        p = parse_agent_id(p);
        // TODO
    }
    else if (strstart(uri, "none:", &p)) {
        p = parse_agent_id(p);
        fuzzer_object_finalize();
    }
    else {
        error_setg(errp, "unknown fuzzer protocol: %s", uri);
        fuzzer_object_finalize();
    }
}

static Property fuzzer_properties[] = {
    DEFINE_PROP_STRING("uri", FuzzerState, uri), // format: "fuzzer:agent_id"
    DEFINE_PROP_INT32("st_pipe", FuzzerState, st_pipe, -1),
    DEFINE_PROP_INT32("ctl_pipe", FuzzerState, ctl_pipe, -1),
    DEFINE_PROP_STRING("mgr_pipe", FuzzerState, mgr_pipe),
    DEFINE_PROP_INT32("shm_id", FuzzerState, shm_id, -1),
    DEFINE_PROP_INT32("chkpt_pool_size", FuzzerState, chkpt_pool_size, 1024), // unit: MB
    DEFINE_PROP_INT32("fuzzer_id", FuzzerState, fuzzer_id, -1),
#if 1
    DEFINE_PROP_LINK("hostmem1", FuzzerState, hostmem[0], TYPE_MEMORY_BACKEND,
                     HostMemoryBackend *),
    DEFINE_PROP_LINK("hostmem2", FuzzerState, hostmem[1], TYPE_MEMORY_BACKEND,
                     HostMemoryBackend *),
#else
    DEFINE_PROP_LINK("shmem1", FuzzerState, shmem[0], "ivshmem-plain",
                     IVShmemState *),
#endif
    DEFINE_PROP_END_OF_LIST(),
};

#define FUZZER_TEST
#undef FUZZER_TEST

#ifdef FUZZER_TEST
static void fuzzer_test(FuzzerState *fs)
{
    if (fs) {
        MemoryRegion *mr = fs->mr[0];
        if (mr) {
            uint64_t *ptr = memory_region_get_ram_ptr(mr);
            printf("periscope: ptr=0x%lx *ptr=0x%lx\n", (uint64_t)ptr, *ptr);
        }
    }
}
#endif

static void fuzzer_realize(DeviceState *dev, Error **errp)
{
    FuzzerState *s = FUZZER_OBJ(dev);

    fuzzer_object_init(s);

    const char *p;

    // let the module have convenient access to the fuzzer device by caching it
    // to a static variable
    current_fuzzer = s;

    if (strstart(s->uri, "syzkaller:", &p)) {
        p = parse_agent_id(p);
        start_syzkaller_fuzzer(NULL, s->st_pipe, s->ctl_pipe,
                               s->mgr_pipe, s->shm_id, errp);
    }
    else if (strstart(s->uri, "profiler:", &p)) {
        p = parse_agent_id(p);
        start_profiler(0, errp);
    }
    else if (strstart(s->uri, "profiler-baseline:", &p)) {
        p = parse_agent_id(p);
        start_profiler(1, errp);
    }

#if 1
    if (s->hostmem[0]) {
        s->mr[0] = &s->hostmem[0]->mr;
    }
    if (s->hostmem[1]) {
        s->mr[1] = &s->hostmem[1]->mr;
    }
#else
    if (s->shmem[0] && s->shmem[0]->ivshmem_bar2) {
        s->mr[0] = s->shmem[0]->ivshmem_bar2;
    }
#endif

#ifdef PERI_DEDUP
   delta_snap_init(s->fuzzer_id, s->chkpt_pool_size);
   cp_root.snapshot.n_peri_rb = create_prb_and_fill(&cp_root.snapshot.peri_rb, NULL, SNAPSHOT_ROOT_ID, false);
#endif

#ifdef FUZZER_TEST
    fuzzer_test(s);
#endif
}

static void fuzzer_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->props = fuzzer_properties;
    dc->realize = fuzzer_realize;
}

static const TypeInfo fuzzer_type = {
    .name = TYPE_FUZZER,
    .parent = TYPE_DEVICE,
    .class_size = sizeof(FuzzerClass),
    .instance_size = sizeof(FuzzerState),
    .class_init = fuzzer_class_init,
};

static void register_fuzzer_types(void)
{
    type_register_static(&fuzzer_type);
}

type_init(register_fuzzer_types);
