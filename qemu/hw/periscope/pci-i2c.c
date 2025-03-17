#include "qemu/osdep.h"
#include "exec/cpu-common.h"
#include "hw/pci/pci.h"
#include "migration/ram.h"

#define DEVICE_NAME "periscope-pci-i2c"

typedef struct {
  /*< private >*/
  PCIDevice parent_obj;
  MemoryRegion mmio;
  /*< public >*/
} I2CState;

typedef struct {
  unsigned int cmd;
  unsigned int arg;
  unsigned int ret;
} mmio_state;

static uint64_t mmio_read(void *opaque, hwaddr addr, unsigned size) {
  I2CState *s = opaque;
  (void)s;

  printf(DEVICE_NAME " mmio_read addr = %llx size = %x\n",
         (unsigned long long)addr, size);
  switch (addr) {
  default:
    break;
  }
  return 0x1234567812345678;
}

static void mmio_write(void *opaque, hwaddr addr, uint64_t val, unsigned size) {
  I2CState *s = opaque;
  (void)s;

  printf(DEVICE_NAME " mmio_write addr = %llx val = %llx size = %x\n",
         (unsigned long long)addr, (unsigned long long)val, size);
  switch (addr) {
  default:
    break;
  }
}

static const MemoryRegionOps mmio_ops = {
    .read = mmio_read,
    .write = mmio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void realize(PCIDevice *pdev, Error **errp) {
  I2CState *state = DO_UPCAST(I2CState, parent_obj, pdev);

  pci_config_set_interrupt_pin(pdev->config, 1);
  memory_region_init_io(&state->mmio, OBJECT(state), &mmio_ops, state,
                        DEVICE_NAME, 1024);
  pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &state->mmio);
}

static int pre_save(void *opaque) { return 0; }

static int post_load(void *opaque, int version_id) {
  I2CState *s = opaque;
  (void)s;

  return 0;
}

static const VMStateDescription vmstate = {
    .name = DEVICE_NAME,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = pre_save,
    .post_load = post_load,
    .fields = (VMStateField[]){VMSTATE_PCI_DEVICE(parent_obj, I2CState),
                               VMSTATE_END_OF_LIST()}};

static Property properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void class_init(ObjectClass *class, void *data) {
  DeviceClass *dc = DEVICE_CLASS(class);
  PCIDeviceClass *k = PCI_DEVICE_CLASS(class);

  k->realize = realize;
  k->vendor_id = PCI_VENDOR_ID_QEMU;
  k->device_id = 0x11eb;
  k->revision = 0x0;
  k->class_id = PCI_CLASS_OTHERS;
  dc->vmsd = &vmstate;
  dc->props = properties;
}

static const TypeInfo type_info = {
    .name = DEVICE_NAME,
    .parent = TYPE_PCI_DEVICE,
    .class_init = class_init,
    .instance_size = sizeof(I2CState),
    .class_size = sizeof(PCIDeviceClass),
    .interfaces =
        (InterfaceInfo[]){
            {INTERFACE_CONVENTIONAL_PCI_DEVICE},
            {},
        },

};

static void register_types(void) { type_register_static(&type_info); }

type_init(register_types)
