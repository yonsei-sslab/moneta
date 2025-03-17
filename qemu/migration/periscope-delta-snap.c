#include "periscope-delta-snap.h"
#include "hw/boards.h"
#include "qemu/osdep.h"
#include "cpu.h"
#include "migration/periscope.h"
#include "migration/ram.h"
#include "exec/ram_addr.h"

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


// this flag enables selective storing of dirty pages in ram_save ...
#ifdef ENABLE_LW_CHKPT
bool periscope_delta_snapshot = true;
#else
bool periscope_delta_snapshot = false;
#endif
bool periscope_save_ram_only = false;

struct DirtyBitmapSnapshot {
    ram_addr_t start;
    ram_addr_t end;
    unsigned long dirty[];
};

unsigned long compute_prb_cost(periscope_ramblock *prbs, unsigned int nprb) {
   unsigned long cost = 0;
   for(unsigned int i=0; i<nprb; ++i) {
      cost += prbs[i].npages_stored * TARGET_PAGE_SIZE;
#ifdef DBG_RAM_STORED
      cost += prbs[i].rambkp_size;
#endif
   }
   return cost;
 }

void restore_ram_pages(RAMBlock *rb, void* ram, unsigned long* offsets,
    unsigned long npages_stored, unsigned long npages, unsigned long* dirty) {
    rcu_read_lock();
    assert(rb);
    unsigned long page_index = 0;
    unsigned long pages_restored = 0;
    for(unsigned long i=0; i<BITS_TO_LONGS(npages); ++i) {
       if(dirty[i] == 0) continue;
       unsigned long mask = dirty[i];
       for(int mask_offset = 0; mask_offset < BITS_PER_LONG; ++mask_offset) {
          if((mask & (1ul<<mask_offset)) != 0) {
             unsigned long page = ((i * BITS_PER_LONG) + mask_offset) ;
             unsigned long offset = page << TARGET_PAGE_BITS;
             // skip to the next page to be restored
             for(unsigned long restore_page = page_index;
                   restore_page < npages_stored; ++restore_page) {
                if(offsets[restore_page] == offset) {
                   page_index = restore_page;
                   break;
                }
             }
             assert(page_index < npages_stored);
             void *host = host_from_ram_block_offset(rb, offset);
             assert(host);
             //printf("restoring offset %#lx, page_index %#lx\n", offset, page_index);
             memcpy(host,
                   ram + (page_index * TARGET_PAGE_SIZE),
                   TARGET_PAGE_SIZE);
             page_index++;
             pages_restored++;
          }
       }
    }
    rcu_read_unlock();
    printf("restored %ld pages\n", pages_restored);
}

static void store_ram_pages(RAMBlock *rb, void* ram, unsigned long* offsets,
    unsigned long npages_dirty, unsigned long npages, unsigned long* dirty) {
    rcu_read_lock();
    assert(rb);
    printf("%s: %s\n", __FUNCTION__, rb->idstr);
    unsigned long pages_stored = 0;
    for(unsigned long i=0; i<BITS_TO_LONGS(npages); ++i) {
       if(dirty[i] == 0) continue;
       unsigned long mask = dirty[i];
       for(int mask_offset = 0; mask_offset < BITS_PER_LONG; ++mask_offset) {
          if((mask & (1ul<<mask_offset)) != 0) {
             unsigned long page = ((i * BITS_PER_LONG) + mask_offset) ;
             unsigned long offset = page << TARGET_PAGE_BITS;
             void *host = host_from_ram_block_offset(rb, offset);
             if(!host) {
                printf("offset %lx, host %lx, %lx\n", offset, rb->max_length, rb->used_length);
             }
             assert(host);
             assert(pages_stored < npages_dirty);
             offsets[pages_stored] = offset;
             memcpy(ram + (pages_stored * TARGET_PAGE_SIZE),
                   host, TARGET_PAGE_SIZE);
             pages_stored++;
          }
       }
    }
   assert(pages_stored == npages_dirty);
   rcu_read_unlock();
}

periscope_ramblock *get_ramblock(periscope_ramblock *prbs, unsigned int nprb, const char *name) {
   for(unsigned int i=0; i<nprb; ++i) {
      if(strcmp(name, prbs[i].idstr) == 0)
         return &prbs[i];
   }
   return NULL;
}

static periscope_ramblock *delete_ramblock(periscope_ramblock *prb) {
   printf("Deleting peri_rb %s\n", prb->idstr);
   if(prb->dirty) {
      g_free(prb->dirty);
      prb->dirty = NULL;
   }
   prb->npages = 0;
#ifdef DBG_RAM_STORED
   if(prb->rambkp) {
      g_free(prb->rambkp);
      prb->rambkp = NULL;
   }
   prb->rambkp_size = 0;
#endif
   prb->npages_stored = 0;
   if(prb->offsets){
      g_free(prb->offsets);
      prb->offsets = NULL;
   }
   if(prb->ram) {
      g_free(prb->ram);
      prb->ram = NULL;
   }
   return prb;
}
void delete_peri_rbs(periscope_ramblock *prb, unsigned int nprb) {
   assert(prb);
   printf("Deleting %d peri_rbs\n", nprb);
   for(unsigned int i=0; i<nprb; ++i) {
      delete_ramblock(&prb[i]);
   }
}

