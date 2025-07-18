#include "headers/flatpak-proxy-client.h"

void ProxySide::init_side(FlatpakProxyClient *client, bool is_bus_side) {
    got_first_byte = is_bus_side;
    this->client = client;
    header_buffer.size = 16;
    header_buffer.pos = 0;
    current_read_buffer = &header_buffer;
    expected_replies.clear();
}


void ProxySide::free_side() {
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

//todo side_in_cb
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