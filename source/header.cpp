#include "headers/flatpak-proxy-client.h"
#include "headers/utils.h."

Header::Header(Buffer *buffer) {
    buffer->ref();
    try {
        if (buffer->size < 16) {
            throw std::runtime_error("Buffer too small: " + std::to_string(buffer->size));
        }
        if (buffer->data[3] != 1){
            throw std::runtime_error("Wrong protocol version: " + std::to_string(buffer->data[3]));
        }
        buffer->data[0] == 'B'? big_endian=true:buffer->data[0] == 'l'?big_endian=false:
                                                 throw std::runtime_error("Invalid endianess marker: " + std::to_string(buffer->data[0]));
        type=buffer->data[1];
        flags = buffer->data[2];

        length = read_uint32(this,&buffer->data[4]);
        serial = read_uint32(this,&buffer->data[8]);

    } catch (std::exception &ex) {
        std::cerr << ex.what() << "\n";
    }


};

Header::~Header() {
    if (buffer) {
        delete buffer;
    }
};

bool Header::client_message_generates_reply() {
    switch (type) {
        case G_DBUS_MESSAGE_TYPE_METHOD_CALL:
            return (flags & G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED) == 0;
        case G_DBUS_MESSAGE_TYPE_SIGNAL:
        case G_DBUS_MESSAGE_TYPE_METHOD_RETURN:
        case G_DBUS_MESSAGE_TYPE_ERROR:
        default:
            return false;
    }
}

std::string debug_str(std::string s);

void print_outgoing();

void print_incoming();