static void create_ramblock(char *name, periscope_ramblock *prb) {
   assert(name);
   assert(prb);
   prb->dirty = NULL;
   prb->npages = 0;
#ifdef DBG_RAM_STORED
   prb->rambkp = NULL;
   prb->rambkp_size = 0;
#endif
   prb->npages_stored = 0;
   prb->offsets = NULL;
   prb->ram = NULL;
   strcpy(prb->idstr, name);
}

// must hold rcu and ramlist lock
static unsigned int count_ramblocks(void) {
   RAMBlock *rb;
   unsigned int nrb = 0;
   RAMBLOCK_FOREACH_MIGRATABLE(rb) {
      nrb++;
   }
   return nrb;
}


unsigned int create_prb_and_fill(periscope_ramblock **prbs) {
   RAMBlock *rb;
   qemu_mutex_lock_ramlist();
   rcu_read_lock(); // see comment on INTERNAL_RAMBLOCK_FOREACH

   unsigned int nrb = count_ramblocks();
   printf("%s: #ramblocks %d\n", __FUNCTION__, nrb);

   *prbs = g_malloc(sizeof(periscope_ramblock) * nrb);
   assert(*prbs);

   unsigned int rbs_idx = 0;

   RAMBLOCK_FOREACH_MIGRATABLE(rb) {
      periscope_ramblock *prb = &((*prbs)[rbs_idx++]);
      create_ramblock(rb->idstr, prb);

      unsigned long npages_snap = (rb->max_length) >> TARGET_PAGE_BITS;
      prb->dirty = bitmap_new(npages_snap);
      bitmap_fill(prb->dirty, rb->used_length >> TARGET_PAGE_BITS);
      prb->npages = npages_snap;
#ifdef DBG_RAM_STORED
      prb->rambkp = g_malloc(rb->max_length);
      prb->rambkp_size = rb->max_length;
      memcpy(prb->rambkp, rb->host, rb->max_length);
#endif
      unsigned long npages_dirty = bitmap_count_one(prb->dirty, npages_snap);
      prb->offsets = g_malloc(npages_dirty * sizeof(unsigned long));
      prb->ram = g_malloc(npages_dirty * TARGET_PAGE_SIZE);
      store_ram_pages(rb, prb->ram, prb->offsets, npages_dirty, npages_snap, prb->dirty);
      prb->npages_stored = npages_dirty;
   }

   rcu_read_unlock();
   qemu_mutex_unlock_ramlist();
   assert(rbs_idx == nrb);
   return nrb;
}

unsigned int create_prb_and_clear_delta_bm(periscope_ramblock **prbs) {
   RAMBlock *rb;
   qemu_mutex_lock_ramlist();
   rcu_read_lock(); // see comment on INTERNAL_RAMBLOCK_FOREACH

   unsigned int nrb = count_ramblocks();
   printf("%s: #ramblocks %d\n", __FUNCTION__, nrb);

   *prbs = g_malloc(sizeof(periscope_ramblock) * nrb);
   assert(*prbs);


   unsigned int rbs_idx = 0;

   RAMBLOCK_FOREACH_MIGRATABLE(rb) {
      periscope_ramblock *prb = &((*prbs)[rbs_idx++]);
      create_ramblock(rb->idstr, prb);

      struct DirtyBitmapSnapshot *snap = memory_region_snapshot_and_clear_dirty(
            rb->mr,
            0, rb->max_length,
            DIRTY_MEMORY_DELTA
            );
      unsigned long npages_snap = (snap->end - snap->start) >> TARGET_PAGE_BITS;
      printf("%s: update bm %s %lx - %lx\n", __FUNCTION__, rb->idstr, snap->start, snap->end);
      prb->dirty = bitmap_new(npages_snap);
      bitmap_copy(prb->dirty, snap->dirty, npages_snap);
      prb->npages = npages_snap;
#ifdef DBG_RAM_STORED
      prb->rambkp = g_malloc(rb->max_length);
      prb->rambkp_size = rb->max_length;
      memcpy(prb->rambkp, rb->host, rb->max_length);
#endif
      unsigned long npages_dirty = bitmap_count_one(prb->dirty, npages_snap);
      prb->offsets = g_malloc(npages_dirty * sizeof(unsigned long));
      prb->ram = g_malloc(npages_dirty * TARGET_PAGE_SIZE);
      store_ram_pages(rb, prb->ram, prb->offsets, npages_dirty, npages_snap, prb->dirty);
      prb->npages_stored = npages_dirty;
      g_free(snap);
   }

   rcu_read_unlock();
   qemu_mutex_unlock_ramlist();
   assert(rbs_idx == nrb);
   return nrb;
}


