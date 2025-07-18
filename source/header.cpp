#include "headers/flatpak-proxy-client.h"

bool Header::client_message_generates_reply() {
    switch (type) {
        case G_DBUS_MESSAGE_TYPE_METHOD_CALL:
            return (flags && G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED) == 0;
        case G_DBUS_MESSAGE_TYPE_SIGNAL:
        case G_DBUS_MESSAGE_TYPE_METHOD_RETURN:
        case G_DBUS_MESSAGE_TYPE_ERROR:
        default:
            return false;
    }
}