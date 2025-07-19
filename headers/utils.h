#pragma once
#include "headers/flatpak-proxy-client.h"

static uint32_t read_uint32(Header* header, uint8_t *ptr);

static inline uint32_t align_by_8 (uint32_t offset);

static inline uint32_t align_by_4(uint32_t offset);