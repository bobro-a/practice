#pragma once

#include <cassert>
#include <filesystem>
#include <iostream>
#include <unordered_map>
#include <string>
#include <vector>
#include <list>
#include "flatpak-proxy.h"

//#include <glibmm.h>
//#include <giomm/init.h>
#include <giomm/socketservice.h>
#include <gio/gdbusaddress.h>
#include <gio/gsocket.h>
#include <gio/gsocketcontrolmessage.h>
#include<gio/gunixfdmessage.h>


class FlatpakProxyClient;
class ProxySide;
class Header;

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

typedef enum {
    HANDLE_PASS,
    HANDLE_DENY,
    HANDLE_HIDE,
    HANDLE_FILTER_NAME_LIST_REPLY,
    HANDLE_FILTER_HAS_OWNER_REPLY,
    HANDLE_FILTER_GET_OWNER_REPLY,
    HANDLE_VALIDATE_OWN,
    HANDLE_VALIDATE_SEE,
    HANDLE_VALIDATE_TALK,
    HANDLE_VALIDATE_MATCH,
} BusHandler;//todo realize all methods with it

#define MAX_CLIENT_SERIAL (G_MAXUINT32 - 65536)

class Filter {
public:
    Filter(std::string name, bool name_is_subtree, FlatpakPolicy policy);

    Filter(std::string name, bool name_is_subtree, FilterTypeMask types, std::string rule);

    std::string name;
    bool name_is_subtree;
    FlatpakPolicy policy;
    std::string path;
    FilterTypeMask types;
    bool path_is_subtree;
    std::string interface;
    std::string member;
    //todo: add ordinary methods to class, to make the fields private
};

class Buffer {
public:
    Buffer(size_t size, Buffer* old);
    ~Buffer();
    void ref();
    bool read(ProxySide *side,
              GSocket   *socket);

    void unref();
    size_t size;
    size_t pos;
    bool send_credentials;
    std::vector<uint8_t> data;
    std::list<GSocketControlMessage*> control_messages;

private:

    bool write(ProxySide *side,
               GSocket   *socket);

    size_t sent;
    int refcount;
};

class Header {
public:
    ~Header();
    void parse(Buffer *buffer); //parse_header
    void print_outgoing();
    bool is_introspection_call();
    bool is_dbus_method_call();
    bool is_for_bus();
    bool big_endian;
    std::string path;
    std::string interface;
    std::string member;
    std::string error_name;
    std::string destination;
    std::string sender;
    uint32_t unix_fds;
    uint32_t serial;
    uint8_t type;
    bool has_reply_serial;
    uint32_t reply_serial;
private:
    bool client_message_generates_reply();
//    void parse_header (Buffer *buffer, GError **error);
    void print_incoming();

    Buffer *buffer;
    uint8_t flags;
    uint32_t length;
    std::string signature;
};

class ProxySide {
public:
    ProxySide(FlatpakProxyClient *client, bool is_bus_side);//init_side
    ~ProxySide();//free_side
    void start_reading();
    void side_closed();
    void got_buffer_from_side(Buffer* buffer);

    GSocketConnection *connection;
    Buffer *current_read_buffer;
    Buffer header_buffer;
    FlatpakProxyClient *client;
    bool closed;
    bool got_first_byte;
    std::vector<uint8_t> extra_input_data;
    std::list<GSocketControlMessage*> control_messages;
    std::unordered_map<uint32_t, ExpectedReplyType> expected_replies;
private:
    void stop_reading();
    ProxySide* get_other_side();

    GSource *in_source;
    GSource *out_source;

    std::list<std::unique_ptr<Buffer>> buffers;//todo: change, causes an error
};

class FlatpakProxyClient {
public:
    FlatpakProxyClient(
            FlatpakProxy *proxy,
            Glib::RefPtr<Gio::SocketConnection> client_conn
    );
    ~FlatpakProxyClient();

    void got_buffer_from_client(Buffer* buffer);
    void got_buffer_from_bus(Buffer* buffer);
    FlatpakPolicy get_max_policy_and_matched(std::string source,
                                             std::vector<Filter *> *matched_filters);

    ProxySide bus_side;
    ProxySide client_side;
    FlatpakProxy *proxy;
    AuthState auth_state;
    size_t auth_requests;
    std::vector<uint8_t> auth_buffer;
    size_t auth_replies;
private:
    void add_unique_id_owned_name(std::string unique_id, std::string owned_name);

    void update_unique_id_policy(std::string unique_id,
                                 FlatpakPolicy policy);

    FlatpakPolicy get_max_policy(std::string source);
    bool validate_arg0_name (Buffer *buffer, FlatpakPolicy required_policy, FlatpakPolicy *has_policy);
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
    std::unordered_map<std::string, std::vector<Filter *>> filters;
    bool log_messages;
    bool filter;
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
    Glib::RefPtr<Gio::SocketService> parent;
    std::string socket_path;
    std::string dbus_address;
    bool sloppy_names;
};

