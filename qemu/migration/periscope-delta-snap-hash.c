#include "periscope-delta-snap.h"
#include "hw/boards.h"
#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "cpu.h"
#include "migration/periscope.h"
#include "migration/ram.h"
#include "exec/ram_addr.h"

#ifdef PERI_DEDUP
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <semaphore.h>
#include <sys/mman.h>
#ifndef PERI_DEDUP_NOHASH
//#include "migration/c_hashmap/hashmap_custom.h"
//#include "migration/meow_hash_x64_aesni.h"
#endif
#endif


#define TRACE_TIME
#undef TRACE_TIME
#define TRACE_DEBUG
#undef TRACE_DEBUG
#define TRACE_DEBUG_STATS
#undef TRACE_DEBUG_STATS
#define TRACE_PAGES_STORED
#undef TRACE_PAGES_STORED
#define TEST_HASHING_OVERHEAD
#undef TEST_HASHING_OVERHEAD

#define CHUNK_SIZE (TARGET_PAGE_SIZE / CHUNK_DIV)
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

#ifdef PERI_DEDUP
extern bool buffer_is_zero(const void *buf, size_t len);
static bool is_zero_range(uint8_t *p, uint64_t size)
{
    return buffer_is_zero(p, size);
}

#ifndef PERI_DEDUP_NOHASH
#define INVALID_KEY _mm_set_epi64x(0xdeadbeefdeadbeef, 0xdeadbeefdeadbeef);
#ifdef FINE_CHUNKS
typedef struct ram_cache_meta {
   char idstr[256];
   meow_u128* cache_meta_key;
   int* cache_meta_id;
} ram_cache_meta;
static ram_cache_meta *ram_cache = NULL;
static unsigned int n_cache;


static ram_cache_meta* get_cache_meta(char* idstr) {
   for(unsigned int i=0; i<n_cache; ++i) {
      if(strcmp(ram_cache[i].idstr, idstr) == 0) {
         return &ram_cache[i];
      }
   }
   return NULL;
}
#endif /* FINE_CHUNKS */

#define MAX_NEW_UPDATE 0.1f
//#define MAX_NEW_PERC 0.1f
static long max_new_min = 2000 * CHUNK_DIV;
static long max_new_pages = 0;
static int fuzzer_id = 0;
static unsigned long page_pool_entries = 262144; // 1gb //fuzzer->chkpt_pool_size/getpagesize();
static map_t ram_page_pool_hashmap = NULL;

typedef struct ram_page_pool_info {
   unsigned int n_pages;
   unsigned int n_pages_free;
} ram_page_pool_info;
static ram_page_pool_info *rpp_info = NULL;
static void *ram_page_pool = NULL;
static unsigned int *ram_refc_pool = NULL;
static meow_u128 *ram_key_pool = NULL;
static unsigned long *ram_page_pool_free_bm = NULL;

uint64_t get_ram_page_pool_size(void) {
   return rpp_info->n_pages * CHUNK_SIZE;
}

uint64_t get_rpp_hashmap_size(void) {
   return hashmap_fixed_size(ram_page_pool_hashmap);
}

static void init_ram_page_pool(void *rpp_info_ptr, void *rpp_free_bm_ptr,
      void *ram_shared_ram_page_pool_ptr, unsigned long size, bool init) {
   if(rpp_info_ptr) {
      printf("Sharing ram_page_pool_info\n");
      rpp_info = (ram_page_pool_info*)rpp_info_ptr;
   } else {
      printf("Allocating ram_page_pool_info\n");
      rpp_info = (ram_page_pool_info*)g_malloc(sizeof(ram_page_pool_info));
   }
   printf("page pool map %p\n", rpp_info);
   page_pool_entries = size/CHUNK_SIZE;
   unsigned long bitmap_sz = BITS_TO_LONGS(page_pool_entries) * sizeof(unsigned long);
   if(init) {
      rpp_info->n_pages = page_pool_entries;
      rpp_info->n_pages_free = page_pool_entries;
   }
   if(ram_shared_ram_page_pool_ptr) {
      printf("Sharing ram_page_pool\n");
      ram_page_pool = ram_shared_ram_page_pool_ptr;
   } else {
      printf("Allocating ram_page_pool %ld\n", size);
      ram_page_pool = g_malloc(size);
      //madvise(ram_page_pool, size, MADV_HUGEPAGE | MADV_WILLNEED | MADV_SEQUENTIAL);
      ram_refc_pool = g_malloc(page_pool_entries * sizeof(unsigned int));
      memset(ram_refc_pool, 0, page_pool_entries * sizeof(unsigned int));
      ram_key_pool = g_malloc(page_pool_entries * sizeof(meow_u128));
      for(int i=0; i<page_pool_entries; ++i) {
         ram_key_pool[i] = INVALID_KEY; //_mm_setzero_si128();
      }
   }
   printf("Page pool %p/%p, size %ld, entries %ld\n", ram_page_pool, ram_shared_ram_page_pool_ptr, size, page_pool_entries);
   //ram_page_pool_free_bm = bitmap_new(page_pool_entries);
   if(rpp_free_bm_ptr) {
      printf("Sharing ram_page_pool_free_bm\n");
      ram_page_pool_free_bm = rpp_free_bm_ptr;
   } else {
      printf("Allocating ram_page_pool_free_bm\n");
      unsigned long long bitmap_sz = BITS_TO_LONGS(rpp_info->n_pages) * sizeof(unsigned long);
      ram_page_pool_free_bm = g_malloc(bitmap_sz); //bitmap_new(page_pool_entries);
   }
   printf("Bitmap page pool %p, size %lx\n", ram_page_pool_free_bm, bitmap_sz);
   if(init) {
      bitmap_zero(ram_page_pool_free_bm, page_pool_entries);
   }
}

