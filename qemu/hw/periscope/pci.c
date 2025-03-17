#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/pci/pci.h"
#include "sysemu/dma.h"
#include "sysemu/sysemu.h"
#include "qemu/cutils.h"
#include "qemu/iov.h"
#include "qemu/range.h"
#include "migration/snapshot.h"
#include "block/snapshot.h"
#include "migration/periscope.h"
#include "hw/periscope/pci.h"
#include "hw/i386/x86-iommu.h"
#include "hw/i386/intel_iommu.h"
#include "migration/periscope_dma.h"
#include <stdio.h>
#include <sys/mman.h>

/* PCI Device IDs */
#define QCA6174_DEVICE_ID 0x003e // ath10k
#define QCA9565_DEVICE_ID 0x0036 // ath9k
#define QCA9377_DEVICE_ID 0x0042 // ath10k
#define QCA9980_DEVICE_ID 0x0040 // ath10k
#define QCA9990_DEVICE_ID 0x0040 // ath10k

// ath10k/hw.h
#define FW_IND_INITIALIZED 2

// ath10k/hw.c
#define QCA988x_FW_INDICATOR_ADDRESS 0x9030
#define QCA6174_FW_INDICATOR_ADDRESS 0x3a028
#define QCA99x0_FW_INDICATOR_ADDRESS 0x40050
#define QCA99x0_RTC_STATE_VAL_ON 5

#define QCA99x0_RTC_SOC_BASE_ADDRESS 0x00080000
#define QCA99x0_SOC_CORE_BASE_ADDRESS 0x00082000
#define QCA99x0_SOC_CHIP_ID_ADDRESS 0x000000ec
#define QCA99x0_SOC_CHIP_ID (0x1 << 8)

#define QCA_DEBUG 1

#ifdef QCA_DEBUG
enum {
    DEBUG_GENERAL,
    DEBUG_IO,
    DEBUG_MMIO,
    DEBUG_INTERRUPT,
    DEBUG_RX,
    DEBUG_TX,
    DEBUG_MDIC,
    DEBUG_EEPROM,
    DEBUG_UNKNOWN,
    DEBUG_TXSUM,
    DEBUG_TXERR,
    DEBUG_RXERR,
    DEBUG_RXFILTER,
    DEBUG_PHY,
    DEBUG_NOTYET,
};
#define DBGBIT(x) (1 << DEBUG_##x)
static int debugflags = DBGBIT(TXERR) | DBGBIT(GENERAL);

