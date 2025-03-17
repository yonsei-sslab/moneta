#include <linux/ioctl.h>
#include <linux/types.h>
// typedef __signed__ char __s8;
// typedef unsigned char __u8;

// typedef __signed__ short __s16;
// typedef unsigned short __u16;

// typedef __signed__ int __s32;
// typedef unsigned int __u32;

// typedef __signed__ long long __s64;
// typedef unsigned long long __u64;

#define KBASE_IOCTL_TYPE 0x80
typedef __u8 base_kcpu_queue_id;


struct kbase_ioctl_version_check {
	__u16 major;
	__u16 minor;
};

#define KBASE_IOCTL_VERSION_CHECK \
	_IOWR(KBASE_IOCTL_TYPE, 52, struct kbase_ioctl_version_check)

struct kbase_ioctl_set_flags {
    __u32 create_flags;
};

#define KBASE_IOCTL_SET_FLAGS \
	_IOW(KBASE_IOCTL_TYPE, 1, struct kbase_ioctl_set_flags)

struct kbase_ioctl_apc_request {
	__u32 dur_usec;
};

#define KBASE_IOCTL_APC_REQUEST \
	_IOW(KBASE_IOCTL_TYPE, 66, struct kbase_ioctl_apc_request)

struct kbase_ioctl_kinstr_prfcnt_enum_info {
    __u32 info_item_size;
    __u32 info_item_count;
    __u64 info_list_ptr;
};

#define KBASE_IOCTL_KINSTR_PRFCNT_ENUM_INFO                                    \
	_IOWR(KBASE_IOCTL_TYPE, 56, struct kbase_ioctl_kinstr_prfcnt_enum_info)

union kbase_ioctl_kinstr_prfcnt_setup {
    struct {
        __u32 request_item_count;
        __u32 request_item_size;
        __u64 requests_ptr;
    } in;
    struct {
        __u32 prfcnt_metadata_item_size;
        __u32 prfcnt_mmap_size_bytes;
    } out;
};

#define KBASE_IOCTL_KINSTR_PRFCNT_SETUP                                        \
	_IOWR(KBASE_IOCTL_TYPE, 57, union kbase_ioctl_kinstr_prfcnt_setup)

struct kbase_ioctl_get_gpuprops {
    __u64 buffer;
    __u32 size;
    __u32 flags;
};


#define KBASE_IOCTL_GET_GPUPROPS \
	_IOW(KBASE_IOCTL_TYPE, 3, struct kbase_ioctl_get_gpuprops)

union kbase_ioctl_mem_alloc {
    struct {
        __u64 va_pages;
        __u64 commit_pages;
        __u64 extension;
        __u64 flags;
    } in;
    struct {
        __u64 flags;
        __u64 gpu_va;
    } out;
};

#define KBASE_IOCTL_MEM_ALLOC \
	_IOWR(KBASE_IOCTL_TYPE, 5, union kbase_ioctl_mem_alloc)

union kbase_ioctl_mem_alloc_ex {
    struct {
        __u64 va_pages;
        __u64 commit_pages;
        __u64 extension;
        __u64 flags;
        __u64 fixed_address;
        __u64 extra[3];
    } in;
    struct {
        __u64 flags;
        __u64 gpu_va;
    } out;
};

#define KBASE_IOCTL_MEM_ALLOC_EX _IOWR(KBASE_IOCTL_TYPE, 59, union kbase_ioctl_mem_alloc_ex)

union kbase_ioctl_mem_query {
    struct {
        __u64 gpu_addr;
        __u64 query;
    } in;
    struct {
        __u64 value;
    } out;
};

#define KBASE_IOCTL_MEM_QUERY \
	_IOWR(KBASE_IOCTL_TYPE, 6, union kbase_ioctl_mem_query)

struct kbase_ioctl_mem_free {
    __u64 gpu_addr;
};

#define KBASE_IOCTL_MEM_FREE \
	_IOW(KBASE_IOCTL_TYPE, 7, struct kbase_ioctl_mem_free)

struct kbase_ioctl_disjoint_query {
    __u32 counter;
};

#define KBASE_IOCTL_DISJOINT_QUERY \
	_IOR(KBASE_IOCTL_TYPE, 12, struct kbase_ioctl_disjoint_query)

struct kbase_ioctl_get_ddk_version {
    __u64 version_buffer;
    __u32 size;
    __u32 padding;
};

#define KBASE_IOCTL_GET_DDK_VERSION \
	_IOW(KBASE_IOCTL_TYPE, 13, struct kbase_ioctl_get_ddk_version)

struct kbase_ioctl_mem_jit_init_10_2 {
    __u64 va_pages;
};