static int next_free = 0;
static void delete_rpp_index(int idx) {
   //assert(idx >= 0 && idx < rpp_info->n_pages);
   if(!test_bit(idx, ram_page_pool_free_bm)) {
     printf("periscope: Error bit %d not set\n", idx);
     //assert(false);
   }
   clear_bit(idx, ram_page_pool_free_bm);
   rpp_info->n_pages_free++;
   next_free = 0;
}


static int set_free_rpp_index(meow_u128 key, void* host) {
   if(rpp_info->n_pages_free == 0) return -1;
   unsigned int idx = find_next_zero_bit(ram_page_pool_free_bm, rpp_info->n_pages, next_free);
   if(idx >= rpp_info->n_pages) {
      printf("%d, %d, %d, %d\n", rpp_info->n_pages, rpp_info->n_pages_free, idx, next_free);
      assert(false);
   }
   //assert(ram_refc_pool[idx] == 0);

   ram_refc_pool[idx] = 1;
   ram_key_pool[idx] = key;
   memcpy(ram_page_pool + (idx * CHUNK_SIZE), host, CHUNK_SIZE);

   //assert(idx < rpp_info->n_pages);
   //assert(!test_bit(idx, ram_page_pool_free_bm));
   next_free = idx + 1;
   if(next_free >= rpp_info->n_pages) {
      next_free = 0;
   }
   set_bit(idx, ram_page_pool_free_bm);
   rpp_info->n_pages_free--;
   return idx;
}

uint64_t get_free_pages(void) {
   return rpp_info->n_pages_free;
}

int grow_ram_page_pool(unsigned long max_new)
{
   next_free = rpp_info->n_pages;
   printf("%s by %ld\n", __FUNCTION__, max_new);
   unsigned int new_entries = max_new / CHUNK_SIZE;
   printf("%s new entries %d\n", __FUNCTION__, new_entries);
   unsigned long oldsize = rpp_info->n_pages * CHUNK_SIZE;
   unsigned long newsize = (new_entries + rpp_info->n_pages) * CHUNK_SIZE;
   //unsigned long oldsize_meta = rpp_info->n_pages * sizeof(page_meta);
   //unsigned long newsize_meta = (new_entries + rpp_info->n_pages) * sizeof(page_meta);
   unsigned long oldsize_refc = (rpp_info->n_pages) * sizeof(unsigned int);
   unsigned long newsize_refc = (new_entries + rpp_info->n_pages) * sizeof(unsigned int);
   //unsigned long oldsize_key = (rpp_info->n_pages) * sizeof(meow_u128);
   unsigned long newsize_key = (new_entries + rpp_info->n_pages) * sizeof(meow_u128);
   unsigned long bitmap_sz_old = BITS_TO_LONGS(rpp_info->n_pages) * sizeof(unsigned long);
   unsigned long bitmap_sz_new = BITS_TO_LONGS(rpp_info->n_pages + new_entries) * sizeof(unsigned long);
   printf("%s oldsize %ld, newsize %ld\n", __FUNCTION__, oldsize, newsize);
   printf("%s bitmap old_sz %ld, new_sz %ld\n", __FUNCTION__, bitmap_sz_old, bitmap_sz_new);
   ram_page_pool_free_bm = g_realloc(ram_page_pool_free_bm, bitmap_sz_new);
   assert(ram_page_pool_free_bm);
   memset((void*)ram_page_pool_free_bm + bitmap_sz_old, 0, bitmap_sz_new - bitmap_sz_old);
   ram_page_pool = g_realloc(ram_page_pool, newsize);
   assert(ram_page_pool);
   //madvise(ram_page_pool, newsize, MADV_HUGEPAGE | MADV_WILLNEED | MADV_SEQUENTIAL);
   ram_refc_pool = g_realloc(ram_refc_pool, newsize_refc);
   assert(ram_refc_pool);
   memset((void*)ram_refc_pool + oldsize_refc, 0, newsize_refc - oldsize_refc);
   ram_key_pool = g_realloc(ram_key_pool, newsize_key);
   assert(ram_key_pool);
   for(int i=rpp_info->n_pages; i<rpp_info->n_pages + new_entries; ++i) {
      ram_key_pool[i] = INVALID_KEY; //_mm_setzero_si128();
   }
   rpp_info->n_pages += new_entries;
   printf("%s old free %d\n", __FUNCTION__, rpp_info->n_pages_free);
   rpp_info->n_pages_free += new_entries;
   printf("%s new free %d\n", __FUNCTION__, rpp_info->n_pages_free);
   return 0;
}


static void get_page_meta_idx(int idx) {
   //assert(idx >= 0 && idx < rpp_info->n_pages);
   //assert(ram_refc_pool[idx] > 0);
   ram_refc_pool[idx]++;
}

static void put_page_meta_idx(int idx) {
   //assert(idx >= 0 && idx < rpp_info->n_pages);
   //assert(ram_refc_pool[idx] > 0);
   ram_refc_pool[idx]--;
   if(ram_refc_pool[idx] == 0) {
      delete_rpp_index(idx);
      hashmap_remove(ram_page_pool_hashmap, &ram_key_pool[idx]);
      //int remove_status = hashmap_remove(ram_page_pool_hashmap, &ram_key_pool[idx]);
      //assert(remove_status == MAP_OK);
      //assert(hashmap_remove(ram_page_pool_hashmap, &ram_key_pool[idx]) == MAP_MISSING);
      ram_key_pool[idx] = INVALID_KEY;
   }
}
#endif /* PERI_DEDUP_NOHASH */

unsigned long count_stored_pages_prbs(periscope_ramblock *prbs, unsigned int nprb) {
   unsigned long count = 0;
   for(unsigned int i=0; i<nprb; ++i) {
      count += (prbs[i].npages_stored/CHUNK_DIV);
   }
   return count;
}

