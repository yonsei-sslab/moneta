/*
 * Fake Mali G610 GPU device  
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include <sys/ioctl.h>
#include <linux/vfio.h>

#include "hw/vfio/vfio-platform.h"
#include "qemu/error-report.h"
#include "qemu/range.h"
#include "sysemu/sysemu.h"
#include "exec/memory.h"
#include "exec/address-spaces.h"
#include "qemu/queue.h"
#include "hw/sysbus.h"
#include "hw/platform-bus.h"
#include "sysemu/kvm.h"

#include "exec/memory.h"

#define REGION_SIZE 0x200000

#define TYPE_PLATFORM_MALI "fb000000.gpu"
#define PLATFORM_MALI(obj) OBJECT_CHECK(VFIOPlatformDevice, obj, TYPE_PLATFORM_MALI)

#define MALI_IRQ 1
#ifdef MALI_IRQ

#define JOB_CONTROL_BASE        0x1000
#define JOB_CONTROL_REG(r)      (JOB_CONTROL_BASE + (r))
#define JOB_IRQ_STATUS          0x00C   /* Interrupt status register */
#define JOB_IRQ_CLEAR           0x004   /* Interrupt clear register */

#define MEMORY_MANAGEMENT_BASE  0x2000
#define MMU_REG(r)              (MEMORY_MANAGEMENT_BASE + (r))
#define MMU_IRQ_STATUS          0x00C   /* (RO) Interrupt status register */
#define MMU_IRQ_CLEAR           0x004   /* (WO) Interrupt clear register */

#define GPU_CONTROL_BASE        0x0000
#define GPU_CONTROL_REG(r)      (GPU_CONTROL_BASE + (r))
#define GPU_IRQ_STATUS          0x02C   /* (RO) */
#define GPU_IRQ_CLEAR           0x024   /* (WO) */

#define	JOB_IRQ_TAG	2
#define MMU_IRQ_TAG	1
#define GPU_IRQ_TAG	0

#endif

static uint64_t CS0x0000100c = 0x00000001;
static uint64_t CS0x0000200c = 0x00000001;
static uint64_t CS0x0000002c = 0x00000001;

/*
 * It is assumed this access corresponds to the IRQ status
 * register reset. With such a mechanism, a single IRQ can be
 * handled at a time since there is no way to know which IRQ
 * was completed by the guest (we would need additional details
 * about the IRQ status register mask).
 * 
 * TODO: IRQ status implementation from Mali model device
*/
static void mali_eoi(VFIOPlatformDevice *vdev) {

    VFIODevice *vbasedev = &vdev->vbasedev;
    
    for (int i = 0; i < vbasedev->num_irqs; i++) {
        if (vdev->irq_state[i] == VFIO_IRQ_ACTIVE) {
            vdev->irq_state[i] = VFIO_IRQ_INACTIVE;
            qemu_set_irq(vdev->irq[i], 0);
        }
        break;
    }

}

void mali_request_irq(VFIOPlatformDevice *vdev, int irq) {

    vdev->irq_state[irq] = VFIO_IRQ_ACTIVE;
    qemu_set_irq(vdev->irq[irq], 1);

}

static uint64_t mali_region_read(void *ptr, hwaddr addr, unsigned size) {

    uint64_t val;
    VFIOPlatformDevice *vdev = (VFIOPlatformDevice *)ptr;

    mali_eoi(vdev);

    switch (addr) {
        // READ
        case 0x00000000:
            val = 0xa8670005; break;
        case 0x00000010:
            val = 0x00000301; break;
        case 0x00000144:
            val = 0x00000000; break;
        case 0x00000150:
            val = 0x00000000; break;
        case 0x00000154:
            val = 0x00000000; break;
        case 0x00000100:
            val = 0x00050005; break;
        case 0x00000104:
            val = 0x00000000; break;
        case 0x00000120:
            val = 0x00000001; break;
        case 0x00000124:
            val = 0x00000000; break;
        case 0x00000164:
            val = 0x00000000; break;
        case 0x00000200:
            val = 0x00000000; break;
        case 0x00000204:
            val = 0x00000000; break;
        case 0x00000220:
            val = 0x00000000; break;
        case 0x00000224:
            val = 0x00000000; break;
        case 0x00040004:
            val = 0x00000200; break;
        // mid-Workload READs
        case 0x0000002c:
            val = 0x00000600; break;
        case 0x00000140:
            val = 0x00050005; break;
        case 0x00000160:
            val = 0x00000001; break;
        // READ-once
        case 0x00000004:
            val = 0x07120306; break;
        case 0x00000008:
            val = 0x00000000; break;
        case 0x0000000c:
            val = 0x00000809; break;
        case 0x00000014:
            val = 0x00002830; break;
        case 0x00000018:
            val = 0x000000ff; break;
        case 0x00000034:
            val = 0x00000000; break;
        case 0x000000a0:
            val = 0x00000800; break;
        case 0x000000a4:
            val = 0x00000400; break;
        case 0x000000a8:
            val = 0x00000400; break;
        case 0x000000ac:
            val = 0x04010000; break;
        case 0x000000b0:
            val = 0xc1ffff9e; break;
        case 0x000000b4:
            val = 0x00000000; break;
        case 0x000000b8:
            val = 0x00000000; break;
        case 0x000000bc:
            val = 0x00000000; break;
        case 0x00000110:
            val = 0x00000001; break;
        case 0x00000114:
            val = 0x00000000; break;
        case 0x00000300:
            val = 0x00000000; break;
        case 0x00000310:
            val = 0x00000000; break;
        case 0x00000e00:
            val = 0x00000055; break;
        case 0x00000e04:
            val = 0x00000000; break;
        case 0x00002428:
            val = 0x00000000; break;
        case 0x00002468:
            val = 0x00000000; break;
        case 0x00040100:
            val = 0x00000000; break;
        case 0x00040104:
            val = 0x00000000; break;
        // Read-after-Write
        case 0x00001008: // Suspected, single case only
            val = 0xffffffff; break;
        // Irregular
        case 0x00000f00:
            val = 0x00000000; break;
        case 0x00000f04:
            val = 0x00000000; break;
        case 0x00000f08:
            val = 0x00000000; break;
        case 0x00000f0c:
            val = 0x00000000; break; // Above 4 "write" 0 after "read"
        case 0x00002008:
            val = 0x000000ff; break; // Write ffff but read ff
        // Consecutive
        case 0x0000100c:
            val = CS0x0000100c; CS0x0000100c = 0x00000001; break;
        case 0x0000200c:
            val = CS0x0000200c; CS0x0000200c = 0x00000001; break;
        default:
            // printf("Mali Fakedev default READ 0x%08lx, 0x%08x, 0x%08x\n", addr, size, 0);
            return 0x0;
    }
    
    // printf("Mali Fakedev READ 0x%08lx, 0x%08x, 0x%08x\n", addr, size, val);

    return val;
}

