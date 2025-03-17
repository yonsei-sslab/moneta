#include "qemu/osdep.h"
#include "qapi/error.h"

#include <sys/shm.h>

#include "periscope.h"
#include "hw/periscope/kcov_vdev.h"

#define MAX_CORPUS_ARGS 10

static int total_corpus_paths = 0;
static int cur_corpus_path = -1;
static char *corpus_paths[MAX_CORPUS_ARGS];
static char *corpus_blob = NULL;
static int corpus_blob_size = 0;

static DIR *open_dir = NULL;
static char *open_dirname = NULL;
static struct dirent *dp = NULL;

static int kcov_mmio_read(unsigned size, uint64_t *out) {
    FuzzerState *s = fuzzer_get_current();
    assert(s != NULL);

    periscope_input_desc *cur = s->cur_input;
    assert(cur != NULL);

    kcov_flush_area(false, 0);

    *out = 0;

    switch (size) {
    case 1:
        if (cur->used_len + size <= corpus_blob_size) {
            *out = *((uint8_t*)&corpus_blob[cur->used_len]);
            cur->used_len += size;
        }
        break;
    case 2:
        if (cur->used_len + size <= corpus_blob_size) {
            *out = *((uint16_t*)&corpus_blob[cur->used_len]);
            cur->used_len += size;
        }
        break;
    case 4:
        if (cur->used_len + size <= corpus_blob_size) {
            *out = *((uint32_t*)&corpus_blob[cur->used_len]);
            cur->used_len += size;
        }
        break;
    case 8:
        if (cur->used_len + sizeof(uint64_t) <= corpus_blob_size) {
            *out = *((uint64_t*)&corpus_blob[cur->used_len]);
            cur->used_len += sizeof(uint64_t);
        }
        break;
    default:
        printf("periscope: unexpected size!\n");
        if (cur->used_len + sizeof(uint64_t) <= corpus_blob_size) {
            *out = *((uint64_t*)&corpus_blob[cur->used_len]);
            cur->used_len += sizeof(uint64_t);
        }
        break;
    }

    return 0;
}

static void kcov_maybe_exit(void) {
    if (cur_corpus_path >= total_corpus_paths) {
        printf("periscope: no corpus left. \n");

        if (open_dir) {
            closedir(open_dir);
            open_dir = NULL;
        }

        exit(0);
    }
}

static char *kcov_fetch_next(uint32_t* len) {
    char cpath[1024];
    struct stat cpath_stat;

    if (open_dir) {
        while ((dp = readdir(open_dir)) != NULL) {
            if (dp->d_type == DT_REG) {
                sprintf(cpath, "%s/%s", open_dirname, dp->d_name);
                stat(cpath, &cpath_stat);
                break;
            }
        }

        if (dp == NULL) {
            closedir(open_dir);
            open_dir = NULL;
        }
    }
    
    if (open_dir == NULL) {
        cur_corpus_path++;
        kcov_maybe_exit();
        strncpy(cpath, corpus_paths[cur_corpus_path], sizeof(cpath));
    }

    // Prune invalid paths.
    while (stat(cpath, &cpath_stat) < 0) {
        printf("periscope: wrong corpus path '%s'. skipping...\n", cpath);
        cur_corpus_path++;
        kcov_maybe_exit();
        strncpy(cpath, corpus_paths[cur_corpus_path], sizeof(cpath));
    }

    // We ignore sub-directories of the given directories.
    if (open_dir == NULL && S_ISDIR(cpath_stat.st_mode)) {
        printf("periscope: opening corpus directory %s.\n", cpath);
        open_dir = opendir(cpath);
        open_dirname = corpus_paths[cur_corpus_path];
        dp = NULL;
        return kcov_fetch_next(len);
    }

    // cpath must refer to a regular file at this point.
    if (!S_ISREG(cpath_stat.st_mode)) {
        cur_corpus_path++;
        kcov_maybe_exit();
        strncpy(cpath, corpus_paths[cur_corpus_path], sizeof(cpath));
    }

    int cfd = qemu_open(cpath, O_RDONLY|O_BINARY);

    printf("periscope: opened corpus file '%s' (fd=%d, len=%ld)\n",
           cpath, cfd, cpath_stat.st_size);

    if (corpus_blob != NULL) {
        munmap(corpus_blob, cpath_stat.st_size);
    }

    corpus_blob = mmap(NULL, cpath_stat.st_size, PROT_READ,
                       MAP_PRIVATE | MAP_POPULATE,
                       cfd, 0);
    corpus_blob_size = cpath_stat.st_size;

    *len = cpath_stat.st_size;

    return corpus_blob;
}

// This function returns a correct result only if the trace-pc option was used.
static void kcov_cur_executed(uint8_t *trace_pcs, uint32_t used_len, uint64_t elapsed_ms, bool timed_out) {
    if (!trace_pcs) return;

    kcov_flush_area(false, -1);
}

void start_coverage_collector(const char *args, Error **errp) {
    printf("periscope: initializing coverage collector\n");

    char tmp[1024];
    strncpy(tmp, args, sizeof(tmp));

    char *path = strtok(tmp, ",");

    while (path && total_corpus_paths < MAX_CORPUS_ARGS) {
        corpus_paths[total_corpus_paths] = strdup(path);
        total_corpus_paths++;

        printf("periscope: corpus path '%s'\n", path);

        path = strtok(NULL, ",");
    }

    if (total_corpus_paths < 0) {
        printf("periscope: no corpus dir/file paths given.\n");
        exit(1);
    }

    FuzzerState *s = fuzzer_get_current();
    assert(s != NULL);

    s->mode = PERISCOPE_MODE_COVERAGE;
    s->mmio_read = kcov_mmio_read;
    s->fetch_next = kcov_fetch_next;
    s->get_cur = NULL;
    s->cur_executed = kcov_cur_executed;
}
