#include "../headers/flatpak-proxy-client.h"
#include "../headers/utils.h"

ProxySide::ProxySide() : 
    client(nullptr),
    connection(nullptr),
    current_read_buffer(nullptr),
    header_buffer(nullptr),
    closed(false),
    got_first_byte(false),
    in_source(nullptr),
    out_source(nullptr) {
    
    header_buffer = new Buffer(16, nullptr);
    current_read_buffer = header_buffer;
    std::cerr << "ProxySide(): created with header_buffer=" << header_buffer << "\n";
}

ProxySide::ProxySide(std::shared_ptr<FlatpakProxyClient> client, bool is_bus_side) :
    client(std::move(client)),
    connection(nullptr),
    current_read_buffer(nullptr),
    header_buffer(nullptr),
    closed(false),
    got_first_byte(is_bus_side),
    in_source(nullptr),
    out_source(nullptr) {
    
    header_buffer = new Buffer(16, nullptr);
    current_read_buffer = header_buffer;
    std::cerr << "ProxySide(client): created with header_buffer=" << header_buffer 
              << " for " << (is_bus_side ? "BUS" : "CLIENT") << " side\n";
}

// Конструктор перемещения
ProxySide::ProxySide(ProxySide&& other) noexcept :
    client(std::move(other.client)),
    connection(other.connection),
    current_read_buffer(other.current_read_buffer),
    header_buffer(other.header_buffer),
    closed(other.closed),
    got_first_byte(other.got_first_byte),
    extra_input_data(std::move(other.extra_input_data)),
    control_messages(std::move(other.control_messages)),
    expected_replies(std::move(other.expected_replies)),
    buffers(std::move(other.buffers)),
    in_source(other.in_source),
    out_source(other.out_source) {
    
    // Обнуляем указатели в перемещенном объекте
    other.connection = nullptr;
    other.current_read_buffer = nullptr;
    other.header_buffer = nullptr;
    other.in_source = nullptr;
    other.out_source = nullptr;
    
    std::cerr << "ProxySide(move): moved header_buffer=" << header_buffer << "\n";
}

// Оператор перемещения
ProxySide& ProxySide::operator=(ProxySide&& other) noexcept {
    if (this != &other) {
        // Освобождаем текущие ресурсы
        cleanup();
        
        // Перемещаем данные
        client = std::move(other.client);
        connection = other.connection;
        current_read_buffer = other.current_read_buffer;
        header_buffer = other.header_buffer;
        closed = other.closed;
        got_first_byte = other.got_first_byte;
        extra_input_data = std::move(other.extra_input_data);
        control_messages = std::move(other.control_messages);
        expected_replies = std::move(other.expected_replies);
        buffers = std::move(other.buffers);
        in_source = other.in_source;
        out_source = other.out_source;
        
        // Обнуляем указатели в перемещенном объекте
        other.connection = nullptr;
        other.current_read_buffer = nullptr;
        other.header_buffer = nullptr;
        other.in_source = nullptr;
        other.out_source = nullptr;
        
        std::cerr << "ProxySide(operator=): moved header_buffer=" << header_buffer << "\n";
    }
    return *this;
}

void ProxySide::cleanup() {
    if (connection) {
        g_object_unref(connection);
        connection = nullptr;
    }

    if (header_buffer) {
        header_buffer->unref();
        header_buffer = nullptr;
    }

    extra_input_data.clear();

    for (auto buffer : buffers) {
        buffer->unref();
    }
    buffers.clear();

    for (auto msg : control_messages) {
        g_object_unref(msg);
    }
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

ProxySide::~ProxySide() {
    std::cerr << "~ProxySide(): destroying with header_buffer=" << header_buffer << "\n";
    cleanup();
}

void ProxySide::side_closed() {
    if (closed) return;

    ProxySide *other_side = get_other_side();
    
    GSocket *socket = g_socket_connection_get_socket(connection);
    g_socket_close(socket, nullptr);
    closed = true;

    GSocket *other_socket = g_socket_connection_get_socket(other_side->connection);

    if (!other_side->closed && other_side->buffers.empty()) {
        if (other_socket && G_IS_SOCKET(other_socket)) {
            g_socket_close(other_socket, nullptr);
        }
        other_side->closed = true;
    }

    if (other_side->closed) {
        client.reset();
    } else {
        GError *error = nullptr;
        if (!g_socket_shutdown(other_socket, TRUE, FALSE, &error)) {
            std::cerr << "Unable to shutdown read side: " << error->message << "\n";
            g_error_free(error);
        }
    }
}

void ProxySide::got_buffer_from_side(Buffer *buffer) {
    if (this == &client->client_side) {
        client->got_buffer_from_client(buffer);
    } else {
        client->got_buffer_from_bus(buffer);
    }
}

ProxySide *ProxySide::get_other_side() {
    FlatpakProxyClient *client_ptr = client.get();
    if (this == &client_ptr->client_side) {
        return &client_ptr->bus_side;
    }
    return &client_ptr->client_side;
}

void ProxySide::start_reading() {
    GSocket *socket = g_socket_connection_get_socket(connection);
    if (!G_IS_SOCKET(socket)) {
        std::cerr << "[start_reading] invalid socket\n";
        return;
    }

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