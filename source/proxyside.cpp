#include "headers/flatpak-proxy-client.h"

#define AUTH_LINE_SENTINEL "\r\n"
#define FIND_AUTH_END_CONTINUE -1
#define FIND_AUTH_END_ABORT -2

ProxySide::ProxySide(FlatpakProxyClient *client, bool is_bus_side) :
        got_first_byte(is_bus_side),
        client(client),
        header_buffer(16, nullptr),
        connection(nullptr),
        closed(false),
        in_source(nullptr),
        out_source(nullptr) {
    current_read_buffer = &header_buffer;
}


ProxySide::~ProxySide() {
    if (connection) {
        g_object_unref(connection);
        connection = nullptr;
    }

    extra_input_data.clear();

    buffers.clear();
    control_messages.clear();

    if (in_source) {
        g_source_destroy(in_source);
        in_source = nullptr;
    }

    if (out_source) {
        g_source_destroy(out_source);
        out_source = nullptr;
    }

    expected_replies.clear();
}

bool auth_line_is_begin;

bool auth_line_is_valid (uint8_t *line, uint8_t *line_end)
{
    for (const uint8_t* p = line; p < line_end; ++p) {
        if (*p > 127 || *p < ' ')
            return false;
    }
    if (line[0] < 'A' || line[0] > 'Z')
        return false;

    return true;
}

uint8_t* find_auth_line_end(uint8_t* data, size_t size) {
    std::string_view buffer(reinterpret_cast<char*>(data), size);
    size_t pos = buffer.find(AUTH_LINE_SENTINEL);
    return (pos != std::string_view::npos) ? data + pos : nullptr;
}

size_t find_auth_end(FlatpakProxyClient *client, Buffer *buffer, size_t *out_lines_skipped){
    size_t offset=0;
    client->auth_buffer.insert(client->auth_buffer.end(), buffer->data.begin(), buffer->data.begin() + buffer->pos);
    while (true){
        uint8_t *line_start=client->auth_buffer.data()+offset;
        size_t remaining_data=client->auth_buffer.size()-offset;
        uint8_t *line_end=find_auth_line_end (line_start, remaining_data);
        if (line_end){
            offset=(line_end + strlen (AUTH_LINE_SENTINEL) - client->auth_buffer.data());

            if (!auth_line_is_valid(line_start,line_end))
                return FIND_AUTH_END_ABORT;

        }
    }
}

bool side_in_cb(GSocket *socket, GIOCondition condition, void* user_data){
    ProxySide *side = static_cast<ProxySide *>(user_data);
    FlatpakProxyClient *client = side->client;
    Buffer* buffer;

    while (!side->closed){
        buffer=(!side->got_first_byte)?new Buffer(1, nullptr):
                client->auth_state != AUTH_COMPLETE?new Buffer(256, nullptr):
                side->current_read_buffer;
        if (!buffer->read(side,socket)){
            if (buffer!=side->current_read_buffer)
                buffer->unref();
            break;
        }
        if (client->auth_state!=AUTH_COMPLETE){
            if (buffer->pos==0)
            {
                buffer->unref();
                continue;
            }
            AuthState new_auth_state = client->auth_state;
            buffer->size=buffer->pos;
            if (!side->got_first_byte)
                buffer->send_credentials=side->got_first_byte=true;
            else if (side==client->client_side && client->auth_state==AUTH_WAITING_FOR_BEGIN){
                size_t lines_skipped=0;
                size_t auth_end=find_auth_end(client,buffer,&lines_skipped);
                client->auth_requests+=lines_skipped;
                //todo: need finish
            }
        }
    }
}

void ProxySide::start_reading() {
    GSocket *socket = g_socket_connection_get_socket(connection);
    in_source = g_socket_create_source(socket, G_IO_IN, nullptr);
    g_source_set_callback(in_source, G_SOURCE_FUNC(side_in_cb), this, nullptr);
    g_source_attach(in_source, nullptr);
    g_source_unref(in_source);
}

void ProxySide::stop_reading() {
    if (in_source) {
        g_source_destroy(in_source);
        in_source = nullptr;
    }
}