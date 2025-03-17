#ifndef PERISCOPE_PERF_SWITCHES
#define PERISCOPE_PERF_SWITCHES

//#define PERI_ENABLE_RESTORE_OPTS_RESET_DEVS
//#define PERI_ENABLE_RESTORE_OPTS_RESET_RAM

extern bool periscope_no_loadvm_state_setup;
extern bool periscope_no_loadvm_state_cleanup;

// if set:
// * skips addition of ram pages to snapshot file -> empty snapshot
extern bool quick_snapshot;
static inline void set_quick_snapshot(void) {
#ifdef PERI_ENABLE_RESTORE_OPTS_RESET_RAM
   quick_snapshot = true;
#endif
}
static inline void unset_quick_snapshot(void) {
   quick_snapshot = false;
}

// if set:
// * skips device->reset and device->post_load
extern bool quick_reset_devs;
// * resets MAP_PRIVATE ram mappings instead of doing a full snapshot restore
extern bool quick_reset_ram;

static inline void set_quick_reset_devs(void) {
#ifdef PERI_ENABLE_RESTORE_OPTS_RESET_DEVS
   quick_reset_devs = true;
#endif
}
static inline void unset_quick_reset_devs(void) {
   quick_reset_devs = false;
}

static inline void set_quick_reset_ram(void) {
#ifdef PERI_ENABLE_RESTORE_OPTS_RESET_RAM
   quick_reset_ram = true;
#endif
}
static inline void unset_quick_reset_ram(void) {
   quick_reset_ram = false;
}

#endif