unsigned long count_hashed_pages_prbs(periscope_ramblock *prbs, unsigned int nprb) {
   unsigned long count = 0;
   for(unsigned int i=0; i<nprb; ++i) {
      count += (prbs[i].npages_hashes_added/CHUNK_DIV);
   }
   return count;
}

unsigned long count_zero_pages_prbs(periscope_ramblock *prbs, unsigned int nprb) {
   unsigned long count = 0;
   for(unsigned int i=0; i<nprb; ++i) {
      count += (prbs[i].npages_zero/CHUNK_DIV);
   }
   return count;
}

unsigned long count_skipped_pages_prbs(periscope_ramblock *prbs, unsigned int nprb) {
   unsigned long count = 0;
   for(unsigned int i=0; i<nprb; ++i) {
      count += (prbs[i].npages_skipped/CHUNK_DIV);
   }
   return count;
}

unsigned long count_dirty_pages(void) {
   unsigned long numdirtypages = 0;
   RAMBlock *rb;
   RAMBLOCK_FOREACH_MIGRATABLE(rb) {
      struct DirtyBitmapSnapshot *snap = memory_region_snapshot_and_get_dirty(
            rb->mr,
            0, rb->max_length,
            DIRTY_MEMORY_DELTA
            );
      unsigned long npages_snap = (snap->end - snap->start) >> TARGET_PAGE_BITS;
      numdirtypages += bitmap_count_one(snap->dirty, npages_snap);
      g_free(snap);
   }
   return numdirtypages;
}

#ifndef PERI_DEDUP_NOHASH
float compute_uniqueness(periscope_ramblock *prbs, unsigned int nprb) {
   float cost = 0;
   for(int i=0; i<nprb; i++) {
      periscope_ramblock *prb = &prbs[i];
      if(prb->empty || prb->npages_stored == 0) {
         cost += prb->npages;
         continue;
      }
      float cost_prb = 0;
      for(unsigned int j=0; j<prb->npages_stored; ++j) {
         unsigned long page_chunk = prb->offsets[j];
         if(test_bit(page_chunk, prb->zero_pages)) continue;
         unsigned int meta_idx = prb->ram_idx[j];
         cost_prb += ram_refc_pool[meta_idx];
      }
      cost += cost_prb / prb->npages_stored;
   }
   return cost;
}

unsigned long compute_prb_freed(periscope_ramblock *prb) {
   unsigned long cost = 0;
   for(unsigned int i=0; i<prb->npages_stored; ++i) {
      unsigned int meta_idx = prb->ram_idx[i];
      if(ram_refc_pool[meta_idx] == 1)
         cost++;

   }
   return cost;
}
#endif

uint64_t compute_dirty_cost(unsigned long num_dirty_pages) {
#ifndef PERI_DEDUP_NOHASH
    uint64_t dirty_cost = num_dirty_pages *
       (sizeof(unsigned int) +
        sizeof(meow_u128) +
        hashmap_element_size());
   return dirty_cost;
#else
   return num_dirty_pages * (TARGET_PAGE_SIZE + sizeof(unsigned int));
#endif
}


#ifndef PERI_DEDUP_NOHASH
unsigned long get_max_new_pages(void) {
   return max_new_pages;
}
unsigned long compute_hash_cost(periscope_ramblock *prbs, unsigned int nprb) {
   unsigned long cost = 0;
   for(unsigned int i=0; i<nprb; ++i) {
      cost += (prbs[i].npages_hashes_added * CHUNK_SIZE);
   }
   return cost;
}
#endif

unsigned long compute_prb_cost(periscope_ramblock *prbs, unsigned int nprb) {
   unsigned long cost = 0;
   unsigned long list_cost = 0;
   unsigned long bitmap_cost = 0;
   unsigned long fixed_cost = 0;
   unsigned long hash_cost = 0;
   for(unsigned int i=0; i<nprb; ++i) {
      // struct cost
      fixed_cost += sizeof(periscope_ramblock);
      if(prbs[i].empty) continue;
      // offsets
      list_cost += prbs[i].npages_stored * sizeof(unsigned int);
#ifdef PERI_DEDUP_NOHASH
      // ram
      list_cost += prbs[i].npages_stored * TARGET_PAGE_SIZE;
#else
      // hash cost
      hash_cost += prbs[i].npages_stored * hashmap_element_size();
      // ram_idx
      list_cost += prbs[i].npages_stored * sizeof(unsigned int);
#endif
      // dirty
      bitmap_cost += (BITS_TO_LONGS(prbs[i].npages) * sizeof(unsigned long));
      // zero_pages
      bitmap_cost += (BITS_TO_LONGS(prbs[i].npages * CHUNK_DIV) * sizeof(unsigned long));
//#ifdef FINE_CHUNKS
      //  dirty_fine
      bitmap_cost += (BITS_TO_LONGS(prbs[i].npages * CHUNK_DIV) * sizeof(unsigned long));
//#endif
   }
   cost = list_cost + bitmap_cost + fixed_cost + hash_cost;
   //printf("%s: %ld + %ld + %ld + %ld= %ld\n", __FUNCTION__,
   //      hash_cost, list_cost, bitmap_cost, hash_cost, cost);
   return cost;
}

static inline unsigned long get_page_index(unsigned long start_index,
      unsigned long npages_stored,
      unsigned int *offsets,
      unsigned int target
      ) {
   unsigned long page_index = 0;
   for(unsigned long restore_page = start_index;
         restore_page < npages_stored; ++restore_page) {
      if(offsets[restore_page] == target)
      {
         page_index = restore_page;
         break;
      }
   }
   return page_index;
}

