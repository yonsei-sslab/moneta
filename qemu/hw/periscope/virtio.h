#ifndef VIRTIO_PERISCOPE_H
#define VIRTIO_PERISCOPE_H

#include "qemu/queue.h"
#include "hw/virtio/virtio.h"
#include "qemu/log.h"

#define TYPE_VIRTIO_PERISCOPE "virtio-periscope-device"
#define VIRTIO_PERISCOPE(obj)                                        \
        OBJECT_CHECK(VirtIOPeriScope, (obj), TYPE_VIRTIO_PERISCOPE)

#define VIRTIO_ID_PERISCOPE 16

#if 0
typedef struct VirtIOPeriScopeConf {

} VirtIOPeriScopeConf;
#endif

typedef struct VirtIOPeriScope {
    VirtIODevice parent_obj;

    VirtQueue *kcov_ovq;
} VirtIOPeriScope;

#if 0
struct KCovDevice {
    DeviceState parent_obj;
    AddressSpace *dma_as;
    MemoryListener listener;
    VirtQueueElement *vqelem;
};

typedef KCovDevice KCovDevice;

typedef struct KCovDeviceClass {
    /*< private >*/
    DeviceClass parent;
    /*< public >*/

} KCovDeviceClass;

#define TYPE_KCOV_DEVICE "kcov-device"
#define KCOV_DEVICE_GET_CLASS(obj) \
        OBJECT_GET_CLASS(KCovDeviceClass, obj, TYPE_KCOV_DEVICE)
#define KCOV_DEVICE_CLASS(klass) \
        OBJECT_CLASS_CHECK(KCovDeviceClass, klass, TYPE_KCOV_DEVICE)
#define KCOV_DEVICE(obj) \
        OBJECT_CHECK(KCovDevice, (obj), TYPE_KCOV_DEVICE)
#endif

#endif