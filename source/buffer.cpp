#include "headers/flatpak-proxy-client.h"

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

        gssize res = g_socket_receive_message(
                socket,
                nullptr,
                &vec, 1,
                &messages,
                &num_messages,
                &flags,
                nullptr,
                nullptr
        );
        if (res < 0) return false;
        if (res == 0) {
            side->side_closed();
            return false;
        }
        received = res;
        for(int i=0;i<num_messages;++i)
            control_messages.push_back(messages[i]);
        g_free (messages);
    }
    pos+=received;
    return true;
}

bool write(ProxySide *side,
           GSocket *socket){
    }


void set_send_credentials(bool value){

}

bool should_send_credentials();

size_t get_pos();

void set_pos(size_t p);

size_t get_size();

void set_size(size_t s);

size_t get_sent();

void append_control_message(GSocketControlMessage *msg);

const uint8_t *get_data();

uint8_t *get_data_mutable();