#define DBGOUT(what, fmt, ...)                                                 \
    do {                                                                       \
        if (debugflags & DBGBIT(what))                                         \
            fprintf(stderr, "periscope-pci: " fmt, ##__VA_ARGS__);                \
    } while (0)
#else
#define DBGOUT(what, fmt, ...)                                                 \
    do {                                                                       \
    } while (0)
#endif

typedef struct io_memory_region_desc {
    uint8_t type;
    int size;
} io_memory_region_desc;

struct IommuTrace;
typedef struct IommuTrace {
    IOMMUMemoryRegion *iommu;
    uint64_t gpa;
    IOMMUNotifier n;
    QLIST_ENTRY(IommuTrace) list;
} IommuTrace;

static QLIST_HEAD( ,IommuTrace) iommu_trace_notifiers =
    QLIST_HEAD_INITIALIZER(iommu_trace_notifiers);

static int num_io_descs = 0;
static io_memory_region_desc io_desc[MAX_IO_MEMORY_REGIONS];

typedef struct PeriScopeBaseClass {
    PCIDeviceClass parent_class;
} PeriScopeBaseClass;

#define TYPE_PERISCOPE "periscope"

#define PERISCOPE(obj) OBJECT_CHECK(QCAState, (obj), TYPE_PERISCOPE)

#define PERISCOPE_DEVICE_CLASS(klass)                                         \
    OBJECT_CLASS_CHECK(PeriScopeBaseClass, (klass), TYPE_PERISCOPE)
#define PERISCOPE_DEVICE_GET_CLASS(obj)                                       \
    OBJECT_GET_CLASS(PeriScopeBaseClass, (obj), TYPE_PERISCOPE)

static uint32_t vendor_id = 0;
static uint32_t device_id = 0;
static uint32_t revision_id = 0;
static uint32_t class_id = 0;
static uint32_t subsystem_vendor_id = 0;
static uint32_t subsystem_id = 0;
static bool configured = false;

void periscope_configure_dev(const char *optarg) {
    char str[256];
    const char *p;

    strncpy(str, optarg, sizeof(str));
    printf("periscope: initializing device config %s...\n", str);

    // default device
    vendor_id = PCI_VENDOR_ID_ATHEROS;
    device_id = QCA6174_DEVICE_ID;
    revision_id = 0x1;
    class_id = PCI_CLASS_WIRELESS_OTHER;

    const char *tok = strtok(str, ",");

    if (tok && strstart(tok, "vendor=", &p)) {
        long id;
        if (qemu_strtol(p, NULL, 16, &id) == 0) {
            vendor_id = id;
            // printf("periscope: vendor %lu\n", id);
        }
        tok = strtok(NULL, ",");
    }

    if (tok && vendor_id > 0) {
        if (strstart(tok, "device=", &p)) {
            long id;
            if (qemu_strtol(p, NULL, 16, &id) == 0) {
                device_id = id;
                // printf("periscope: device %lu\n", id);
            }
        }
        tok = strtok(NULL, ",");
    }

    if (tok && strstart(tok, "revision=", &p)) {
        long id;
        if (qemu_strtol(p, NULL, 16, &id) == 0) {
            revision_id = id;
            // printf("periscope: revision %lu\n", id);
        }
        tok = strtok(NULL, ",");
    }

    if (tok && strstart(tok, "class=", &p)) {
        long id;
        if (qemu_strtol(p, NULL, 16, &id) == 0) {
            class_id = id;
            // printf("periscope: class %lu\n", id);
        }
        tok = strtok(NULL, ",");
    }

    // TODO: subsystem_vendor_id and subsystem_id

    if (tok && strstart(tok, "subsystem_vendor_id=", &p)) {
        long id;
        if (qemu_strtol(p, NULL, 16, &id) == 0) {
            subsystem_vendor_id = id;
            // printf("periscope: subsystem_vendor %lu\n", id);
        }
        tok = strtok(NULL, ",");
    }

    if (tok && strstart(tok, "subsystem_id=", &p)) {
        long id;
        if (qemu_strtol(p, NULL, 16, &id) == 0) {
            subsystem_id = id;
            // printf("periscope: subsystem %lu\n", id);
        }
        tok = strtok(NULL, ",");
    }

    while (tok && num_io_descs < MAX_IO_MEMORY_REGIONS) {
        printf("periscope: io region %s\n", tok);

        if (strstart(tok, "mmio=", &p)) {
            io_desc[num_io_descs].type = PCI_BASE_ADDRESS_SPACE_MEMORY;
            long id;
            if (qemu_strtol(p, NULL, 16, &id) == 0) {
                io_desc[num_io_descs].size = id;
                num_io_descs++;
            }
        }
        else if (strstart(tok, "io=", &p)) {
            io_desc[num_io_descs].type = PCI_BASE_ADDRESS_SPACE_IO;
            long id;
            if (qemu_strtol(p, NULL, 16, &id) == 0) {
                io_desc[num_io_descs].size = id;
                num_io_descs++;
            }
        }
        tok = strtok(NULL, ",");
    }

    printf("periscope: initializing 0x%x:0x%x (rev:0x%x, class:0x%x) 0x%x:0x%x...\n",
            vendor_id, device_id, revision_id, class_id,
            subsystem_vendor_id, subsystem_id);

    configured = true;
}

static void remove_all_iommu_trace_notifiers(void)
{
   IommuTrace *iot;
   QLIST_FOREACH(iot, &iommu_trace_notifiers, list) {
      MemoryRegion *mr = &iot->iommu->parent_obj;
      memory_region_unregister_iommu_notifier(mr, &iot->n);
      QLIST_REMOVE(iot, list);
      g_free(iot);
   }
}

static void pci_unmap_notify_func(IOMMUNotifier *n, IOMMUTLBEntry *iotlb) {
   IommuTrace *iot = container_of(n, IommuTrace, n);
   if(iot) {
      //printf("iova: %lx, tr. addr %lx, gpa: %lx\n", iotlb->iova, iotlb->translated_addr, iot->gpa);
      //if(periscope_dma_remove(iot->gpa, 1) == 0) {
      if(periscope_dma_unmap(iot->gpa, 1) == 0) {
         MemoryRegion *mr = &iot->iommu->parent_obj;
         memory_region_unregister_iommu_notifier(mr, n);
         QLIST_REMOVE(iot, list);
         g_free(iot);
      }
   }
}

static void translate_trace_iova(QCAState* state, uint64_t val, unsigned size) {
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
           // only translate writable mappings
           ret = vtd_iova_to_gpa_writable(iommu, pci_bus_num(bus), pci_dev->devfn, val, &gpa);
           if (ret == 0) {
              // TODO: when this message gets printed, then we need to implement DMA fuzzing
              //printf("periscope: mmio_write domain=%u iova=0x%lx gpa=0x%lx\n",
              //      domain_id, val, gpa);

              // start dma tracing
              // todo detect multi page dma mappings
              int size = getpagesize();
              // todo: which mmio region should get the dma exits?
              if(periscope_dma_add(gpa, size, &state->io[1]) == 0) {
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
                       QLIST_INSERT_HEAD(&iommu_trace_notifiers, iot, list);
                    }
                 }
              }
           }
        }
    }
