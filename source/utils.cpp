#include "../headers/utils.h"
#include "../headers/flatpak-proxy-client.h"

#define AUTH_LINE_SENTINEL "\r\n"
#define AUTH_BEGIN "BEGIN"
#define FIND_AUTH_END_CONTINUE -1
#define FIND_AUTH_END_ABORT -2

uint32_t read_uint32(Header *header, uint8_t *ptr) {
    return header->big_endian
           ? GUINT32_FROM_BE(*(guint32 *) ptr)
           : GUINT32_FROM_LE(*(guint32 *) ptr);
}

uint32_t align_by_8(uint32_t offset) {
    return (offset + 8 - 1) & ~(8 - 1);
}

uint32_t align_by_4(uint32_t offset) {
    return (offset + 4 - 1) & ~(4 - 1);
}

bool auth_line_is_begin(const std::string& line) {
    const std::string auth_begin = "BEGIN";
    if (!line.starts_with(auth_begin)) 
        return false;
    
    if (line.size() == auth_begin.size()) 
        return true;
        
    char ch = line[auth_begin.size()];
    return ch == ' ' || ch == '\t';
}

#define _DBUS_ISASCII(c) ((c) != '\0' && (((c) & ~0x7f) == 0))

bool auth_line_is_valid(const std::string& line) {
    if (line.empty())
        return false;
        
    for (char ch : line) {
        if (!_DBUS_ISASCII(ch))
            return false;
        if (ch < ' ') 
            return false;
    }
    
    if (line[0] < 'A' || line[0] > 'Z')
        return false;

    return true;
}

size_t find_auth_line_end(const std::vector<uint8_t> &buffer, size_t offset = 0) {
    const std::string sentinel = AUTH_LINE_SENTINEL;
    auto it = std::search(buffer.begin() + offset, buffer.end(), 
                         sentinel.begin(), sentinel.end());
    return (it != buffer.end()) ? std::distance(buffer.begin(), it) : std::string::npos;
}

ssize_t find_auth_end(FlatpakProxyClient *client, Buffer *buffer, size_t *out_lines_skipped) {
    size_t offset = 0;
    size_t original_size = client->auth_buffer.size();
    size_t lines_skipped = 0;
    
    client->auth_buffer.insert(client->auth_buffer.end(), 
                              buffer->data.begin(), 
                              buffer->data.begin() + buffer->pos);
    
    while (true) {
        size_t line_end = find_auth_line_end(client->auth_buffer, offset);
        if (line_end != std::string::npos) {
            std::string line(reinterpret_cast<char *>(client->auth_buffer.data() + offset),
                           line_end - offset);
            
            if (!auth_line_is_valid(line))
                return FIND_AUTH_END_ABORT;
            
            offset = line_end + strlen(AUTH_LINE_SENTINEL);
            
            if (auth_line_is_begin(line)) {
                *out_lines_skipped = lines_skipped;
                return static_cast<ssize_t>(offset - original_size);
            }
            
            ++lines_skipped;
        } else {
            *out_lines_skipped = lines_skipped;
            client->auth_buffer.erase(client->auth_buffer.begin(), 
                                    client->auth_buffer.begin() + offset);

            if (client->auth_buffer.size() >= 16 * 1024)
                return FIND_AUTH_END_ABORT;

            return FIND_AUTH_END_CONTINUE;
        }
    }
}

