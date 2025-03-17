#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/pci/msi.h"
#include "hw/pci/pci.h"
#include "hw/vfio/pci.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "moneta.h"

#include "sysemu/sysemu.h"

#define REGION_0_SIZE 0x1000000
#define REGION_1_SIZE 0x10000000
#define REGION_3_SIZE 0x2000000
#define IOPORT_SIZE 0x80

#define TYPE_PCI_NVIDIA "nvidia"
#define PCI_NVIDIA(obj) OBJECT_CHECK(VFIOPCIDevice, obj, TYPE_PCI_NVIDIA)

static uint64_t RW0x00001700 = 0x0002f57c;
static uint64_t RW0x00088080 = 0x00002910;
static uint64_t RW0x00100c10 = 0x00000000;
static uint64_t RW0x00100c40 = 0x00000000;
static uint64_t RW0x001103c0 = 0x00000000;
static uint64_t RW0x0070bff0 = 0x2f57d301;
static uint64_t RW0x0070bff4 = 0x00000000;
static uint64_t RW0x008403c0 = 0x00000000;

static uint64_t RW0x00009140 = 0x00000000;
static uint64_t RW0x00110118 = 0x00000614;
static uint64_t RW0x0070e000 = 0x00000000;
static uint64_t RW0x00b830b0 = 0x00010005;

bool irq_armed_3060 = false;
bool irq_fire_3060 = false;

void request_irq_3060(void *dev) {
    VFIOPCIDevice *vdev = dev;

    printf("IRQ: %lu\n", qemu_clock_get_us(QEMU_CLOCK_VIRTUAL));

    pci_irq_assert(&vdev->pdev);
    pci_irq_deassert(&vdev->pdev);

    irq_armed_3060 = false;
    irq_fire_3060 = false;
}

