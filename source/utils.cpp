#include "headers/utils.h"
#include "headers/flatpak-proxy-client.h"

#define AUTH_LINE_SENTINEL "\r\n"
#define FIND_AUTH_END_CONTINUE -1
#define FIND_AUTH_END_ABORT -2


uint32_t read_uint32(Header *header, uint8_t *ptr) {
    return header->big_endian
           ? GUINT32_FROM_BE (*(guint32 *) ptr)
           : GUINT32_FROM_LE (*(guint32 *) ptr);
}

uint32_t align_by_8(uint32_t offset) {
    return (offset + 8 - 1) & ~(8 - 1);
}

uint32_t
align_by_4(uint32_t offset) {
    return (offset + 4 - 1) & ~(4 - 1);
}

bool auth_line_is_begin(std::string line) {
    std::cerr<<"auth_line_is_begin start\n";
    std::string auth_beg = "BEGIN";
    if (!line.starts_with(auth_beg)) return false;
    if (line.size() == auth_beg.size()) return true;
    char ch = line[auth_beg.size()];

    std::cerr<<"auth_line_is_begin end\n";
    return ch == ' ' || ch == '\t';
};

bool auth_line_is_valid(std::string line) {
    for (auto ch: line) {
        if (ch > 127 || ch < ' ')
            return false;
    }
    if (line[0] < 'A' || line[0] > 'Z')
        return false;

    return true;
}

size_t find_auth_line_end(const std::vector<uint8_t> &buffer, size_t offset = 0) {
    std::string_view view(reinterpret_cast<const char *>(buffer.data() + offset), buffer.size() - offset);
    size_t pos = view.find("\r\n");
    return (pos != std::string_view::npos) ? offset + pos : -1;
}

size_t find_auth_end(FlatpakProxyClient *client, Buffer *buffer, size_t *out_lines_skipped) {
    size_t offset = 0;
    size_t original_size = client->auth_buffer.size();
    size_t lines_skipped = 0;
    client->auth_buffer.insert(client->auth_buffer.end(), buffer->data.begin(), buffer->data.begin() + buffer->pos);
    while (true) {
        size_t line_end = find_auth_line_end(client->auth_buffer, offset);
        if (line_end != static_cast<size_t>(-1)) {
            std::string line(
                    reinterpret_cast<char *>(client->auth_buffer.data() + offset),
                    line_end - offset
            );

            if (!auth_line_is_valid(line))
                return FIND_AUTH_END_ABORT;
            offset = line_end + strlen(AUTH_LINE_SENTINEL);
            if (auth_line_is_begin(line)) {
                *out_lines_skipped = lines_skipped;
                return offset - original_size;
            }
            ++lines_skipped;
        } else {
            *out_lines_skipped = lines_skipped;
            client->auth_buffer.erase(client->auth_buffer.begin(), client->auth_buffer.begin() + offset);

            if (client->auth_buffer.size() >= 16 * 1024)
                return FIND_AUTH_END_ABORT;

            return FIND_AUTH_END_CONTINUE;
        }
    }
}

gboolean side_in_cb(GSocket *socket, GIOCondition condition, gpointer user_data) {
    std::cerr << "side_in_cb\n";
    ProxySide *side = static_cast<ProxySide *>(user_data);
    FlatpakProxyClient *client = side->client;
    Buffer *buffer;
    bool wake_client_reader = false;
    while (!side->closed) {
        std::cerr<<"[side_in_cb] side->got_first_byte = "<<side->got_first_byte<<"\n";
        buffer = (!side->got_first_byte) ? new Buffer(1, nullptr) :
                 client->auth_state != AUTH_COMPLETE ? new Buffer(256, nullptr) :
                 side->current_read_buffer;
        if (!buffer->read(side, socket)) {
            if (buffer != side->current_read_buffer)
                buffer->unref();
            std::cerr<<"break\n";
            break;
        }
        std::cerr << "read buffer\n";
        if (client->auth_state != AUTH_COMPLETE) {
            std::cerr << "client->auth_state != AUTH_COMPLETE\n";
            if (buffer->pos == 0) {
                buffer->unref();
                continue;
            }
            AuthState new_auth_state = client->auth_state;
            buffer->size = buffer->pos;
            if (!side->got_first_byte)
                buffer->send_credentials = side->got_first_byte = true;
            else if (side == &client->client_side && client->auth_state == AUTH_WAITING_FOR_BEGIN) {
                size_t lines_skipped = 0;
                long auth_end = find_auth_end(client, buffer, &lines_skipped);
                client->auth_requests += lines_skipped;
                if (auth_end >= 0) {
                    new_auth_state = AUTH_COMPLETE;
                    std::cerr << "[auth] Forcing AUTH_COMPLETE\n";
                    new_auth_state =
                            client->auth_requests == client->auth_replies ? AUTH_COMPLETE : AUTH_WAITING_FOR_BACKLOG;
                    size_t extra_data = buffer->pos - auth_end;
                    buffer->size = buffer->pos = auth_end;
                    if (extra_data > 0)
                        side->extra_input_data.assign(buffer->data.begin() + auth_end,
                                                      buffer->data.begin() + auth_end + extra_data);
                } else if (auth_end == FIND_AUTH_END_ABORT) {
                    std::cerr << "[proxy] AUTH failed: invalid line\n";
                    buffer->unref();
                    if (client->proxy->log_messages)
                        std::cout << "Invalid AUTH line, aborting\n";
//       side->side_closed(); todo  uncomment
                    client->auth_state = AUTH_COMPLETE;//todo delete
                    break;
                }
            } else if (side == &client->bus_side) {
                size_t remaining = buffer->pos;
                uint8_t *line_start = buffer->data.data();
                while (remaining > 0) {
                    if (client->auth_replies == client->auth_requests) {
                        buffer->unref();
                        if (client->proxy->log_messages)
                            std::cout << "Unexpected auth reply line from bus, aborting\n";
                        side->side_closed();
                        break;
                    }
                    size_t rel_offset = find_auth_line_end(
                            std::vector<uint8_t>(line_start, line_start + remaining));
                    uint8_t *line_end;
                    if (rel_offset == static_cast<size_t>(-1)) {
                        line_end = line_start + remaining;
                    } else {
                        line_end = line_start + rel_offset;
                        ++client->auth_replies;
                        line_end += strlen(AUTH_LINE_SENTINEL);
                    }
                    remaining -= line_end - line_start;
                    line_start = line_end;

                    if (client->auth_state == AUTH_WAITING_FOR_BACKLOG &&
                        client->auth_replies == client->auth_requests) {
                        new_auth_state = AUTH_COMPLETE;
                        wake_client_reader = true;

                        buffer->pos = buffer->size = line_start - buffer->data.data();
                        if (remaining > 0)
                            client->bus_side.extra_input_data.assign(line_start, line_start + remaining);

                        break;
                    }
                }
            }
            side->got_buffer_from_side(buffer);
            client->auth_state = new_auth_state;
            std::cerr << "[side_in_cb] auth_state -> " << new_auth_state << "\n";

        }//todo
        if (client->auth_state == AUTH_COMPLETE) {
            break;
        }
    }
    std::cerr<<"side_in_cb end\n";
    return true;
}