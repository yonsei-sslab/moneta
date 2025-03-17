#ifndef KCOV_VDEV_H
#define KCOV_VDEV_H

#define KCOV_MAP_SIZE_POW2 16
#define KCOV_MAP_SIZE (1 << KCOV_MAP_SIZE_POW2)

uint8_t *kcov_get_area(void);
void kcov_flush_area(bool, int);
int kcov_get_area_offset(void *opaque);
void kcov_print_coverage(void);
int kcov_ioctl(void *opaque, unsigned int cmd, unsigned int arg);

#define KCOV_SET_IRQ 0x0
#define KCOV_RESET_IRQ 0x4
#define KCOV_CMD_OFFSET 0x10
#define KCOV_ARG_OFFSET 0x20
#define KCOV_CMD_MMAP 0x30
#define KCOV_RET_OFFSET 0x40
#define KCOV_GET_AREA_OFFSET 0x50
#define KCOV_CCMODE_OFFSET 0x60
#define KCOV_COV_FULL 0x70
#define KCOV_COV_ENABLE 0x80
#define KCOV_COV_DISABLE 0x90
#define KCOV_COV_REMOTE_ENABLE 0xa0
#define KCOV_COV_REMOTE_DISABLE 0xb0
#define KCOV_COV_COLLECT 0xc0
#define KCOV_GMODE_OFFSET 0xf0

#define KCOV_INIT_TRACE       _IOR('c', 1, unsigned long)
#define KCOV_ENABLE        _IO('c', 100)
#define KCOV_DISABLE       _IO('c', 101)

typedef struct {
   /*< private >*/
   PCIDevice parent_obj;
   MemoryRegion mmio;
   /*< public >*/
   uint64_t area_offset;
   bool trace_pc; // as opposed to trace_afl
   bool trace_pc_flush; // flush cov buffer when full only if trace_pc && trace_pc_flush
   bool trace_global; // enable collecting global coverage regardless of kernel control path contexts (e.g., softirq)
   char *dump_path;
   int fd;
} KCovState;

enum {
   KCOV_TRACE_PC = 0,
   KCOV_TRACE_CMP = 1,
	KCOV_TRACE_AFL = 2,
};

/*
 * The format for the types of collected comparisons.
 *
 * Bit 0 shows whether one of the arguments is a compile-time constant.
 * Bits 1 & 2 contain log2 of the argument size, up to 8 bytes.
 */
#define KCOV_CMP_CONST          (1 << 0)
#define KCOV_CMP_SIZE(n)        ((n) << 1)
#define KCOV_CMP_MASK           KCOV_CMP_SIZE(3)

#endif
