#include "qemu/osdep.h"
#include "qapi/error.h"

#include <sys/shm.h>

#include "periscope.h"

static char *exec_input = NULL;
static int exec_input_used_len = 0;
static int exec_input_size = -1;

static int executor_mmio_read(unsigned size, uint64_t *out) {
    if (exec_input == NULL) {
        printf("periscope: no exec input\n");
        return -1;
    }

    if (exec_input_used_len + size > exec_input_size) {
        printf("periscope: exec input fully consumed\n");
        *out = 0;
        return 0;
    }

    exec_input_used_len += size;

    printf("periscope: exec input \"");

    FuzzerState *s = fuzzer_get_current();
    assert(s != NULL);

    periscope_input_desc *cur = s->cur_input;
    assert(cur != NULL);

    switch (size) {
    case 1:
        if (cur->used_len + size <= exec_input_size) {
            *out = *((uint8_t*)&exec_input[cur->used_len]);
            cur->used_len += size;
        }
        break;
    case 2:
        if (cur->used_len + size <= exec_input_size) {
            *out = *((uint16_t*)&exec_input[cur->used_len]);
            cur->used_len += size;
        }
        break;
    case 4:
        if (cur->used_len + size <= exec_input_size) {
            *out = *((uint32_t*)&exec_input[cur->used_len]);
            cur->used_len += size;
        }
        break;
    case 8:
        if (cur->used_len + sizeof(uint64_t) <= exec_input_size) {
            *out = *((uint64_t*)&exec_input[cur->used_len]);
            cur->used_len += sizeof(uint64_t);
        }
        break;
    default:
        printf("periscope: unexpected size!\n");
        if (cur->used_len + sizeof(uint64_t) <= exec_input_size) {
            *out = *((uint64_t*)&exec_input[cur->used_len]);
            cur->used_len += sizeof(uint64_t);
        }
        break;
    }

    printf("0x%lx\" consumed. (%d/%d bytes)\n", *out,
           exec_input_used_len,
           exec_input_size);

    return 0;
}

static char *executor_fetch_next(uint32_t* len) {
    printf("periscope: executor input fetch requested\n");
    *len = exec_input_size;
    return exec_input;
}

void start_input_executor(const char *uri, Error **errp) {
    FuzzerState *s = fuzzer_get_current();
    assert(s != NULL);

    int fd = qemu_open(uri, O_RDONLY|O_BINARY);
    if (fd < 0) {
        printf("periscope: exec file open failed\n");
        return;
    }

    struct stat st;
    stat(uri, &st);

    exec_input_size = st.st_size;

    printf("periscope: executing input %s (size: %u)\n", uri, exec_input_size);

    exec_input = mmap(NULL, exec_input_size, PROT_READ,
                      MAP_PRIVATE | MAP_POPULATE,
                      fd, 0);
    if (!exec_input) {
        printf("periscope: exec input mmap failed\n");
    }

    // HACK
    s->cur_input->input = exec_input;
    s->cur_input->len = exec_input_size;
    s->cur_input->used_len = 0;

    s->mode = PERISCOPE_MODE_EXEC;
    s->mmio_read = executor_mmio_read;
    s->fetch_next = executor_fetch_next;
    s->get_cur = NULL;
    s->cur_executed = NULL;
}
