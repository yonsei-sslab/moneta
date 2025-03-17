#include "qemu/osdep.h"
#include "hw/hw.h"
#include "ui/console.h"
#include "hw/usb.h"
#include "hw/usb/desc.h"

/* Interface requests */
#define WACOM_GET_REPORT	0x2101
#define WACOM_SET_REPORT	0x2109

/* HID interface requests */
#define HID_GET_REPORT		0xa101
#define HID_GET_IDLE		0xa102
#define HID_GET_PROTOCOL	0xa103
#define HID_SET_IDLE		0x210a
#define HID_SET_PROTOCOL	0x210b

typedef struct USBPeriScopeState {
    USBDevice dev;
    USBEndpoint *intr;
    QEMUPutMouseEntry *eh_entry;
    int dx, dy, dz, buttons_state;
    int x, y;
    int mouse_grabbed;
    enum {
        WACOM_MODE_HID = 1,
        WACOM_MODE_WACOM = 2,
    } mode;
    uint8_t idle;
    int changed;
} USBPeriScopeState;

#define TYPE_USB_PERISCOPE "usb-periscope"
#define USB_PERISCOPE(obj) OBJECT_CHECK(USBPeriScopeState, (obj), TYPE_USB_PERISCOPE)

enum {
    STR_MANUFACTURER = 1,
    STR_PRODUCT,
    STR_SERIALNUMBER,
};

static const USBDescStrings desc_strings = {
    [STR_MANUFACTURER]     = "QEMU",
    [STR_PRODUCT]          = "PeriScope",
    [STR_SERIALNUMBER]     = "1",
};

static const USBDescIface desc_iface_periscope = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 2,
    .bInterfaceClass               = USB_CLASS_VENDOR_SPEC,
    .bInterfaceSubClass            = USB_CLASS_VENDOR_SPEC,
    .bInterfaceProtocol            = 0x02,
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress      = USB_DIR_IN | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x200,
            .bInterval             = 0x00,
        },
        {
            .bEndpointAddress      = USB_DIR_OUT | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 0x200,
            .bInterval             = 0x00,
        },
    },
};

static const USBDescDevice desc_device_periscope = {
    .bcdUSB                        = 0x0200,
    .bMaxPacketSize0               = 64,
    .bNumConfigurations            = 1,
    .bDeviceClass = USB_CLASS_VENDOR_SPEC,
    .bDeviceSubClass = USB_CLASS_VENDOR_SPEC,
    .bDeviceProtocol = USB_CLASS_VENDOR_SPEC,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 1,
            .bConfigurationValue   = 1,
            .bmAttributes          = USB_CFG_ATT_ONE,
            .bMaxPower             = 40,
            .nif = 1,
            .ifs = &desc_iface_periscope,
        },
    },
};

static const USBDesc desc_periscope = {
    .id = {
        .idVendor          = 0x4cc,
        .idProduct         = 0x2533,
        .bcdDevice         = 0x0,
        .iManufacturer     = STR_MANUFACTURER,
        .iProduct          = STR_PRODUCT,
        .iSerialNumber     = STR_SERIALNUMBER,
    },
    .full = &desc_device_periscope,
    .str  = desc_strings,
};

static void usb_mouse_event(void *opaque,
                            int dx1, int dy1, int dz1, int buttons_state)
{
    USBPeriScopeState *s = opaque;

    s->dx += dx1;
    s->dy += dy1;
    s->dz += dz1;
    s->buttons_state = buttons_state;
    s->changed = 1;
    usb_wakeup(s->intr, 0);
}

static void usb_periscope_event(void *opaque,
                            int x, int y, int dz, int buttons_state)
{
    USBPeriScopeState *s = opaque;

    /* scale to Penpartner resolution */
    s->x = (x * 5040 / 0x7FFF);
    s->y = (y * 3780 / 0x7FFF);
    s->dz += dz;
    s->buttons_state = buttons_state;
    s->changed = 1;
    usb_wakeup(s->intr, 0);
}

static inline int int_clamp(int val, int vmin, int vmax)
{
    if (val < vmin)
        return vmin;
    else if (val > vmax)
        return vmax;
    else
        return val;
}

static int usb_mouse_poll(USBPeriScopeState *s, uint8_t *buf, int len)
{
    int dx, dy, dz, b, l;

    if (!s->mouse_grabbed) {
        s->eh_entry = qemu_add_mouse_event_handler(usb_mouse_event, s, 0,
                        "QEMU PenPartner tablet");
        qemu_activate_mouse_event_handler(s->eh_entry);
        s->mouse_grabbed = 1;
    }

    dx = int_clamp(s->dx, -128, 127);
    dy = int_clamp(s->dy, -128, 127);
    dz = int_clamp(s->dz, -128, 127);

    s->dx -= dx;
    s->dy -= dy;
    s->dz -= dz;

    b = 0;
    if (s->buttons_state & MOUSE_EVENT_LBUTTON)
        b |= 0x01;
    if (s->buttons_state & MOUSE_EVENT_RBUTTON)
        b |= 0x02;
    if (s->buttons_state & MOUSE_EVENT_MBUTTON)
        b |= 0x04;

    buf[0] = b;
    buf[1] = dx;
    buf[2] = dy;
    l = 3;
    if (len >= 4) {
        buf[3] = dz;
        l = 4;
    }
    return l;
}

