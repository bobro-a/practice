#include "headers/flatpak-proxy-client.h"
#include "headers/utils.h"

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

void ProxySide::side_closed(){
    ProxySide* other_side=this->get_other_side();
    if (closed) return;

    GSocket *socket=g_socket_connection_get_socket (connection);
    g_socket_close (socket, nullptr);
    closed=true;

    GSocket *other_socket = g_socket_connection_get_socket (other_side->connection);
    if (!other_side->closed && other_side->buffers.empty()){
        g_socket_close (other_socket, nullptr);
        other_side->closed = true;
    }
    if (other_side->closed) delete client;
    else{
        GError *error = nullptr;
        if (!g_socket_shutdown(other_socket, TRUE, FALSE, &error)) {
            std::cerr << "Unable to shutdown read side: " << error->message << "\n";
            g_error_free(error);
        }
    }
}

void ProxySide::got_buffer_from_side(Buffer* buffer){
    if (this==&client->client_side)
        client->got_buffer_from_client(buffer);
    else client->got_buffer_from_bus(buffer);
}

ProxySide* ProxySide::get_other_side(){
    FlatpakProxyClient *client = this->client;
    if (this==&client->client_side)
        return &client->bus_side;
    return &client->client_side;
}



void ProxySide::start_reading() {
    std::cout << "[watch] watch started\n";
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