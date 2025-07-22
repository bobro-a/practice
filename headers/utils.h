#pragma once
#include "headers/flatpak-proxy-client.h"

uint32_t read_uint32(Header* header, uint8_t *ptr);

uint32_t align_by_8 (uint32_t offset);

uint32_t align_by_4(uint32_t offset);

gboolean side_in_cb(GSocket *socket, GIOCondition condition, gpointer user_data) ;