static int usb_periscope_poll(USBPeriScopeState *s, uint8_t *buf, int len)
{
    int b;

    if (!s->mouse_grabbed) {
        s->eh_entry = qemu_add_mouse_event_handler(usb_periscope_event, s, 1,
                        "QEMU PeriScope");
        qemu_activate_mouse_event_handler(s->eh_entry);
        s->mouse_grabbed = 1;
    }

    b = 0;
    if (s->buttons_state & MOUSE_EVENT_LBUTTON)
        b |= 0x01;
    if (s->buttons_state & MOUSE_EVENT_RBUTTON)
        b |= 0x40;
    if (s->buttons_state & MOUSE_EVENT_MBUTTON)
        b |= 0x20; /* eraser */

    if (len < 7)
        return 0;

    buf[0] = s->mode;
    buf[5] = 0x00 | (b & 0xf0);
    buf[1] = s->x & 0xff;
    buf[2] = s->x >> 8;
    buf[3] = s->y & 0xff;
    buf[4] = s->y >> 8;
    if (b & 0x3f) {
        buf[6] = 0;
    } else {
        buf[6] = (unsigned char) -127;
    }

    return 7;
}

static void usb_periscope_handle_reset(USBDevice *dev)
{
    USBPeriScopeState *s = (USBPeriScopeState *) dev;

    s->dx = 0;
    s->dy = 0;
    s->dz = 0;
    s->x = 0;
    s->y = 0;
    s->buttons_state = 0;
    s->mode = WACOM_MODE_HID;
}

static void usb_periscope_handle_control(USBDevice *dev, USBPacket *p,
               int request, int value, int index, int length, uint8_t *data)
{
    USBPeriScopeState *s = (USBPeriScopeState *) dev;
    int ret;

    ret = usb_desc_handle_control(dev, p, request, value, index, length, data);
    if (ret >= 0) {
        return;
    }

    switch (request) {
    case WACOM_SET_REPORT:
        if (s->mouse_grabbed) {
            qemu_remove_mouse_event_handler(s->eh_entry);
            s->mouse_grabbed = 0;
        }
        s->mode = data[0];
        break;
    case WACOM_GET_REPORT:
        data[0] = 0;
        data[1] = s->mode;
        p->actual_length = 2;
        break;
    /* USB HID requests */
    case HID_GET_REPORT:
        if (s->mode == WACOM_MODE_HID)
            p->actual_length = usb_mouse_poll(s, data, length);
        else if (s->mode == WACOM_MODE_WACOM)
            p->actual_length = usb_periscope_poll(s, data, length);
        break;
    case HID_GET_IDLE:
        data[0] = s->idle;
        p->actual_length = 1;
        break;
    case HID_SET_IDLE:
        s->idle = (uint8_t) (value >> 8);
        break;
    default:
        p->status = USB_RET_STALL;
        break;
    }
}

static void usb_periscope_handle_data(USBDevice *dev, USBPacket *p)
{
    USBPeriScopeState *s = (USBPeriScopeState *) dev;
    uint8_t buf[p->iov.size];
    int len = 0;

    switch (p->pid) {
    case USB_TOKEN_IN:
        if (p->ep->nr == 1) {
            if (!(s->changed || s->idle)) {
                p->status = USB_RET_NAK;
                return;
            }
            s->changed = 0;
            if (s->mode == WACOM_MODE_HID)
                len = usb_mouse_poll(s, buf, p->iov.size);
            else if (s->mode == WACOM_MODE_WACOM)
                len = usb_periscope_poll(s, buf, p->iov.size);
            usb_packet_copy(p, buf, len);
            break;
        }
        /* Fall through.  */
    case USB_TOKEN_OUT:
    default:
        p->status = USB_RET_STALL;
    }
}

static void usb_periscope_unrealize(USBDevice *dev, Error **errp)
{
    USBPeriScopeState *s = (USBPeriScopeState *) dev;

    if (s->mouse_grabbed) {
        qemu_remove_mouse_event_handler(s->eh_entry);
        s->mouse_grabbed = 0;
    }
}

static void usb_periscope_realize(USBDevice *dev, Error **errp)
{
    USBPeriScopeState *s = USB_PERISCOPE(dev);
    usb_desc_create_serial(dev);
    usb_desc_init(dev);
    s->intr = usb_ep_get(dev, USB_TOKEN_IN, 1);
    s->changed = 1;
}

static const VMStateDescription vmstate_usb_periscope = {
    .name = "usb-periscope",
    .unmigratable = 1,
};

static void usb_periscope_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->product_desc   = "QEMU PeriScope USB";
    uc->usb_desc       = &desc_periscope;
    uc->realize        = usb_periscope_realize;
    uc->handle_reset   = usb_periscope_handle_reset;
    uc->handle_control = usb_periscope_handle_control;
    uc->handle_data    = usb_periscope_handle_data;
    uc->unrealize      = usb_periscope_unrealize;
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
    dc->desc = "QEMU PeriScope USB";
    dc->vmsd = &vmstate_usb_periscope;
}

static const TypeInfo periscope_info = {
    .name          = TYPE_USB_PERISCOPE,
    .parent        = TYPE_USB_DEVICE,
    .instance_size = sizeof(USBPeriScopeState),
    .class_init    = usb_periscope_class_init,
};

static void usb_periscope_register_types(void)
{
    type_register_static(&periscope_info);
    usb_legacy_register(TYPE_USB_PERISCOPE, "periscope", NULL);
}

type_init(usb_periscope_register_types)
