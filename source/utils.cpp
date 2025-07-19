#include "headers/utils.h"



static uint32_t read_uint32(Header* header, uint8_t *ptr) {
    return header->big_endian
           ? GUINT32_FROM_BE (*(guint32 *) ptr)
           : GUINT32_FROM_LE (*(guint32 *) ptr);
}

static inline uint32_t align_by_8 (uint32_t offset)
{
    return (offset + 8 - 1) & ~(8 - 1);
}

static inline uint32_t
align_by_4(uint32_t offset) {
    return (offset + 4 - 1) & ~(4 - 1);
}