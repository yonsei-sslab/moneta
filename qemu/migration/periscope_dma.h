#ifndef PERISCOPE_DMA_H
#define PERISCOPE_DMA_H

#include "qemu/osdep.h"
#include "qapi/error.h"

#include <sys/shm.h>

#include "periscope.h"


//#include <stdint.h>
//#include "exec/memory.h"

struct periscope_dmar;
struct periscope_dmar {
   uint64_t gpa;
   unsigned long size;
   MemoryRegion *mr;
   bool is_mapped;
   unsigned n_accessed;
   char accessed_bm[512]; // -> 0x1000 bits (one for each byte)
   // ram should have the values -> we prob. dont need this
   QLIST_ENTRY(periscope_dmar) list;
};
typedef struct periscope_dmar periscope_dmar;



// initialize dma tracing
int periscope_dma_init(void);

// add a dma region to be traced and start tracing
// the memory region is pretty arbitrary,
// but determines which io callbacks will be invoked
int periscope_dma_add(uint64_t gpa, unsigned long size, MemoryRegion* mr);
// stop tracing and remove the dma region
int periscope_dma_remove(uint64_t gpa, unsigned long size);
// mark dma region as unmapped, accesses are still intercepted
int periscope_dma_unmap(uint64_t gpa, unsigned long size);

// return the memory region associated with the dma region for the given address
// the memory region determines which io callbacks will be invoked
periscope_dmar *periscope_dma_get(uint64_t gpa, unsigned long size);

int periscope_dma_read_access(periscope_dmar *dmar, uint64_t gpa, uint8_t *val);
int periscope_dma_write_access(periscope_dmar *dmar, uint64_t gpa, uint8_t *val, unsigned size);

// remove all remaining dma traces
void periscope_dma_remove_all(void);
int periscop_dma_maybe_remove(periscope_dmar *dmar, uint64_t gpa);
#endif
