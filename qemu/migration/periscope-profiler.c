/// Before running this, DISABLE zero-page opt by adding the following code to save_zero_page_to_file
///     if (strcmp(block->idstr, "pc.ram") == 0)
///         return 0;


#include "qemu/osdep.h"

#include "hw/boards.h"
#include "cpu.h"
#include "exec/ram_addr.h"
#include "io/channel-file.h"
#include "io/channel-buffer.h"
#include "migration/migration.h"
#include "migration/qemu-file.h"
#include "migration/savevm.h"
#include "migration/global_state.h"
#include "qemu-file-channel.h"

#include "migration/periscope.h"
#include "migration/periscope_perf_switches.h"

enum {
    PERISCOPE_PROFILE_CHECKPOINT = 1,
    PERISCOPE_PROFILE_RESTORE,
    PERISCOPE_PROFILE_BASELINE,
};

#define DIRTY_PAGE_RANGE_BEGIN 1024 // 4MB worh of dirty pages
#define DIRTY_PAGE_RANGE_INCR 1024
#define DIRTY_PAGE_RANGE_MAX 1024 * 512 / 8 // 512MB

static int profiler_type = 0;
static int profiler_mode = PERISCOPE_PROFILE_CHECKPOINT;
static int num_dirty_pages = DIRTY_PAGE_RANGE_BEGIN;

unsigned char dummy[1024];
unsigned char dummy_val = 0x1;
int used_len = 0;

static char *fetch_next(uint32_t *len)
{
    if (dummy_val == 0xff) {
        dummy_val = 0x1;
    }
    if (profiler_mode == PERISCOPE_PROFILE_RESTORE) {
        dummy_val = 0xff; // give a value that never get matched with any checkpoint
    }
    memset(dummy, dummy_val++, sizeof(dummy));
    *len = sizeof(dummy);
    return (char *)dummy;
}

static int snapshot_and_restore_requested = 0;

static void snapshot_and_restore_request(void)
{
    vm_stop(RUN_STATE_SAVE_VM);

    snapshot_and_restore_requested = 1;
}

int periscope_snapshot_and_restore_baseline_requested(void)
{
    return snapshot_and_restore_requested;
}

#define USE_CHANNEL_BUFFER

void periscope_snapshot_and_restore_baseline(void)
{
    snapshot_and_restore_requested = 0;

    Error *err = NULL;
    int ret = -1;

    QEMUFile *file;
#ifdef USE_CHANNEL_BUFFER
    QIOChannelBuffer *iochannel;
#else
    QIOChannelFile *iochannel;
    int memfd = -1;
#endif

    qemu_timeval tv1, tv2, tv3, sub;

    vm_stop(RUN_STATE_SAVE_VM);
    global_state_store_running();

    // snapshot
    qemu_gettimeofday(&tv1);

#ifdef USE_CHANNEL_BUFFER
    MachineState *machine = MACHINE(qdev_get_machine());
    assert(machine != NULL);
    size_t channel_buffer_size = machine->ram_size + 32*1024*1024;
    iochannel = qio_channel_buffer_new(channel_buffer_size);
    file = qemu_fopen_channel_output(QIO_CHANNEL(iochannel));
#else
    memfd = memfd_create("baseline", 0);
    int dupfd = dup(memfd);

    iochannel = qio_channel_file_new_fd(dupfd);
    file = qemu_fopen_channel_output(QIO_CHANNEL(iochannel));
#endif
    //iochannel = NULL;

    ret = qemu_savevm_state(file, &err);
    qemu_gettimeofday(&tv2);

    qemu_fflush(file);
    size_t file_sz = qemu_ftell(file);
#ifdef USE_CHANNEL_BUFFER
    char *tmp = g_malloc(file_sz);
    memcpy(tmp, iochannel->data, file_sz);
#endif
    object_unref(OBJECT(iochannel));
    iochannel = NULL;
    qemu_fclose(file);
    file = NULL;

    if (ret < 0) {
        printf("periscope: benchmark-baseline-snapshot failed\n");
        goto exit;
    }

    timersub(&tv2, &tv1, &sub);

    printf("periscope: benchmark-baseline-snapshot took %lu ms\n",
           sub.tv_sec * 1000L + sub.tv_usec / 1000L);

    printf("periscope: benchmark-baseline-snapshot consumed %lu MiB\n",
           file_sz/1024UL/1024UL);

    vm_stop(RUN_STATE_RESTORE_VM);

    // restore
#ifdef USE_CHANNEL_BUFFER
    iochannel = qio_channel_buffer_new(file_sz);
    iochannel->usage = file_sz;
    memcpy(iochannel->data, tmp, file_sz);
    g_free(tmp);
#else
    lseek(memfd, 0, SEEK_SET);
    iochannel = qio_channel_file_new_fd(dup(memfd));
#endif
    file = qemu_fopen_channel_input(QIO_CHANNEL(iochannel));
    MigrationIncomingState *mis = migration_incoming_get_current();
    mis->from_src_file = file;

    qemu_gettimeofday(&tv2);
    ret = qemu_loadvm_state(file);
    qemu_gettimeofday(&tv3);

    mis->from_src_file = NULL;
    migration_incoming_state_destroy();

    qemu_fflush(file);
    qemu_fclose(file);
    file = NULL;

    if (ret < 0) {
        printf("periscope: benchmark-baseline-restore failed\n");
        goto exit;
    }

    timersub(&tv3, &tv2, &sub);

    printf("periscope: benchmark-baseline-restore took %lu ms\n",
           sub.tv_sec * 1000L + sub.tv_usec / 1000L);

exit:
    if (iochannel) {
        object_unref(OBJECT(iochannel));
        iochannel = NULL;
    }

#ifndef USE_CHANNEL_BUFFER
    close(memfd);
    memfd = -1;
#endif

    vm_start();
}

