// addr = rreg: reg * 4

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/pci/msi.h"
#include "hw/pci/pci.h"
#include "hw/vfio/pci.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"

#include "moneta.h"

#define REGION_0_SIZE 0x10000000
#define REGION_2_SIZE 0x200000
#define REGION_5_SIZE 0x40000
#define IOPORT_SIZE 0x100

#define TYPE_PCI_AMDGPU "amdgpu"
#define PCI_AMDGPU(obj) OBJECT_CHECK(VFIOPCIDevice, obj, TYPE_PCI_AMDGPU)

static uint64_t RW0x0000065c =  0x7c37d7fb;
static uint64_t RW0x00000648 =  0x00000000;
static uint64_t RW0x00005314 =  0x00000000;
static uint64_t RW0x00006ea4 =  0x00000000;
static uint64_t RW0x000076a4 =  0x00000000;
static uint64_t RW0x00007c0c =  0x00000000;
static uint64_t RW0x0001040c =  0x00000000;
static uint64_t RW0x000106a4 =  0x00000000;
static uint64_t RW0x00010c0c =  0x00000000;
static uint64_t RW0x00010ea4 =  0x00000000;
static uint64_t RW0x0001140c =  0x00000000;
static uint64_t RW0x000116a4 =  0x00000000;
static uint64_t RW0x00006188 =  0x6db6d800;
static uint64_t RW0x00005bd4 =  0x00000000;
static uint64_t RW0x00002000 =  0x000000ff;
static uint64_t RW0x00001734 =  0x00000000;
static uint64_t RW0x000055a0 =  0x00000000;
static uint64_t RW0x00030194 =  0x00000000;
static uint64_t RW0x0003019c =  0x00000000;
static uint64_t RW0x00000324 =  0x00000000;
static uint64_t RW0x00002a58 =  0x308cf00d;
static uint64_t RW0x000028c0 =  0x00003033;
static uint64_t RW0x00001740 =  0x00000000;
static uint64_t RW0x00005404 =  0x00000000;
static uint64_t RW0x00000330 =  0x00000000;
static uint64_t RW0x00002784 =  0x00000000;
static uint64_t RW0x000014b0 =  0x00000000;
static uint64_t RW0x000014b4 =  0x00000000;
static uint64_t RW0x000014b8 =  0x00000000;
static uint64_t RW0x000014bc =  0x00000000;
static uint64_t RW0x00000328 =  0x00000000;
static uint64_t RW0x00000300 =  0x0000000f;
static uint64_t RW0x00002f4c =  0x00121fe0;
static uint64_t RW0x00002c00 =  0x0f200029;
static uint64_t RW0x00005480 =  0x00000001;
static uint64_t RW0x00002064 =  0x00000503;
static uint64_t RW0x00001400 =  0x0c0b8602;
static uint64_t RW0x00001404 =  0x00000000;
static uint64_t RW0x00001408 =  0x80100004;
static uint64_t RW0x000015e0 =  0x00000001;
static uint64_t RW0x0003b1c4 =  0x00000000;
static uint64_t RW0x00005468 =  0x00000010;
static uint64_t RW0x00003908 =  0x00000000;
static uint64_t RW0x00000248 =  0x00000000;
static uint64_t RW0x00003350 =  0x000c0200;
static uint64_t RW0x00009a10 =  0x00008208;
static uint64_t RW0x00009a18 =  0x00000000;
static uint64_t RW0x00009a0c =  0x00000000;
static uint64_t RW0x00009834 =  0x00000000;
static uint64_t RW0x00030a04 =  0x00000000;
static uint64_t RW0x00028354 =  0x0000002a;
static uint64_t RW0x0003b124 =  0x0001003f;
static uint64_t RW0x00008c00 =  0x01180000;
static uint64_t RW0x00009508 =  0x00000000;
static uint64_t RW0x0000ae00 =  0xf30fff7f;
static uint64_t RW0x0000ac14 =  0x000000f7;
static uint64_t RW0x000088c8 =  0x00000000;
static uint64_t RW0x00008000 =  0x00000018;
static uint64_t RW0x0000c700 =  0x00000000;
static uint64_t RW0x0000c9b0 =  0x00018006;
static uint64_t RW0x0000c950 =  0x00000000;
static uint64_t RW0x0000c99c =  0x18000100;
static uint64_t RW0x0000c958 =  0x00318509;
static uint64_t RW0x0000c93c =  0x00000000;
static uint64_t RW0x0000c968 =  0x18300000;
static uint64_t RW0x0000c96c =  0x18000000;
static uint64_t RW0x0000c9c8 =  0x00000006;
static uint64_t RW0x0000c9b4 =  0x40000000;
static uint64_t RW0x0000c9b8 =  0x007f0000;
static uint64_t RW0x0000c9c0 =  0x00000000;
static uint64_t RW0x0000c9c4 =  0x00000000;
static uint64_t RW0x0000c9cc =  0x00000000;
static uint64_t RW0x0000c9d0 =  0x00000000;
static uint64_t RW0x0000c9d4 =  0x00000000;
static uint64_t RW0x0000c9d8 =  0x00000000;
static uint64_t RW0x0000c9bc =  0x00000000;
static uint64_t RW0x0000c9e0 =  0x00000000;
static uint64_t RW0x0000c9e4 =  0x00000000;
static uint64_t RW0x0000c930 =  0x00000000;
static uint64_t RW0x0000c20c =  0x00000001;
static uint64_t RW0x0000c164 =  0x00000000;
static uint64_t RW0x000086d8 =  0x15000000;
static uint64_t RW0x0000c2e0 =  0x00000000;
static uint64_t RW0x0000d00c =  0xff000100;
static uint64_t RW0x0000d428 =  0x00000000;
static uint64_t RW0x0000d628 =  0x00000000;
static uint64_t RW0x0000d814 =  0x00810007;
static uint64_t RW0x0000d80c =  0xff000100;
static uint64_t RW0x0000dc28 =  0x00000000;
static uint64_t RW0x0000de28 =  0x00000000;
static uint64_t RW0x0000d248 =  0x00000000;
static uint64_t RW0x0000d214 =  0x00401000;
static uint64_t RW0x0000da48 =  0x00000000;
static uint64_t RW0x0000da14 =  0x00401000;
static uint64_t RW0x000025bc =  0x0000000f;
static uint64_t RW0x000027e8 =  0x0000040b;
static uint64_t RW0x00003600 =  0x05023924;
static uint64_t RW0x00012230 =  0x44440448;
static uint64_t RW0x00012234 =  0x00000000;
static uint64_t RW0x00012238 =  0x22220202;
static uint64_t RW0x00006310 =  0x00000000;
static uint64_t RW0x000062d0 =  0x00000000;
static uint64_t RW0x00006270 =  0x00000000;
static uint64_t RW0x000062f0 =  0x00000000;
static uint64_t RW0x000062b0 =  0x00000000;
static uint64_t RW0x00017000 =  0x01040000;
static uint64_t RW0x0001391c =  0x00010000;
static uint64_t RW0x000170e0 =  0x01040000;
static uint64_t RW0x00013d1c =  0x00010000;
static uint64_t RW0x00017070 =  0x01040000;
static uint64_t RW0x0001311c =  0x00010000;
static uint64_t RW0x00017150 =  0x01040000;
static uint64_t RW0x0001351c =  0x00010000;
static uint64_t RW0x00017230 =  0x01040000;
static uint64_t RW0x0001291c =  0x00010000;
static uint64_t RW0x00006e74 =  0x00000100;
static uint64_t RW0x00000338 =  0x00000000;
static uint64_t RW0x00007674 =  0x00000100;
static uint64_t RW0x000003e0 =  0x00000000;
static uint64_t RW0x00007e74 =  0x00000100;
static uint64_t RW0x000003e4 =  0x00000000;
static uint64_t RW0x00010674 =  0x00000100;
static uint64_t RW0x000003e8 =  0x00000000;
static uint64_t RW0x00010e74 =  0x00000100;
static uint64_t RW0x000003ec =  0x00000000;
static uint64_t RW0x00011674 =  0x00000100;
static uint64_t RW0x000060b4 =  0x00020070;
static uint64_t RW0x000060bc =  0xc0000009;
static uint64_t RW0x000058ac =  0x00000000;
static uint64_t RW0x000058a8 =  0x00000000;
static uint64_t RW0x000058a4 =  0x00000000;
static uint64_t RW0x00005944 =  0x00000000;
static uint64_t RW0x0000ef98 =  0x00000000;
static uint64_t RW0x0000f4a4 =  0x00680000;
static uint64_t RW0x0000f6f4 =  0xcafedead;
static uint64_t RW0x000202f8 =  0x00000040;
static uint64_t RW0x000216f4 =  0x00000000;
static uint64_t RW0x00021674 =  0x00000030;
static uint64_t RW0x0000c124 =  0x04fc0000;
static uint64_t RW0x0000c1ac =  0x00000000;
static uint64_t RW0x0000c1b0 =  0x00000000;
static uint64_t RW0x0000c2d0 =  0x04000000;
static uint64_t RW0x0000c224 =  0x00000000;
static uint64_t RW0x0000c228 =  0x00000000;
static uint64_t RW0x0000c22c =  0x00000000;
static uint64_t RW0x0000c230 =  0x00000000;
static uint64_t RW0x00008c54 =  0x00000000;
static uint64_t RW0x00021504 =  0x00000008;
static uint64_t RW0x00000018 =  0x00000000;
static uint64_t RW0x00005c24 =  0x64000002;
static uint64_t RW0x00005f94 =  0x00640018;
static uint64_t RW0x000003f0 =  0x00000100;
static uint64_t RW0x00017020 =  0x00320000;
static uint64_t RW0x00017090 =  0x00320000;
static uint64_t RW0x00017100 =  0x00320000;
static uint64_t RW0x00017170 =  0x00320000;
static uint64_t RW0x000171e0 =  0x00320000;
static uint64_t RW0x00017250 =  0x00320000;
static uint64_t RW0x00006ef8 =  0x00000000;
static uint64_t RW0x00006c0c =  0x00000000;
static uint64_t RW0x0000740c =  0x00000000;
static uint64_t RW0x0000d200 =  0x00000000;
static uint64_t RW0x0000da00 =  0x00000000;
static uint64_t RW0x0000d048 =  0x00000001;
static uint64_t RW0x0000d848 =  0x00000001;
static uint64_t RW0x00002774 =  0x2d270d13;
static uint64_t RW0x00000038 =  0x01400002;
static uint64_t RW0x00002778 =  0x3039338a;
static uint64_t RW0x0000ca04 =  0x00000000;
static uint64_t RW0x000020a0 =  0x000030ff;
static uint64_t RW0x00000004 =  0x00000044;
static uint64_t RW0x00000208 =  0xc0200000;
static uint64_t RW0x0000172c =  0x00000000;
static uint64_t RW0x0000173c =  0x00000000;
static uint64_t RW0x00001730 =  0x00000000;
static uint64_t RW0x00005428 =  0x00000000;
static uint64_t RW0x00000bd8 =  0x00000c04;
static uint64_t RW0x00000424 =  0x00000000;
static uint64_t RW0x00005f90 =  0x00000010;
static uint64_t RW0x00000350 =  0x00005018;
static uint64_t RW0x000004c8 =  0x00000001;
static uint64_t RW0x00017028 =  0x123d1110;
static uint64_t RW0x00017098 =  0x123d1110;
static uint64_t RW0x00017108 =  0x123d1110;
static uint64_t RW0x00017178 =  0x123d1110;
static uint64_t RW0x00017258 =  0x123d1110;
static uint64_t RW0x0001225c =  0x00010001;
static uint64_t RW0x000120ec =  0x0a0a0100;
static uint64_t RW0x000120f0 =  0x04040000;
static uint64_t RW0x00012074 =  0x00010000;
static uint64_t RW0x0001207c =  0x0000007d;
static uint64_t RW0x0001206c =  0x00000000;
static uint64_t RW0x00012310 =  0x00000004;
static uint64_t RW0x00012590 =  0x00000004;
static uint64_t RW0x00026810 =  0x00000004;
static uint64_t RW0x00026a90 =  0x00000004;
static uint64_t RW0x00026d10 =  0x00000004;
static uint64_t RW0x00026f90 =  0x00000004;
static uint64_t RW0x00000be8 =  0x00000000;
static uint64_t RW0x00006b30 =  0x00000002;
static uint64_t RW0x00006a44 =  0x00000035;
static uint64_t RW0x00006b68 =  0x00000111;
static uint64_t RW0x000069f4 =  0x00000000;
static uint64_t RW0x00006ce8 =  0x00000000;
static uint64_t RW0x00006cc8 =  0x00070707;
static uint64_t RW0x00006ccc =  0x00000000;
static uint64_t RW0x00006ed8 =  0x00000000;
static uint64_t RW0x00006e18 =  0x00000003;
static uint64_t RW0x00006f9c =  0x00010000;
static uint64_t RW0x00006c00 =  0x00000000;
static uint64_t RW0x00006e70 =  0x80400110;
static uint64_t RW0x00000bec =  0x00000000;
static uint64_t RW0x000076f8 =  0x00000000;
static uint64_t RW0x00007330 =  0x00000002;
static uint64_t RW0x00007244 =  0x00000035;
static uint64_t RW0x00007368 =  0x00000111;
static uint64_t RW0x000071f4 =  0x00000000;
static uint64_t RW0x000074d8 =  0x00000000;
static uint64_t RW0x000074e8 =  0x00000000;
static uint64_t RW0x000074c8 =  0x00070707;
static uint64_t RW0x000074cc =  0x00000000;
static uint64_t RW0x000076d8 =  0x00000000;
static uint64_t RW0x00007618 =  0x00000003;
static uint64_t RW0x0000779c =  0x00010000;
static uint64_t RW0x00007400 =  0x00000000;
static uint64_t RW0x00007670 =  0x80400110;
static uint64_t RW0x00000bf0 =  0x00000000;
static uint64_t RW0x00007ef8 =  0x00000000;
static uint64_t RW0x00007b30 =  0x00000002;
static uint64_t RW0x00007a44 =  0x00000035;
static uint64_t RW0x00007b68 =  0x00000111;
static uint64_t RW0x000079f4 =  0x00000000;
static uint64_t RW0x00007ce8 =  0x00000000;
static uint64_t RW0x00007cc8 =  0x00070707;
static uint64_t RW0x00007ccc =  0x00000000;
static uint64_t RW0x00007ed8 =  0x00000000;
static uint64_t RW0x00007e18 =  0x00000003;
static uint64_t RW0x00007f9c =  0x00010000;
static uint64_t RW0x00007c00 =  0x00000000;
static uint64_t RW0x00007e70 =  0x80400110;
static uint64_t RW0x00000bf4 =  0x00000000;
static uint64_t RW0x000106f8 =  0x00000000;
static uint64_t RW0x00010330 =  0x00000002;
static uint64_t RW0x00010244 =  0x00000035;
static uint64_t RW0x00010368 =  0x00000111;
static uint64_t RW0x000101f4 =  0x00000000;
static uint64_t RW0x000104e8 =  0x00000000;
static uint64_t RW0x000104c8 =  0x00070707;
static uint64_t RW0x000104cc =  0x00000000;
static uint64_t RW0x000106d8 =  0x00000000;
static uint64_t RW0x00010618 =  0x00000003;
static uint64_t RW0x0001079c =  0x00010000;
static uint64_t RW0x00010400 =  0x00000000;
static uint64_t RW0x00010670 =  0x80400110;
static uint64_t RW0x00000bf8 =  0x00000000;
static uint64_t RW0x00010ef8 =  0x00000000;
static uint64_t RW0x00010b30 =  0x00000002;
static uint64_t RW0x00010a44 =  0x00000035;
static uint64_t RW0x00010b68 =  0x00000111;
static uint64_t RW0x000109f4 =  0x00000000;
static uint64_t RW0x00010ce8 =  0x00000000;
static uint64_t RW0x00010cc8 =  0x00070707;
static uint64_t RW0x00010ccc =  0x00000000;
static uint64_t RW0x00010ed8 =  0x00000000;
static uint64_t RW0x00010e18 =  0x00000003;
static uint64_t RW0x00010f9c =  0x00010000;
static uint64_t RW0x00010c00 =  0x00000000;
static uint64_t RW0x00010e70 =  0x80400110;
static uint64_t RW0x00000bfc =  0x00000000;
static uint64_t RW0x000116f8 =  0x00000000;
static uint64_t RW0x00011330 =  0x00000002;
static uint64_t RW0x00011244 =  0x00000035;
static uint64_t RW0x00011368 =  0x00000111;
static uint64_t RW0x000111f4 =  0x00000000;
static uint64_t RW0x000114e8 =  0x00000000;
static uint64_t RW0x000114c8 =  0x00070707;
static uint64_t RW0x000114cc =  0x00000000;
static uint64_t RW0x000116d8 =  0x00000000;
static uint64_t RW0x00011618 =  0x00000003;
static uint64_t RW0x0001179c =  0x00010000;
static uint64_t RW0x00011400 =  0x00000000;
static uint64_t RW0x00011670 =  0x80400110;
static uint64_t RW0x00005bd0 =  0x00000000;
static uint64_t RW0x00005bec =  0x00000000;
static uint64_t RW0x00005bdc =  0x00000002;
static uint64_t RW0x00005be4 =  0x00000000;
static uint64_t RW0x00005490 =  0x00000000;
static uint64_t RW0x000020ac =  0x00000001;
static uint64_t RW0x00002004 =  0x00430210;
static uint64_t RW0x00002754 =  0x0e00c406;
static uint64_t RW0x00002a68 =  0x00000000;
static uint64_t RW0x00002a64 =  0x7005e000;
static uint64_t RW0x00002024 =  0xf5fff400;
static uint64_t RW0x00002aac =  0x00000000;
static uint64_t RW0x00002a0c =  0x00000000;
static uint64_t RW0x000027cc =  0x0000901b;
static uint64_t RW0x00002b9c =  0x00000000;
static uint64_t RW0x00001410 =  0x00fffed8;
static uint64_t RW0x00001414 =  0x00fffed8;
static uint64_t RW0x00028350 =  0x2a00126a;
static uint64_t RW0x0003b000 =  0x00000001;
static uint64_t RW0x0000c1a8 =  0x003c0000;
static uint64_t RW0x00008020 =  0x00000000;
static uint64_t RW0x0000d228 =  0x00000000;
static uint64_t RW0x0000da28 =  0x00000000;
static uint64_t RW0x0000d010 =  0x08010400;
static uint64_t RW0x0000d810 =  0x08010400;
static uint64_t RW0x00000cc0 =  0x000120ff;
static uint64_t RW0x00006304 =  0x00000000;
static uint64_t RW0x000062c4 =  0x00000000;
static uint64_t RW0x00006264 =  0x00000000;
static uint64_t RW0x000062e4 =  0x00000000;
static uint64_t RW0x000062a4 =  0x00000000;
static uint64_t RW0x0000ef90 =  0x00000000;
static uint64_t RW0x0000f4a8 =  0x003fffff;
static uint64_t RW0x0000f4b0 =  0x1fff018d;
static uint64_t RW0x0000e310 =  0x00000100;
static uint64_t RW0x0000f4f4 =  0x003e0030;
static uint64_t RW0x0000f6a4 =  0x1101010c;
static uint64_t RW0x0000f690 =  0x00000000;
static uint64_t RW0x0002027c =  0x00010000;
static uint64_t RW0x000207bc =  0xffc00040;
static uint64_t RW0x000202fc =  0x00ef0100;
static uint64_t RW0x000207c0 =  0x000007ff;
static uint64_t RW0x00020e40 =  0x00000000;
static uint64_t RW0x00020014 =  0x00200000;
static uint64_t RW0x00021500 =  0x00000000;
static uint64_t RW0x00020120 =  0x00000001;
static uint64_t RW0x0000c214 =  0x00000000;
static uint64_t RW0x0000c218 =  0x00000000;
static uint64_t RW0x0000c21c =  0x00000000;
static uint64_t RW0x0000c220 =  0x00000000;
static uint64_t RW0x00002f30 =  0x00000001;

