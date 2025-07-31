#pragma once

#include <cstdint>
#include <string>
#include <glib.h>
#include <gio/gio.h>

class Header;
class ProxySide;

uint32_t read_uint32(Header* header, uint8_t *ptr);
uint32_t align_by_8(uint32_t offset);
uint32_t align_by_4(uint32_t offset);

bool auth_line_is_begin(const std::string& line);
bool auth_line_is_valid(const std::string& line);

gboolean side_in_cb(GSocket *socket, GIOCondition condition, gpointer user_data);
gboolean side_out_cb(GSocket *socket, GIOCondition condition, gpointer user_data);

bool send_outgoing_buffers(GSocket *socket, ProxySide *side);