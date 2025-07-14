#pragma once

#include <unordered_map>
#include <string>
#include <vector>
#include <list>
#include <glibmm.h>
#include <giomm/init.h>
#include <giomm/socketservice.h>
#include <gio/gdbusaddress.h>
#include "flatpak-proxy.h"

class FlatpakProxyClient;

typedef enum {
    FILTER_TYPE_CALL = 1 << 0,
    FILTER_TYPE_BROADCAST = 1 << 1,
    FILTER_TYPE_ALL = FILTER_TYPE_CALL | FILTER_TYPE_BROADCAST,
} FilterTypeMask;

typedef enum {
    EXPECTED_REPLY_NONE,
    EXPECTED_REPLY_NORMAL,
    EXPECTED_REPLY_HELLO,
    EXPECTED_REPLY_FILTER,
    EXPECTED_REPLY_FAKE_GET_NAME_OWNER,
    EXPECTED_REPLY_FAKE_LIST_NAMES,
    EXPECTED_REPLY_LIST_NAMES,
    EXPECTED_REPLY_REWRITE,
} ExpectedReplyType;

typedef enum {
    AUTH_WAITING_FOR_BEGIN,
    AUTH_WAITING_FOR_BACKLOG,
    AUTH_COMPLETE,
} AuthState;

class Filter {
public:
    Filter(std::string name, bool name_is_subtree, FlatpakPolicy policy);

    Filter(std::string name, bool name_is_subtree, FilterTypeMask types, std::string rule);

    std::string name;
    bool name_is_subtree;
    FlatpakPolicy policy;
private:
    FilterTypeMask types;
    std::string path;
    bool path_is_subtree;
    std::string interface;
    std::string member;
};

class Buffer {
public:
    size_t size;
    size_t pos;

private:
    size_t sent;
    int refcount;
    bool send_credentials;
    GList *control_messages;

    uint8_t data[16];
};

class Header {
    Buffer *buffer;
    bool big_endian;
    uint8_t type;
    uint8_t flags;
    uint32_t length;
    uint32_t serial;
    std::string path;
    std::string interface;
    std::string member;
    std::string error_name;
    std::string destination;
    std::string sender;
    std::string signature;
    bool has_reply_serial;
    uint32_t reply_serial;
    uint32_t unix_fds;
};

class ProxySide {
public:
    void init_side(FlatpakProxyClient *client, bool is_bus_side);
    GSocketConnection *connection;

    void free_side();

    void start_reading();
private:

    void stop_reading();

    void side_closed();

    void get_other_side();

    bool got_first_byte;
    bool closed;

    FlatpakProxyClient *client;
    GSource *in_source;
    GSource *out_source;

    std::vector<uint8_t> extra_input_data;
    Buffer *current_read_buffer;
    Buffer header_buffer;

    std::list<std::unique_ptr<Buffer>> buffers;
    std::list<std::shared_ptr<void>> control_messages;

    std::unordered_map<uint32_t, ExpectedReplyType> expected_replies;
};

class FlatpakProxyClient {
public:
    FlatpakProxyClient(
            FlatpakProxy *proxy,
            Glib::RefPtr<Gio::SocketConnection> client_conn
    );

    ~FlatpakProxyClient();

    std::unique_ptr<ProxySide> bus_side;
    std::unique_ptr<ProxySide> client_side;
private:
    void add_unique_id_owned_name();
    void update_unique_id_policy();
    void get_max_policy();
    void get_max_policy_and_matched();
    void init_side();
    void init();

    AuthState auth_state;
    FlatpakProxy *proxy;

    size_t auth_requests;
    size_t auth_replies;
    std::vector<uint8_t> auth_buffer;

    uint32_t hello_serial;
    uint32_t last_fake_serial;
    std::unordered_map<uint32_t, GDBusMessage *> rewrite_reply;//todo replace GDBusMessage
    std::unordered_map<uint32_t, std::string> get_owner_reply;

    std::unordered_map<std::string, int> unique_id_policy;
    std::unordered_map<std::string, std::vector<std::string>> unique_id_owned_names;
};

class FlatpakProxy {
public:
    FlatpakProxy(std::string dbus_address, std::string socket_path);

    ~FlatpakProxy();

    std::list<FlatpakProxyClient *> clients;

private:
    void set_filter(bool filter);

    void set_sloppy_names(bool sloopy_names);

    void set_log_messages(bool log);

    void add_filter(Filter *filter);

    void add_policy(std::string name, bool name_is_subtree, FlatpakPolicy policy);

    void add_call_rule(std::string name, bool name_is_subtree, std::string rule);

    void add_broadcast_rule(std::string name, bool name_is_subtree, std::string rule);

    bool start();

    void stop();

    bool incoming_connection(
            const Glib::RefPtr<Gio::SocketConnection> &connection,
            const Glib::RefPtr<Glib::Object> &source_object
    );

    bool log_messages;
    Glib::RefPtr<Gio::SocketService> parent;
    std::string socket_path;
    std::string dbus_address;
    bool filter;
    bool sloppy_names;

    std::unordered_map<std::string, std::vector<Filter *>> filters;

};