void request_irq_amd(void *dev) {
    VFIOPCIDevice *vdev = dev;

    // printf("IRQ: %lu\n", qemu_clock_get_us(QEMU_CLOCK_VIRTUAL));

    pci_irq_assert(&vdev->pdev);
    pci_irq_deassert(&vdev->pdev);
}

static uint64_t region_read(hwaddr addr) {

    switch(addr){

        case 0x00004a6c:
            return  0x00000000;
        case 0x00002a4c:
            return  0x000000e4;
        case 0x00002a50:
            return  0x000000e4;
        case 0x000027ac:
            return  0x00028189;
        case 0x0003b2a8:
            return  0x58504840;
        case 0x0000001c:
            return  0x00000000;
        case 0x0000cc04:
            return  0x00010000;
        case 0x0000cd20:
            return  0x0000047f;
        case 0x00000e7c:
            return  0x00000001;
        case 0x00002a04:
            return  0x2014022a;
        case 0x00006d74:
            return  0x00000000;
        case 0x00000e50:
            return  0x20000040;
        case 0x0003b184:
            return  0x00000000;
        case 0x0003b188:
            return  0x00000000;
        case 0x0000c9e8:
            return  0x00000000;
        case 0x00002ba4:
            return  0x00000000;
        case 0x00002ba0:
            return  0x00000000;
        case 0x00002bc0:
            return  0x00000000;
        case 0x00002bc4:
            return  0x00000000;
        case 0x00002bb4:
            return  0x000000aa;
        case 0x00002bb8:
            return  0x00000000;
        case 0x00002bbc:
            return  0x00090004;
        case 0x00002bcc:
            return  0x00000000;
        case 0x00002bd0:
            return  0x00000000;
        case 0x00006cd4:
            return  0x00000200;
        case 0x0003b100:
            return  0x00080016;
        case 0x00012064:
            return  0x00000013;
        case 0x00012060:
            return  0x0000c400;
        case 0x00005eb4:
            return  0x185600f0;
        case 0x00005ec4:
            return  0x185600f0;
        case 0x00005ed4:
            return  0x185600f0;
        case 0x00005ee4:
            return  0x185600f0;
        case 0x00005ef4:
            return  0x185600f0;
        case 0x00005804:
            return  0x00000001;
        case 0x00006300:
            return  0x00000000;
        case 0x000062c0:
            return  0x00000000;
        case 0x00006260:
            return  0x00000000;
        case 0x000062e0:
            return  0x00000000;
        case 0x000062a0:
            return  0x00000000;
        case 0x00000e60:
            return  0x00000000;
        case 0x0000535c:
            return  0x00000003;


        case 0x0000065c:
            return RW0x0000065c;
        case 0x00000648:
            return RW0x00000648;
        case 0x00005314:
            return RW0x00005314;
        case 0x00006ea4:
            return RW0x00006ea4;
        case 0x000076a4:
            return RW0x000076a4;
        case 0x00007c0c:
            return RW0x00007c0c;
        case 0x0001040c:
            return RW0x0001040c;
        case 0x000106a4:
            return RW0x000106a4;
        case 0x00010c0c:
            return RW0x00010c0c;
        case 0x00010ea4:
            return RW0x00010ea4;
        case 0x0001140c:
            return RW0x0001140c;
        case 0x000116a4:
            return RW0x000116a4;
        case 0x00006188:
            return RW0x00006188;
        case 0x00005bd4:
            return RW0x00005bd4;
        case 0x00002000:
            return RW0x00002000;
        case 0x00001734:
            return RW0x00001734;
        case 0x000055a0:
            return RW0x000055a0;
        case 0x00030194:
            return RW0x00030194;
        case 0x0003019c:
            return RW0x0003019c;
        case 0x00000324:
            return RW0x00000324;
        case 0x00002a58:
            return RW0x00002a58;
        case 0x000028c0:
            return RW0x000028c0;
        case 0x00001740:
            return RW0x00001740;
        case 0x00005404:
            return RW0x00005404;
        case 0x00000330:
            return RW0x00000330;
        case 0x00002784:
            return RW0x00002784;
        case 0x000014b0:
            return RW0x000014b0;
        case 0x000014b4:
            return RW0x000014b4;
        case 0x000014b8:
            return RW0x000014b8;
        case 0x000014bc:
            return RW0x000014bc;
        case 0x00000328:
            return RW0x00000328;
        case 0x00000300:
            return RW0x00000300;
        case 0x00002f4c:
            return RW0x00002f4c;
        case 0x00002c00:
            return RW0x00002c00;
        case 0x00005480:
            return RW0x00005480;
        case 0x00002064:
            return RW0x00002064;
        case 0x00001400:
            return RW0x00001400;
        case 0x00001404:
            return RW0x00001404;
        case 0x00001408:
            return RW0x00001408;
        case 0x000015e0:
            return RW0x000015e0;
        case 0x0003b1c4:
            return RW0x0003b1c4;
        case 0x00005468:
            return RW0x00005468;
        case 0x00003908:
            return RW0x00003908;
        case 0x00000248:
            return RW0x00000248;
        case 0x00003350:
            return RW0x00003350;
        case 0x00009a10:
            return RW0x00009a10;
        case 0x00009a18:
            return RW0x00009a18;
        case 0x00009a0c:
            return RW0x00009a0c;
        case 0x00009834:
            return RW0x00009834;
        case 0x00030a04:
            return RW0x00030a04;
        case 0x00028354:
            return RW0x00028354;
        case 0x0003b124:
            return RW0x0003b124;
        case 0x00008c00:
            return RW0x00008c00;
        case 0x00009508:
            return RW0x00009508;
        case 0x0000ae00:
            return RW0x0000ae00;
        case 0x0000ac14:
            return RW0x0000ac14;
        case 0x000088c8:
            return RW0x000088c8;
        case 0x00008000:
            return RW0x00008000;
        case 0x0000c700:
            return RW0x0000c700;
        case 0x0000c9b0:
            return RW0x0000c9b0;
        case 0x0000c950:
            return RW0x0000c950;
        case 0x0000c99c:
            return RW0x0000c99c;
        case 0x0000c958:
            return RW0x0000c958;
        case 0x0000c93c:
            return RW0x0000c93c;
        case 0x0000c968:
            return RW0x0000c968;
        case 0x0000c96c:
            return RW0x0000c96c;
        case 0x0000c9c8:
            return RW0x0000c9c8;
        case 0x0000c9b4:
            return RW0x0000c9b4;
        case 0x0000c9b8:
            return RW0x0000c9b8;
        case 0x0000c9c0:
            return RW0x0000c9c0;
        case 0x0000c9c4:
            return RW0x0000c9c4;
        case 0x0000c9cc:
            return RW0x0000c9cc;
        case 0x0000c9d0:
            return RW0x0000c9d0;
        case 0x0000c9d4:
            return RW0x0000c9d4;
        case 0x0000c9d8:
            return RW0x0000c9d8;
        case 0x0000c9bc:
            return RW0x0000c9bc;
        case 0x0000c9e0:
            return RW0x0000c9e0;
        case 0x0000c9e4:
            return RW0x0000c9e4;
        case 0x0000c930:
            return RW0x0000c930;
        case 0x0000c20c:
            return RW0x0000c20c;
        case 0x0000c164:
            return RW0x0000c164;
        case 0x000086d8:
            return RW0x000086d8;
        case 0x0000c2e0:
            return RW0x0000c2e0;
        case 0x0000d00c:
            return RW0x0000d00c;
        case 0x0000d428:
            return RW0x0000d428;
        case 0x0000d628:
            return RW0x0000d628;
        case 0x0000d814:
            return RW0x0000d814;
        case 0x0000d80c:
            return RW0x0000d80c;
        case 0x0000dc28:
            return RW0x0000dc28;
        case 0x0000de28:
            return RW0x0000de28;
        case 0x0000d248:
            return RW0x0000d248;
        case 0x0000d214:
            return RW0x0000d214;
        case 0x0000da48:
            return RW0x0000da48;
        case 0x0000da14:
            return RW0x0000da14;
        case 0x000025bc:
            return RW0x000025bc;
        case 0x000027e8:
            return RW0x000027e8;
        case 0x00003600:
            return RW0x00003600;
        case 0x00012230:
            return RW0x00012230;
        case 0x00012234:
            return RW0x00012234;
        case 0x00012238:
            return RW0x00012238;
        case 0x00006310:
            return RW0x00006310;
        case 0x000062d0:
            return RW0x000062d0;
        case 0x00006270:
            return RW0x00006270;
        case 0x000062f0:
            return RW0x000062f0;
        case 0x000062b0:
            return RW0x000062b0;
        case 0x00017000:
            return RW0x00017000;
        case 0x0001391c:
            return RW0x0001391c;
        case 0x000170e0:
            return RW0x000170e0;
        case 0x00013d1c:
            return RW0x00013d1c;
        case 0x00017070:
            return RW0x00017070;
        case 0x0001311c:
            return RW0x0001311c;
        case 0x00017150:
            return RW0x00017150;
        case 0x0001351c:
            return RW0x0001351c;
        case 0x00017230:
            return RW0x00017230;
        case 0x0001291c:
            return RW0x0001291c;
        case 0x00006e74:
            return RW0x00006e74;
        case 0x00000338:
            return RW0x00000338;
        case 0x00007674:
            return RW0x00007674;
        case 0x000003e0:
            return RW0x000003e0;
        case 0x00007e74:
            return RW0x00007e74;
        case 0x000003e4:
            return RW0x000003e4;
        case 0x00010674:
            return RW0x00010674;
        case 0x000003e8:
            return RW0x000003e8;
        case 0x00010e74:
            return RW0x00010e74;
        case 0x000003ec:
            return RW0x000003ec;
        case 0x00011674:
            return RW0x00011674;
        case 0x000060b4:
            return RW0x000060b4;
        case 0x000060bc:
            return RW0x000060bc;
        case 0x000058ac:
            return RW0x000058ac;
        case 0x000058a8:
            return RW0x000058a8;
        case 0x000058a4:
            return RW0x000058a4;
        case 0x00005944:
            return RW0x00005944;
        case 0x0000ef98:
            return RW0x0000ef98;
        case 0x0000f4a4:
            return RW0x0000f4a4;
        case 0x0000f6f4:
            return RW0x0000f6f4;
        case 0x000202f8:
            return RW0x000202f8;
        case 0x000216f4:
            return RW0x000216f4;
        case 0x00021674:
            return RW0x00021674;
        case 0x0000c124:
            return RW0x0000c124;
        case 0x0000c1ac:
            return RW0x0000c1ac;
        case 0x0000c1b0:
            return RW0x0000c1b0;
        case 0x0000c2d0:
            return RW0x0000c2d0;
        case 0x0000c224:
            return RW0x0000c224;
        case 0x0000c228:
            return RW0x0000c228;
        case 0x0000c22c:
            return RW0x0000c22c;
        case 0x0000c230:
            return RW0x0000c230;
        case 0x00008c54:
            return RW0x00008c54;
        case 0x00021504:
            return RW0x00021504;
        case 0x00000018:
            return RW0x00000018;
        case 0x00005c24:
            return RW0x00005c24;
        case 0x00005f94:
            return RW0x00005f94;
        case 0x000003f0:
            return RW0x000003f0;
        case 0x00017020:
            return RW0x00017020;
        case 0x00017090:
            return RW0x00017090;
        case 0x00017100:
            return RW0x00017100;
        case 0x00017170:
            return RW0x00017170;
        case 0x000171e0:
            return RW0x000171e0;
        case 0x00017250:
            return RW0x00017250;
        case 0x00006ef8:
            return RW0x00006ef8;
        case 0x00006c0c:
            return RW0x00006c0c;
        case 0x0000740c:
            return RW0x0000740c;
        case 0x0000d200:
            return RW0x0000d200;
        case 0x0000da00:
            return RW0x0000da00;
        case 0x0000d048:
            return RW0x0000d048;
        case 0x0000d848:
            return RW0x0000d848;
        case 0x00002774:
            return RW0x00002774;
        case 0x00000038:
            return RW0x00000038;
        case 0x00002778:
            return RW0x00002778;
        case 0x0000ca04:
            return RW0x0000ca04;
        case 0x000020a0:
            return RW0x000020a0;
        case 0x00000004:
            return RW0x00000004;
        case 0x00000208:
            return RW0x00000208;
        case 0x0000172c:
            return RW0x0000172c;
        case 0x0000173c:
            return RW0x0000173c;
        case 0x00001730:
            return RW0x00001730;
        case 0x00005428:
            return RW0x00005428;
        case 0x00000bd8:
            return RW0x00000bd8;
        case 0x00000424:
            return RW0x00000424;
        case 0x00005f90:
            return RW0x00005f90;
        case 0x00000350:
            return RW0x00000350;
        case 0x000004c8:
            return RW0x000004c8;
        case 0x00017028:
            return RW0x00017028;
        case 0x00017098:
            return RW0x00017098;
        case 0x00017108:
            return RW0x00017108;
        case 0x00017178:
            return RW0x00017178;
        case 0x00017258:
            return RW0x00017258;
        case 0x0001225c:
            return RW0x0001225c;
        case 0x000120ec:
            return RW0x000120ec;
        case 0x000120f0:
            return RW0x000120f0;
        case 0x00012074:
            return RW0x00012074;
        case 0x0001207c:
            return RW0x0001207c;
        case 0x0001206c:
            return RW0x0001206c;
        case 0x00012310:
            return RW0x00012310;
        case 0x00012590:
            return RW0x00012590;
        case 0x00026810:
            return RW0x00026810;
        case 0x00026a90:
            return RW0x00026a90;
        case 0x00026d10:
            return RW0x00026d10;
        case 0x00026f90:
            return RW0x00026f90;
        case 0x00000be8:
            return RW0x00000be8;
        case 0x00006b30:
            return RW0x00006b30;
        case 0x00006a44:
            return RW0x00006a44;
        case 0x00006b68:
            return RW0x00006b68;
        case 0x000069f4:
            return RW0x000069f4;
        case 0x00006ce8:
            return RW0x00006ce8;
        case 0x00006cc8:
            return RW0x00006cc8;
        case 0x00006ccc:
            return RW0x00006ccc;
        case 0x00006ed8:
            return RW0x00006ed8;
        case 0x00006e18:
            return RW0x00006e18;
        case 0x00006f9c:
            return RW0x00006f9c;
        case 0x00006c00:
            return RW0x00006c00;
        case 0x00006e70:
            return RW0x00006e70;
        case 0x00000bec:
            return RW0x00000bec;
        case 0x000076f8:
            return RW0x000076f8;
        case 0x00007330:
            return RW0x00007330;
        case 0x00007244:
            return RW0x00007244;
        case 0x00007368:
            return RW0x00007368;
        case 0x000071f4:
            return RW0x000071f4;
        case 0x000074d8:
            return RW0x000074d8;
        case 0x000074e8:
            return RW0x000074e8;
        case 0x000074c8:
            return RW0x000074c8;
        case 0x000074cc:
            return RW0x000074cc;
        case 0x000076d8:
            return RW0x000076d8;
        case 0x00007618:
            return RW0x00007618;
        case 0x0000779c:
            return RW0x0000779c;
        case 0x00007400:
            return RW0x00007400;
        case 0x00007670:
            return RW0x00007670;
        case 0x00000bf0:
            return RW0x00000bf0;
        case 0x00007ef8:
            return RW0x00007ef8;
        case 0x00007b30:
            return RW0x00007b30;
        case 0x00007a44:
            return RW0x00007a44;
        case 0x00007b68:
            return RW0x00007b68;
        case 0x000079f4:
            return RW0x000079f4;
        case 0x00007ce8:
            return RW0x00007ce8;
        case 0x00007cc8:
            return RW0x00007cc8;
        case 0x00007ccc:
            return RW0x00007ccc;
        case 0x00007ed8:
            return RW0x00007ed8;
        case 0x00007e18:
            return RW0x00007e18;
        case 0x00007f9c:
            return RW0x00007f9c;
        case 0x00007c00:
            return RW0x00007c00;
        case 0x00007e70:
            return RW0x00007e70;
        case 0x00000bf4:
            return RW0x00000bf4;
        case 0x000106f8:
            return RW0x000106f8;
        case 0x00010330:
            return RW0x00010330;
        case 0x00010244:
            return RW0x00010244;
        case 0x00010368:
            return RW0x00010368;
        case 0x000101f4:
            return RW0x000101f4;
        case 0x000104e8:
            return RW0x000104e8;
        case 0x000104c8:
            return RW0x000104c8;
        case 0x000104cc:
            return RW0x000104cc;
        case 0x000106d8:
            return RW0x000106d8;
        case 0x00010618:
            return RW0x00010618;
        case 0x0001079c:
            return RW0x0001079c;
        case 0x00010400:
            return RW0x00010400;
        case 0x00010670:
            return RW0x00010670;
        case 0x00000bf8:
            return RW0x00000bf8;
        case 0x00010ef8:
            return RW0x00010ef8;
        case 0x00010b30:
            return RW0x00010b30;
        case 0x00010a44:
            return RW0x00010a44;
        case 0x00010b68:
            return RW0x00010b68;
        case 0x000109f4:
            return RW0x000109f4;
        case 0x00010ce8:
            return RW0x00010ce8;
        case 0x00010cc8:
            return RW0x00010cc8;
        case 0x00010ccc:
            return RW0x00010ccc;
        case 0x00010ed8:
            return RW0x00010ed8;
        case 0x00010e18:
            return RW0x00010e18;
        case 0x00010f9c:
            return RW0x00010f9c;
        case 0x00010c00:
            return RW0x00010c00;
        case 0x00010e70:
            return RW0x00010e70;
        case 0x00000bfc:
            return RW0x00000bfc;
        case 0x000116f8:
            return RW0x000116f8;
        case 0x00011330:
            return RW0x00011330;
        case 0x00011244:
            return RW0x00011244;
        case 0x00011368:
            return RW0x00011368;
        case 0x000111f4:
            return RW0x000111f4;
        case 0x000114e8:
            return RW0x000114e8;
        case 0x000114c8:
            return RW0x000114c8;
        case 0x000114cc:
            return RW0x000114cc;
        case 0x000116d8:
            return RW0x000116d8;
        case 0x00011618:
            return RW0x00011618;
        case 0x0001179c:
            return RW0x0001179c;
        case 0x00011400:
            return RW0x00011400;
        case 0x00011670:
            return RW0x00011670;
        case 0x00005bd0:
            return RW0x00005bd0;
        case 0x00005bec:
            return RW0x00005bec;
        case 0x00005bdc:
            return RW0x00005bdc;
        case 0x00005be4:
            return RW0x00005be4;
        case 0x00005490:
            return RW0x00005490;
        case 0x000020ac:
            return RW0x000020ac;
        case 0x00002004:
            return RW0x00002004;
        case 0x00002754:
            return RW0x00002754;
        case 0x00002a68:
            return RW0x00002a68;
        case 0x00002a64:
            return RW0x00002a64;
        case 0x00002024:
            return RW0x00002024;
        case 0x00002aac:
            return RW0x00002aac;
        case 0x00002a0c:
            return RW0x00002a0c;
        case 0x000027cc:
            return RW0x000027cc;
        case 0x00002b9c:
            return RW0x00002b9c;
        case 0x00001410:
            return RW0x00001410;
        case 0x00001414:
            return RW0x00001414;
        case 0x00028350:
            return RW0x00028350;
        case 0x0003b000:
            return RW0x0003b000;
        case 0x0000c1a8:
            return RW0x0000c1a8;
        case 0x00008020:
            return RW0x00008020;
        case 0x0000d228:
            return RW0x0000d228;
        case 0x0000da28:
            return RW0x0000da28;
        case 0x0000d010:
            return RW0x0000d010;
        case 0x0000d810:
            return RW0x0000d810;
        case 0x00000cc0:
            return RW0x00000cc0;
        case 0x00006304:
            return RW0x00006304;
        case 0x000062c4:
            return RW0x000062c4;
        case 0x00006264:
            return RW0x00006264;
        case 0x000062e4:
            return RW0x000062e4;
        case 0x000062a4:
            return RW0x000062a4;
        case 0x0000ef90:
            return RW0x0000ef90;
        case 0x0000f4a8:
            return RW0x0000f4a8;
        case 0x0000f4b0:
            return RW0x0000f4b0;
        case 0x0000e310:
            return RW0x0000e310;
        case 0x0000f4f4:
            return RW0x0000f4f4;
        case 0x0000f6a4:
            return RW0x0000f6a4;
        case 0x0000f690:
            return RW0x0000f690;
        case 0x0002027c:
            return RW0x0002027c;
        case 0x000207bc:
            return RW0x000207bc;
        case 0x000202fc:
            return RW0x000202fc;
        case 0x000207c0:
            return RW0x000207c0;
        case 0x00020e40:
            return RW0x00020e40;
        case 0x00020014:
            return RW0x00020014;
        case 0x00021500:
            return RW0x00021500;
        case 0x00020120:
            return RW0x00020120;
        case 0x0000c214:
            return RW0x0000c214;
        case 0x0000c218:
            return RW0x0000c218;
        case 0x0000c21c:
            return RW0x0000c21c;
        case 0x0000c220:
            return RW0x0000c220;
        case 0x00002f30:
            return RW0x00002f30;

        default:
            return 0x0;     
    }

    return 0x0;
}