static int store_ram_pages(RAMBlock *rb, periscope_ramblock *prb) {
    //rcu_read_lock();
    //assert(rb);

#ifdef FINE_CHUNKS
    ram_cache_meta *rcm = get_cache_meta(rb->idstr);
    //assert(rcm);
#endif

    if(prb->empty) {
#ifdef TRACE_DEBUG
       printf("%s no dirty -> skip\n", rb->idstr);
#endif
       return 0;
    }
    unsigned long npages = prb->npages;
    //unsigned long npages_dirty = prb->npages_dirty * CHUNK_DIV;

#ifdef TRACE_DEBUG
    printf("%s: %s pages %ld\n", __FUNCTION__, rb->idstr, npages);
#endif

    unsigned long page = find_first_bit(prb->dirty, npages);
    while(page < npages) {
       unsigned long offset_base = page << TARGET_PAGE_BITS;
       for(int chunk=0; chunk<CHUNK_DIV; ++chunk) {
          unsigned long offset = offset_base + (CHUNK_SIZE * chunk);
          void *host = host_from_ram_block_offset(rb, offset);
          unsigned long page_chunk = (offset) >> (TARGET_PAGE_BITS - CHUNK_SHIFT);
          //assert(host);
          //assert(prb->npages_stored < npages_dirty);
          if(is_zero_range(host, CHUNK_SIZE)) {
#ifdef FINE_CHUNKS
             if(!MeowHashesAreEqual(rcm->cache_meta_key[page_chunk], _mm_setzero_si128())) {
                set_bit(page_chunk, prb->dirty_fine);
                rcm->cache_meta_key[page_chunk] = _mm_setzero_si128();
                rcm->cache_meta_id[page_chunk] = prb->id;
                // XXX
                //prb->offsets_zero[prb->npages_zero] = page_chunk;
                set_bit(page_chunk, prb->zero_pages);
                prb->npages_zero++;
             } else {
                prb->npages_skipped++;
             }
#else
             set_bit(page_chunk, prb->zero_pages);
             prb->npages_zero++;
#endif
             continue;
          }
#ifndef PERI_DEDUP_NOHASH
          meow_u128 key = MeowHash(MeowDefaultSeed, CHUNK_SIZE, host);
#ifdef FINE_CHUNKS
          if(MeowHashesAreEqual(rcm->cache_meta_key[page_chunk], key)) {
              prb->npages_skipped++;
              continue;
          }
#endif /* FINE_CHUNKS */

          unsigned int idx_get, *idx_put;
          int status = hashmap_get_and_put_static(
                ram_page_pool_hashmap,
                &key, &idx_get, &idx_put);
          if(status == MAP_MISSING) {
             int rm_idx = set_free_rpp_index(key, host);
             if(rm_idx >= 0) {
                *idx_put = rm_idx;
                prb->npages_hashes_added++;
                prb->ram_idx[prb->npages_stored] = rm_idx;
             } else {
                hashmap_remove(ram_page_pool_hashmap, &key);
                //int remove_status = hashmap_remove(ram_page_pool_hashmap, &key);
                //assert(remove_status == MAP_OK);
                //assert(hashmap_remove(ram_page_pool_hashmap, &key) == MAP_MISSING);
                printf("periscope: page pool full\n");
                return -1;
             }
          } else {
             //assert(idx_get < rpp_info->n_pages);
             //assert(ram_refc_pool[idx_get] > 0);
             get_page_meta_idx(idx_get);
             prb->ram_idx[prb->npages_stored] = idx_get;
          }
#ifdef FINE_CHUNKS
          set_bit(page_chunk, prb->dirty_fine);
          rcm->cache_meta_key[page_chunk] = key;
          rcm->cache_meta_id[page_chunk] = prb->id;
#endif /* FINE_CHUNKS */
#else /*PERI_DEDUP_NOHASH*/
          memcpy(prb->ram + prb->npages_stored * TARGET_PAGE_SIZE, host, TARGET_PAGE_SIZE);
#endif
          prb->offsets[prb->npages_stored] = page_chunk;
          prb->npages_stored++;
       }
       page = find_next_bit(prb->dirty, npages, page+1);
    }
#ifdef TRACE_DEBUG_STATS
   printf("periscope: %s stored %ld,%ld,%ld,%ld stored/skipped/zero/hashes\n",
         prb->idstr, prb->npages_stored, prb->npages_skipped, prb->npages_zero, prb->npages_hashes_added);
#endif
   //assert(prb->npages_skipped + prb->npages_stored + prb->npages_zero == npages_dirty);
   //rcu_read_unlock();
#ifndef PERI_DEDUP_NOHASH
   if(max_new_pages > 0 && prb->npages_hashes_added * 2 > max_new_min) {
      long new_val = max_new_pages + ((long)(prb->npages_hashes_added * 2) - max_new_pages) * MAX_NEW_UPDATE;
      printf("Updating max new pages with %ld: %ld -> %ld\n", prb->npages_hashes_added * 2, max_new_pages, new_val);
      max_new_pages = new_val;
   }
#endif
   return 0;
}
void restore_ram_pages(RAMBlock *rb, periscope_ramblock *prb, unsigned long* dirty) {
    //rcu_read_lock();
#ifdef FINE_CHUNKS
    ram_cache_meta *rcm = get_cache_meta(rb->idstr);
#endif

   unsigned long npages = prb->npages * CHUNK_DIV;
#ifdef TRACE_DEBUG_STATS
    //printf("%s: %s %p %ld\n", __FUNCTION__, rb->idstr, dirty, npages);
    printf("periscope: %s restoring %ld,%ld,%ld,%ld,%ld stored/skipped/zero/hashes/dirty\n",
         prb->idstr, prb->npages_stored, prb->npages_skipped, prb->npages_zero,
         prb->npages_hashes_added, bitmap_count_one(dirty, npages));
#endif
    //assert(prb->npages_stored + prb->npages_zero >= bitmap_count_one(dirty, npages));

//   for(unsigned int page_index = 0; page_index < prb->npages_stored; ++page_index) {
//      unsigned int page_chunk = prb->offsets[page_index];
//      if(!test_bit(page_chunk, dirty)) continue;
//      unsigned long offset = page_chunk << (TARGET_PAGE_BITS-CHUNK_SHIFT);
//      void *host = host_from_ram_block_offset(rb, offset);
//      unsigned int meta_idx = prb->ram_idx[page_index];
//      //assert(ram_refc_pool[meta_idx] > 0);
//#ifdef FINE_CHUNKS
//      rcm->cache_meta_key[page_chunk] = ram_key_pool[meta_idx];
//      rcm->cache_meta_id[page_chunk] = prb->id;
//#endif
//       memcpy(host,
//             ram_page_pool + (meta_idx * CHUNK_SIZE),
//             CHUNK_SIZE);
//
//   }
//
//   for(unsigned int page_index = 0; page_index < prb->npages_zero; ++page_index) {
//      unsigned int page_chunk = prb->offsets_zero[page_index];
//      if(!test_bit(page_chunk, dirty)) continue;
//      unsigned long offset = page_chunk << (TARGET_PAGE_BITS-CHUNK_SHIFT);
//      void *host = host_from_ram_block_offset(rb, offset);
//#ifdef FINE_CHUNKS
//      rcm->cache_meta_key[page_chunk] = _mm_setzero_si128();
//      rcm->cache_meta_id[page_chunk] = prb->id;
//#endif
//      memset(host, 0, CHUNK_SIZE);
//   }

    unsigned long page_index = 0;
    unsigned long page = find_first_bit(dirty, npages);
    while(page < npages) {
       unsigned long offset = page << (TARGET_PAGE_BITS-CHUNK_SHIFT);
       unsigned int page_chunk = page;
       void *host = host_from_ram_block_offset(rb, offset);
       if(test_bit(page_chunk, prb->zero_pages)) {
#ifdef FINE_CHUNKS
          rcm->cache_meta_key[page_chunk] = _mm_setzero_si128();
          rcm->cache_meta_id[page_chunk] = prb->id;
#endif
          memset(host, 0, CHUNK_SIZE);
          page = find_next_bit(dirty, npages, page + 1);
          continue;
       }

       unsigned int target = page_chunk;
       //assert(page_index < prb->npages_stored);
       page_index = get_page_index(
             page_index,
             prb->npages_stored,
             prb->offsets,
             target);
#ifdef PERI_DEDUP_NOHASH
       memcpy(host,
             prb->ram + page_index * TARGET_PAGE_SIZE,
             TARGET_PAGE_SIZE);
#else /* PERI_DEDUP_NOHASH */
       unsigned int meta_idx = prb->ram_idx[page_index];
       //assert(ram_refc_pool[meta_idx] > 0);
#ifdef FINE_CHUNKS
       rcm->cache_meta_key[page_chunk] = ram_key_pool[meta_idx];
       rcm->cache_meta_id[page_chunk] = prb->id;
#endif /*FINE_CHUNKS*/
       memcpy(host,
             ram_page_pool + (meta_idx * CHUNK_SIZE),
             CHUNK_SIZE);
#endif /* PERI_DEDUP_NOHASH */
       page = find_next_bit(dirty, npages, page + 1);
       page_index++;
    }
    //rcu_read_unlock();
}