#define KBASE_IOCTL_MEM_JIT_INIT_10_2 \
	_IOW(KBASE_IOCTL_TYPE, 14, struct kbase_ioctl_mem_jit_init_10_2)


struct kbase_ioctl_mem_jit_init_11_5 {
    __u64 va_pages;
    __u8 max_allocations;
    __u8 trim_level;
    __u8 group_id;
    __u8 padding[5];
};

#define KBASE_IOCTL_MEM_JIT_INIT_11_5 \
	_IOW(KBASE_IOCTL_TYPE, 14, struct kbase_ioctl_mem_jit_init_11_5)


struct kbase_ioctl_mem_jit_init {
    __u64 va_pages;
    __u8 max_allocations;
    __u8 trim_level;
    __u8 group_id;
    __u8 padding[5];
    __u64 phys_pages;
};


#define KBASE_IOCTL_MEM_JIT_INIT \
	_IOW(KBASE_IOCTL_TYPE, 14, struct kbase_ioctl_mem_jit_init)


struct kbase_ioctl_mem_exec_init {
    __u64 va_pages;
};

#define KBASE_IOCTL_MEM_EXEC_INIT \
	_IOW(KBASE_IOCTL_TYPE, 38, struct kbase_ioctl_mem_exec_init)


struct kbase_ioctl_mem_sync {
    __u64 handle;
    __u64 user_addr;
    __u64 size;
    __u8 type;
    __u8 padding[7];
};

#define KBASE_IOCTL_MEM_SYNC \
	_IOW(KBASE_IOCTL_TYPE, 15, struct kbase_ioctl_mem_sync)


union kbase_ioctl_mem_find_cpu_offset {
    struct {
        __u64 gpu_addr;
        __u64 cpu_addr;
        __u64 size;
    } in;
    struct {
        __u64 offset;
    } out;
};

#define KBASE_IOCTL_MEM_FIND_CPU_OFFSET \
	_IOWR(KBASE_IOCTL_TYPE, 16, union kbase_ioctl_mem_find_cpu_offset)


union kbase_ioctl_mem_find_gpu_start_and_offset {
    struct {
        __u64 gpu_addr;
        __u64 size;
    } in;
    struct {
        __u64 start;
        __u64 offset;
    } out;
};

#define KBASE_IOCTL_MEM_FIND_GPU_START_AND_OFFSET \
	_IOWR(KBASE_IOCTL_TYPE, 31, union kbase_ioctl_mem_find_gpu_start_and_offset)


struct kbase_ioctl_get_context_id {
    __u32 id;
};

#define KBASE_IOCTL_GET_CONTEXT_ID \
	_IOR(KBASE_IOCTL_TYPE, 17, struct kbase_ioctl_get_context_id)


struct kbase_ioctl_tlstream_acquire {
    __u32 flags;
};

#define KBASE_IOCTL_TLSTREAM_ACQUIRE \
	_IOW(KBASE_IOCTL_TYPE, 18, struct kbase_ioctl_tlstream_acquire)

#define KBASE_IOCTL_TLSTREAM_FLUSH \
	_IO(KBASE_IOCTL_TYPE, 19)


struct kbase_ioctl_mem_commit {
    __u64 gpu_addr;
    __u64 pages;
};

#define KBASE_IOCTL_MEM_COMMIT \
	_IOW(KBASE_IOCTL_TYPE, 20, struct kbase_ioctl_mem_commit)


union kbase_ioctl_mem_alias {
    struct {
        __u64 flags;
        __u64 stride;
        __u64 nents;
        __u64 aliasing_info;
    } in;
    struct {
        __u64 flags;
        __u64 gpu_va;
        __u64 va_pages;
    } out;
};

#define KBASE_IOCTL_MEM_ALIAS \
	_IOWR(KBASE_IOCTL_TYPE, 21, union kbase_ioctl_mem_alias)


union kbase_ioctl_mem_import {
    struct {
        __u64 flags;
        __u64 phandle;
        __u32 type;
        __u32 padding;
    } in;
    struct {
        __u64 flags;
        __u64 gpu_va;
        __u64 va_pages;
    } out;
};


#define KBASE_IOCTL_MEM_IMPORT \
	_IOWR(KBASE_IOCTL_TYPE, 22, union kbase_ioctl_mem_import)


struct kbase_ioctl_mem_flags_change {
    __u64 gpu_va;
    __u64 flags;
    __u64 mask;
};

#define KBASE_IOCTL_MEM_FLAGS_CHANGE \
	_IOW(KBASE_IOCTL_TYPE, 23, struct kbase_ioctl_mem_flags_change)


struct kbase_ioctl_stream_create {
    char name[32];
};

#define KBASE_IOCTL_STREAM_CREATE \
	_IOW(KBASE_IOCTL_TYPE, 24, struct kbase_ioctl_stream_create)