#endif
}

static void pci_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    QCAState *s = opaque;
    PCIDevice *pci_dev = opaque;
    unsigned int index = (addr & 0x1ffff) >> 2;

    pci_irq_deassert(&s->parent_obj);

#ifdef TRACE_PERISCOPE_MMIO_WRITE
    printf("periscope: mmio write 0x%lx 0x%lx 0x%x\n", addr, val, size);
#endif
    translate_trace_iova(s, val, size);

    switch (index) {
    case 0:
        if (val) {
            pci_irq_assert(pci_dev);
        } else {
            pci_irq_deassert(pci_dev);
        }
        s->reg[0] = val;
        break;
    case 1:
        break;
    default:
        DBGOUT(UNKNOWN, "MMIO unknown write addr=0x%08x,val=0x%08" PRIx64 "\n",
               index << 2, val);
        break;
    }

    if (index < 10) {
    } else {
        DBGOUT(UNKNOWN, "MMIO unknown write addr=0x%08x,val=0x%08" PRIx64 "\n",
               index << 2, val);
    }
}

static uint64_t pci_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    QCAState *s = opaque;
    PCIDevice *pci_dev = &s->parent_obj;

    pci_irq_deassert(pci_dev);
    //unsigned int index = (addr & 0x1ffff) >> 2;

    uint64_t out;

    periscope_mmio_read(opaque, size, &out);

    periscope_maybe_raise_irq(&s->parent_obj);

#ifdef TRACE_PERISCOPE_MMIO_READ
    printf("periscope: mmio read 0x%lx 0x%x 0x%lx\n", addr, size, out);
#endif

    return out;

#ifdef CREATE_SHARED_MEMORY
    uint64_t offset = s->shmem[0];

    if(offset > 0x1000) {
        //printf("offset oor %ld\n", offset);
        return 0;
    }

    //switch (index) {
    //case 0:
    //    return QCA99x0_RTC_STATE_VAL_ON;
    //}

    //if (addr == QCA99x0_RTC_SOC_BASE_ADDRESS + QCA99x0_SOC_CHIP_ID_ADDRESS) {
    //    printf("QCA99x0 soc_chip_id_address\n");
    //    return QCA99x0_SOC_CHIP_ID;
    //}

    //if (addr == QCA6174_FW_INDICATOR_ADDRESS) {
    //    printf("QCA6174 fw_indicator_address\n");
    //    return FW_IND_INITIALIZED;
    //}
    //if (addr == QCA99x0_FW_INDICATOR_ADDRESS) {
    //    printf("QCA99x0 fw_indicator_address\n");
    //    return FW_IND_INITIALIZED;
    //}

    (void)s;
    //return 0;
    s->shmem[0] = s->shmem[0] + 1;
    return s->shmem[offset];
#endif
}

static const MemoryRegionOps periscope_mmio_ops = {
    .read = pci_mmio_read,
    .write = pci_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl =
        {
            .min_access_size = 1,
            .max_access_size = 4,
        },
};

#if 0
static uint64_t periscope_io_read(void *opaque, hwaddr addr, unsigned size)
{
    QCAState *s = opaque;

    //printf("qca io read %ld %d\n", addr, size);

    uint64_t ret;
    periscope_mmio_read(size, &ret);

    (void)s;
    return ret;
}

static void periscope_io_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    QCAState *s = opaque;

    //printf("qca io write %ld %ld %d\n", addr, val, size);

    (void)s;
}
#endif