static bool ensure_dirty_pages(int in_num_dirty_pages, int *real_dirty_pages)
{
    //size_t sz = in_num_dirty_pages * TARGET_PAGE_SIZE;
    RAMBlock *rb = qemu_ram_block_by_name("pc.ram");
    assert(rb);
    MemoryRegion *mr = rb->mr;
    assert(mr);

    unsigned long *dirty = NULL;
    unsigned long num_pages = get_current_delta_bm(&dirty);
    assert(dirty != NULL);

    unsigned long num_dirty_pages = bitmap_count_one(dirty, num_pages);

    unsigned long pgidx = 0;
    while (num_dirty_pages < in_num_dirty_pages) {
        while (test_bit(pgidx, dirty) != 0) {
            pgidx++;
        }
        memory_region_set_dirty(mr, pgidx*TARGET_PAGE_SIZE, TARGET_PAGE_SIZE);
        pgidx++;
        num_dirty_pages++;
    }

    g_free(dirty);
    dirty = NULL;

    if (real_dirty_pages) {
        num_pages = get_current_delta_bm(&dirty);
        assert(dirty != NULL);

        num_dirty_pages = bitmap_count_one(dirty, num_pages);
        g_free(dirty);
        dirty = NULL;

        *real_dirty_pages = num_dirty_pages;
    }
    return true;
}

#define CHKPT_TRIALS 10
static int chkpt_trials = CHKPT_TRIALS;

#define RESTORE_TRIALS 10
static int restore_trials = RESTORE_TRIALS;

static int baseline_trials = 10;

void periscope_benchmark_hypercall(uint64_t arg)
{
    printf("periscope: benchmark hypercall\n");

    int num_real_dirty_pages;

    switch (profiler_mode) {
    case PERISCOPE_PROFILE_CHECKPOINT:
        if (!periscope_snapshot_inited()) {
            periscope_maybe_checkpoint_request();
            periscope_restore_request();
            break;
        }

        if (ensure_dirty_pages(num_dirty_pages, &num_real_dirty_pages)) {
            printf("periscope: benchmark-checkpoint %d dirty pages\n",
                   num_real_dirty_pages);

            FuzzerState *fs = fuzzer_get_current();
            assert(fs);
            assert(fs->cur_cp);
            fs->cur_cp->len = 1;
            memcpy(fs->cur_cp->io, dummy + used_len, 1);

            assert(fs->cur_input);
            fs->cur_input->used_len++;

            assert(fs->cur_input->used_len <= sizeof(dummy));

            periscope_purge_and_checkpoint_request();
            periscope_restore_request();

            chkpt_trials--;
            if (chkpt_trials == 0) {
                num_dirty_pages += DIRTY_PAGE_RANGE_INCR;
                chkpt_trials = RESTORE_TRIALS;
            }

            if (num_dirty_pages > DIRTY_PAGE_RANGE_MAX) {
                num_dirty_pages = DIRTY_PAGE_RANGE_BEGIN;
                profiler_mode = PERISCOPE_PROFILE_RESTORE;
            }
        } else {
            periscope_restore_request();
        }
        break;

    case PERISCOPE_PROFILE_RESTORE:
        if (ensure_dirty_pages(num_dirty_pages, &num_real_dirty_pages)) {
            printf("periscope: benchmark-restore %d dirty pages\n",
                   num_real_dirty_pages);

            periscope_restore_request();

            restore_trials--;
            if (restore_trials == 0) {
                num_dirty_pages += DIRTY_PAGE_RANGE_INCR;
                restore_trials = RESTORE_TRIALS;
            }

            if (num_dirty_pages > DIRTY_PAGE_RANGE_MAX) {
                num_dirty_pages = DIRTY_PAGE_RANGE_BEGIN;
                profiler_mode = -1;
            }
        } else {
            periscope_restore_request();
        }
        break;

    case PERISCOPE_PROFILE_BASELINE:
        // make sure these are turned off for baseline benchmarking
        periscope_no_loadvm_state_setup = false;
        periscope_no_loadvm_state_cleanup = false;

        if (baseline_trials > 0) {
            snapshot_and_restore_request();
            baseline_trials--;
            break;
        }
        profiler_mode = -1;
        break;

    default:
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_RESET);
        break;
    }
}

void start_profiler(int type, Error **errp)
{
    FuzzerState *s = fuzzer_get_current();
    assert(s != NULL);

    profiler_type = type;

    switch (profiler_type) {
#define PROF_MICRO 0
#define PROF_BASELINE 1
    case PROF_MICRO:
        profiler_mode = PERISCOPE_PROFILE_CHECKPOINT;
        break;
    case PROF_BASELINE:
        profiler_mode = PERISCOPE_PROFILE_BASELINE;
        periscope_delta_snapshot = false;
        periscope_save_ram_only = false;
        periscope_no_loadvm_state_setup = false;
        periscope_no_loadvm_state_cleanup = false;
        quick_snapshot = false;
        quick_reset_devs = false;
        quick_reset_ram = false;
        break;
    }

    s->mode = PERISCOPE_MODE_PROFILER;
    s->mmio_read = NULL;
    s->fetch_next = fetch_next;
    s->get_cur = NULL;
    s->cur_executed = NULL;
    s->restored = NULL;
    s->should_restore = NULL;
    s->guest_crashed = NULL;
    s->get_stat = NULL;

    printf("periscope: profiler initialized\n");
}