periscope_ramblock *get_ramblock(periscope_ramblock *prbs, unsigned int nprb, const char *name) {
   for(unsigned int i=0; i<nprb; ++i) {
      if(strcmp(name, prbs[i].idstr) == 0)
         return &prbs[i];
   }
   return NULL;
}

#ifdef PERI_DEDUP_NOHASH
static void delete_stored_pages(periscope_ramblock *prb, periscope_ramblock *prb_parent) {
   return;
}
#else
static void delete_stored_pages(periscope_ramblock *prb, periscope_ramblock *prb_parent) {
#ifdef FINE_CHUNKS
   ram_cache_meta *rcm = get_cache_meta(prb->idstr);
#endif

   unsigned int page_index = 0;
   unsigned int npages_fine = prb->npages * CHUNK_DIV;
#ifdef FINE_CHUNKS
   unsigned int page = find_first_bit(prb->dirty_fine, npages_fine);
#else
   unsigned int page = find_first_bit(prb->dirty, npages_fine);
#endif
   unsigned int n_cache_reset = 0;
   while(page < npages_fine) {
      //unsigned int offset_base = page << (TARGET_PAGE_BITS-CHUNK_SHIFT);
      //unsigned long offset = offset_base;
      unsigned long page_chunk = page;
      if(test_bit(page_chunk, prb->zero_pages)) {
#ifdef FINE_CHUNKS
         if(rcm->cache_meta_id[page_chunk] == prb->id) {
            //assert(MeowHashesAreEqual(rcm->cache_meta_key[page_chunk], _mm_setzero_si128()));
            rcm->cache_meta_key[page_chunk] = INVALID_KEY;
            rcm->cache_meta_id[page_chunk] =  0;
            n_cache_reset++;
         }
#endif
#ifdef FINE_CHUNKS
         page = find_next_bit(prb->dirty_fine, npages_fine, page + 1);
#else
         page = find_next_bit(prb->dirty, npages_fine, page + 1);
#endif
         continue;
      }

      unsigned int target = page_chunk;
      //assert(page_index < prb->npages_stored);
      page_index = get_page_index(
            page_index,
            prb->npages_stored,
            prb->offsets,
            target);

      unsigned int meta_idx = prb->ram_idx[page_index];
#ifdef FINE_CHUNKS
      if(rcm->cache_meta_id[page_chunk] == prb->id) {
         //assert(MeowHashesAreEqual(rcm->cache_meta_key[page_chunk], ram_key_pool[meta_idx]));
         rcm->cache_meta_key[page_chunk] =  INVALID_KEY;
         rcm->cache_meta_id[page_chunk] =  0;
         n_cache_reset++;
      }
#endif
      put_page_meta_idx(meta_idx);
#ifdef FINE_CHUNKS
      page = find_next_bit(prb->dirty_fine, npages_fine, page + 1);
#else
      page = find_next_bit(prb->dirty, npages_fine, page + 1);
#endif
      page_index++;
   }
#ifdef TRACE_DEBUG
   printf("%s: reset %d cache entries\n", prb->idstr, n_cache_reset);
#endif
}
#endif

