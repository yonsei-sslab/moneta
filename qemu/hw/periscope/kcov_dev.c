#include "qemu/osdep.h"
#include "exec/cpu-common.h"
#include "migration/ram.h"
#include "hw/pci/pci.h"
#include "kcov_vdev.h"

#define DEVICE_NAME "kcov_vdev"




typedef struct {
   unsigned int cmd;
   unsigned int arg;
   unsigned int ret;
} kcov_mmio_state;

static kcov_mmio_state mstate;
static uint64_t kcov_area_offset = 0;

#define FF(_b) (0xff << ((_b) << 3))

static uint32_t count_bytes(uint8_t *mem) {
  uint32_t *ptr = (uint32_t *)mem;
  uint32_t i = (KCOV_MAP_SIZE >> 2);
  uint32_t ret = 0;

  while (i--) {
    uint32_t v = *(ptr++);

    if (!v) continue;
    if (v & FF(0)) ret++;
    if (v & FF(1)) ret++;
    if (v & FF(2)) ret++;
    if (v & FF(3)) ret++;
  }

  return ret;
}

void kcov_print_coverage(void)
{
   if(!kcov_area_offset) {
      printf("%s: no kcov area offset set (use kcov-get-area-offset qmp)\n", __FUNCTION__);
      return;
   }

   RAMBlock *block = qemu_ram_block_by_name("pc.ram");
   if(!block) {
      printf("%s: could not find ramblock\n", __FUNCTION__);
      return;
   }

   uint64_t* host = host_from_ram_block_offset(block, kcov_area_offset);
   if(host) {
      printf("%s: %lx (count=%u)\n", __FUNCTION__, host[0], count_bytes((uint8_t *)host));
   } else {
      printf("%s: could not get host ptr\n", __FUNCTION__);
   }
}

uint8_t *kcov_get_area(void)
{
   if(kcov_area_offset == 0) {
      printf("%s: no kcov area offset set (use kcov-get-area-offset qmp)\n", __FUNCTION__);
      return NULL;
   }

   RAMBlock *block = qemu_ram_block_by_name("pc.ram");
   if(!block) {
      printf("%s: could not find ramblock\n", __FUNCTION__);
      return NULL;
   }

   uint64_t* host = host_from_ram_block_offset(block, kcov_area_offset);
   if(!host) {
      printf("%s: could not find host from ramblock\n", __FUNCTION__);
      return NULL;
   }
   return (uint8_t *)host;
}

// get physical offset of kcov coverage bitmap from guest
int kcov_get_area_offset(void *opaque)
{
   PCIDevice *pdev = opaque;
   mstate.cmd = KCOV_GET_AREA_OFFSET;
   mstate.arg = 0;
   pci_set_irq(pdev, 1);
   return 0;
}

void kcov_flush_area(bool no_print, int fd) {
   uint64_t *area = (uint64_t *)kcov_get_area();
   static uint64_t prev_pc = 0L;
   uint64_t max_pcs = KCOV_MAP_SIZE / sizeof(uint64_t) - 1;
   uint64_t num_pcs = area[0]; // HERE SEGFAULT
   if (num_pcs > max_pcs) {
      printf("periscope: kcov area overflown? max=%lu, num=%lu\n",
            max_pcs, num_pcs);
      num_pcs = max_pcs;
   }
    if (num_pcs == 0) return;

   uint64_t num_skipped = 1;
   while (num_skipped < 1 + num_pcs) {
      if (area[num_skipped] != prev_pc) {
         break;
      }
      num_skipped++;
   }
    if (num_skipped == 1 + num_pcs) return;

   // printf("periscope: flushing kcov area\n");
   if (!no_print)
      printf("periscope: pcs=[");

   bool printed = false;
   for (uint64_t i = num_skipped; i < 1 + num_pcs; i++) {
      if (prev_pc == area[i]) continue;
      if (!no_print && printed) {
         printf(",");
      }

      if (!no_print) {
         printf("0x%lx", area[i]);
      }

      if (fd != -1) {
         // "0xffffffffffffffff\n"
         char buf[2 + 16 + 1];
         sprintf(buf, "0x%lx\n", area[i]);
         // dump to a file indicated by path
         write(fd, buf, sizeof(buf));
      }

      prev_pc = area[i];
      printed = true;
   }

   if (!no_print)
   printf("]\n");

   area[0] = 0;
}

// send kcov ioctl to guest kcov driver
int kcov_ioctl(void *opaque, unsigned int cmd, unsigned int arg)
{
   PCIDevice *pdev = opaque;
   mstate.cmd = cmd;
   mstate.arg = arg;
   printf("%s: state %p\n", __FUNCTION__, pdev);
   pci_set_irq(pdev, 1);
   return 0;
}

static uint64_t mmio_read(void *opaque, hwaddr addr, unsigned size)
{
   KCovState *s = opaque;
   //printf(DEVICE_NAME " mmio_read addr = %llx size = %x\n", (unsigned long long)addr, size);
   switch (addr) {
      case KCOV_CMD_OFFSET:
         return mstate.cmd;
      case KCOV_ARG_OFFSET:
         return mstate.arg;
      case KCOV_CCMODE_OFFSET:
         if (s->trace_pc) {
            if (s->trace_pc_flush)
               return KCOV_TRACE_PC | ((unsigned int)1<<16);
            return KCOV_TRACE_PC;
         }
         return KCOV_TRACE_AFL;
      case KCOV_GMODE_OFFSET:
         if (s->trace_global)
            return 1;
         return 0;
   }
   return 0x1234567812345678;
}