gboolean side_in_cb(GSocket *socket, GIOCondition, gpointer user_data) {
    ProxySide *side = static_cast<ProxySide *>(user_data);
    std::shared_ptr<FlatpakProxyClient> client = side->client;
    
    std::cerr << "SIDE_IN_CB: Enter for " << (side == &client->client_side ? "CLIENT" : "BUS") 
              << " side\n";
    
    if (!client) {
        std::cerr << "SIDE_IN_CB: No client, removing source\n";
        return G_SOURCE_REMOVE;
    }
    
    Buffer *buffer = nullptr;
    gboolean retval = G_SOURCE_CONTINUE;
    bool wake_client_reader = false;

    while (!side->closed) {
        if (!side->got_first_byte) {
            buffer = new Buffer(1);
            std::cerr << "SIDE_IN_CB: Created first byte buffer\n";
        } else if (client->auth_state != AUTH_COMPLETE) {
            buffer = new Buffer(256);
            std::cerr << "SIDE_IN_CB: Created auth buffer\n";
        } else {
            buffer = side->current_read_buffer;
            std::cerr << "SIDE_IN_CB: Using current_read_buffer=" << buffer 
                      << " (id=" << (buffer ? buffer->buffer_id : -1) << ")\n";
        }

        if (!buffer) {
            std::cerr << "SIDE_IN_CB ERROR: buffer is NULL!\n";
            side->side_closed();
            break;
        }

        if (!buffer->read(side, socket)) {
            if (buffer != side->current_read_buffer) {
                std::cerr << "SIDE_IN_CB: Deleting temporary buffer\n";
                buffer->unref();
            }
            break;
        }

        if (client->auth_state != AUTH_COMPLETE) {
            if (buffer->pos == 0) {
                buffer->unref();
                continue;
            }

            AuthState new_auth_state = client->auth_state;
            buffer->size = buffer->pos;
            
            if (!side->got_first_byte) {
                buffer->send_credentials = true;
                side->got_first_byte = true;
            } else if (side == &client->client_side && client->auth_state == AUTH_WAITING_FOR_BEGIN) {
                size_t lines_skipped = 0;
                ssize_t auth_end = find_auth_end(client.get(), buffer, &lines_skipped);
                
                client->auth_requests += lines_skipped;
                
                if (auth_end >= 0) {
                    if (client->auth_replies == client->auth_requests) {
                        new_auth_state = AUTH_COMPLETE;
                    } else {
                        new_auth_state = AUTH_WAITING_FOR_BACKLOG;
                    }

                    size_t extra_data = buffer->pos - static_cast<size_t>(auth_end);
                    buffer->size = buffer->pos = static_cast<size_t>(auth_end);
                    
                    if (extra_data > 0) {
                        side->extra_input_data.assign(buffer->data.begin() + auth_end,
                                                    buffer->data.begin() + buffer->pos + extra_data);
                    }
                } else if (auth_end == FIND_AUTH_END_ABORT) {
                    buffer->unref();
                    if (client->proxy->log_messages) {
                        std::cerr << "Invalid AUTH line, aborting\n";
                    }
                    side->side_closed();
                    break;
                }
            } else if (side == &client->bus_side) {
                size_t remaining = buffer->pos;
                uint8_t *line_start = buffer->data.data();
                
                while (remaining > 0) {
                    if (client->auth_replies == client->auth_requests) {
                        buffer->unref();
                        if (client->proxy->log_messages) {
                            std::cerr << "Unexpected auth reply line from bus, aborting\n";
                        }
                        side->side_closed();
                        break;
                    }

                    std::vector<uint8_t> temp_buffer(line_start, line_start + remaining);
                    size_t line_end = find_auth_line_end(temp_buffer);
                    
                    if (line_end == std::string::npos) {
                        line_end = remaining;
                    } else {
                        line_end += strlen(AUTH_LINE_SENTINEL);
                        client->auth_replies++;
                    }

                    remaining -= line_end;
                    line_start += line_end;

                    if (client->auth_state == AUTH_WAITING_FOR_BACKLOG &&
                        client->auth_replies == client->auth_requests) {
                        new_auth_state = AUTH_COMPLETE;
                        wake_client_reader = true;

                        buffer->pos = buffer->size = line_start - buffer->data.data();

                        if (remaining > 0) {
                            side->extra_input_data.assign(line_start, line_start + remaining);
                        }
                        break;
                    }
                }
            }

            side->got_buffer_from_side(buffer);
            client->auth_state = new_auth_state;
        } else if (buffer->pos == buffer->size) {
            if (buffer == side->header_buffer) {
                GError *error = nullptr;
                gssize required = g_dbus_message_bytes_needed(buffer->data.data(), buffer->size, &error);
                
                if (required < 0) {
                    std::cerr << "Invalid message header\n";
                    side->side_closed();
                    if (error) g_error_free(error);
                } else if (required == 0 || required > 1000000) {
                    std::cerr << "Invalid message size: " << required << "\n";
                    side->current_read_buffer = new Buffer(16, buffer);
                } else {
                    std::cerr << "SIDE_IN_CB: Creating message buffer of size " << required << "\n";
                    side->current_read_buffer = new Buffer(static_cast<size_t>(required), buffer);
                }
            } else {
                std::cerr << "SIDE_IN_CB: Message complete, processing\n";
                side->got_buffer_from_side(buffer);
                side->header_buffer->pos = 0;
                side->current_read_buffer = side->header_buffer;
                std::cerr << "SIDE_IN_CB: Reset to header_buffer\n";
            }
        }
    }

    if (side->closed) {
        std::cerr << "SIDE_IN_CB: Side closed, removing source\n";
        side->in_source = nullptr;
        retval = G_SOURCE_REMOVE;
    } else if (wake_client_reader) {
        GSocket *client_socket = g_socket_connection_get_socket(client->client_side.connection);
        side_in_cb(client_socket, G_IO_IN, &client->client_side);
    }

    std::cerr << "SIDE_IN_CB: Exit with " << (retval == G_SOURCE_CONTINUE ? "CONTINUE" : "REMOVE") << "\n";
    return retval;
}

bool send_outgoing_buffers(GSocket *socket, ProxySide *side) {
    bool all_done = false;

    while (!side->buffers.empty()) {
        Buffer *buffer = side->buffers.front();
        
        if (buffer->write(side, socket)) {
            if (buffer->sent == buffer->size) {
                side->buffers.pop_front();
                buffer->unref();
            }
        } else {
            break;
        }
    }

    if (side->buffers.empty()) {
        ProxySide *other_side = side->get_other_side();
        all_done = true;

        if (other_side->closed) {
            side->side_closed();
        }
    }

    return all_done;
}

gboolean side_out_cb(GSocket *socket, GIOCondition, gpointer user_data) {
    ProxySide *side = static_cast<ProxySide *>(user_data);

    bool all_done = send_outgoing_buffers(socket, side);
    if (all_done) {
        side->out_source = nullptr;
        return G_SOURCE_REMOVE;
    } else {
        return G_SOURCE_CONTINUE;
    }
}