static const MemoryRegionOps periscope_io_ops = {
    .read = pci_mmio_read,
    .write = pci_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static int periscope_pre_save(void *opaque)
{
    QCAState *s = opaque;

    //printf("periscope: pre_save\n");

    // TODO
    (void)s;

    return 0;
}

static int periscope_post_load(void *opaque, int version_id)
{
    QCAState *s = opaque;

    //printf("periscope: post_load\n");

    // TODO
    (void)s;

#if 0
    pcie_cap_slot_post_load(opaque, version_id);
#endif

    return 0;
}

static const VMStateDescription vmstate_periscope = {
    .name = "periscope",
    .version_id = 2,
    .minimum_version_id = 1,
    .pre_save = periscope_pre_save,
    .post_load = periscope_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, QCAState),
        VMSTATE_END_OF_LIST()
    }
};

static void pci_periscope_uninit(PCIDevice *dev)
{
    QCAState *d = PERISCOPE(dev);

    g_free(d->irq);
    d->irq = NULL;
}

#ifdef CREATE_SHARED_MEMORY
static int create_shared_memory(QCAState *d)
{
   int fd = shm_open("/qca_shm.file", O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
   if(fd < 0) {
      printf("OPEN FAILED %d\n", fd);
      return -1;
   }
   if (ftruncate(fd, 0x1000) == -1) {
      perror("ftruncate");
      return -1;
   }
   d->shmem = (uint64_t*)mmap(NULL, 0x1000, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
   if(d->shmem == MAP_FAILED) {
      printf("MMAP FAILED\n");
      return -1;
   }
   d->shmem[0] = 1;
   return 0;
}
#endif

static void pci_periscope_realize(PCIDevice *pci_dev, Error **errp)
{
    // DeviceState *dev = DEVICE(pci_dev);
    QCAState *d = PERISCOPE(pci_dev);
    uint8_t *pci_conf;

    pci_conf = pci_dev->config;

    pci_set_word(pci_dev->config + PCI_SUBSYSTEM_VENDOR_ID,
                 subsystem_vendor_id);
    pci_set_word(pci_dev->config + PCI_SUBSYSTEM_ID,
                 subsystem_id);

    pci_set_word(pci_conf + PCI_STATUS, PCI_STATUS_DEVSEL_MEDIUM |
                                        PCI_STATUS_FAST_BACK);

    /* TODO: RST# value should be 0, PCI spec 6.2.4 */
    pci_conf[PCI_CACHE_LINE_SIZE] = 0x10;

    // The Interrupt Pin register is a read-only register that identifies the
    // legacy interrupt Message(s) the Function uses (see Section 6.1 for
    // further details). Valid values are 01h, 02h, 03h, and 04h that map to
    // legacy interrupt Messages for INTA, INTB, INTC, and INTD respectively.
    pci_conf[PCI_INTERRUPT_PIN] = 1; /* interrupt pin A */

    for (unsigned i=0; i<num_io_descs; i++) {
        if (io_desc[i].size <= 0) continue;

        char id_str[50];
        sprintf(id_str, "periscope-io%d", i);

        printf("periscope: configuring periscope-io%d (size=%d,type=%u)\n",
               i, io_desc[i].size, io_desc[i].type);

        const MemoryRegionOps *ops = &periscope_mmio_ops;
        if (io_desc[i].type == PCI_BASE_ADDRESS_SPACE_IO)
            ops = &periscope_io_ops;

        memory_region_init_io(&d->io[i], OBJECT(d), ops, d,
                              id_str, io_desc[i].size);

        pci_register_bar(pci_dev, i, io_desc[i].type, &d->io[i]);
    }

    d->irq = pci_allocate_irq(pci_dev);

    // TODO: not sure if or when we need the following
    if (pci_is_express(pci_dev)) {
        printf("periscope: initializing pcie configuration space\n");
        int pos;

        pos = pcie_endpoint_cap_init(pci_dev, 0);
        assert(pos > 0);

        pos = pci_add_capability(pci_dev, PCI_CAP_ID_PM, 0,
                                 PCI_PM_SIZEOF, errp);
        if (pos < 0) {
            return;
        }

        pci_dev->exp.pm_cap = pos;

        /*
         * Indicates that this function complies with revision 1.2 of the
         * PCI Power Management Interface Specification.
         */
        pci_set_word(pci_dev->config + pos + PCI_PM_PMC, 0x3);
    }

#ifdef CREATE_SHARED_MEMORY
    if(create_shared_memory(d) != 0)
      d->shmem = NULL;
#endif

#if 0
    qemu_macaddr_default_if_unset(&d->conf.macaddr);
    macaddr = d->conf.macaddr.a;

    qcax_core_prepare_eeprom(d->eeprom_data,
                               qca_eeprom_template,
                               sizeof(qca_eeprom_template),
                               PCI_DEVICE_GET_CLASS(pci_dev)->device_id,
                               macaddr);

    d->nic = qemu_new_nic(&net_periscope_info, &d->conf,
                          object_get_typename(OBJECT(d)), dev->id, d);

    qemu_format_nic_info_str(qemu_get_queue(d->nic), macaddr);

    d->autoneg_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, qca_autoneg_timer, d);
    d->mit_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, qca_mit_timer, d);
#endif
}

