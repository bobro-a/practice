#include "headers/utils.h"



static uint32_t read_uint32(Header* header, uint8_t *ptr) {
    return header->big_endian
           ? GUINT32_FROM_BE (*(guint32 *) ptr)
           : GUINT32_FROM_LE (*(guint32 *) ptr);
}