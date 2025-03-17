#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu-common.h"
#include "qemu/iov.h"
#include "sysemu/dma.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-bus.h"
#include "qemu/log.h"
#include "qapi/error.h"

#include "virtio.h"


static void virtio_periscope_get_config(VirtIODevice *vdev, uint8_t *config)
{
    VirtIOPeriScope *p = VIRTIO_PERISCOPE(vdev);

    (void)p;
}

static void virtio_periscope_set_config(VirtIODevice *vdev, const uint8_t *config)
{
    VirtIOPeriScope *p = VIRTIO_PERISCOPE(vdev);

    (void)p;
}

static uint64_t virtio_periscope_get_features(VirtIODevice *vdev, uint64_t features,
                                              Error **errp)
{
    VirtIOPeriScope *p = VIRTIO_PERISCOPE(vdev);

    (void)p;

    return features;
}

static void virtio_periscope_set_features(VirtIODevice *vdev, uint64_t features)
{
    VirtIOPeriScope *p = VIRTIO_PERISCOPE(vdev);

    (void)p;
}

#if 0
static int virtio_periscope_save(QEMUFile *f, void *opaque, size_t size,
                                 const VMStateField *field, QJSON *vmdesc)
{
    VirtIOPeriScope *p = opaque;

    (void)p;

    return 0;
}

static int virtio_periscope_load(QEMUFile *f, void *opaque, size_t size,
                                 const VMStateField *field)
{
    VirtIOPeriScope *p = opaque;

    (void)p;

    return 0;
}
#endif

static int virtio_periscope_load(VirtIODevice *vdev, QEMUFile *f,
                                 int version_id) {

    return 0;
}

/* Guest wrote coverage */
static void handle_kcov_output(VirtIODevice *vdev, VirtQueue *vq)
{

}

static void virtio_periscope_device_realize(DeviceState *qdev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(qdev);
    VirtIOPeriScope *p = VIRTIO_PERISCOPE(qdev);
    // Error *local_err = NULL;

    virtio_init(vdev, "virtio-periscope", VIRTIO_ID_PERISCOPE, 0);

    /* Add a queue for kcov map transfers from guest to host */
    p->kcov_ovq = virtio_add_queue(vdev, 128, handle_kcov_output);

    (void)vdev;
    (void)p;
}

static void virtio_periscope_device_unrealize(DeviceState *qdev, Error **errp)
{
    VirtIOPeriScope *p = VIRTIO_PERISCOPE(qdev);

    (void)p;
}

static void virtio_periscope_instance_init(Object *obj)
{
}

static void virtio_periscope_reset(VirtIODevice *vdev)
{
    VirtIOPeriScope *p = VIRTIO_PERISCOPE(vdev);

    (void)p;
}

static Property virtio_periscope_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_periscope_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

    vdc->realize = virtio_periscope_device_realize;
    vdc->unrealize = virtio_periscope_device_unrealize;
    vdc->get_config = virtio_periscope_get_config;
    vdc->set_config = virtio_periscope_set_config;
    vdc->get_features = virtio_periscope_get_features;
    vdc->set_features = virtio_periscope_set_features;

    vdc->reset = virtio_periscope_reset;
    vdc->load = virtio_periscope_load;

    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    dc->props = virtio_periscope_properties;
    dc->hotpluggable = false;
}

static const TypeInfo virtio_periscope_info = {
    .name = TYPE_VIRTIO_PERISCOPE,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOPeriScope),
    .instance_init = virtio_periscope_instance_init,
    .class_init = virtio_periscope_class_init,
};

static void virtio_register_types(void)
{
    type_register_static(&virtio_periscope_info);
}

type_init(virtio_register_types)


#if 0

#include "kcov.h"

static void kcov_memory_listener_commit(MemoryListener *listener) {

}

static void kcov_device_realize(DeviceState *dev, Error **errp)
{
    KCovDevice *kcdev = KCOV_DEVICE(dev);
    KCovDeviceClass *kcdc = KCOV_DEVICE_GET_CLASS(dev);
    Error *err = NULL;

    virtio_bus_device_plugged(kdev, &err);
    if (err != NULL) {
        error_propagate(errp, err);
        kcdc->unrealize(dev, NULL);
        return;
    }

    memory_listener_register(&kcdev->listener, kcdev->dma_as);
}

static void kcov_device_class_init(ObjectClass *klass, void *data) {
    /* Set the default value here. */
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = virtio_device_realize;
    dc->unrealize = virtio_device_unrealize;
    dc->bus_type = TYPE_VIRTIO_BUS;
};

static const TypeInfo kcov_device_info = {
    .name = TYPE_KCOV_DEVICE,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(KCovDevice),
    .class_init = kcov_device_class_init,
    .instance_finalize = kcov_device_instance_finalize,
    .abstract = true,
    .class_size = sizeof(KCovDeviceClass),
};

static void kcov_register_types(void)
{
    type_register_static(&kcov_device_info);
}

type_init(kcov_register_types)
#endif
