#ifndef PERISCOPE_DELTA_SNAP_H
#define PERISCOPE_DELTA_SNAP_H

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"

#define ENABLE_LW_CHKPT
//#undef ENABLE_LW_CHKPT

// TODO: remove
extern bool periscope_delta_snapshot;
extern bool periscope_save_ram_only;

struct periscope_ramblock;

unsigned long count_unique_pages(void);
unsigned long count_dirty_pages(void);
unsigned long count_hashed_pages_prbs(struct periscope_ramblock *prbs, unsigned int nprb);
unsigned long count_stored_pages_prbs(struct periscope_ramblock *prbs, unsigned int nprb);
unsigned long count_zero_pages_prbs(struct periscope_ramblock *prbs, unsigned int nprb);
unsigned long count_skipped_pages_prbs(struct periscope_ramblock *prbs, unsigned int nprb);
//void delta_snap_init(MemoryRegion *mem_pool, MemoryRegion *mem_meta, int id);
void delta_snap_init(int id, unsigned long chkpt_pool_size);
uint64_t chkpt_memory_free(void);
// compute cost of stored data for all ramblocks
unsigned long compute_hashmap_cost(void);
unsigned long compute_total_hashmap_cost(void);
unsigned long compute_hash_cost(struct periscope_ramblock *prbs, unsigned int nprb);
unsigned long compute_prb_cost(struct periscope_ramblock *prbs, unsigned int nprb);
unsigned long compute_prb_freed(struct periscope_ramblock *prb);
uint64_t get_ram_page_pool_size(void);
uint64_t get_rpp_hashmap_size(void);
uint64_t get_free_pages(void);
uint64_t compute_dirty_cost(unsigned long num_dirty_pages);
// delete all periscope ramblock data stored in the peri_rb array
void delete_peri_rbs(struct periscope_ramblock *prbs, unsigned int nprb, struct periscope_ramblock *prbs_parent);
// Go through all ramblocks and create a periscope ramblock entry for each one
// copy the current delta dirty bitmap of that ramblock in the peri_rb structure
// and store all the dirty pages
int create_prb_and_clear_delta_bm(struct periscope_ramblock **prbs, unsigned long *num_dirty_pages, int id);
// Go through all ramblocks and create a periscope ramblock entry for each one
// fill the dirty bitmap of each new entry and copy the complete ramblock data
unsigned int create_prb_and_fill(struct periscope_ramblock **prbs, unsigned long *num_dirty_pages, int id, bool store_ram);
struct periscope_ramblock *get_ramblock(struct periscope_ramblock *prbs, unsigned int nprb, const char *name);
// restore stored ram pages
void restore_ram_pages(RAMBlock *rb, struct periscope_ramblock *prb, unsigned long* dirty);
float compute_uniqueness(struct periscope_ramblock *prbs, unsigned int nprb);

unsigned long get_current_delta_bm(unsigned long **dirty);
unsigned long get_max_new_pages(void);
int grow_ram_page_pool(unsigned long max_new);


unsigned long update_and_clear_delta_snap_bm(unsigned long ** dirty);
// refresh bmap_delta_snap from kvm and clear kvm bitmap
// copy bmap_delta_snap into **dirty
// it **dirty is null, it will be allocated
// but do not zero bmap_delta_snap
//unsigned long get_current_delta_bm(unsigned long **dirty);
// copy bmap_delta_snap (managed by qemu into *dirty
int update_delta_snap_bm(unsigned long *dirty, unsigned long npages);
unsigned long *copy_fine_bitmap(unsigned long *bm, unsigned long npages);
unsigned long *new_fine_bitmap(unsigned long npages);

#endif