static void mali_region_write(void *ptr, hwaddr addr, uint64_t val, unsigned size) {

    VFIOPlatformDevice *vdev = (VFIOPlatformDevice *)ptr;

    mali_eoi(vdev);

    switch (addr) {
        // IRQ requests
        case JOB_CONTROL_REG(JOB_IRQ_STATUS):
            mali_request_irq(vdev, JOB_IRQ_TAG); break;
        case MMU_REG(MMU_IRQ_STATUS): // no recorded case of MMU IRQ yet.
            mali_request_irq(vdev, MMU_IRQ_TAG); break;
        case GPU_CONTROL_REG(GPU_IRQ_STATUS):
            mali_request_irq(vdev, GPU_IRQ_TAG); break;
        // IRQ clear
        case JOB_CONTROL_REG(JOB_IRQ_CLEAR):
            CS0x0000100c = 0x00000000; break;
        case MMU_REG(MMU_IRQ_CLEAR):
            CS0x0000200c = 0x00000000; break;
        case GPU_CONTROL_REG(GPU_IRQ_CLEAR):
            CS0x0000002c = 0x00000000; break;
    }

    // printf("Mali Fakedev WRITE 0x%08lx, 0x%08x, 0x%08lx\n", addr, size, val);
}

static const MemoryRegionOps platform_mali_region_ops = {
    .read = mali_region_read,
    .write = mali_region_write,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/**
 * mali_realize  - the device realize function
 * @dev: device state pointer
 * @errp: error
 *
 * initialize the device, its memory regions and IRQ structures
 * IRQ are started separately
 */
static void mali_realize(DeviceState *dev, Error **errp)
{
    VFIOPlatformDevice *vdev = PLATFORM_MALI(dev);
    SysBusDevice *sbdev = SYS_BUS_DEVICE(dev);
    VFIODevice *vbasedev = &vdev->vbasedev;

    vbasedev->type = VFIO_DEVICE_TYPE_PLATFORM;
    vbasedev->dev = dev;
    vbasedev->name = "fb000000.gpu";
    vbasedev->sysfsdev = g_strdup_printf("/sys/bus/platform/devices/%s",
                                             vbasedev->name);

    vdev->regions = g_new0(VFIORegion *, 1);
    vdev->regions[0] = g_new0(VFIORegion, 1);

    vdev->regions[0]->vbasedev = vbasedev;
    vdev->regions[0]->flags = (VFIO_REGION_INFO_FLAG_READ | VFIO_REGION_INFO_FLAG_WRITE);
    vdev->regions[0]->size = 2097152;
    vdev->regions[0]->fd_offset = 0;
    vdev->regions[0]->nr = 0;

    vdev->regions[0]->mem = g_new0(MemoryRegion, 1);
    memory_region_init_io(vdev->regions[0]->mem, OBJECT(vdev), &platform_mali_region_ops,
                            vdev, "mali-fakedev-region", REGION_SIZE);
    sysbus_init_mmio(sbdev, vdev->regions[0]->mem);

    vbasedev->num_irqs = 3;

    for (int i = 0; i < vbasedev->num_irqs; i++) {
        sysbus_init_irq(sbdev, &vdev->irq[i]);
        vdev->irq_state[i] = VFIO_IRQ_INACTIVE;
    }
}

static const VMStateDescription vmstate_mali = {
    .name = TYPE_VFIO_PLATFORM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_PLATFORMBUS_DEVICE(sbdev, VFIOPlatformDevice),
        VMSTATE_END_OF_LIST()
    }
};

static void vfio_platform_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->user_creatable = true;
    dc->hotpluggable = true;
    dc->realize = mali_realize;
    // dc->props = vfio_platform_dev_properties;
    dc->vmsd = &vmstate_mali;
    dc->desc = "Fake Mali dev";
    // sbc->connect_irq_notifier = vfio_start_irqfd_injection;
    // set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    /* Supported by TYPE_VIRT_MACHINE */
    // dc->user_creatable = true;
}

static const TypeInfo mali_dev_info = {
    .name = TYPE_PLATFORM_MALI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(VFIOPlatformDevice),
    .class_init = vfio_platform_class_init,
    .class_size = sizeof(VFIOPlatformDeviceClass),
};

static void register_mali_type(void)
{
    type_register_static(&mali_dev_info);
}

type_init(register_mali_type)