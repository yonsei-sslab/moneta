#ifndef PERISCOPE_PCI_H
#define PERISCOPE_PCI_H

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/pci/pci.h"

#define CREATE_SHARED_MEMORY
#undef CREATE_SHARED_MEMORY

#define MAX_IO_MEMORY_REGIONS 10

typedef struct QCAState_st {
    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/

    MemoryRegion modern_bar;
    MemoryRegion io[MAX_IO_MEMORY_REGIONS];

    uint32_t reg[0x80];

    qemu_irq irq;

#ifdef CREATE_SHARED_MEMORY
    uint64_t* shmem;
#endif
} QCAState;

#endif