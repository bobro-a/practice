#include "headers/flatpak-proxy-client.h"
#include "headers/utils.h"

Buffer::Buffer(size_t size, Buffer *old) {
    this->size = size;
    refcount = 1;
    if (old) {
        pos = old->pos;
        sent = old->sent;
        control_messages = old->control_messages;
        old->control_messages.clear();
        assert(size >= old->size);
        std::copy(old->data.begin(), old->data.begin() + old->size, this->data.begin());
    }
}

Buffer::~Buffer() {
    for (auto *msg: control_messages) {
        g_object_unref(msg);
    }
    control_messages.clear();
}

void Buffer::ref() {
    assert(refcount++ > 0);
}

void Buffer::unref() {
    assert(refcount-- > 0);
    if (refcount == 0)
        delete this;
}

bool Buffer::read(ProxySide *side,
                  GSocket *socket) {
    FlatpakProxyClient *client = side->client;
    size_t received = 0;
    if (client->auth_state == AUTH_WAITING_FOR_BACKLOG && side == client->client_side)
        return false;
    if (!side->extra_input_data.empty() && client->auth_state == AUTH_COMPLETE) {
        assert(size >= pos);
        received = std::min(size - pos, side->extra_input_data.size());
        std::copy_n(side->extra_input_data.begin(), received, data.begin() + pos);
        if (received < side->extra_input_data.size())
            side->extra_input_data.erase(side->extra_input_data.begin(), side->extra_input_data.begin() + received);
        else {
            side->extra_input_data.clear();
        }
    } else if (side->extra_input_data.empty()) {
        GInputVector vec;
        vec.buffer = data.data() + pos;
        vec.size = data.size() - pos;

        GSocketControlMessage **messages = nullptr;
        int num_messages = 0;
        int flags = 0;
        GError *error;

        size_t res = g_socket_receive_message(
                socket,
                nullptr,
                &vec, 1,
                &messages,
                &num_messages,
                &flags,
                nullptr,
                &error
        );
        if (res < 0 && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
            g_error_free(error);
            return false;
        }

        if (res <= 0) {
            if (res != 0) {
                std::cerr << "Error reading from socket: " << error->message << "\n";
                g_error_free(error);
            }

            side->side_closed();
            return false;
        }
        received = res;
        for (int i = 0; i < num_messages; ++i)
            control_messages.push_back(messages[i]);
        g_free(messages);
    }
    pos += received;
    return true;
}

bool Buffer::write(ProxySide *side,
                   GSocket *socket) {
    GError *error;
    if (send_credentials && G_IS_UNIX_CONNECTION(side->connection)) {
        assert(size == 1);
        if (!g_unix_connection_send_credentials(G_UNIX_CONNECTION (side->connection),
                                                nullptr,
                                                &error)) {
            if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
                g_error_free(error);
                return false;
            }
            std::cerr << "Error sending credentials" << std::endl;
            side->side_closed();
            g_error_free(error);
            return false;
        }
        sent = 1;
        return true;
    }

    const uint8_t *data_to_send = data.data() + sent;

    std::vector<GSocketControlMessage *> messages(control_messages.begin(), control_messages.end());
    GOutputVector vec;
    vec.buffer = const_cast<void *>(reinterpret_cast<const void *>(data_to_send));
    vec.size = pos - sent;
    gssize res = g_socket_send_message(
            socket,
            nullptr,
            &vec,
            1,
            messages.empty() ? nullptr : messages.data(),
            static_cast<int>(messages.size()),
            G_SOCKET_MSG_NONE,
            nullptr,
            &error
    );
    if (res < 0 && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
        g_error_free(error);
        return false;
    }
    if (res <= 0) {
        if (res < 0) {
            std::cerr << "Error writing credentials to socket: " << error->message << "\n";
            g_error_free(error);
        }

        side->side_closed();
        return false;
    }
    control_messages.clear();
    sent += res;
    return true;
}