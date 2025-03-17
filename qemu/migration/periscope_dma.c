#include "migration/periscope_dma.h"
#include "sysemu/kvm.h"
#include "qemu/bitops.h"
#include <unistd.h>

static bool is_init = false;
static QLIST_HEAD( ,periscope_dmar) dma_regions =
    QLIST_HEAD_INITIALIZER(dma_regions);

int periscope_dma_init(void)
{
   if(is_init) return -1;
   return 0;
}

//static periscope_dmar* find_dma_region_exact(uint64_t gpa, unsigned long size)
//{
//   periscope_dmar *dmar;
//   QLIST_FOREACH(dmar, &dma_regions, list) {
//      if (dmar->gpa == gpa && dmar->size == size) {
//         return dmar;
//      }
//   }
//   return NULL;
//}

static periscope_dmar* find_dma_region(uint64_t gpa, unsigned long size)
{
   periscope_dmar *dmar;
   QLIST_FOREACH(dmar, &dma_regions, list) {
      if (dmar->gpa <= gpa && dmar->gpa + dmar->size > gpa) {
         return dmar;
      }
   }
   return NULL;
}

int periscope_dma_add(uint64_t gpa, unsigned long size, MemoryRegion* mr)
{
#ifdef TARGET_X86_64
   int pagesize = getpagesize();
   periscope_dmar *dmar = find_dma_region(gpa, size);

   //printf("%s: addr %lx, size %lx\n", __FUNCTION__, gpa, size);
   // TODO what to do with partial overlaps?
   if(dmar != NULL) {
      // reset all tracking data fields
      dmar->is_mapped = true;
      memset(dmar->accessed_bm, 0, sizeof(dmar->accessed_bm) * sizeof(char));
      //printf("Warning: dma region at %lx already registered\n", gpa);
      return 0;
   }
   for(unsigned long addr = gpa; addr < gpa + size; addr += pagesize) {
      if(kvm_enable_dma_trace(addr) != 0) {
         printf("Error: could not enable dma trace for %lx\n", addr);
         return -1;
      }
   }


   dmar = g_malloc(sizeof(periscope_dmar));
   dmar->gpa = gpa;
   dmar->size = size;
   dmar->mr = mr;
   dmar->is_mapped = true;
   dmar->n_accessed = 0;
   QLIST_INSERT_HEAD(&dma_regions, dmar, list);
   //printf("%s: success\n", __FUNCTION__);
#endif
   return 0;
}

int periscope_dma_remove(uint64_t gpa, unsigned long size)
{
#ifdef TARGET_X86_64
   int pagesize = getpagesize();
   //periscope_dmar *dmar = find_dma_region_exact(gpa, size);
   periscope_dmar *dmar = find_dma_region(gpa, size);
   //printf("%s: Enter gpa %lx (+%lx)\n", __FUNCTION__, gpa, size);
   // TODO what to do with partial overlaps?
   if(dmar == NULL) {
      printf("Warning: dma region at %lx (+%lx) not registered\n", gpa, size);
      return -1;
   }


   for(unsigned long addr = dmar->gpa; addr < dmar->gpa + dmar->size; addr += pagesize) {
      if(kvm_disable_dma_trace(addr) != 0) {
         printf("Error: could not disable dma trace for %lx\n", addr);
         // remove anyway, if there are future pfs they will be handled by kvm
         // removing the entry just avoids the detour through this module
         // when the guest is reset the page tables will be reset anyway
         //return -1;
      }
   }
   QLIST_REMOVE(dmar, list);
   g_free(dmar);
   //printf("%s: success\n", __FUNCTION__);
#endif
   return 0;
}

int periscope_dma_write_access(periscope_dmar *dmar, uint64_t gpa, uint8_t *val, unsigned size)
{
   unsigned long offset;
   int pagesize = getpagesize();
   if(!dmar) return -1;
   if(dmar->is_mapped) return 0; // if the dma region is still under device control -> do nothing
   offset = gpa - dmar->gpa;
   if(offset > pagesize) return -1; // should not happen
   //printf("%s: Enter\n", __FUNCTION__);
   // mark bytes as overwritten (no matter if they were set already)
   for(int i=0; i<size; ++i) {
      if(!test_and_set_bit(offset + i, (unsigned long*)dmar->accessed_bm)) {
         dmar->n_accessed++;
      }
   }
   //printf("#Accessed dmas %d\n", dmar->n_accessed);
   return 0;
}

int periscope_dma_read_access(periscope_dmar *dmar, uint64_t gpa, uint8_t *val)
{
   unsigned long offset;
   int pagesize = getpagesize();
   if(!dmar) return -1;
   if(dmar->is_mapped) return 0; // if the dma region is still under device control -> do nothing
   //printf("%s: Enter\n", __FUNCTION__);
   offset = gpa - dmar->gpa;
   if(offset > pagesize) return -1; // should not happen
   // mark bytes as overwritten (no matter if they were set already)
   if(test_and_set_bit(offset, (unsigned long*)dmar->accessed_bm)) {
      // read data from actual guest ram
      dma_memory_read(&address_space_memory, gpa, val, 1);
      //printf("read value at %lx -> %x\n", offset, *val);
      return 1;
   }
   return 0;
}

int periscope_dma_unmap(uint64_t gpa, unsigned long size)
{
   //periscope_dmar *dmar = find_dma_region_exact(gpa, size);
   periscope_dmar *dmar = find_dma_region(gpa, size);
   //printf("%s: Enter gpa %lx (+%lx)\n", __FUNCTION__, gpa, size);
   // TODO what to do with partial overlaps?
   if(dmar == NULL) {
      //printf("Warning: dma region at %lx (+%lx) not registered\n", gpa, size);
      return -1;
   }

   // mark as not mapped
   dmar->is_mapped = false;
   // reset accessed bitmap (from now on writes to this area have to be buffered)
   memset(dmar->accessed_bm, 0, sizeof(dmar->accessed_bm) * sizeof(char));
   return 0;
}

void periscope_dma_remove_all(void)
{
#ifdef TARGET_X86_64
   periscope_dmar *dmar;
   int pagesize = getpagesize();
   QLIST_FOREACH(dmar, &dma_regions, list) {

      for(unsigned long addr = dmar->gpa; addr < dmar->gpa + dmar->size; addr += pagesize) {
         if(kvm_disable_dma_trace(addr) != 0) {
            printf("Error: could not disable dma trace for %lx\n", addr);
         }
      }

      //printf("Removed dmar %lx\n", dmar->gpa);
      QLIST_REMOVE(dmar, list);
      g_free(dmar);
   }
#endif
}

int periscop_dma_maybe_remove(periscope_dmar *dmar, uint64_t gpa)
{
   if(dmar == NULL) return 0;
   if(!dmar->is_mapped && dmar->n_accessed >= 0x1000 - 8) {
      periscope_dma_remove(gpa, 1);
      return 1;
   }
   return 0;
}

periscope_dmar *periscope_dma_get(uint64_t gpa, unsigned long size)
{
   periscope_dmar *dmar = find_dma_region(gpa, size);
   //printf("%s %lx (+%lx)\n", __FUNCTION__, gpa, size);
   if(dmar == NULL) {
      printf("Warning: dma region at %lx (+%lx) not registered\n", gpa, size);
      return NULL;
   }
   return dmar;
}