static void region_write(hwaddr addr, uint64_t val) {
    switch(addr){
        case 0x0000065c:
            RW0x0000065c = val;
            break;
        case 0x00000648:
            RW0x00000648 = val;
            break;
        case 0x00005314:
            RW0x00005314 = val;
            break;
        case 0x00006ea4:
            RW0x00006ea4 = val;
            break;
        case 0x000076a4:
            RW0x000076a4 = val;
            break;
        case 0x00007c0c:
            RW0x00007c0c = val;
            break;
        case 0x0001040c:
            RW0x0001040c = val;
            break;
        case 0x000106a4:
            RW0x000106a4 = val;
            break;
        case 0x00010c0c:
            RW0x00010c0c = val;
            break;
        case 0x00010ea4:
            RW0x00010ea4 = val;
            break;
        case 0x0001140c:
            RW0x0001140c = val;
            break;
        case 0x000116a4:
            RW0x000116a4 = val;
            break;
        case 0x00006188:
            RW0x00006188 = val;
            break;
        case 0x00005bd4:
            RW0x00005bd4 = val;
            break;
        case 0x00002000:
            RW0x00002000 = val;
            break;
        case 0x00001734:
            RW0x00001734 = val;
            break;
        case 0x000055a0:
            RW0x000055a0 = val;
            break;
        case 0x00030194:
            RW0x00030194 = val;
            break;
        case 0x0003019c:
            RW0x0003019c = val;
            break;
        case 0x00000324:
            RW0x00000324 = val;
            break;
        case 0x00002a58:
            RW0x00002a58 = val;
            break;
        case 0x000028c0:
            RW0x000028c0 = val;
            break;
        case 0x00001740:
            RW0x00001740 = val;
            break;
        case 0x00005404:
            RW0x00005404 = val;
            break;
        case 0x00000330:
            RW0x00000330 = val;
            break;
        case 0x00002784:
            RW0x00002784 = val;
            break;
        case 0x000014b0:
            RW0x000014b0 = val;
            break;
        case 0x000014b4:
            RW0x000014b4 = val;
            break;
        case 0x000014b8:
            RW0x000014b8 = val;
            break;
        case 0x000014bc:
            RW0x000014bc = val;
            break;
        case 0x00000328:
            RW0x00000328 = val;
            break;
        case 0x00000300:
            RW0x00000300 = val;
            break;
        case 0x00002f4c:
            RW0x00002f4c = val;
            break;
        case 0x00002c00:
            RW0x00002c00 = val;
            break;
        case 0x00005480:
            RW0x00005480 = val;
            break;
        case 0x00002064:
            RW0x00002064 = val;
            break;
        case 0x00001400:
            RW0x00001400 = val;
            break;
        case 0x00001404:
            RW0x00001404 = val;
            break;
        case 0x00001408:
            RW0x00001408 = val;
            break;
        case 0x000015e0:
            RW0x000015e0 = val;
            break;
        case 0x0003b1c4:
            RW0x0003b1c4 = val;
            break;
        case 0x00005468:
            RW0x00005468 = val;
            break;
        case 0x00003908:
            RW0x00003908 = val;
            break;
        case 0x00000248:
            RW0x00000248 = val;
            break;
        case 0x00003350:
            RW0x00003350 = val;
            break;
        case 0x00009a10:
            RW0x00009a10 = val;
            break;
        case 0x00009a18:
            RW0x00009a18 = val;
            break;
        case 0x00009a0c:
            RW0x00009a0c = val;
            break;
        case 0x00009834:
            RW0x00009834 = val;
            break;
        case 0x00030a04:
            RW0x00030a04 = val;
            break;
        case 0x00028354:
            RW0x00028354 = val;
            break;
        case 0x0003b124:
            RW0x0003b124 = val;
            break;
        case 0x00008c00:
            RW0x00008c00 = val;
            break;
        case 0x00009508:
            RW0x00009508 = val;
            break;
        case 0x0000ae00:
            RW0x0000ae00 = val;
            break;
        case 0x0000ac14:
            RW0x0000ac14 = val;
            break;
        case 0x000088c8:
            RW0x000088c8 = val;
            break;
        case 0x00008000:
            RW0x00008000 = val;
            break;
        case 0x0000c700:
            RW0x0000c700 = val;
            break;
        case 0x0000c9b0:
            RW0x0000c9b0 = val;
            break;
        case 0x0000c950:
            RW0x0000c950 = val;
            break;
        case 0x0000c99c:
            RW0x0000c99c = val;
            break;
        case 0x0000c958:
            RW0x0000c958 = val;
            break;
        case 0x0000c93c:
            RW0x0000c93c = val;
            break;
        case 0x0000c968:
            RW0x0000c968 = val;
            break;
        case 0x0000c96c:
            RW0x0000c96c = val;
            break;
        case 0x0000c9c8:
            RW0x0000c9c8 = val;
            break;
        case 0x0000c9b4:
            RW0x0000c9b4 = val;
            break;
        case 0x0000c9b8:
            RW0x0000c9b8 = val;
            break;
        case 0x0000c9c0:
            RW0x0000c9c0 = val;
            break;
        case 0x0000c9c4:
            RW0x0000c9c4 = val;
            break;
        case 0x0000c9cc:
            RW0x0000c9cc = val;
            break;
        case 0x0000c9d0:
            RW0x0000c9d0 = val;
            break;
        case 0x0000c9d4:
            RW0x0000c9d4 = val;
            break;
        case 0x0000c9d8:
            RW0x0000c9d8 = val;
            break;
        case 0x0000c9bc:
            RW0x0000c9bc = val;
            break;
        case 0x0000c9e0:
            RW0x0000c9e0 = val;
            break;
        case 0x0000c9e4:
            RW0x0000c9e4 = val;
            break;
        case 0x0000c930:
            RW0x0000c930 = val;
            break;
        case 0x0000c20c:
            RW0x0000c20c = val;
            break;
        case 0x0000c164:
            RW0x0000c164 = val;
            break;
        case 0x000086d8:
            RW0x000086d8 = val;
            break;
        case 0x0000c2e0:
            RW0x0000c2e0 = val;
            break;
        case 0x0000d00c:
            RW0x0000d00c = val;
            break;
        case 0x0000d428:
            RW0x0000d428 = val;
            break;
        case 0x0000d628:
            RW0x0000d628 = val;
            break;
        case 0x0000d814:
            RW0x0000d814 = val;
            break;
        case 0x0000d80c:
            RW0x0000d80c = val;
            break;
        case 0x0000dc28:
            RW0x0000dc28 = val;
            break;
        case 0x0000de28:
            RW0x0000de28 = val;
            break;
        case 0x0000d248:
            RW0x0000d248 = val;
            break;
        case 0x0000d214:
            RW0x0000d214 = val;
            break;
        case 0x0000da48:
            RW0x0000da48 = val;
            break;
        case 0x0000da14:
            RW0x0000da14 = val;
            break;
        case 0x000025bc:
            RW0x000025bc = val;
            break;
        case 0x000027e8:
            RW0x000027e8 = val;
            break;
        case 0x00003600:
            RW0x00003600 = val;
            break;
        case 0x00012230:
            RW0x00012230 = val;
            break;
        case 0x00012234:
            RW0x00012234 = val;
            break;
        case 0x00012238:
            RW0x00012238 = val;
            break;
        case 0x00006310:
            RW0x00006310 = val;
            break;
        case 0x000062d0:
            RW0x000062d0 = val;
            break;
        case 0x00006270:
            RW0x00006270 = val;
            break;
        case 0x000062f0:
            RW0x000062f0 = val;
            break;
        case 0x000062b0:
            RW0x000062b0 = val;
            break;
        case 0x00017000:
            RW0x00017000 = val;
            break;
        case 0x0001391c:
            RW0x0001391c = val;
            break;
        case 0x000170e0:
            RW0x000170e0 = val;
            break;
        case 0x00013d1c:
            RW0x00013d1c = val;
            break;
        case 0x00017070:
            RW0x00017070 = val;
            break;
        case 0x0001311c:
            RW0x0001311c = val;
            break;
        case 0x00017150:
            RW0x00017150 = val;
            break;
        case 0x0001351c:
            RW0x0001351c = val;
            break;
        case 0x00017230:
            RW0x00017230 = val;
            break;
        case 0x0001291c:
            RW0x0001291c = val;
            break;
        case 0x00006e74:
            RW0x00006e74 = val;
            break;
        case 0x00000338:
            RW0x00000338 = val;
            break;
        case 0x00007674:
            RW0x00007674 = val;
            break;
        case 0x000003e0:
            RW0x000003e0 = val;
            break;
        case 0x00007e74:
            RW0x00007e74 = val;
            break;
        case 0x000003e4:
            RW0x000003e4 = val;
            break;
        case 0x00010674:
            RW0x00010674 = val;
            break;
        case 0x000003e8:
            RW0x000003e8 = val;
            break;
        case 0x00010e74:
            RW0x00010e74 = val;
            break;
        case 0x000003ec:
            RW0x000003ec = val;
            break;
        case 0x00011674:
            RW0x00011674 = val;
            break;
        case 0x000060b4:
            RW0x000060b4 = val;
            break;
        case 0x000060bc:
            RW0x000060bc = val;
            break;
        case 0x000058ac:
            RW0x000058ac = val;
            break;
        case 0x000058a8:
            RW0x000058a8 = val;
            break;
        case 0x000058a4:
            RW0x000058a4 = val;
            break;
        case 0x00005944:
            RW0x00005944 = val;
            break;
        case 0x0000ef98:
            RW0x0000ef98 = val;
            break;
        case 0x0000f4a4:
            RW0x0000f4a4 = val;
            break;
        case 0x0000f6f4:
            RW0x0000f6f4 = val;
            break;
        case 0x000202f8:
            RW0x000202f8 = val;
            break;
        case 0x000216f4:
            RW0x000216f4 = val;
            break;
        case 0x00021674:
            RW0x00021674 = val;
            break;
        case 0x0000c124:
            RW0x0000c124 = val;
            break;
        case 0x0000c1ac:
            RW0x0000c1ac = val;
            break;
        case 0x0000c1b0:
            RW0x0000c1b0 = val;
            break;
        case 0x0000c2d0:
            RW0x0000c2d0 = val;
            break;
        case 0x0000c224:
            RW0x0000c224 = val;
            break;
        case 0x0000c228:
            RW0x0000c228 = val;
            break;
        case 0x0000c22c:
            RW0x0000c22c = val;
            break;
        case 0x0000c230:
            RW0x0000c230 = val;
            break;
        case 0x00008c54:
            RW0x00008c54 = val;
            break;
        case 0x00021504:
            RW0x00021504 = val;
            break;
        case 0x00000018:
            RW0x00000018 = val;
            break;
        case 0x00005c24:
            RW0x00005c24 = val;
            break;
        case 0x00005f94:
            RW0x00005f94 = val;
            break;
        case 0x000003f0:
            RW0x000003f0 = val;
            break;
        case 0x00017020:
            RW0x00017020 = val;
            break;
        case 0x00017090:
            RW0x00017090 = val;
            break;
        case 0x00017100:
            RW0x00017100 = val;
            break;
        case 0x00017170:
            RW0x00017170 = val;
            break;
        case 0x000171e0:
            RW0x000171e0 = val;
            break;
        case 0x00017250:
            RW0x00017250 = val;
            break;
        case 0x00006ef8:
            RW0x00006ef8 = val;
            break;
        case 0x00006c0c:
            RW0x00006c0c = val;
            break;
        case 0x0000740c:
            RW0x0000740c = val;
            break;
        case 0x0000d200:
            RW0x0000d200 = val;
            break;
        case 0x0000da00:
            RW0x0000da00 = val;
            break;
        case 0x0000d048:
            RW0x0000d048 = val;
            break;
        case 0x0000d848:
            RW0x0000d848 = val;
            break;
        case 0x00002774:
            RW0x00002774 = val;
            break;
        case 0x00000038:
            RW0x00000038 = val;
            break;
        case 0x00002778:
            RW0x00002778 = val;
            break;
        case 0x0000ca04:
            RW0x0000ca04 = val;
            break;
        case 0x000020a0:
            RW0x000020a0 = val;
            break;
        case 0x00000004:
            RW0x00000004 = val;
            break;
        case 0x00000208:
            RW0x00000208 = val;
            break;
        case 0x0000172c:
            RW0x0000172c = val;
            break;
        case 0x0000173c:
            RW0x0000173c = val;
            break;
        case 0x00001730:
            RW0x00001730 = val;
            break;
        case 0x00005428:
            RW0x00005428 = val;
            break;
        case 0x00000bd8:
            RW0x00000bd8 = val;
            break;
        case 0x00000424:
            RW0x00000424 = val;
            break;
        case 0x00005f90:
            RW0x00005f90 = val;
            break;
        case 0x00000350:
            RW0x00000350 = val;
            break;
        case 0x000004c8:
            RW0x000004c8 = val;
            break;
        case 0x00017028:
            RW0x00017028 = val;
            break;
        case 0x00017098:
            RW0x00017098 = val;
            break;
        case 0x00017108:
            RW0x00017108 = val;
            break;
        case 0x00017178:
            RW0x00017178 = val;
            break;
        case 0x00017258:
            RW0x00017258 = val;
            break;
        case 0x0001225c:
            RW0x0001225c = val;
            break;
        case 0x000120ec:
            RW0x000120ec = val;
            break;
        case 0x000120f0:
            RW0x000120f0 = val;
            break;
        case 0x00012074:
            RW0x00012074 = val;
            break;
        case 0x0001207c:
            RW0x0001207c = val;
            break;
        case 0x0001206c:
            RW0x0001206c = val;
            break;
        case 0x00012310:
            RW0x00012310 = val;
            break;
        case 0x00012590:
            RW0x00012590 = val;
            break;
        case 0x00026810:
            RW0x00026810 = val;
            break;
        case 0x00026a90:
            RW0x00026a90 = val;
            break;
        case 0x00026d10:
            RW0x00026d10 = val;
            break;
        case 0x00026f90:
            RW0x00026f90 = val;
            break;
        case 0x00000be8:
            RW0x00000be8 = val;
            break;
        case 0x00006b30:
            RW0x00006b30 = val;
            break;
        case 0x00006a44:
            RW0x00006a44 = val;
            break;
        case 0x00006b68:
            RW0x00006b68 = val;
            break;
        case 0x000069f4:
            RW0x000069f4 = val;
            break;
        case 0x00006ce8:
            RW0x00006ce8 = val;
            break;
        case 0x00006cc8:
            RW0x00006cc8 = val;
            break;
        case 0x00006ccc:
            RW0x00006ccc = val;
            break;
        case 0x00006ed8:
            RW0x00006ed8 = val;
            break;
        case 0x00006e18:
            RW0x00006e18 = val;
            break;
        case 0x00006f9c:
            RW0x00006f9c = val;
            break;
        case 0x00006c00:
            RW0x00006c00 = val;
            break;
        case 0x00006e70:
            RW0x00006e70 = val;
            break;
        case 0x00000bec:
            RW0x00000bec = val;
            break;
        case 0x000076f8:
            RW0x000076f8 = val;
            break;
        case 0x00007330:
            RW0x00007330 = val;
            break;
        case 0x00007244:
            RW0x00007244 = val;
            break;
        case 0x00007368:
            RW0x00007368 = val;
            break;
        case 0x000071f4:
            RW0x000071f4 = val;
            break;
        case 0x000074d8:
            RW0x000074d8 = val;
            break;
        case 0x000074e8:
            RW0x000074e8 = val;
            break;
        case 0x000074c8:
            RW0x000074c8 = val;
            break;
        case 0x000074cc:
            RW0x000074cc = val;
            break;
        case 0x000076d8:
            RW0x000076d8 = val;
            break;
        case 0x00007618:
            RW0x00007618 = val;
            break;
        case 0x0000779c:
            RW0x0000779c = val;
            break;
        case 0x00007400:
            RW0x00007400 = val;
            break;
        case 0x00007670:
            RW0x00007670 = val;
            break;
        case 0x00000bf0:
            RW0x00000bf0 = val;
            break;
        case 0x00007ef8:
            RW0x00007ef8 = val;
            break;
        case 0x00007b30:
            RW0x00007b30 = val;
            break;
        case 0x00007a44:
            RW0x00007a44 = val;
            break;
        case 0x00007b68:
            RW0x00007b68 = val;
            break;
        case 0x000079f4:
            RW0x000079f4 = val;
            break;
        case 0x00007ce8:
            RW0x00007ce8 = val;
            break;
        case 0x00007cc8:
            RW0x00007cc8 = val;
            break;
        case 0x00007ccc:
            RW0x00007ccc = val;
            break;
        case 0x00007ed8:
            RW0x00007ed8 = val;
            break;
        case 0x00007e18:
            RW0x00007e18 = val;
            break;
        case 0x00007f9c:
            RW0x00007f9c = val;
            break;
        case 0x00007c00:
            RW0x00007c00 = val;
            break;
        case 0x00007e70:
            RW0x00007e70 = val;
            break;
        case 0x00000bf4:
            RW0x00000bf4 = val;
            break;
        case 0x000106f8:
            RW0x000106f8 = val;
            break;
        case 0x00010330:
            RW0x00010330 = val;
            break;
        case 0x00010244:
            RW0x00010244 = val;
            break;
        case 0x00010368:
            RW0x00010368 = val;
            break;
        case 0x000101f4:
            RW0x000101f4 = val;
            break;
        case 0x000104e8:
            RW0x000104e8 = val;
            break;
        case 0x000104c8:
            RW0x000104c8 = val;
            break;
        case 0x000104cc:
            RW0x000104cc = val;
            break;
        case 0x000106d8:
            RW0x000106d8 = val;
            break;
        case 0x00010618:
            RW0x00010618 = val;
            break;
        case 0x0001079c:
            RW0x0001079c = val;
            break;
        case 0x00010400:
            RW0x00010400 = val;
            break;
        case 0x00010670:
            RW0x00010670 = val;
            break;
        case 0x00000bf8:
            RW0x00000bf8 = val;
            break;
        case 0x00010ef8:
            RW0x00010ef8 = val;
            break;
        case 0x00010b30:
            RW0x00010b30 = val;
            break;
        case 0x00010a44:
            RW0x00010a44 = val;
            break;
        case 0x00010b68:
            RW0x00010b68 = val;
            break;
        case 0x000109f4:
            RW0x000109f4 = val;
            break;
        case 0x00010ce8:
            RW0x00010ce8 = val;
            break;
        case 0x00010cc8:
            RW0x00010cc8 = val;
            break;
        case 0x00010ccc:
            RW0x00010ccc = val;
            break;
        case 0x00010ed8:
            RW0x00010ed8 = val;
            break;
        case 0x00010e18:
            RW0x00010e18 = val;
            break;
        case 0x00010f9c:
            RW0x00010f9c = val;
            break;
        case 0x00010c00:
            RW0x00010c00 = val;
            break;
        case 0x00010e70:
            RW0x00010e70 = val;
            break;
        case 0x00000bfc:
            RW0x00000bfc = val;
            break;
        case 0x000116f8:
            RW0x000116f8 = val;
            break;
        case 0x00011330:
            RW0x00011330 = val;
            break;
        case 0x00011244:
            RW0x00011244 = val;
            break;
        case 0x00011368:
            RW0x00011368 = val;
            break;
        case 0x000111f4:
            RW0x000111f4 = val;
            break;
        case 0x000114e8:
            RW0x000114e8 = val;
            break;
        case 0x000114c8:
            RW0x000114c8 = val;
            break;
        case 0x000114cc:
            RW0x000114cc = val;
            break;
        case 0x000116d8:
            RW0x000116d8 = val;
            break;
        case 0x00011618:
            RW0x00011618 = val;
            break;
        case 0x0001179c:
            RW0x0001179c = val;
            break;
        case 0x00011400:
            RW0x00011400 = val;
            break;
        case 0x00011670:
            RW0x00011670 = val;
            break;
        case 0x00005bd0:
            RW0x00005bd0 = val;
            break;
        case 0x00005bec:
            RW0x00005bec = val;
            break;
        case 0x00005bdc:
            RW0x00005bdc = val;
            break;
        case 0x00005be4:
            RW0x00005be4 = val;
            break;
        case 0x00005490:
            RW0x00005490 = val;
            break;
        case 0x000020ac:
            RW0x000020ac = val;
            break;
        case 0x00002004:
            RW0x00002004 = val;
            break;
        case 0x00002754:
            RW0x00002754 = val;
            break;
        case 0x00002a68:
            RW0x00002a68 = val;
            break;
        case 0x00002a64:
            RW0x00002a64 = val;
            break;
        case 0x00002024:
            RW0x00002024 = val;
            break;
        case 0x00002aac:
            RW0x00002aac = val;
            break;
        case 0x00002a0c:
            RW0x00002a0c = val;
            break;
        case 0x000027cc:
            RW0x000027cc = val;
            break;
        case 0x00002b9c:
            RW0x00002b9c = val;
            break;
        case 0x00001410:
            RW0x00001410 = val;
            break;
        case 0x00001414:
            RW0x00001414 = val;
            break;
        case 0x00028350:
            RW0x00028350 = val;
            break;
        case 0x0003b000:
            RW0x0003b000 = val;
            break;
        case 0x0000c1a8:
            RW0x0000c1a8 = val;
            break;
        case 0x00008020:
            RW0x00008020 = val;
            break;
        case 0x0000d228:
            RW0x0000d228 = val;
            break;
        case 0x0000da28:
            RW0x0000da28 = val;
            break;
        case 0x0000d010:
            RW0x0000d010 = val;
            break;
        case 0x0000d810:
            RW0x0000d810 = val;
            break;
        case 0x00000cc0:
            RW0x00000cc0 = val;
            break;
        case 0x00006304:
            RW0x00006304 = val;
            break;
        case 0x000062c4:
            RW0x000062c4 = val;
            break;
        case 0x00006264:
            RW0x00006264 = val;
            break;
        case 0x000062e4:
            RW0x000062e4 = val;
            break;
        case 0x000062a4:
            RW0x000062a4 = val;
            break;
        case 0x0000ef90:
            RW0x0000ef90 = val;
            break;
        case 0x0000f4a8:
            RW0x0000f4a8 = val;
            break;
        case 0x0000f4b0:
            RW0x0000f4b0 = val;
            break;
        case 0x0000e310:
            RW0x0000e310 = val;
            break;
        case 0x0000f4f4:
            RW0x0000f4f4 = val;
            break;
        case 0x0000f6a4:
            RW0x0000f6a4 = val;
            break;
        case 0x0000f690:
            RW0x0000f690 = val;
            break;
        case 0x0002027c:
            RW0x0002027c = val;
            break;
        case 0x000207bc:
            RW0x000207bc = val;
            break;
        case 0x000202fc:
            RW0x000202fc = val;
            break;
        case 0x000207c0:
            RW0x000207c0 = val;
            break;
        case 0x00020e40:
            RW0x00020e40 = val;
            break;
        case 0x00020014:
            RW0x00020014 = val;
            break;
        case 0x00021500:
            RW0x00021500 = val;
            break;
        case 0x00020120:
            RW0x00020120 = val;
            break;
        case 0x0000c214:
            RW0x0000c214 = val;
            break;
        case 0x0000c218:
            RW0x0000c218 = val;
            break;
        case 0x0000c21c:
            RW0x0000c21c = val;
            break;
        case 0x0000c220:
            RW0x0000c220 = val;
            break;
        case 0x00002f30:
            RW0x00002f30 = val;
            break;

        default:
            break;
    }

    return;
}

