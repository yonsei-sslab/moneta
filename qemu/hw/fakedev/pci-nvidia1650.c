#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/pci/msi.h"
#include "hw/pci/pci.h"
#include "hw/vfio/pci.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "moneta.h"

/*
        Region 0: Memory at fd000000 (32-bit, non-prefetchable) [size=16M]                                                                                                                                                                                                         
        Region 1: Memory at e0000000 (64-bit, prefetchable) [size=256M]                                                                                                                                                                                                            
        Region 3: Memory at f0000000 (64-bit, prefetchable) [size=32M]                                                                                                                                                                                                             
        Region 5: I/O ports at c000 [size=128]

*/
#define REGION_0_SIZE 0x1000000
#define REGION_1_SIZE 0x10000000
#define REGION_3_SIZE 0x2000000
#define IOPORT_SIZE 0x80

#define TYPE_PCI_NVIDIA1650 "nvidia1650"
#define PCI_NVIDIA1650(obj) OBJECT_CHECK(VFIOPCIDevice, obj, TYPE_PCI_NVIDIA1650)

static uint64_t RW0x001103c0 = 0x00000000;
static uint64_t RW0x00110044 = 0x00000000;
static uint64_t RW0x008403c0 = 0x00000000;

bool irq_armed_1650 = false;
bool irq_fire_1650 = false;

void request_irq_1650(void *dev) {
    VFIOPCIDevice *vdev = dev;

    // printf("IRQ: %lu\n", qemu_clock_get_us(QEMU_CLOCK_VIRTUAL));

    pci_irq_assert(&vdev->pdev);
    pci_irq_deassert(&vdev->pdev);

    irq_armed_1650 = false;
    irq_fire_1650 = false;
}

static uint64_t region_read(hwaddr addr) {
#if DEVICE_SIDE_EMULATION

    irq_armed_1650 = false;

    if (addr == 0xa00) {
        return 0x168a1000;
    }

    switch(addr){
        // Read-only / once
        case 0x0008802c:
            return 0x158319da;
        case 0x00088000:
            return 0x218710de;
        case 0x00021c04:
            return 0x00000000;
        case 0x00009430:
            return 0x000000ff;
        case 0x0002174c:
            return 0x00000001;
        case 0x00100ce0:
            return 0x00000207;
        case 0x00625f04:
            return 0x00020e09;
        case 0x00118128:
            return 0xffffff8f;
        case 0x00118234:
            return 0x000003ff;
        case 0x001100f4:
            return 0x000007f7;
        case 0x00110108:
            return 0x40420100;
        case 0x00001438:
            return 0x00000000;
        case 0x001fa824:
            return 0x00ffee00;
        case 0x00840040:
            return 0x00000000;
        case 0x00840044:
            return 0x00000000;
        case 0x00840624:
            return 0x00000110;
        case 0x00111240:
            return 0x00000035;
        case 0x00110804:
            return 0x00000000;
        case 0x00b81008:
            return 0x00000000;
        case 0x00b81018:
            return 0x00000000;
        case 0x00b81600:
            return 0x00000000;
        case 0x00000000:
            return 0x168000a1;
        case 0x00110018:
            return 0x00000000;
        case 0x0011001c:
            return 0x00000000;
        case 0x001112b4:
            return 0x00049040;
        case 0x001112b8:
            return 0x00000040;
        case 0x00611ec0:
            return 0x00001001;
        case 0x00611c00:
            return 0x00000007;

        // Read-write
        case 0x001103c0:
            return RW0x001103c0;
        case 0x00110044:
            return RW0x00110044;
        case 0x008403c0:
            return RW0x008403c0;

        case 0x00000140:       // IRQ pattern step 1 DIFFERENT FROM RTX GPUs
            irq_armed_1650 = true;
            return 0x00000000;


        default:
            return 0x0;

    }
#endif
    return 0x0;
}

static void region_write(hwaddr addr, uint64_t val) {
#if DEVICE_SIDE_EMULATION
    switch(addr){
        // Read-write
        case 0x001103c0:
            RW0x001103c0 = val;
            break;
        case 0x00110044:
            RW0x00110044 = val;
            break;
        case 0x008403c0:
            RW0x008403c0 = val;
            break;
        // IRQ patterns
        case 0x00b81640:          // NVIDIA driver init IRQ
            if (val == 0x00000081)
                irq_fire_1650 = true;
            break;
        case 0x00b81608:          // IRQ pattern step 2
            if (val == 0x00000001 && irq_armed_1650)
                irq_fire_1650 = true;
            break;
        default:
            break;
    }
#endif
    return;
}

static uint64_t nvidia_bar_read(void *ptr, hwaddr addr, unsigned size) {

    uint64_t val = region_read(addr);
    // printf("bar_read 0x%08lx, 0x%08x, 0x%08lx\n", addr, size, val);
    return val;
}

static void nvidia_bar_write(void *ptr, hwaddr addr, uint64_t val, unsigned size) {

    region_write(addr, val);
    // printf("bar_write 0x%08lx, 0x%08x, 0x%08lx\n", addr, size, val);

    if (irq_fire_1650)         // IRQ pattern
        request_irq_1650(ptr);
}

