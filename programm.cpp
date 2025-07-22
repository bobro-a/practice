#include <gio/gio.h>
#include <iostream>
#include <vector>
#include <cstring>
#include <unistd.h>
#include <cstdint>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " /path/to/dbus/socket\n";
        return 1;
    }

    const char* socket_path = argv[1];

    GError* error = nullptr;

    // Адрес и сокет
    GSocketAddress* address = g_unix_socket_address_new(socket_path);
    GSocket* socket = g_socket_new(G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, &error);
    if (!socket) {
        std::cerr << "Failed to create socket: " << error->message << "\n";
        g_error_free(error);
        return 1;
    }

    if (!g_socket_connect(socket, address, nullptr, &error)) {
        std::cerr << "Failed to connect: " << error->message << "\n";
        g_error_free(error);
        g_object_unref(socket);
        g_object_unref(address);
        return 1;
    }

    // Создаем D-Bus сообщение Hello
    GDBusMessage* msg = g_dbus_message_new_method_call(
            "org.freedesktop.DBus",             // destination
            "/org/freedesktop/DBus",            // path
            "org.freedesktop.DBus",             // interface
            "Hello"                              // method
    );

    g_dbus_message_set_serial(msg, 1); // обязательно
    g_dbus_message_set_body(msg, g_variant_new("()"));

    // сериализация
    gsize blob_size = 0;
    guchar* blob = g_dbus_message_to_blob(msg, &blob_size, G_DBUS_CAPABILITY_FLAGS_NONE, &error);
    if (!blob) {
        std::cerr << "Failed to serialize D-Bus message: " << error->message << "\n";
        g_error_free(error);
        g_object_unref(msg);
        g_object_unref(socket);
        g_object_unref(address);
        return 1;
    }

    // отправка
    gssize sent = g_socket_send(socket, reinterpret_cast<const gchar*>(blob), blob_size, nullptr, &error);
    if (sent < 0) {
        std::cerr << "Failed to send message: " << error->message << "\n";
        g_error_free(error);
    } else {
        std::cout << "Sent " << sent << " bytes.\n";
    }

    // читаем ответ (блокирующе)
    char recv_buf[1024];
    gssize n = g_socket_receive(socket, recv_buf, sizeof(recv_buf), nullptr, &error);
    if (n < 0) {
        std::cerr << "Receive failed: " << error->message << "\n";
        g_error_free(error);
    } else {
        std::cout << "Received " << n << " bytes\n";
    }

    // очистка
    g_free(blob);
    g_object_unref(msg);
    g_object_unref(socket);
    g_object_unref(address);
    return 0;
}