// TODO: to be removed
// get the bitmap of currently diry pages
// this is used to determine which pages to RE-STORE
unsigned long get_current_delta_bm(unsigned long **dirty) {
    RAMBlock *rb;
    unsigned long npages_snap = 0;
    qemu_mutex_lock_ramlist();
    rcu_read_lock();

    MachineState *machine = MACHINE(qdev_get_machine());
    assert(machine != NULL);
    npages_snap = machine->ram_size / TARGET_PAGE_SIZE;

    if(dirty) {
       *dirty = bitmap_new(npages_snap);
       bitmap_zero(*dirty, npages_snap);
    }

    RAMBLOCK_FOREACH_MIGRATABLE(rb) {

        // for now only handle ram
        if (strcmp(rb->idstr, "pc.ram") != 0) {
            continue;
        }

        // sync dirty bitmap from kvm and copy to local buffer
        struct DirtyBitmapSnapshot *snap = memory_region_snapshot_and_get_dirty(
            rb->mr,
            0, rb->max_length,
            DIRTY_MEMORY_DELTA
        );

        printf("%s: update bm %lx - %lx\n", __FUNCTION__, snap->start, snap->end);
        bitmap_or(*dirty, *dirty, snap->dirty, npages_snap);
        g_free(snap);
    }

    rcu_read_unlock();
    qemu_mutex_unlock_ramlist();
    return npages_snap;
}

#if 0
unsigned long update_and_clear_delta_snap_bm(unsigned long ** dirty) {
    RAMBlock *rb;
    unsigned long npages_snap = 0;
    qemu_mutex_lock_ramlist();
    rcu_read_lock(); // see comment on INTERNAL_RAMBLOCK_FOREACH

    MachineState *machine = MACHINE(qdev_get_machine());
    assert(machine != NULL);
    npages_snap = machine->ram_size / TARGET_PAGE_SIZE;

    if(dirty) {
       *dirty = bitmap_new(npages_snap);
       bitmap_zero(*dirty, npages_snap);
    }

    RAMBLOCK_FOREACH_MIGRATABLE(rb) {

        // for now only handle ram
        if (strcmp(rb->idstr, "pc.ram") != 0) {
            continue;
        }

        struct DirtyBitmapSnapshot *snap = memory_region_snapshot_and_clear_dirty(
            rb->mr,
            0, rb->max_length,
            DIRTY_MEMORY_DELTA // TODO: do we need a custom flag?
        );
        //printf("%s: update bm %lx - %lx\n", __FUNCTION__, snap->start, snap->end);
        if(dirty)
           bitmap_or(*dirty, *dirty, snap->dirty, npages_snap);
        g_free(snap);
    }
    rcu_read_unlock();
    qemu_mutex_unlock_ramlist();
    return npages_snap;
}

// get the bitmap of currently diry pages
// this is used to determine which pages to RE-STORE
unsigned long get_current_delta_bm(unsigned long **dirty) {
    RAMBlock *rb;
    unsigned long npages_snap = 0;
    qemu_mutex_lock_ramlist();
    rcu_read_lock();

    MachineState *machine = MACHINE(qdev_get_machine());
    assert(machine != NULL);
    npages_snap = machine->ram_size / TARGET_PAGE_SIZE;

    if(dirty) {
       *dirty = bitmap_new(npages_snap);
       bitmap_zero(*dirty, npages_snap);
    }

    RAMBLOCK_FOREACH_MIGRATABLE(rb) {

        // for now only handle ram
        if (strcmp(rb->idstr, "pc.ram") != 0) {
            continue;
        }

        // sync dirty bitmap from kvm and copy to local buffer
        struct DirtyBitmapSnapshot *snap = memory_region_snapshot_and_get_dirty(
            rb->mr,
            0, rb->max_length,
            DIRTY_MEMORY_DELTA
        );

        //printf("%s: update bm %lx - %lx\n", __FUNCTION__, snap->start, snap->end);
        //npages_snap = (snap->end - snap->start) >> TARGET_PAGE_BITS;
        bitmap_or(*dirty, *dirty, snap->dirty, npages_snap);
        g_free(snap);
        //goto out;
        //return npages_snap;
    }

//out:
    rcu_read_unlock();
    qemu_mutex_unlock_ramlist();
    return npages_snap;
}

// call this before initiating checkpoint creation
// this will update ramblock->bmap_delta_snap
// ram_save_ ... will then only store pages which are
// marked as dirty in ramblock->bmap_delta_snap
int update_delta_snap_bm(unsigned long *dirty, unsigned long npages) {
    RAMBlock *rb;
    int ret = 1;

    qemu_mutex_lock_ramlist();
    rcu_read_lock();
    RAMBLOCK_FOREACH(rb) {
        // for now only handle ram
        if (strcmp(rb->idstr, "pc.ram") != 0) {
            continue;
        }
        if (rb->bmap_delta_snap == NULL) {
           unsigned long npages_rb = rb->max_length >> TARGET_PAGE_BITS;
           assert(npages_rb == npages);
           rb->bmap_delta_snap = bitmap_new(npages);
        }
        bitmap_copy(rb->bmap_delta_snap, dirty, npages);
        ret = 0;
        //goto out;
    }
//out:
    rcu_read_unlock();
    qemu_mutex_unlock_ramlist();
    return ret;
}
#endif