static void qdev_periscope_reset(DeviceState *dev)
{
    QCAState *d = PERISCOPE(dev);

    remove_all_iommu_trace_notifiers();
    // neccessary?
    // right now it seems like the system reset takes care of pt but list should be emptied
    periscope_dma_remove_all();
    vm_stop(RUN_STATE_RESTORE_VM);

    (void)d;
}

#define PCI_READ_WRITE_CONFIG
#undef PCI_READ_WRITE_CONFIG

#ifdef PCI_READ_WRITE_CONFIG
static uint32_t periscope_pci_read_config(PCIDevice *d, uint32_t addr,
                                          int len)
{
    QCAState *s = PERISCOPE(d);

    return pci_default_read_config(d, addr, val, len);
}

static void periscope_pci_write_config(PCIDevice *d, uint32_t addr,
                                       uint32_t val, int len)
{
    QCAState *s = PERISCOPE(d);

    pci_default_write_config(d, addr, val, len);
}
#endif

static Property periscope_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

#if 0
typedef struct QCAInfo {
    const char *name;
    uint16_t device_id;
    uint8_t revision;
} QCAInfo;
#endif

static void periscope_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    if (!configured) {
        printf("periscope: device not configured?\n");
    }

    printf("periscope: initializing device class...\n");

#if 0
    QCABaseClass *e = QCA_DEVICE_CLASS(klass);
    const QCAInfo *info = data;
#endif

    k->realize = pci_periscope_realize;
    k->exit = pci_periscope_uninit;

    k->romfile = 0;

    /* Device identifiers */
    k->vendor_id = vendor_id;
    k->device_id = device_id;
    k->subsystem_vendor_id = subsystem_vendor_id;
    k->subsystem_id = subsystem_id;
    k->revision = revision_id;
    k->class_id = class_id;

    DeviceCategory cat = DEVICE_CATEGORY_MISC;
    switch (class_id) {
    case PCI_CLASS_NETWORK_ETHERNET:
    case PCI_CLASS_WIRELESS_OTHER:
        cat = DEVICE_CATEGORY_NETWORK;
        break;
    }
    set_bit(cat, dc->categories);

    // dc->desc = "";

#ifdef PCI_READ_WRITE_CONFIG
    k->config_read = periscope_pci_read_config;
    k->config_write = periscope_pci_write_config;
#endif

    /* qemu user things */
    dc->vmsd = &vmstate_periscope;
    dc->props = periscope_properties;
    dc->reset = qdev_periscope_reset;
}

static void periscope_instance_init(Object *obj)
{
    PCI_DEVICE(obj)->cap_present |= QEMU_PCI_CAP_EXPRESS;

#if 0
    QCAState *n = PERISCOPE(obj);
    device_add_bootindex_property(obj, &n->bootindex, "bootindex", "/ssllab@0",
                                  DEVICE(n), NULL);
#endif
}

static const TypeInfo pci_periscope_info = {
    .name = TYPE_PERISCOPE,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(QCAState),
    .instance_init = periscope_instance_init,
    .class_size = sizeof(PeriScopeBaseClass),
    .class_init = periscope_class_init,
    .interfaces =
        (InterfaceInfo[]){
            {INTERFACE_PCIE_DEVICE},
            {INTERFACE_CONVENTIONAL_PCI_DEVICE},
            {},
        },
};

#if 0
static QCAInfo periscope_devices[] = {
    {
        .name = "periscope", .device_id = 0, .revision = 0x02,
    },
};
#endif

static void periscope_register_types(void)
{
#if 0
    int i;
#endif

    type_register_static(&pci_periscope_info);

#if 0
    for (i = 0; i < ARRAY_SIZE(periscope_devices); i++) {
        const QCAInfo *info = &periscope_devices[i];
        TypeInfo type_info = {};
        type_info.name = info->name;
        type_info.parent = TYPE_PERISCOPE_BASE;
        type_info.class_data = (void *)info;
        type_info.class_init = periscope_class_init;
        type_info.instance_init = periscope_instance_init;

        type_register(&type_info);
    }
#endif
}

type_init(periscope_register_types)