static uint64_t amdgpu_bar0_read(void *ptr, hwaddr addr, unsigned size) {
    uint64_t val = region_read(addr);
    // printf("bar0_read 0x%llx, 0x%08x, 0x%08x\n", addr, size, val);
    return val;
    // return 0x0;
}

static void amdgpu_bar0_write(void *ptr, hwaddr addr, uint64_t val, unsigned size) {
    // printf("bar0_write 0x%llx, 0x%08x, 0x%08lx\n", addr, size, val);
    region_write(addr, val);
}

static const MemoryRegionOps pci_amdgpu_bar0_ops = {
    .read = amdgpu_bar0_read,
    .write = amdgpu_bar0_write,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t amdgpu_bar2_read(void *ptr, hwaddr addr, unsigned size) {
    uint64_t val = region_read(addr);
    // printf("bar2_read 0x%llx, 0x%08x, 0x%08x\n", addr, size, val);
    return val;
    // return 0x0;
}

static void amdgpu_bar2_write(void *ptr, hwaddr addr, uint64_t val, unsigned size) {
    // printf("bar2_write 0x%llx, 0x%08x, 0x%08lx\n", addr, size, val);
    if (addr == 0x7a0)
        request_irq_amd(ptr);
    region_write(addr, val);
}

static const MemoryRegionOps pci_amdgpu_bar2_ops = {
    .read = amdgpu_bar2_read,
    .write = amdgpu_bar2_write,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t amdgpu_bar5_read(void *ptr, hwaddr addr, unsigned size) {
    uint64_t val = region_read(addr);
    // printf("bar5_read 0x%llx, 0x%08x, 0x%08x\n", addr, size, val);
    return val;
    // return 0x0;
}

static void amdgpu_bar5_write(void *ptr, hwaddr addr, uint64_t val, unsigned size) {
    // printf("bar5_write 0x%llx, 0x%08x, 0x%08lx\n", addr, size, val);
    region_write(addr, val);
}

static const MemoryRegionOps pci_amdgpu_bar5_ops = {
    .read = amdgpu_bar5_read,
    .write = amdgpu_bar5_write,
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

static void amdgpu_realize(PCIDevice *pdev, Error **errp) {
    VFIOPCIDevice *vdev = PCI_AMDGPU(pdev);
    VFIOBAR *bar0 = &vdev->bars[0];
    VFIOBAR *bar2 = &vdev->bars[2];
    VFIOBAR *bar5 = &vdev->bars[5];
    VFIOBAR *bar4 = &vdev->bars[4];

    bar0->mr = g_new0(MemoryRegion, 1);
    bar2->mr = g_new0(MemoryRegion, 1);
    bar5->mr = g_new0(MemoryRegion, 1);
    bar4->mr = g_new0(MemoryRegion, 1);

    printf("DBG: amdgpu realize\n");
    // Region 0: Memory at 4210000000 (64-bit, prefetchable) [size=256M]
    memory_region_init_io(bar0->mr, OBJECT(vdev), &pci_amdgpu_bar0_ops, vdev, "region0",
                          REGION_0_SIZE);
    pci_register_bar(&vdev->pdev, 0, PCI_BASE_ADDRESS_MEM_TYPE_64 | PCI_BASE_ADDRESS_MEM_PREFETCH, bar0->mr);

    // Region 2: Memory at 4208000000 (64-bit, prefetchable) [size=2M]
    memory_region_init_io(bar2->mr, OBJECT(vdev), &pci_amdgpu_bar2_ops, vdev, "region2",
                          REGION_2_SIZE);
    pci_register_bar(&vdev->pdev, 2, PCI_BASE_ADDRESS_MEM_TYPE_64 | PCI_BASE_ADDRESS_MEM_PREFETCH,
                     bar2->mr);

    // Region 5: Memory at 64100000 (32-bit, non-prefetchable) [size=256K]
    memory_region_init_io(bar5->mr, OBJECT(vdev), &pci_amdgpu_bar5_ops, vdev, "region5",
                          REGION_5_SIZE);
    pci_register_bar(&vdev->pdev, 5, PCI_BASE_ADDRESS_MEM_TYPE_32,
                     bar5->mr);

    // Region 4: I/O ports at 7000 [size=256]
    memory_region_init_io(bar4->mr, OBJECT(vdev), &pci_io_ops, vdev, "region4",
                          IOPORT_SIZE);
    pci_register_bar(&vdev->pdev, 4, PCI_BASE_ADDRESS_SPACE_IO, bar4->mr);

    pci_set_byte(&vdev->pdev.config[PCI_INTERRUPT_PIN], 0x1);

}

static void amdgpu_instance_finalize(Object *obj) {}

static void amdgpu_instance_init(Object *obj)
{
    PCIDevice *pci_dev = PCI_DEVICE(obj);
    VFIOPCIDevice *vdev = PCI_AMDGPU(obj);

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

static const VMStateDescription vmstate_amdgpu = {
    .name = "vfio-pci",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]){
        VMSTATE_PCI_DEVICE(pdev, VFIOPCIDevice), 
        VMSTATE_END_OF_LIST()
        }
};

static void amdgpu_pci_class_init(ObjectClass *klass, void *data) {
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pdc = PCI_DEVICE_CLASS(klass);

    dc->hotpluggable = false;
    dc->vmsd = &vmstate_amdgpu;
    dc->desc = "Fake AMDGPU dev";

    pdc->realize = amdgpu_realize;
    pdc->vendor_id = 0x1002;           // Advanced Micro Devices, Inc. [AMD/ATI] Ellesmere [Radeon RX 470/480/570/570X/580/580X/590]
    pdc->device_id = 0x67df;           // Radeon RX 580 Series (POLARIS10, DRM 3.49.0, 6.2.0+, LLVM 11.0.1)
    pdc->subsystem_vendor_id = 0x1458; // Subsystem: Gigabyte Technology Co., Ltd Ellesmere [Radeon RX 470/480/570/570X/580/580X/590]
    pdc->subsystem_id = 0x22fc;        // 
    pdc->class_id = PCI_CLASS_DISPLAY_VGA;
}

static const TypeInfo amdgpu_type_info = {
    .name = TYPE_PCI_AMDGPU,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(VFIOPCIDevice),
    .class_init = amdgpu_pci_class_init,
    .instance_init = amdgpu_instance_init,
    .instance_finalize = amdgpu_instance_finalize,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void register_amdgpu_dev_type(void) { type_register_static(&amdgpu_type_info); }

type_init(register_amdgpu_dev_type)
