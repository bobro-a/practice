#include <glib.h>
#include <gio/gio.h>

#include <iostream>

int main(int argc, char* argv[]) {
    GError *error = NULL;
    GDBusConnection *conn = NULL;
    GVariant *reply = NULL;

    conn = g_dbus_connection_new_for_address_sync(
            argv[1],
            G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
            NULL,
            NULL,
            &error
    );

    if (error) {
        std::cout << "[1] ERROR: " << error->message << std::endl;
        g_error_free(error);
        return 1;
    }

    reply = g_dbus_connection_call_sync(
            conn,
            "org.freedesktop.DBus",
            "/org/freedesktop/DBus",
            "org.freedesktop.DBus",
            "Hello",
            NULL,
            G_VARIANT_TYPE("(s)"),
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            NULL,
            &error
    );

    if (error) {
        std::cout << "[2] ERROR: " << error->message << std::endl;
        g_error_free(error);
        g_object_unref(conn);
        return 1;
    }

    if (reply && g_variant_is_of_type(reply, G_VARIANT_TYPE("(s)"))) {
        const char *name;
        g_variant_get(reply, "(s)", &name);
        std::cout << "[3] SUCCESS: " << name << std::endl;
    }

    g_object_unref(conn);
    g_object_unref(reply);

    return 0;
}