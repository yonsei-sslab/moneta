#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/smbus_slave.h"
#include "qemu-common.h"
#include "qemu/log.h"

#include "hw/display/edid.h"

struct I2CPeriScopeState {
  /*< private >*/
#if 1
  SMBusDevice smb;
#else
  I2CSlave i2c;
#endif
  /*< public >*/
  bool firstbyte;
  uint8_t reg;
  qemu_edid_info edid_info;
  uint8_t edid_blob[128];
};

typedef struct I2CPeriScopeState I2CPeriScopeState;

#define TYPE_I2CPERISCOPE "periscope-i2c"
#define I2CPERISCOPE(obj)                                                      \
  OBJECT_CHECK(I2CPeriScopeState, (obj), TYPE_I2CPERISCOPE)

static void i2c_periscope_reset(DeviceState *ds) {
  I2CPeriScopeState *s = I2CPERISCOPE(ds);

  s->firstbyte = false;
  s->reg = 0;
}

static int i2c_periscope_event(I2CSlave *i2c, enum i2c_event event) {
  I2CPeriScopeState *s = I2CPERISCOPE(i2c);

  printf("periscope: i2c_periscope_event\n");

  if (event == I2C_START_SEND) {
    s->firstbyte = true;
  }

  return 0;
}

static uint8_t i2c_periscope_rx(I2CSlave *i2c) {
  I2CPeriScopeState *s = I2CPERISCOPE(i2c);

  printf("periscope: i2c_periscope_rx\n");

  int value;
  value = s->edid_blob[s->reg % sizeof(s->edid_blob)];
  s->reg++;
  return value;
}

static int i2c_periscope_tx(I2CSlave *i2c, uint8_t data) {
  I2CPeriScopeState *s = I2CPERISCOPE(i2c);

  printf("periscope: i2c_periscope_tx\n");

  if (s->firstbyte) {
    s->reg = data;
    s->firstbyte = false;
    printf("periscope: [EDID] Written new pointer: %u\n", data);
    return 0;
  }

  /* Ignore all writes */
  s->reg++;
  return 0;
}

static void quick_cmd(SMBusDevice *dev, uint8_t read) {
  printf("periscope: quick_cmd\n");
}

static int write_data(SMBusDevice *dev, uint8_t *buf, uint8_t len) {
  printf("periscope: write_data\n");
  return 0;
}

static uint8_t receive_byte(SMBusDevice *dev) {
  printf("periscope: receive_byte\n");
  return 0;
}

static void i2c_periscope_init(Object *obj) {
  I2CPeriScopeState *s = I2CPERISCOPE(obj);

  qemu_edid_generate(s->edid_blob, sizeof(s->edid_blob), &s->edid_info);
}

static const VMStateDescription vmstate_i2c_periscope = {
    .name = TYPE_I2CPERISCOPE,
    .version_id = 1,
    .fields = (VMStateField[]){VMSTATE_BOOL(firstbyte, I2CPeriScopeState),
                               VMSTATE_UINT8(reg, I2CPeriScopeState),
                               VMSTATE_END_OF_LIST()}};

static Property i2c_periscope_properties[] = {
    DEFINE_EDID_PROPERTIES(I2CPeriScopeState, edid_info),
    DEFINE_PROP_END_OF_LIST(),
};

static void i2c_periscope_class_init(ObjectClass *oc, void *data) {
  DeviceClass *dc = DEVICE_CLASS(oc);
#if 1
  SMBusDeviceClass *smbdc = SMBUS_DEVICE_CLASS(oc);
//#else
  I2CSlaveClass *isc = I2C_SLAVE_CLASS(oc);
#endif

  dc->reset = i2c_periscope_reset;
  dc->vmsd = &vmstate_i2c_periscope;
  dc->props = i2c_periscope_properties;
#if 1
  smbdc->quick_cmd = quick_cmd;
  smbdc->write_data = write_data;
  smbdc->receive_byte = receive_byte;
//#else
  isc->event = i2c_periscope_event;
  isc->recv = i2c_periscope_rx;
  isc->send = i2c_periscope_tx;
#endif
}

static TypeInfo i2c_periscope_info = {
    .name = TYPE_I2CPERISCOPE,
#if 1
    .parent = TYPE_SMBUS_DEVICE,
#else
    .parent = TYPE_I2C_SLAVE,
#endif
    .instance_size = sizeof(I2CPeriScopeState),
    .instance_init = i2c_periscope_init,
    .class_init = i2c_periscope_class_init
};

static void register_devices(void) {
  type_register_static(&i2c_periscope_info);
}

type_init(register_devices);