static void delete_ramblock(periscope_ramblock *prb, periscope_ramblock *prb_parent) {
#ifdef TRACE_DEBUG
   printf("Delete rb %s\n", prb->idstr);
#endif
   if(prb->empty) return;
   delete_stored_pages(prb, prb_parent);
   if(prb->dirty) {
      g_free(prb->dirty);
      prb->dirty = NULL;
   }
#ifdef FINE_CHUNKS
   if(prb->dirty_fine) {
      g_free(prb->dirty_fine);
      prb->dirty_fine = NULL;
   }
#endif
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
#ifdef PERI_DEDUP_NOHASH
   if(prb->ram) {
      g_free(prb->ram);
      prb->ram = NULL;
   }
#else
   if(prb->ram_idx) {
      g_free(prb->ram_idx);
      prb->ram_idx = NULL;
   }
#endif
   if(prb->zero_pages){
      g_free(prb->zero_pages);
      prb->zero_pages = NULL;
   }
}

void delete_peri_rbs(periscope_ramblock *prbs, unsigned int nprb,
      periscope_ramblock *prbs_parent) {
   if(prbs == NULL) return;
   for(unsigned int i=0; i<nprb; ++i) {
      delete_ramblock(&prbs[i], &prbs_parent[i]);
   }
   //g_free(prb);
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

static void zero_static_counters(periscope_ramblock *prb) {
   prb->npages = 0;
   prb->npages_dirty = 0;
}
static void zero_dynamic_counters(periscope_ramblock *prb) {
   prb->npages_stored = 0;
   prb->npages_skipped = 0;
   prb->npages_hashes_added = 0;
   prb->npages_zero = 0;
}

static void create_ramblock(char *name, periscope_ramblock *prb) {
   prb->dirty_done = false;
   prb->store_done = false;
   prb->dirty = NULL;
#ifdef FINE_CHUNKS
   prb->dirty_fine = NULL;
#endif
   zero_static_counters(prb);
   zero_dynamic_counters(prb);
   prb->offsets = NULL;
   //prb->offsets_zero = NULL;
   prb->zero_pages = NULL;
#ifdef PERI_DEDUP_NOHASH
   prb->ram = NULL;
#else
   prb->ram_idx = NULL;
#endif
   prb->empty = true;
   strcpy(prb->idstr, name);
#ifdef DBG_RAM_STORED
   prb->rambkp = NULL;
   prb->rambkp_size = 0;
#endif
}

static bool maybe_reset_ramblock(periscope_ramblock *prb)
{
   if(prb->store_done) return true;
   if(!prb->dirty_done) return false;
   printf("Restart %s\n", prb->idstr);
#ifndef PERI_DEDUP_NOHASH
   delete_stored_pages(prb, NULL);
#endif
   bitmap_zero(prb->zero_pages, prb->npages * CHUNK_DIV);
#ifdef FINE_CHUNKS
   bitmap_zero(prb->dirty_fine, prb->npages * CHUNK_DIV);
#endif
   zero_dynamic_counters(prb);
   prb->empty = false;
   return false;
}

static void maybe_realloc_ramblock(periscope_ramblock *prb)
{
   if(prb->npages_stored == 0) {
      if(prb->offsets) {
         g_free(prb->offsets);
         prb->offsets = NULL;
      }
#ifdef PERI_DEDUP_NOHASH
      if(prb->ram) {
         g_free(prb->ram);
         prb->ram = NULL;
      }
#else
      if(prb->ram_idx) {
         g_free(prb->ram_idx);
         prb->ram_idx = NULL;
      }
#endif
   } else if (prb->npages_stored != prb->npages_dirty * CHUNK_DIV ) {
      // TODO maybe determine zero pages befor, might be faster?
      if(prb->offsets) prb->offsets = g_realloc(prb->offsets, prb->npages_stored * sizeof(unsigned int));
#ifdef PERI_DEDUP_NOHASH
      if(prb->ram) prb->ram = g_realloc(prb->ram, prb->npages_stored * TARGET_PAGE_SIZE);
#else
      if(prb->ram_idx) prb->ram_idx = g_realloc(prb->ram_idx, prb->npages_stored * sizeof(unsigned int));
#endif
   }
#if 0
   if(prb->npages_zero == 0) {
      if(prb->offsets_zero) {
         g_free(prb->offsets_zero);
         prb->offsets_zero = NULL;
      }
   } else if (prb->npages_zero != prb->npages_dirty * CHUNK_DIV ) {
      if(prb->offsets_zero) prb->offsets_zero = g_realloc(prb->offsets_zero, prb->npages_zero * sizeof(unsigned int));
   }
#endif
   if(prb->npages_zero == 0 && prb->npages_stored == 0) {
      prb->empty = true;
      if(prb->dirty) g_free(prb->dirty);
#ifdef FINE_CHUNKS
      if(prb->dirty_fine) g_free(prb->dirty_fine);
#endif
      if(prb->zero_pages) g_free(prb->zero_pages);
   }
}

static bool init_ramblock(periscope_ramblock *prb,
      unsigned long *dirty,
      unsigned long npages_create,
      unsigned long npages_dirty, bool fill) {
   if(npages_dirty == 0) return true;
   prb->dirty = bitmap_new(npages_create);
   if(fill) bitmap_fill(prb->dirty, npages_dirty);
   else     bitmap_copy(prb->dirty, dirty, npages_create);
#ifdef FINE_CHUNKS
   prb->dirty_fine = bitmap_new(npages_create * CHUNK_DIV);
   if(fill) bitmap_fill(prb->dirty_fine, npages_dirty * CHUNK_DIV);
   else bitmap_zero(prb->dirty_fine, npages_create * CHUNK_DIV);
#endif
   prb->npages = npages_create;
   prb->npages_dirty = npages_dirty;
   prb->zero_pages = bitmap_new(npages_create * CHUNK_DIV);
   bitmap_zero(prb->zero_pages, npages_create * CHUNK_DIV);
   prb->offsets = g_malloc(npages_dirty * CHUNK_DIV * sizeof(unsigned int));
#ifdef PERI_DEDUP_NOHASH
   prb->ram = g_malloc(npages_dirty * TARGET_PAGE_SIZE);
#else
   prb->ram_idx = g_malloc(npages_dirty * CHUNK_DIV  * sizeof(unsigned int));
#endif
   // XXX
   //prb->offsets_zero = g_malloc(npages_dirty * CHUNK_DIV * sizeof(unsigned int));
   prb->empty = false;
   prb->dirty_done = true;
   return false;
}

//#undef TRACE_PAGES_STORED
unsigned int create_prb_and_fill(periscope_ramblock **prbs, unsigned long *num_dirty_pages, int id, bool store_ram) {
   RAMBlock *rb;
   //qemu_mutex_lock_ramlist();
   //rcu_read_lock(); // see comment on INTERNAL_RAMBLOCK_FOREACH

#ifdef TRACE_DEBUG
   printf("%s\n", __FUNCTION__);
#endif
#ifndef PERI_DEDUP_NOHASH
   max_new_pages = 0;
#endif
   unsigned int nrb = count_ramblocks();

   *prbs = g_malloc(sizeof(periscope_ramblock) * nrb);
   //assert(*prbs);
#ifdef FINE_CHUNKS
   if(store_ram && ram_cache == NULL) {
      ram_cache = (ram_cache_meta*)g_malloc(sizeof(ram_cache_meta) * nrb);
      n_cache = nrb;
   }
#endif

   unsigned int rbs_idx = 0;

   RAMBLOCK_FOREACH_MIGRATABLE(rb) {
      unsigned long npages_snap = (rb->max_length) >> TARGET_PAGE_BITS;
      unsigned long npages_dirty = (rb->used_length) >> TARGET_PAGE_BITS;
      if(num_dirty_pages != NULL) *num_dirty_pages = *num_dirty_pages + (npages_dirty/CHUNK_DIV);
      // based on observation, early small memory areas are resized to 64 later
      // might have to adapt.
      if(npages_snap < 64) npages_snap = 64;
      periscope_ramblock *prb = &((*prbs)[rbs_idx]);
      prb->id = id;
      create_ramblock(rb->idstr, prb);
      init_ramblock(prb, NULL,
            npages_snap, npages_dirty, true);
#ifdef FINE_CHUNKS
      if(store_ram) {
         ram_cache_meta *prb_cache = &((ram_cache)[rbs_idx]);
         //prb_cache->cache_meta = (void**)g_malloc(sizeof(void*) * npages_snap * CHUNK_DIV);
         //memset(prb_cache->cache_meta, 0, sizeof(void*) * npages_snap * CHUNK_DIV);
         prb_cache->cache_meta_key = (meow_u128*)g_malloc(sizeof(meow_u128) * npages_snap * CHUNK_DIV);
         for(int i=0; i<npages_snap * CHUNK_DIV; ++i) {
            prb_cache->cache_meta_key[i] = INVALID_KEY;//_mm_setzero_si128();
         }
         prb_cache->cache_meta_id = (int*)g_malloc(sizeof(int) * npages_snap * CHUNK_DIV);
         for(int i=0; i<npages_snap * CHUNK_DIV; ++i) {
            prb_cache->cache_meta_id[i] = 0;//_mm_setzero_si128();
         }
         strcpy(prb_cache->idstr, rb->idstr);
      }
#endif
      rbs_idx++;
      if(store_ram) {
         int ret = store_ram_pages(rb, prb);
         assert(ret == 0);
         //assert(prb->npages_stored <= npages_dirty * CHUNK_DIV );
         //assert(prb->npages_hashes_added <= prb->npages_stored);
         //maybe_realloc_ramblock(prb);
      }
      prb->store_done = true;
      prb->dirty_done = true;
#ifdef TRACE_PAGES_STORED
      printf("periscope: storing %lu/%lu dirty pages for snapshot %s\n", npages_dirty * CHUNK_DIV , npages_snap, rb->idstr);
#endif
   }

   //rcu_read_unlock();
   //qemu_mutex_unlock_ramlist();
#ifndef PERI_DEDUP_NOHASH
   max_new_pages = max_new_min;
#endif
   //assert(rbs_idx == nrb);
   return nrb;
}

int create_prb_and_clear_delta_bm(periscope_ramblock **prbs, unsigned long *num_dirty_pages, int id) {
   RAMBlock *rb;
   //qemu_mutex_lock_ramlist();
   //rcu_read_lock(); // see comment on INTERNAL_RAMBLOCK_FOREACH

#ifdef TRACE_DEBUG
   printf("%s\n", __FUNCTION__);
#endif
   unsigned int nrb = count_ramblocks();
   unsigned long npages_snap;
   unsigned long npages_dirty;

   if(*prbs == NULL) {
      *prbs = g_malloc(sizeof(periscope_ramblock) * nrb);
      memset(*prbs, 0, sizeof(periscope_ramblock) * nrb);
   }
   //assert(*prbs);

   unsigned int rbs_idx = 0;

   RAMBLOCK_FOREACH_MIGRATABLE(rb) {
      periscope_ramblock *prb = &((*prbs)[rbs_idx++]);
      prb->id = id;
      bool skip = maybe_reset_ramblock(prb);
      if(skip) continue;
      if(!prb->dirty_done) {
         struct DirtyBitmapSnapshot *snap = memory_region_snapshot_and_clear_dirty(
               rb->mr,
               0, rb->max_length,
               DIRTY_MEMORY_DELTA
               );
         npages_snap = (snap->end - snap->start) >> TARGET_PAGE_BITS;
         npages_dirty = bitmap_count_one(snap->dirty, npages_snap);
         create_ramblock(rb->idstr, prb);
         init_ramblock(prb, snap->dirty, npages_snap, npages_dirty, false);
      } else {
         npages_snap = prb->npages;
         npages_dirty = bitmap_count_one(prb->dirty, prb->npages);
      }
      if(prb->empty) continue;
      if(num_dirty_pages != NULL) *num_dirty_pages = *num_dirty_pages + (npages_dirty/CHUNK_DIV);
      int ret = store_ram_pages(rb, prb);
      if(ret < 0) {
         //rcu_read_unlock();
         //qemu_mutex_unlock_ramlist();
         return -nrb;
      }
      //assert(prb->npages_stored <= npages_dirty * CHUNK_DIV );
      //assert(prb->npages_hashes_added <= prb->npages_stored);
      maybe_realloc_ramblock(prb);
      prb->store_done = true;
#ifdef TRACE_PAGES_STORED
      printf("periscope: storing %lu/%lu dirty pages for snapshot %s\n", npages_dirty * CHUNK_DIV , npages_snap, rb->idstr);
#endif
   }

   //rcu_read_unlock();
   //qemu_mutex_unlock_ramlist();
   //assert(rbs_idx == nrb);
   return nrb;
}

unsigned long *new_fine_bitmap(unsigned long npages) {
   unsigned long *bm = bitmap_new(npages * CHUNK_DIV);
   bitmap_zero(bm, npages * CHUNK_DIV);
   return bm;
}
unsigned long *copy_fine_bitmap(unsigned long *bm, unsigned long npages) {
   unsigned long *new_bm = new_fine_bitmap(npages);
   unsigned long ii = find_first_bit(bm, npages);
   while(ii < npages) {
      for(int i=0; i<CHUNK_DIV; ++i) {
         set_bit(ii*CHUNK_DIV + i, new_bm);
      }
      ii = find_next_bit(bm, npages, ii+1);
   }
   return new_bm;
}

#define RAM_POOL_PERC 0.8f
void delta_snap_init(int id, unsigned long pool_size) {
#ifndef PERI_DEDUP_NOHASH
    pool_size = pool_size * 1024 * 1024 * RAM_POOL_PERC;
    unsigned long pool_entries = pool_size / CHUNK_SIZE;
    unsigned long hashmap_entries = pool_entries * 8;
    unsigned long hashmap_size = hashmap_entries * hashmap_element_size();
    if(hashmap_size > pool_size * 0.6) {
       hashmap_size = pool_size * 0.6;
    }
    unsigned long ram_page_pool_size = pool_size - hashmap_size;
    pool_entries = ram_page_pool_size / CHUNK_SIZE;
    printf("Instance id %d, pool size %ld/%ld, hashmap %ld/%ld\n",
          id, ram_page_pool_size, pool_entries, hashmap_size, hashmap_entries);
    //max_new_min = pool_entries * MAX_NEW_PERC;
    fuzzer_id = id;
    // TODO move to shared mem
    ram_page_pool_hashmap = hashmap_new(NULL, NULL, hashmap_size, 1); // need twice as many entries for some reason
    init_ram_page_pool(NULL, NULL, NULL, ram_page_pool_size, true);
    assert(ram_page_pool_hashmap != NULL);
    assert(rpp_info != NULL);
#else
    return;
#endif
}


#endif /*PERI_DEDUP */

// TODO: to be removed
// get the bitmap of currently diry pages
// this is used to determine which pages to RE-STORE
unsigned long get_current_delta_bm(unsigned long **dirty) {
    RAMBlock *rb;
    unsigned long npages_snap = 0;
    //qemu_mutex_lock_ramlist();
    //rcu_read_lock();

    MachineState *machine = MACHINE(qdev_get_machine());
    assert(machine != NULL);
    npages_snap = machine->ram_size >> TARGET_PAGE_BITS;

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
        bitmap_or(*dirty, *dirty, snap->dirty, npages_snap);
        g_free(snap);
    }

    //rcu_read_unlock();
    //qemu_mutex_unlock_ramlist();
    return npages_snap;
}

