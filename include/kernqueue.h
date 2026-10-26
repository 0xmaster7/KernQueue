#ifndef KERNQUEUE_UAPI_H
#define KERNQUEUE_UAPI_H
#include <linux/types.h>
#define KQ_PAYLOAD 256
#define KQ_CAPACITY 64
/* Native-endian, fixed-size local ABI. Reserved must be zero. */
struct kq_request {
    __aligned_u64 id;
    __u32 length;
    __u32 reserved;
    __u8 data[KQ_PAYLOAD];
};
struct kq_result {
    __aligned_u64 id;
    __aligned_u64 checksum;
    __u32 length;
    __u32 reserved;
};
#endif
