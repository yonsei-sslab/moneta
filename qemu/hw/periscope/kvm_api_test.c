#include "qemu/osdep.h"
#include "exec/cpu-common.h"
#include "migration/ram.h"
#include "migration/periscope_dma.h"
#include "hw/pci/pci.h"
#include "kvm_api_test.h"
#include "qemu/iov.h"
#include "hw/i386/x86-iommu.h"
#include "hw/i386/intel_iommu.h"

#define DEVICE_NAME "kvm_api_test"


typedef struct {
   /*< private >*/
   PCIDevice parent_obj;
   MemoryRegion mmio;
} KvmApiTestState;

typedef struct {
   uint64_t map_addr;
   uint64_t map_size;
   uint64_t unmap_addr;
} KvmApiDmaState;

static KvmApiDmaState dma_state;

typedef struct IommuTrace {
    IOMMUMemoryRegion *iommu;
    uint64_t gpa;
    IOMMUNotifier n;
} IommuTrace;


static void pci_unmap_notify_func(IOMMUNotifier *n, IOMMUTLBEntry *iotlb) {
   IommuTrace *iot = container_of(n, IommuTrace, n);
   if(iot) {
      printf("iova: %lx, tr. addr %lx, gpa: %lx\n", iotlb->iova, iotlb->translated_addr, iot->gpa);
      if(periscope_dma_unmap(iot->gpa, 1) == 0) {
         MemoryRegion *mr = &iot->iommu->parent_obj;
         memory_region_unregister_iommu_notifier(mr, n);
         g_free(iot);
      }
   }
}


static void translate_trace_iova(KvmApiTestState* state, uint64_t val, unsigned size) {
#ifdef TARGET_X86_64 
    if(size != 4) return;
    PCIDevice *pci_dev = &state->parent_obj;
    PCIBus *bus = pci_get_bus(pci_dev);
    IntelIOMMUState *iommu = INTEL_IOMMU_DEVICE(x86_iommu_get_default());
    if (bus && iommu) {
        uint16_t domain_id;
        int ret = vtd_dev_to_domain_id(iommu, pci_bus_num(bus), pci_dev->devfn, &domain_id);
        if (ret == 0) {
           uint64_t gpa;
           //ret = vtd_iova_to_gpa(iommu, pci_bus_num(bus), pci_dev->devfn, val, &gpa);
           ret = vtd_iova_to_gpa_writable(iommu, pci_bus_num(bus), pci_dev->devfn, val, &gpa);
           if (ret == 0) {
              // TODO: when this message gets printed, then we need to implement DMA fuzzing
              printf("periscope: mmio_write domain=%u iova=0x%lx gpa=0x%lx\n",
                    domain_id, val, gpa);

              // start dma tracing
              // todo detect multi page dma mappings
              int size = getpagesize();
              // todo: which mmio region should get the dma exits?
              if(periscope_dma_add(gpa, size, &state->mmio) == 0) {
                 VTDAddressSpace *vas = vtd_find_add_as(iommu, bus, pci_dev->devfn);
                 if(vas) {
                    MemoryRegion *mr = &vas->iommu.parent_obj;
                    if(mr) {
                       IommuTrace* iot = g_malloc(sizeof(IommuTrace));
                       iot->iommu = &vas->iommu;
                       iot->gpa = gpa;
                       iommu_notifier_init(&iot->n, pci_unmap_notify_func,
                             IOMMU_NOTIFIER_UNMAP, (val >> 12) << 12, ((val + 0x1000) >> 12) << 12, 0);
                       memory_region_register_iommu_notifier(mr, &iot->n);
                    }
                 }
              }
           }
        }
    }
#endif
}


static uint64_t retval = 0x42;
static uint64_t mmio_read(void *opaque, hwaddr addr, unsigned size)
{
   //KvmApiTestState *s = opaque;
   //printf(DEVICE_NAME " mmio_read addr = %llx size = %x\n", (unsigned long long)addr, size);
   switch (addr) {
      default:
         printf("%s: addr %lx, size %x\n", __FUNCTION__, addr, size);
         break;
   }
   return retval++;
}

static void mmio_write(void *opaque, hwaddr addr, uint64_t val,
      unsigned size)
{
   KvmApiTestState *state = opaque;
   //PCIDevice *pdev = opaque;

   //printf(DEVICE_NAME " mmio_write addr = %llx val = %llx size = %x\n",
   //      (unsigned long long)addr, (unsigned long long)val, size);
   switch (addr) {
      case DMA_MAP_ADDR:
         dma_state.map_addr = val;
         break;
      case DMA_MAP_SIZE:
         dma_state.map_size = val;
         break;
      case DMA_UNMAP_ADDR:
         dma_state.unmap_addr = val;
         break;
      case DMA_MAP:
         translate_trace_iova(state, dma_state.map_addr, 4);
         break;
      case DMA_UNMAP:
         printf("UNMAP\n");
         //periscope_dma_remove(dma_state.unmap_addr, 0);
         //translate_iova(state, dma_state.unmap_addr);
         break;
      default:
         printf("%s: addr %lx, val %lx\n", __FUNCTION__, addr, val);
         break;
   }
}

static const MemoryRegionOps mmio_ops = {
   .read = mmio_read,
   .write = mmio_write,
   .endianness = DEVICE_NATIVE_ENDIAN,
};

static void realize(PCIDevice *pdev, Error **errp)
{
   KvmApiTestState *state = DO_UPCAST(KvmApiTestState, parent_obj, pdev);

   pci_config_set_interrupt_pin(pdev->config, 1);
   memory_region_init_io(&state->mmio, OBJECT(state), &mmio_ops, state,
         DEVICE_NAME, 0x1000);
   pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &state->mmio);
}

static int kvm_api_test_pre_save(void *opaque)
{
   return 0;
}

static int kvm_api_test_post_load(void *opaque, int version_id)
{
   //KvmApiTestState *state = opaque;
   return 0;
}

static const VMStateDescription vmstate_kvm_api_test = {
    .name = DEVICE_NAME,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = kvm_api_test_pre_save,
    .post_load = kvm_api_test_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, KvmApiTestState),
        VMSTATE_END_OF_LIST()
    }
};

static void class_init(ObjectClass *class, void *data)
{
   DeviceClass *dc = DEVICE_CLASS(class);
   PCIDeviceClass *k = PCI_DEVICE_CLASS(class);

   k->realize = realize;
   k->vendor_id = PCI_VENDOR_ID_QEMU;
   k->device_id = 0x11ea;
   k->revision = 0x0;
   k->class_id = PCI_CLASS_OTHERS;
   dc->vmsd = &vmstate_kvm_api_test;
}

static const TypeInfo type_info = {
   .name          = DEVICE_NAME,
   .parent        = TYPE_PCI_DEVICE,
   .class_init    = class_init,
   .instance_size = sizeof(KvmApiTestState),
   .class_size = sizeof(PCIDeviceClass),
   .interfaces =
      (InterfaceInfo[]){
         {INTERFACE_CONVENTIONAL_PCI_DEVICE}, {},
      },

};

static void register_types(void)
{
   type_register_static(&type_info);
}

type_init(register_types)