unsigned long update_and_clear_delta_snap_bm(unsigned long ** dirty) {
    RAMBlock *rb;
    unsigned long npages_snap = 0;
    //qemu_mutex_lock_ramlist();
    //rcu_read_lock(); // see comment on INTERNAL_RAMBLOCK_FOREACH

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
    //rcu_read_unlock();
    //qemu_mutex_unlock_ramlist();
    return npages_snap;
}


// call this before initiating checkpoint creation
// this will update ramblock->bmap_delta_snap
// ram_save_ ... will then only store pages which are
// marked as dirty in ramblock->bmap_delta_snap
int update_delta_snap_bm(unsigned long *dirty, unsigned long npages) {
    RAMBlock *rb;
    int ret = 1;

    //qemu_mutex_lock_ramlist();
    //rcu_read_lock();
    RAMBLOCK_FOREACH(rb) {
        // for now only handle ram
        if (strcmp(rb->idstr, "pc.ram") != 0) {
            continue;
        }
        if (rb->bmap_delta_snap == NULL) {
           unsigned long npages_rb = rb->max_length >> TARGET_PAGE_BITS;
           assert(npages_rb == npages);
           if(rb->bmap_delta_snap != NULL) g_free(rb->bmap_delta_snap);
           rb->bmap_delta_snap = bitmap_new(npages);
        }
        bitmap_copy(rb->bmap_delta_snap, dirty, npages);
        ret = 0;
        //goto out;
    }
//out:
    //rcu_read_unlock();
    //qemu_mutex_unlock_ramlist();
    return ret;
}
