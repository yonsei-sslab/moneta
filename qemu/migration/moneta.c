
#include "qemu/osdep.h"
#include "migration/snapshot.h"
#include "moneta.h"
#include "periscope.h"

#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define MAX_SNAPSHOT_COUNT 10

bool loadvm_tick = false;
bool post_rehost_tick = false;
int snapshot_count = 0;

int moneta_savevm(void) {

    printf("QEMU: moneta: Entered moneta_savevm()..\n");
    if(!periscope_snapshot_inited() && !moneta_loadvm_check()) {
        printf("QEMU: moneta: conditions are OK. It will take some time..\n");
        if (snapshot_count < MAX_SNAPSHOT_COUNT) {
            Error *err = NULL;
            char str[15] = "moneta";
            snapshot_count++;
            snprintf(str, sizeof(str), "moneta");
            printf("QEMU: moneta: saving snapshot, name: moneta\n", str);
            save_snapshot(str, &err);
            if (err) {
                printf("QEMU: moneta: savevm failed\n");
                return -1;
            }

        }
    }

    return 0;
}

int moneta_savevm_post_rehost(void) {
    if(!periscope_snapshot_inited()) {
        snapshot_inited = true;
        printf("QEMU: moneta: conditions are OK. It will take some time..\n");
        Error *err = NULL;
        char str[15] = "post-rehost";
        printf("QEMU: moneta: saving snapshot with name %s\n", str);
        save_snapshot(str, &err);
        if (err) {
            printf("QEMU: moneta: savevm failed\n");
            return -1;
        }
    return 0;
    }
}

bool moneta_loadvm_tick(void) {
    loadvm_tick = true;
    return 0;
}

bool moneta_loadvm_check(void) {
    return loadvm_tick;
}