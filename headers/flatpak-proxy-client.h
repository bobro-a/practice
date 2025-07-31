#pragma once

#include <cassert>
#include <filesystem>
#include <iostream>
#include <unordered_map>
#include <string>
#include <vector>
#include <list>
#include <memory>

#include <glibmm.h>
#include <giomm/socketservice.h>
#include <gio/gdbusaddress.h>
#include <gio/gsocket.h>
#include <gio/gsocketcontrolmessage.h>
#include <gio/gunixfdmessage.h>
#include <gio/gunixfdlist.h>
#include <gio/gunixconnection.h>

class FlatpakProxy;
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
} BusHandler;

typedef enum {
    FLATPAK_POLICY_NONE,
    FLATPAK_POLICY_SEE,
    FLATPAK_POLICY_TALK,
    FLATPAK_POLICY_OWN
} FlatpakPolicy;

#define MAX_CLIENT_SERIAL (G_MAXUINT32 - 65536)

class Filter {
public:
    Filter(const std::string& name, bool name_is_subtree, FlatpakPolicy policy);
    Filter(const std::string& name, bool name_is_subtree, FilterTypeMask types, const std::string& rule);
    ~Filter() = default;

    std::string name;
    bool name_is_subtree;
    FlatpakPolicy policy;
    std::string path;
    FilterTypeMask types;
    bool path_is_subtree;
    std::string interface;
    std::string member;
};

class Buffer {
public:
    Buffer(size_t size, Buffer *old = nullptr);
    ~Buffer();

    void ref();
    void unref();
    bool read(ProxySide *side, GSocket *socket);
    bool write(ProxySide *side, GSocket *socket);

    size_t size;
    size_t pos;
    size_t sent;
    bool send_credentials;
    std::vector<uint8_t> data;
    std::list<GSocketControlMessage *> control_messages;
    int buffer_id;

private:
    int refcount;
};

class Header {
public:
    Header() = default;
    ~Header();

    void parse(Buffer *buffer);
    void print_outgoing();
    void print_incoming();
    
    bool is_introspection_call();
    bool is_dbus_method_call();
    bool is_for_bus();
    bool client_message_generates_reply();

    Buffer *buffer = nullptr;
    bool big_endian = false;
    uint8_t type = 0;
    uint8_t flags = 0;
    uint32_t length = 0;
    uint32_t serial = 0;
    std::string path;
    std::string interface;
    std::string member;
    std::string error_name;
    std::string destination;
    std::string sender;
    std::string signature;
    bool has_reply_serial = false;
    uint32_t reply_serial = 0;
    uint32_t unix_fds = 0;
};

class ProxySide {
public:
    ProxySide();
    ProxySide(std::shared_ptr<FlatpakProxyClient> client, bool is_bus_side);
    ~ProxySide();
    
    // Запрещаем копирование
    ProxySide(const ProxySide&) = delete;
    ProxySide& operator=(const ProxySide&) = delete;
    
    // Разрешаем перемещение
    ProxySide(ProxySide&& other) noexcept;
    ProxySide& operator=(ProxySide&& other) noexcept;
    
    void start_reading();
    void stop_reading();
    void side_closed();
    ProxySide *get_other_side();
    void got_buffer_from_side(Buffer *buffer);

    std::shared_ptr<FlatpakProxyClient> client;
    GSocketConnection *connection = nullptr;
    Buffer *current_read_buffer = nullptr;
    Buffer *header_buffer = nullptr;
    bool closed = false;
    bool got_first_byte = false;
    std::vector<uint8_t> extra_input_data;
    std::list<GSocketControlMessage *> control_messages;
    std::unordered_map<uint32_t, ExpectedReplyType> expected_replies;
    std::list<Buffer *> buffers;
    GSource *in_source = nullptr;
    GSource *out_source = nullptr;

private:
    void cleanup();
};

class FlatpakProxyClient {
public:
    FlatpakProxyClient(FlatpakProxy* proxy, GSocketConnection *client_conn);
    ~FlatpakProxyClient();

    void init_side(std::shared_ptr<FlatpakProxyClient> self, GSocketConnection *client_conn);
    void got_buffer_from_client(Buffer *buffer);
    void got_buffer_from_bus(Buffer *buffer);
    
    FlatpakPolicy get_max_policy(const std::string& source);
    FlatpakPolicy get_max_policy_and_matched(const std::string& source, std::vector<Filter *> *matched_filters);
    Buffer *get_error_for_roundtrip(Header *header, const char *error_name);
    Buffer *get_bool_reply_for_roundtrip(Header *header, bool val);
    void add_unique_id_owned_name(const std::string& unique_id, const std::string& owned_name);

    ProxySide client_side;
    ProxySide bus_side;
    FlatpakProxy* proxy;
    AuthState auth_state = AUTH_WAITING_FOR_BEGIN;
    size_t auth_requests = 0;
    size_t auth_replies = 0;
    uint32_t hello_serial = 0;
    uint32_t last_fake_serial = MAX_CLIENT_SERIAL;
    std::vector<uint8_t> auth_buffer;
    std::unordered_map<uint32_t, GDBusMessage *> rewrite_reply;
    std::unordered_map<uint32_t, std::string> get_owner_reply;

private:
    void update_unique_id_policy(const std::string& unique_id, FlatpakPolicy policy);
    bool validate_arg0_name(Buffer *buffer, FlatpakPolicy required_policy, FlatpakPolicy *has_policy);
    
    std::unordered_map<std::string, FlatpakPolicy> unique_id_policy;
    std::unordered_map<std::string, std::vector<std::string>> unique_id_owned_names;
};

class FlatpakProxy {
public:
    FlatpakProxy(const std::string& dbus_address, const std::string& socket_path);
    ~FlatpakProxy();

    void add_policy(const std::string& name, bool name_is_subtree, FlatpakPolicy policy);
    void add_call_rule(const std::string& name, bool name_is_subtree, const std::string& rule);
    void add_broadcast_rule(const std::string& name, bool name_is_subtree, const std::string& rule);
    bool start();
    void stop();
    void set_filter(bool filter);
    void set_sloppy_names(bool sloppy_names);
    void set_log_messages(bool log);
    bool incoming_connection(GSocketService *service, GSocketConnection *conn);

    std::list<std::shared_ptr<FlatpakProxyClient>> clients;
    std::unordered_map<std::string, std::vector<Filter *>> filters;
    bool log_messages = false;
    bool filter = false;
    bool sloppy_names = false;
    std::string dbus_address;
    std::string auth_guid;
    GSocketService* service = nullptr;

private:
    void add_filter(Filter *filter);
    std::string socket_path;
};