static uint64_t region_read(hwaddr addr) {
#if DEVICE_SIDE_EMULATION

    irq_armed_3060 = false;

    if (addr == 0xa00) {
        return 0x196a1000;
    }

    switch(addr){
        // Read-only
        case 0x00000000:
            return 0xb76000a1;
        case 0x0008802c:
            return 0x407b1458;
        case 0x00110018:
            return 0x01000000;
        case 0x0011001c:
            return 0x00000000;
        case 0x00110804:
            return 0x00000000;
        case 0x00111388:
            return 0x00000080;
        case 0x00111528:
            return 0x01009042;
        case 0x0011152c:
            return 0x01000040;
        case 0x00611c00:
            return 0x00000007;
        case 0x00611ec0:
            return 0x00001001;
        case 0x0082074c:
            return 0x00000001;
        case 0x00820c04:
            return 0x00000000;
        case 0x00824148:
            return 0x00000001;
        case 0x00b81008:
            return 0x00000000;
        case 0x00b8100c:
            return 0x00000000;
        case 0x00b81014:
            return 0x000001ef;
        case 0x00b81018:
            return 0x00000004;
        case 0x00b81214:
            return 0x00000000;
        case 0x00b81600:
            return 0x00000000;

        // Read-once
        case 0x00001438:
            return 0x00000000;
        case 0x00001454:
            return 0x00000000;
        case 0x00009430:
            return 0xffffffff;
        case 0x00088000:
            return 0x250410de;
        case 0x00088084:
            return 0x00454d03;
        case 0x00088008:
            return 0x030000a1;
        case 0x00088068:
            return 0x00817805;
        case 0x00088424:
            return 0x00000000;
        case 0x00088430:
            return 0x00000000;
        case 0x00118128:
            return 0x00008b8f;
        case 0x00118234:
            return 0x000003ff;
        case 0x001183a4:
            return 0x00003000;
        case 0x001fa824:
            return 0x02ffee00;
        case 0x00625f04:
            return 0x00010e09;
        case 0x00700000:
            return 0x2f57c102;
        case 0x00700004:
            return 0x00000000;

        // Read-write
        case 0x00009140:
            return RW0x00009140;
        case 0x00110118:
            return RW0x00110118;
        case 0x0070e000:
            return RW0x0070e000;
        case 0x008403c0:
            return RW0x008403c0;
        case 0x00b830b0:
            return RW0x00b830b0;
        case 0x00001700:
            return RW0x00001700;
        case 0x00088080:
            return RW0x00088080;
        case 0x00100c10:
            return RW0x00100c10;
        case 0x00100c40:
            return RW0x00100c40;
        case 0x001103c0:
            return RW0x001103c0;
        case 0x0070bff0:
            return RW0x0070bff0;
        case 0x0070bff4:
            return RW0x0070bff4;

        // Read-write-irrelevant
        case 0x0008800c:
            return 0x00800000;
        case 0x00110600:
            return 0x00000110;
        case 0x00110624:
            return 0x00000110;
        case 0x00b81608:
            return 0x00000000;
        case 0x0070e004:
            return 0xabcdabcd;
        case 0x0070e008:
            return 0xabcdabcd;
        case 0x0070e00c:
            return 0xabcdabcd;
        case 0x008241e0:
            return 0x00000003;
        case 0x00840040:
            return 0x00000000;
        case 0x00840600:
            return 0x00000110;
        case 0x00840624:
            return 0x00000110;

        case 0x00b8101c:       // IRQ pattern step 1
            irq_armed_3060 = true;
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
        case 0x00001700:
            RW0x00001700 = val;
            break;
        case 0x00009140:
            RW0x00009140 = val;
            break;
        case 0x00110118:
            RW0x00110118 = val;
            break;
        case 0x00088080:
            RW0x00088080 = val;
            break;
        case 0x00100c10:
            RW0x00100c10 = val;
            break;
        case 0x00100c40:
            RW0x00100c40 = val;
            break;
        case 0x001103c0:
            RW0x001103c0 = val;
            break;
        case 0x0070bff0:
            RW0x0070bff0 = val;
            break;
        case 0x0070bff4:
            RW0x0070bff4 = val;
            break;
        case 0x0070e000:
            RW0x0070e000 = val;
            break;
        case 0x008403c0:
            RW0x008403c0 = val;
            break;
        case 0x00b830b0:
            RW0x00b830b0 = val;
            break;
        // IRQ patterns
        case 0x00b81640:          // NVIDIA driver init IRQ
            if (val == 0x00000081)
                irq_fire_3060 = true;
            break;
        case 0x00b81608:          // IRQ pattern step 2
            if (val == 0x00000001 && irq_armed_3060)
                irq_fire_3060 = true;
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

    if (irq_fire_3060)         // IRQ pattern
        request_irq_3060(ptr);
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

static void nvidia_realize(PCIDevice *pdev, Error **errp) {
    VFIOPCIDevice *vdev = PCI_NVIDIA(pdev);
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

static void nvidia_instance_finalize(Object *obj) {}

static void nvidia_instance_init(Object *obj)
{
    PCIDevice *pci_dev = PCI_DEVICE(obj);
    VFIOPCIDevice *vdev = PCI_NVIDIA(obj);

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

static const VMStateDescription vmstate_nvidia = {
    .name = "vfio-pci",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]){
        VMSTATE_PCI_DEVICE(pdev, VFIOPCIDevice), 
        VMSTATE_END_OF_LIST()
        }
};

static void nvidia_pci_class_init(ObjectClass *klass, void *data) {
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pdc = PCI_DEVICE_CLASS(klass);

    dc->hotpluggable = false;
    dc->vmsd = &vmstate_nvidia;
    dc->desc = "Fake NVIDIA dev";

    pdc->realize = nvidia_realize;
    pdc->vendor_id = 0x10de;           // NVIDIA Corporation
    pdc->device_id = 0x2504;           // GA106 [GeForce GTX 3060]
    pdc->subsystem_vendor_id = 0x1458; // Gigabyte Technology Co., Ltd
    pdc->subsystem_id = 0x407b;        // GA106 [GeForce GTX 3060]
    pdc->class_id = PCI_CLASS_DISPLAY_VGA;
}

static const TypeInfo nvidia_type_info = {
    .name = TYPE_PCI_NVIDIA,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(VFIOPCIDevice),
    .class_init = nvidia_pci_class_init,
    .instance_init = nvidia_instance_init,
    .instance_finalize = nvidia_instance_finalize,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void register_nvidia_dev_type(void) { type_register_static(&nvidia_type_info); }

type_init(register_nvidia_dev_type)