struct kbase_ioctl_fence_validate {
    int fd;
};

#define KBASE_IOCTL_FENCE_VALIDATE \
	_IOW(KBASE_IOCTL_TYPE, 25, struct kbase_ioctl_fence_validate)


struct kbase_ioctl_mem_profile_add {
    __u64 buffer;
    __u32 len;
    __u32 padding;
};

#define KBASE_IOCTL_MEM_PROFILE_ADD \
	_IOW(KBASE_IOCTL_TYPE, 27, struct kbase_ioctl_mem_profile_add)


struct kbase_ioctl_sticky_resource_map {
    __u64 count;
    __u64 address;
};

#define KBASE_IOCTL_STICKY_RESOURCE_MAP \
	_IOW(KBASE_IOCTL_TYPE, 29, struct kbase_ioctl_sticky_resource_map)


struct kbase_ioctl_sticky_resource_unmap {
    __u64 count;
    __u64 address;
};

#define KBASE_IOCTL_STICKY_RESOURCE_UNMAP \
	_IOW(KBASE_IOCTL_TYPE, 30, struct kbase_ioctl_sticky_resource_unmap)


struct kbase_ioctl_hwcnt_reader_setup {
    __u32 buffer_count;
    __u32 fe_bm;
    __u32 shader_bm;
    __u32 tiler_bm;
    __u32 mmu_l2_bm;
};

#define KBASE_IOCTL_HWCNT_READER_SETUP \
	_IOW(KBASE_IOCTL_TYPE, 8, struct kbase_ioctl_hwcnt_reader_setup)


union kbase_ioctl_get_cpu_gpu_timeinfo {
    struct {
        __u32 request_flags;
        __u32 paddings[7];
    } in;
    struct {
        __u64 sec;
        __u32 nsec;
        __u32 padding;
        __u64 timestamp;
        __u64 cycle_counter;
    } out;
};

#define KBASE_IOCTL_GET_CPU_GPU_TIMEINFO \
	_IOWR(KBASE_IOCTL_TYPE, 50, union kbase_ioctl_get_cpu_gpu_timeinfo)


#define KBASE_IOCTL_CS_EVENT_SIGNAL \
	_IO(KBASE_IOCTL_TYPE, 44)


struct kbase_ioctl_cs_queue_register {
    __u64 buffer_gpu_addr;
    __u32 buffer_size;
    __u8 priority;
    __u8 padding[3];
};

#define KBASE_IOCTL_CS_QUEUE_REGISTER \
	_IOW(KBASE_IOCTL_TYPE, 36, struct kbase_ioctl_cs_queue_register)


struct kbase_ioctl_cs_queue_register_ex {
    __u64 buffer_gpu_addr;
    __u32 buffer_size;
    __u8 priority;
    __u8 padding[3];
    __u64 ex_offset_var_addr;
    __u64 ex_buffer_base;
    __u32 ex_buffer_size;
    __u8 ex_event_size;
    __u8 ex_event_state;
    __u8 ex_padding[2];
};

#define KBASE_IOCTL_CS_QUEUE_REGISTER_EX \
	_IOW(KBASE_IOCTL_TYPE, 40, struct kbase_ioctl_cs_queue_register_ex)


struct kbase_ioctl_cs_queue_terminate {
    __u64 buffer_gpu_addr;
};

#define KBASE_IOCTL_CS_QUEUE_TERMINATE \
	_IOW(KBASE_IOCTL_TYPE, 41, struct kbase_ioctl_cs_queue_terminate)


union kbase_ioctl_cs_queue_bind {
    struct {
        __u64 buffer_gpu_addr;
        __u8 group_handle;
        __u8 csi_index;
        __u8 padding[6];
    } in;
    struct {
        __u64 mmap_handle;
    } out;
};


#define KBASE_IOCTL_CS_QUEUE_BIND \
	_IOWR(KBASE_IOCTL_TYPE, 39, union kbase_ioctl_cs_queue_bind)

struct kbase_ioctl_cs_queue_kick {
    __u64 buffer_gpu_addr;
};


#define KBASE_IOCTL_CS_QUEUE_KICK \
	_IOW(KBASE_IOCTL_TYPE, 37, struct kbase_ioctl_cs_queue_kick)


union kbase_ioctl_cs_queue_group_create_1_6 {
    struct {
        __u64 tiler_mask;
        __u64 fragment_mask;
        __u64 compute_mask;
        __u8 cs_min;
        __u8 priority;
        __u8 tiler_max;
        __u8 fragment_max;
        __u8 compute_max;
        __u8 padding[3];
    } in;
    struct {
        __u8 group_handle;
        __u8 padding[3];
        __u32 group_uid;
    } out;
};