static const MemoryRegionOps pci_nvidia_bar0_ops = {
    .read = nvidia_bar_read,
    .write = nvidia_bar_write,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const MemoryRegionOps pci_nvidia_bar1_ops = {
    .read = nvidia_bar_read,
    .write = nvidia_bar_write,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const MemoryRegionOps pci_nvidia_bar3_ops = {
    .read = nvidia_bar_read,
    .write = nvidia_bar_write,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t io_read(void *opaque, hwaddr addr, unsigned size) {
    // printf("io_read %lx, %x\n", addr, size);
    return 0x0;
}

static void io_write(void *opaque, hwaddr addr, uint64_t val, unsigned size) {
    // printf("io_write %lx, %x, %lx\n", addr, size, val);
}

static const MemoryRegionOps pci_io_ops = {
    .read = io_read,
    .write = io_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 2,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void nvidia1650_realize(PCIDevice *pdev, Error **errp) {
    VFIOPCIDevice *vdev = PCI_NVIDIA1650(pdev);
    VFIOBAR *bar0 = &vdev->bars[0];
    VFIOBAR *bar1 = &vdev->bars[1];
    VFIOBAR *bar3 = &vdev->bars[3];
    VFIOBAR *bar5 = &vdev->bars[5];

    bar0->mr = g_new0(MemoryRegion, 1);
    bar1->mr = g_new0(MemoryRegion, 1);
    bar3->mr = g_new0(MemoryRegion, 1);
    bar5->mr = g_new0(MemoryRegion, 1);

    // Region 0: Memory (32-bit, non-prefetchable) [size=16M]
    memory_region_init_io(bar0->mr, OBJECT(vdev), &pci_nvidia_bar0_ops, vdev, "region0",
                          REGION_0_SIZE);
    pci_register_bar(&vdev->pdev, 0, PCI_BASE_ADDRESS_MEM_TYPE_32, bar0->mr);

    // Region 1: Memory (64-bit, prefetchable) [size=256M]
    memory_region_init_io(bar1->mr, OBJECT(vdev), &pci_nvidia_bar1_ops, vdev, "region1",
                          REGION_1_SIZE);
    pci_register_bar(&vdev->pdev, 1, PCI_BASE_ADDRESS_MEM_TYPE_64 | PCI_BASE_ADDRESS_MEM_PREFETCH,
                     bar1->mr);

    // Region 3: Memory (64-bit, prefetchable) [size=32M]
    memory_region_init_io(bar3->mr, OBJECT(vdev), &pci_nvidia_bar3_ops, vdev, "region3",
                          REGION_3_SIZE);
    pci_register_bar(&vdev->pdev, 3, PCI_BASE_ADDRESS_MEM_TYPE_64 | PCI_BASE_ADDRESS_MEM_PREFETCH,
                     bar3->mr);

    // Region 5: I/O ports [size=128]
    memory_region_init_io(bar5->mr, OBJECT(vdev), &pci_io_ops, vdev, "region5",
                          IOPORT_SIZE);
    pci_register_bar(&vdev->pdev, 5, PCI_BASE_ADDRESS_SPACE_IO, bar5->mr);

    pci_set_byte(&vdev->pdev.config[PCI_INTERRUPT_PIN], 0x1);
}

static void nvidia1650_instance_finalize(Object *obj) {}

static void nvidia1650_instance_init(Object *obj)
{
    PCIDevice *pci_dev = PCI_DEVICE(obj);
    VFIOPCIDevice *vdev = PCI_NVIDIA1650(obj);

    device_add_bootindex_property(obj, &vdev->bootindex,
                                  "bootindex", NULL,
                                  &pci_dev->qdev, NULL);
    vdev->host.domain = ~0U;
    vdev->host.bus = ~0U;
    vdev->host.slot = ~0U;
    vdev->host.function = ~0U;

    vdev->nv_gpudirect_clique = 0xFF;

    /* QEMU_PCI_CAP_EXPRESS initialization does not depend on QEMU command
     * line, therefore, no need to wait to realize like other devices */
    pci_dev->cap_present |= QEMU_PCI_CAP_EXPRESS;
}

static const VMStateDescription vmstate_nvidia1650 = {
    .name = "vfio-pci",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]){
        VMSTATE_PCI_DEVICE(pdev, VFIOPCIDevice), 
        VMSTATE_END_OF_LIST()
        }
};

static void nvidia1650_pci_class_init(ObjectClass *klass, void *data) {
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pdc = PCI_DEVICE_CLASS(klass);

    dc->hotpluggable = false;
    dc->vmsd = &vmstate_nvidia1650;
    dc->desc = "Fake NVIDIA1650 dev";

    pdc->realize = nvidia1650_realize;
    pdc->vendor_id = 0x10de;           // NVIDIA Corporation
    pdc->device_id = 0x2187;           // NVIDIA Corporation TU116 [GeForce GTX 1650 SUPER]
    pdc->subsystem_vendor_id = 0x19da; // ZOTAC International (MCO) Ltd. TU116 [GeForce GTX 1650 SUPER]
    pdc->subsystem_id = 0x1583;        //
    pdc->class_id = PCI_CLASS_DISPLAY_VGA;
}

static const TypeInfo nvidia1650_type_info = {
    .name = TYPE_PCI_NVIDIA1650,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(VFIOPCIDevice),
    .class_init = nvidia1650_pci_class_init,
    .instance_init = nvidia1650_instance_init,
    .instance_finalize = nvidia1650_instance_finalize,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void register_nvidia1650_dev_type(void) { type_register_static(&nvidia1650_type_info); }

type_init(register_nvidia1650_dev_type)