static void mmio_write(void *opaque, hwaddr addr, uint64_t val,
      unsigned size)
{
   KCovState *state = opaque;

#if 0
   printf(DEVICE_NAME " mmio_write addr = %llx val = %llx size = %x\n",
         (unsigned long long)addr, (unsigned long long)val, size);
#endif
   switch (addr) {
      case KCOV_SET_IRQ:
         pci_set_irq(&state->parent_obj, 1);
         break;
      case KCOV_RESET_IRQ:
         pci_set_irq(&state->parent_obj, 0);
         break;
      case KCOV_RET_OFFSET:
         state->area_offset = val;
         kcov_area_offset = val;
         printf(DEVICE_NAME " kcov area offset %lx\n", kcov_area_offset);
         mstate.ret = 0;
         break;
      case KCOV_COV_ENABLE:
         if (state->trace_pc) {
            printf(DEVICE_NAME " kcov buffer enable %lx\n", val);
            kcov_area_offset = val;
         }
         break;
      case KCOV_COV_DISABLE:
         if (state->trace_pc) {
            printf(DEVICE_NAME " kcov buffer disable %lx\n", val);
            kcov_area_offset = 0;
         }
         break;
      case KCOV_COV_REMOTE_ENABLE:
         if (state->trace_pc) {
            printf(DEVICE_NAME " kcov buffer remote enable %lx\n", val);
            kcov_area_offset = val;
         }
         break;
      case KCOV_COV_REMOTE_DISABLE:
         if (state->trace_pc) {
            printf(DEVICE_NAME " kcov buffer remote disable %lx\n", val);
            kcov_area_offset = 0;
         }
         break;
      case KCOV_COV_COLLECT:
         if (state->trace_pc) {
            printf(DEVICE_NAME " kcov buffer collect %lx\n", val);
            // TODO
         }
         break;
      case KCOV_COV_FULL:
         if (state->trace_pc) {
            // printf(DEVICE_NAME " kcov buffer full %lx\n", val);
            kcov_flush_area(true, state->fd);
         }
         break;
//      case KCOV_RET_OFFSET + 4:
//         kcov_area_offset = (kcov_area_offset & ~(0xffffffffUL)) | val;
//         printf(DEVICE_NAME " kcov area offset %lx\n", kcov_area_offset);
//         mstate.ret = 0;
//         break;
   }
}

static const MemoryRegionOps mmio_ops = {
   .read = mmio_read,
   .write = mmio_write,
   .endianness = DEVICE_NATIVE_ENDIAN,
};

static void realize(PCIDevice *pdev, Error **errp)
{
   KCovState *state = DO_UPCAST(KCovState, parent_obj, pdev);

   pci_config_set_interrupt_pin(pdev->config, 1);
   memory_region_init_io(&state->mmio, OBJECT(state), &mmio_ops, state,
         DEVICE_NAME, 1024);
   pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &state->mmio);

   state->fd = -1;
   if (state->dump_path && strlen(state->dump_path) > 0) {
      printf(DEVICE_NAME "opening a coverage dump file\n");
      state->fd = open(state->dump_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
   }
}

static int kcov_pre_save(void *opaque)
{
   return 0;
}

static int kcov_post_load(void *opaque, int version_id)
{
   KCovState *state = opaque;
   kcov_area_offset = state->area_offset;
   return 0;
}

static const VMStateDescription vmstate_kcov = {
    .name = DEVICE_NAME,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = kcov_pre_save,
    .post_load = kcov_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, KCovState),
        VMSTATE_UINT64(area_offset, KCovState),
        VMSTATE_BOOL(trace_pc, KCovState),
        VMSTATE_BOOL(trace_pc_flush, KCovState),
        VMSTATE_BOOL(trace_global, KCovState),
        VMSTATE_END_OF_LIST()
    }
};

static Property kcov_properties[] = {
    DEFINE_PROP_BOOL("trace-pc", KCovState, trace_pc, false),
    DEFINE_PROP_BOOL("trace-pc-flush", KCovState, trace_pc_flush, true),
    DEFINE_PROP_BOOL("trace-global", KCovState, trace_global, true),
    DEFINE_PROP_STRING("dump-path", KCovState, dump_path),
    DEFINE_PROP_END_OF_LIST(),
};

static void class_init(ObjectClass *class, void *data)
{
   DeviceClass *dc = DEVICE_CLASS(class);
   PCIDeviceClass *k = PCI_DEVICE_CLASS(class);

   k->realize = realize;
   k->vendor_id = PCI_VENDOR_ID_QEMU;
   k->device_id = 0x11e9;
   k->revision = 0x0;
   k->class_id = PCI_CLASS_OTHERS;
   dc->vmsd = &vmstate_kcov;
   dc->props = kcov_properties;
}

static const TypeInfo type_info = {
   .name          = DEVICE_NAME,
   .parent        = TYPE_PCI_DEVICE,
   .class_init    = class_init,
   .instance_size = sizeof(KCovState),
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