#define KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_6                                  \
	_IOWR(KBASE_IOCTL_TYPE, 42, union kbase_ioctl_cs_queue_group_create_1_6)


union kbase_ioctl_cs_queue_group_create {
    struct {
        __u64 tiler_mask;
        __u64 fragment_mask;
        __u64 compute_mask;
        __u8 cs_min;
        __u8 priority;
        __u8 tiler_max;
        __u8 fragment_max;
        __u8 compute_max;
        __u8 padding[3];
        /**
         * @reserved: Reserved
         */
        __u64 reserved;
    } in;
    struct {
        __u8 group_handle;
        __u8 padding[3];
        __u32 group_uid;
    } out;
};

#define KBASE_IOCTL_CS_QUEUE_GROUP_CREATE                                      \
	_IOWR(KBASE_IOCTL_TYPE, 58, union kbase_ioctl_cs_queue_group_create)


struct kbase_ioctl_cs_queue_group_term {
    __u8 group_handle;
    __u8 padding[7];
};

#define KBASE_IOCTL_CS_QUEUE_GROUP_TERMINATE \
	_IOW(KBASE_IOCTL_TYPE, 43, struct kbase_ioctl_cs_queue_group_term)


struct kbase_ioctl_kcpu_queue_new {
    base_kcpu_queue_id id;
    __u8 padding[7];
};


#define KBASE_IOCTL_KCPU_QUEUE_CREATE \
	_IOR(KBASE_IOCTL_TYPE, 45, struct kbase_ioctl_kcpu_queue_new)


struct kbase_ioctl_kcpu_queue_delete {
    base_kcpu_queue_id id;
    __u8 padding[7];
};

#define KBASE_IOCTL_KCPU_QUEUE_DELETE \
	_IOW(KBASE_IOCTL_TYPE, 46, struct kbase_ioctl_kcpu_queue_delete)


struct kbase_ioctl_kcpu_queue_enqueue {
    __u64 addr;
    __u32 nr_commands;
    base_kcpu_queue_id id;
    __u8 padding[3];
};

#define KBASE_IOCTL_KCPU_QUEUE_ENQUEUE \
	_IOW(KBASE_IOCTL_TYPE, 47, struct kbase_ioctl_kcpu_queue_enqueue)


union kbase_ioctl_cs_tiler_heap_init {
    struct {
        __u32 chunk_size;
        __u32 initial_chunks;
        __u32 max_chunks;
        __u16 target_in_flight;
        __u8 group_id;
        __u8 padding;
    } in;
    struct {
        __u64 gpu_heap_va;
        __u64 first_chunk_va;
    } out;
};

#define KBASE_IOCTL_CS_TILER_HEAP_INIT \
	_IOWR(KBASE_IOCTL_TYPE, 48, union kbase_ioctl_cs_tiler_heap_init)

struct kbase_ioctl_cs_tiler_heap_term {
    __u64 gpu_heap_va;
};

#define KBASE_IOCTL_CS_TILER_HEAP_TERM \
	_IOW(KBASE_IOCTL_TYPE, 49, struct kbase_ioctl_cs_tiler_heap_term)


union kbase_ioctl_cs_get_glb_iface {
    struct {
        __u32 max_group_num;
        __u32 max_total_stream_num;
        __u64 groups_ptr;
        __u64 streams_ptr;
    } in;
    struct {
        __u32 glb_version;
        __u32 features;
        __u32 group_num;
        __u32 prfcnt_size;
        __u32 total_stream_num;
        __u32 instr_features;
    } out;
};

#define KBASE_IOCTL_CS_GET_GLB_IFACE \
	_IOWR(KBASE_IOCTL_TYPE, 51, union kbase_ioctl_cs_get_glb_iface)


struct kbase_ioctl_cs_cpu_queue_info {
    __u64 buffer;
    __u64 size;
};

#define KBASE_IOCTL_CS_CPU_QUEUE_DUMP \
	_IOW(KBASE_IOCTL_TYPE, 53, struct kbase_ioctl_cs_cpu_queue_info)


struct kbase_ioctl_context_priority_check {
    __u8 priority;
};

#define KBASE_IOCTL_CONTEXT_PRIORITY_CHECK \
	_IOWR(KBASE_IOCTL_TYPE, 54, struct kbase_ioctl_context_priority_check)


struct kbase_ioctl_set_limited_core_count {
    __u8 max_core_count;
};

#define KBASE_IOCTL_SET_LIMITED_CORE_COUNT \
	_IOW(KBASE_IOCTL_TYPE, 55, struct kbase_ioctl_set_limited_core_count)

#define KBASE_IOCTL_VERSION_CHECK_RESERVED \
	_IOWR(KBASE_IOCTL_TYPE, 0, struct kbase_ioctl_version_check)

