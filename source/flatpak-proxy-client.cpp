#include "../headers/flatpak-proxy-client.h"
#include "../headers/utils.h"
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>

void client_connected_to_dbus(GObject *source_object, GAsyncResult *res, void *user_data);
void queue_expected_reply(ProxySide *side, uint32_t serial, ExpectedReplyType type);
void queue_outgoing_buffer(ProxySide *side, Buffer *buffer);
ExpectedReplyType steal_expected_reply(ProxySide *side, uint32_t serial);
bool filter_matches(Filter *filter, FilterTypeMask type, const std::string& path, 
                   const std::string& interface, const std::string& member);
bool any_filter_matches(const std::vector<Filter *>& filters, FilterTypeMask type,
                       const std::string& path, const std::string& interface, const std::string& member);

FlatpakProxy::FlatpakProxy(const std::string& dbus_address, const std::string& socket_path) :
    dbus_address(dbus_address), socket_path(socket_path) {
    
    service = g_socket_service_new();
    add_policy("org.freedesktop.DBus", false, FLATPAK_POLICY_TALK);
}

FlatpakProxy::~FlatpakProxy() {
    if (service && g_socket_service_is_active(G_SOCKET_SERVICE(service))) {
        std::filesystem::remove(socket_path);
    }

    assert(clients.empty());
    
    for (auto &[_, filter_list] : filters) {
        for (auto filter : filter_list) {
            delete filter;
        }
    }
    filters.clear();
    
    if (service) {
        g_object_unref(service);
        service = nullptr;
    }
}

void FlatpakProxy::set_filter(bool filter) {
    this->filter = filter;
}

void FlatpakProxy::set_sloppy_names(bool sloppy_names) {
    this->sloppy_names = sloppy_names;
}

void FlatpakProxy::set_log_messages(bool log) {
    this->log_messages = log;
}

void FlatpakProxy::add_filter(Filter *filter) {
    filters[filter->name].push_back(filter);
}

void FlatpakProxy::add_policy(const std::string& name, bool name_is_subtree, FlatpakPolicy policy) {
    Filter *filter = new Filter(name, name_is_subtree, policy);
    add_filter(filter);
}

void FlatpakProxy::add_call_rule(const std::string& name, bool name_is_subtree, const std::string& rule) {
    Filter *filter = new Filter(name, name_is_subtree, FILTER_TYPE_CALL, rule);
    add_filter(filter);
}

void FlatpakProxy::add_broadcast_rule(const std::string& name, bool name_is_subtree, const std::string& rule) {
    Filter *filter = new Filter(name, name_is_subtree, FILTER_TYPE_BROADCAST, rule);
    add_filter(filter);
}

bool FlatpakProxy::start() {
    std::filesystem::remove(socket_path);

    GError *error = nullptr;
    GSocketAddress *s_address = g_unix_socket_address_new(socket_path.c_str());
    
    bool res = g_socket_listener_add_address(
        G_SOCKET_LISTENER(service),
        s_address,
        G_SOCKET_TYPE_STREAM,
        G_SOCKET_PROTOCOL_DEFAULT,
        nullptr,
        nullptr,
        &error
    );

    g_object_unref(s_address);

    if (!res) {
        if (error) {
            std::cerr << "Failed to start proxy: " << error->message << "\n";
            g_error_free(error);
        }
        return false;
    }

    g_signal_connect(
        service,
        "incoming",
        G_CALLBACK(+[](GSocketService *s, GSocketConnection *conn, GObject *, gpointer data) -> gboolean {
            auto *proxy = static_cast<FlatpakProxy *>(data);
            return proxy->incoming_connection(s, conn);
        }),
        this
    );

    g_socket_service_start(G_SOCKET_SERVICE(service));
    return true;
}

void FlatpakProxy::stop() {
    std::filesystem::remove(socket_path);
    if (service) {
        g_socket_service_stop(G_SOCKET_SERVICE(service));
    }
}

bool FlatpakProxy::incoming_connection(GSocketService *, GSocketConnection *conn) {
    auto client = std::make_shared<FlatpakProxyClient>(this, conn);
    client->init_side(client, conn);
    
    auto client_ptr = new std::shared_ptr<FlatpakProxyClient>(client);
    g_dbus_address_get_stream(
        dbus_address.c_str(),
        nullptr,
        client_connected_to_dbus,
        client_ptr
    );

    return true;
}

void client_connected_to_dbus(GObject *, GAsyncResult *res, void *user_data) {
    auto client_holder = static_cast<std::shared_ptr<FlatpakProxyClient> *>(user_data);
    auto client = *client_holder;
    delete client_holder;

    GError *error = nullptr;
    GIOStream *stream = g_dbus_address_get_stream_finish(res, nullptr, &error);
    
    if (!stream || error) {
        std::cerr << "Failed to connect to bus: ";
        if (error) {
            std::cerr << error->message;
            g_error_free(error);
        }
        std::cerr << "\n";
        return;
    }

    GSocketConnection *connection = G_SOCKET_CONNECTION(stream);
    GSocket *socket = g_socket_connection_get_socket(connection);
    g_socket_set_blocking(socket, FALSE);

    client->bus_side.connection = connection;
    client->client_side.start_reading();
    client->bus_side.start_reading();
}

FlatpakProxyClient::FlatpakProxyClient(FlatpakProxy *proxy, GSocketConnection *) :
    proxy(proxy) {
}

void FlatpakProxyClient::init_side(std::shared_ptr<FlatpakProxyClient> self, GSocketConnection *client_conn) {
    client_side = ProxySide(self, false);
    bus_side = ProxySide(self, true);
    
    g_socket_set_blocking(g_socket_connection_get_socket(client_conn), FALSE);
    client_side.connection = g_object_ref(client_conn);
    
    proxy->clients.push_back(self);
}

FlatpakProxyClient::~FlatpakProxyClient() {
    if (proxy) {
        proxy->clients.remove_if([this](const std::shared_ptr<FlatpakProxyClient>& c) {
            return c.get() == this;
        });
    }

    for (auto &[_, msg] : rewrite_reply) {
        g_object_unref(msg);
    }
    rewrite_reply.clear();
    get_owner_reply.clear();
    unique_id_policy.clear();
    unique_id_owned_names.clear();
}

void queue_expected_reply(ProxySide *side, uint32_t serial, ExpectedReplyType type) {
    side->expected_replies[serial] = type;
}

ExpectedReplyType steal_expected_reply(ProxySide *side, uint32_t serial) {
    auto it = side->expected_replies.find(serial);
    if (it != side->expected_replies.end()) {
        ExpectedReplyType type = it->second;
        side->expected_replies.erase(it);
        return type;
    }
    return EXPECTED_REPLY_NONE;
}

void queue_outgoing_buffer(ProxySide *side, Buffer *buffer) {
    if (side->out_source == nullptr) {
        GSocket *socket = g_socket_connection_get_socket(side->connection);
        side->out_source = g_socket_create_source(socket, G_IO_OUT, nullptr);
        g_source_set_callback(side->out_source, G_SOURCE_FUNC(side_out_cb), side, nullptr);
        g_source_attach(side->out_source, nullptr);
        g_source_unref(side->out_source);
    }

    side->buffers.push_back(buffer);
}

bool filter_matches(Filter *filter, FilterTypeMask type, const std::string& path,
                   const std::string& interface, const std::string& member) {
    if (filter->policy < FLATPAK_POLICY_TALK || (filter->types & type) == 0)
        return false;

    if (!filter->path.empty()) {
        if (path.empty()) 
            return false;

        if (filter->path_is_subtree) {
            if (!path.starts_with(filter->path) ||
                (path.size() != filter->path.size() && path[filter->path.size()] != '/'))
                return false;
        } else if (filter->path != path) {
            return false;
        }
    }

    if (!filter->interface.empty() && filter->interface != interface)
        return false;

    if (!filter->member.empty() && filter->member != member)
        return false;

    return true;
}

bool any_filter_matches(const std::vector<Filter *>& filters, FilterTypeMask type,
                       const std::string& path, const std::string& interface, const std::string& member) {
    for (auto filter : filters) {
        if (filter_matches(filter, type, path, interface, member))
            return true;
    }
    return false;
}

FlatpakPolicy FlatpakProxyClient::get_max_policy(const std::string& source) {
    return get_max_policy_and_matched(source, nullptr);
}

FlatpakPolicy FlatpakProxyClient::get_max_policy_and_matched(const std::string& source,
                                                           std::vector<Filter *> *matched_filters) {
    static Filter *match_all[FLATPAK_POLICY_OWN + 1] = {
        nullptr,
        new Filter("", false, FLATPAK_POLICY_SEE),
        new Filter("", false, FLATPAK_POLICY_TALK),
        new Filter("", false, FLATPAK_POLICY_OWN)
    };

    if (source.empty()) {
        if (matched_filters) 
            matched_filters->push_back(match_all[FLATPAK_POLICY_TALK]);
        return FLATPAK_POLICY_TALK;
    }

    FlatpakPolicy max_policy = FLATPAK_POLICY_NONE;

    if (source[0] == ':') {
        auto it = unique_id_policy.find(source);
        if (it != unique_id_policy.end()) {
            max_policy = it->second;
        }
        
        if (matched_filters && max_policy > FLATPAK_POLICY_NONE) {
            matched_filters->push_back(match_all[max_policy]);
        }

        auto owned = unique_id_owned_names.find(source);
        if (owned != unique_id_owned_names.end()) {
            for (const std::string &name : owned->second) {
                max_policy = std::max(max_policy, get_max_policy_and_matched(name, matched_filters));
            }
        }
        return max_policy;
    }

    std::string name = source;
    bool exact_match = true;
    
    while (true) {
        auto it = proxy->filters.find(name);
        if (it != proxy->filters.end()) {
            for (auto *filter : it->second) {
                if (exact_match || filter->name_is_subtree) {
                    max_policy = std::max(max_policy, filter->policy);
                    if (matched_filters) {
                        matched_filters->push_back(filter);
                    }
                }
            }
        }
        
        exact_match = false;
        size_t dot = name.rfind('.');
        if (dot == std::string::npos) break;
        name.erase(dot);
    }

    return max_policy;
}

void FlatpakProxyClient::update_unique_id_policy(const std::string& unique_id, FlatpakPolicy policy) {
    if (policy > FLATPAK_POLICY_NONE) {
        FlatpakPolicy old_policy = unique_id_policy[unique_id];
        if (policy > old_policy) {
            unique_id_policy[unique_id] = policy;
        }
    }
}

void FlatpakProxyClient::add_unique_id_owned_name(const std::string& unique_id, const std::string& owned_name) {
    unique_id_owned_names[unique_id].push_back(owned_name);
}

bool FlatpakProxyClient::validate_arg0_name(Buffer *buffer, FlatpakPolicy required_policy, FlatpakPolicy *has_policy) {
    GDBusMessage *message = g_dbus_message_new_from_blob(
        buffer->data.data(), buffer->size, static_cast<GDBusCapabilityFlags>(0), nullptr);
    
    if (has_policy) {
        *has_policy = FLATPAK_POLICY_NONE;
    }

    if (!message) {
        return false;
    }

    GVariant *body = g_dbus_message_get_body(message);
    if (!body) {
        g_object_unref(message);
        return false;
    }

    GVariant *arg0 = g_variant_get_child_value(body, 0);
    if (arg0 && g_variant_is_of_type(arg0, G_VARIANT_TYPE_STRING)) {
        const char *name = g_variant_get_string(arg0, nullptr);
        FlatpakPolicy name_policy = get_max_policy(name);

        if (has_policy) {
            *has_policy = name_policy;
        }

        if (name_policy >= required_policy) {
            g_variant_unref(arg0);
            g_object_unref(message);
            return true;
        }

        if (proxy->log_messages) {
            std::cerr << "Filtering message due to arg0 " << name
                      << ", policy: " << name_policy
                      << " (required " << required_policy << ")\n";
        }
    }

    if (arg0) g_variant_unref(arg0);
    g_object_unref(message);
    return false;
}

Buffer *message_to_buffer(GDBusMessage *message) {
    gsize blob_size;
    guchar *blob = g_dbus_message_to_blob(message, &blob_size, G_DBUS_CAPABILITY_FLAGS_NONE, nullptr);
    
    Buffer *buffer = new Buffer(blob_size);
    std::copy(blob, blob + blob_size, buffer->data.begin());
    buffer->pos = blob_size;
    
    g_free(blob);
    return buffer;
}

GDBusMessage *get_error_for_header(Header *header, const char *error) {
    GDBusMessage *reply = g_dbus_message_new();
    g_dbus_message_set_message_type(reply, G_DBUS_MESSAGE_TYPE_ERROR);
    g_dbus_message_set_flags(reply, G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED);
    g_dbus_message_set_reply_serial(reply, header->serial);
    g_dbus_message_set_error_name(reply, error);
    g_dbus_message_set_body(reply, g_variant_new("(s)", error));
    return reply;
}

GDBusMessage *get_bool_reply_for_header(Header *header, bool val) {
    GDBusMessage *reply = g_dbus_message_new();
    g_dbus_message_set_message_type(reply, G_DBUS_MESSAGE_TYPE_METHOD_RETURN);
    g_dbus_message_set_flags(reply, G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED);
    g_dbus_message_set_reply_serial(reply, header->serial);
    g_dbus_message_set_body(reply, g_variant_new("(b)", val));
    return reply;
}

Buffer *get_ping_buffer_for_header(Header *header) {
    GDBusMessage *dummy = g_dbus_message_new_method_call(nullptr, "/", "org.freedesktop.DBus.Peer", "Ping");
    g_dbus_message_set_serial(dummy, header->serial);
    g_dbus_message_set_flags(dummy, static_cast<GDBusMessageFlags>(header->flags));

    Buffer *buffer = message_to_buffer(dummy);
    g_object_unref(dummy);
    return buffer;
}

Buffer *FlatpakProxyClient::get_error_for_roundtrip(Header *header, const char *error_name) {
    Buffer *ping_buffer = get_ping_buffer_for_header(header);
    GDBusMessage *reply = get_error_for_header(header, error_name);
    rewrite_reply[header->serial] = reply;
    return ping_buffer;
}

Buffer *FlatpakProxyClient::get_bool_reply_for_roundtrip(Header *header, bool val) {
    Buffer *ping_buffer = get_ping_buffer_for_header(header);
    GDBusMessage *reply = get_bool_reply_for_header(header, val);
    rewrite_reply[header->serial] = reply;
    return ping_buffer;
}

BusHandler get_dbus_method_handler(FlatpakProxyClient *client, Header *header) {
    if (header->has_reply_serial) {
        ExpectedReplyType expected_reply = steal_expected_reply(&client->bus_side, header->reply_serial);
        if (expected_reply == EXPECTED_REPLY_NONE)
            return HANDLE_DENY;
        return HANDLE_PASS;
    }

    std::vector<Filter *> filters;
    FlatpakPolicy policy = client->get_max_policy_and_matched(header->destination, &filters);
    
    if (policy < FLATPAK_POLICY_SEE) return HANDLE_HIDE;
    if (policy < FLATPAK_POLICY_TALK) return HANDLE_DENY;

    if (!header->is_for_bus()) {
        if (policy == FLATPAK_POLICY_OWN ||
            any_filter_matches(filters, FILTER_TYPE_CALL, header->path, header->interface, header->member))
            return HANDLE_PASS;
        return HANDLE_DENY;
    }

    if (header->is_introspection_call()) {
        return HANDLE_PASS;
    } else if (header->is_dbus_method_call()) {
        const std::string& method = header->member;
        if (method.empty()) return HANDLE_DENY;

        if (method == "AddMatch") return HANDLE_VALIDATE_MATCH;
        if (method == "Hello" || method == "RemoveMatch" || method == "GetId") return HANDLE_PASS;
        if (method == "UpdateActivationEnvironment" || method == "BecomeMonitor") return HANDLE_DENY;
        if (method == "RequestName" || method == "ReleaseName" || method == "ListQueuedOwners") return HANDLE_VALIDATE_OWN;
        if (method == "NameHasOwner") return HANDLE_FILTER_HAS_OWNER_REPLY;
        if (method == "GetNameOwner") return HANDLE_FILTER_GET_OWNER_REPLY;
        if (method == "GetConnectionUnixProcessID" || method == "GetConnectionCredentials" ||
            method == "GetAdtAuditSessionData" || method == "GetConnectionSELinuxSecurityContext" ||
            method == "GetConnectionUnixUser") return HANDLE_VALIDATE_SEE;
        if (method == "StartServiceByName") return HANDLE_VALIDATE_TALK;
        if (method == "ListNames" || method == "ListActivatableNames") return HANDLE_FILTER_NAME_LIST_REPLY;

        std::cerr << "Unknown bus method " << method << "\n";
    }
    return HANDLE_DENY;
}

FlatpakPolicy policy_from_handler(BusHandler handler) {
    switch (handler) {
        case HANDLE_VALIDATE_OWN: return FLATPAK_POLICY_OWN;
        case HANDLE_VALIDATE_TALK: return FLATPAK_POLICY_TALK;
        case HANDLE_VALIDATE_SEE: return FLATPAK_POLICY_SEE;
        default: return FLATPAK_POLICY_NONE;
    }
}

bool validate_arg0_match(Buffer *buffer) {
    GDBusMessage *message = g_dbus_message_new_from_blob(
        buffer->data.data(), buffer->size, static_cast<GDBusCapabilityFlags>(0), nullptr);

    bool result = true;
    if (message) {
        GVariant *body = g_dbus_message_get_body(message);
        if (body) {
            GVariant *arg0 = g_variant_get_child_value(body, 0);
            if (arg0 && g_variant_is_of_type(arg0, G_VARIANT_TYPE_STRING)) {
                std::string match = g_variant_get_string(arg0, nullptr);
                if (match.find("eavesdrop=") != std::string::npos) {
                    result = false;
                }
            }
            if (arg0) g_variant_unref(arg0);
        }
        g_object_unref(message);
    }
    return result;
}

std::list<GSocketControlMessage *> side_get_n_unix_fds(ProxySide *side, int n_fds) {
    while (!side->control_messages.empty()) {
        auto it = side->control_messages.begin();
        GSocketControlMessage *msg = *it;
        
        if (G_IS_UNIX_FD_MESSAGE(msg)) {
            GUnixFDMessage *fd_msg = G_UNIX_FD_MESSAGE(msg);
            GUnixFDList *fd_list = g_unix_fd_message_get_fd_list(fd_msg);
            int len = g_unix_fd_list_get_length(fd_list);
            
            if (len != n_fds) {
                std::cerr << "Not right nr of fds in socket message\n";
                return {};
            }
            
            side->control_messages.erase(it);
            return {msg};
        }
        
        g_object_unref(msg);
        side->control_messages.erase(it);
    }
    return {};
}

bool update_socket_messages(ProxySide *side, Buffer *buffer, Header *header) {
    side->control_messages.splice(side->control_messages.end(), buffer->control_messages);
    buffer->control_messages.clear();
    
    if (header->unix_fds > 0) {
        buffer->control_messages = side_get_n_unix_fds(side, header->unix_fds);
        if (buffer->control_messages.empty()) {
            std::cerr << "Not enough fds for message\n";
            side->side_closed();
            buffer->unref();
            return false;
        }
    }
    return true;
}

void FlatpakProxyClient::got_buffer_from_client(Buffer *buffer) {
    ExpectedReplyType expecting_reply = EXPECTED_REPLY_NONE;
    ProxySide *side = &client_side;

    if (auth_state == AUTH_COMPLETE && proxy->filter) {
        Header header;
        try {
            header.parse(buffer);
        } catch (const std::exception &ex) {
            std::cerr << "Invalid message header format from client: " << ex.what() << "\n";
            side->side_closed();
            buffer->unref();
            return;
        }

        if (!update_socket_messages(side, buffer, &header)) {
            return;
        }

        if (header.serial > MAX_CLIENT_SERIAL) {
            std::cerr << "Invalid client serial: Exceeds maximum value of " << MAX_CLIENT_SERIAL << "\n";
            side->side_closed();
            buffer->unref();
            return;
        }

        if (proxy->log_messages) {
            header.print_outgoing();
        }

        if (header.is_dbus_method_call() && header.member == "Hello") {
            expecting_reply = EXPECTED_REPLY_HELLO;
            hello_serial = header.serial;
        }

        BusHandler handler = get_dbus_method_handler(this, &header);

        switch (handler) {
            case HANDLE_FILTER_HAS_OWNER_REPLY:
            case HANDLE_FILTER_GET_OWNER_REPLY:
                if (!validate_arg0_name(buffer, FLATPAK_POLICY_SEE, nullptr)) {
                    buffer->unref();
                    buffer = (handler == HANDLE_FILTER_GET_OWNER_REPLY) 
                        ? get_error_for_roundtrip(&header, "org.freedesktop.DBus.Error.NameHasNoOwner")
                        : get_bool_reply_for_roundtrip(&header, false);
                    expecting_reply = EXPECTED_REPLY_REWRITE;
                    break;
                }
                // Fall through to HANDLE_PASS
                [[fallthrough]];

            case HANDLE_PASS:
                if (header.client_message_generates_reply()) {
                    if (expecting_reply == EXPECTED_REPLY_NONE) {
                        expecting_reply = EXPECTED_REPLY_NORMAL;
                    }
                }
                break;

            case HANDLE_VALIDATE_MATCH:
                if (!validate_arg0_match(buffer)) {
                    if (proxy->log_messages) {
                        std::cerr << "*DENIED* (ping)\n";
                    }
                    buffer->unref();
                    buffer = get_error_for_roundtrip(&header, "org.freedesktop.DBus.Error.AccessDenied");
                    expecting_reply = EXPECTED_REPLY_REWRITE;
                    break;
                }
                if (header.client_message_generates_reply()) {
                    if (expecting_reply == EXPECTED_REPLY_NONE) {
                        expecting_reply = EXPECTED_REPLY_NORMAL;
                    }
                }
                break;

            case HANDLE_VALIDATE_OWN:
            case HANDLE_VALIDATE_SEE:
            case HANDLE_VALIDATE_TALK: {
                FlatpakPolicy name_policy;
                if (validate_arg0_name(buffer, policy_from_handler(handler), &name_policy)) {
                    if (header.client_message_generates_reply()) {
                        if (expecting_reply == EXPECTED_REPLY_NONE) {
                            expecting_reply = EXPECTED_REPLY_NORMAL;
                        }
                    }
                    break;
                }

                buffer->unref();
                if (name_policy < FLATPAK_POLICY_SEE) {
                    if (header.client_message_generates_reply()) {
                        if (proxy->log_messages) {
                            std::cerr << "*HIDDEN* (ping)\n";
                        }
                        std::string error_str = (!header.destination.empty() && header.destination[0] == ':') ||
                                              (header.flags & G_DBUS_MESSAGE_FLAGS_NO_AUTO_START) != 0
                                              ? "org.freedesktop.DBus.Error.NameHasNoOwner"
                                              : "org.freedesktop.DBus.Error.ServiceUnknown";
                        buffer = get_error_for_roundtrip(&header, error_str.c_str());
                        expecting_reply = EXPECTED_REPLY_REWRITE;
                    } else {
                        if (proxy->log_messages) {
                            std::cerr << "*HIDDEN*\n";
                        }
                        buffer = nullptr;
                    }
                } else {
                    if (header.client_message_generates_reply()) {
                        if (proxy->log_messages) {
                            std::cerr << "*DENIED* (ping)\n";
                        }
                        buffer = get_error_for_roundtrip(&header, "org.freedesktop.DBus.Error.AccessDenied");
                        expecting_reply = EXPECTED_REPLY_REWRITE;
                    } else {
                        if (proxy->log_messages) {
                            std::cerr << "*DENIED*\n";
                        }
                        buffer = nullptr;
                    }
                }
                break;
            }

            case HANDLE_FILTER_NAME_LIST_REPLY:
                expecting_reply = EXPECTED_REPLY_LIST_NAMES;
                if (header.client_message_generates_reply()) {
                    if (expecting_reply == EXPECTED_REPLY_NONE) {
                        expecting_reply = EXPECTED_REPLY_NORMAL;
                    }
                }
                break;

            case HANDLE_HIDE:
                buffer->unref();
                if (header.client_message_generates_reply()) {
                    if (proxy->log_messages) {
                        std::cerr << "*HIDDEN* (ping)\n";
                    }
                    std::string error_str = (!header.destination.empty() && header.destination[0] == ':') ||
                                          (header.flags & G_DBUS_MESSAGE_FLAGS_NO_AUTO_START) != 0
                                          ? "org.freedesktop.DBus.Error.NameHasNoOwner"
                                          : "org.freedesktop.DBus.Error.ServiceUnknown";
                    buffer = get_error_for_roundtrip(&header, error_str.c_str());
                    expecting_reply = EXPECTED_REPLY_REWRITE;
                } else {
                    if (proxy->log_messages) {
                        std::cerr << "*HIDDEN*\n";
                    }
                    buffer = nullptr;
                }
                break;

            case HANDLE_DENY:
            default:
                buffer->unref();
                if (header.client_message_generates_reply()) {
                    if (proxy->log_messages) {
                        std::cerr << "*DENIED* (ping)\n";
                    }
                    buffer = get_error_for_roundtrip(&header, "org.freedesktop.DBus.Error.AccessDenied");
                    expecting_reply = EXPECTED_REPLY_REWRITE;
                } else {
                    if (proxy->log_messages) {
                        std::cerr << "*DENIED*\n";
                    }
                    buffer = nullptr;
                }
                break;
        }

        if (buffer != nullptr && expecting_reply != EXPECTED_REPLY_NONE) {
            queue_expected_reply(side, header.serial, expecting_reply);
        }
    }

    if (buffer) {
        queue_outgoing_buffer(&bus_side, buffer);
    }
}

std::string get_arg0_string(Buffer *buffer) {
    GDBusMessage *message = g_dbus_message_new_from_blob(
        buffer->data.data(), buffer->size, static_cast<GDBusCapabilityFlags>(0), nullptr);
    
    if (!message) {
        return "";
    }

    GVariant *body = g_dbus_message_get_body(message);
    if (!body) {
        g_object_unref(message);
        return "";
    }

    GVariant *arg0 = g_variant_get_child_value(body, 0);
    std::string result;
    
    if (arg0 && g_variant_is_of_type(arg0, G_VARIANT_TYPE_STRING)) {
        const char *str = g_variant_get_string(arg0, nullptr);
        result = str;
    }

    if (arg0) g_variant_unref(arg0);
    g_object_unref(message);
    return result;
}

Buffer *filter_names_list(FlatpakProxyClient *client, Buffer *buffer) {
    GDBusMessage *message = g_dbus_message_new_from_blob(
        buffer->data.data(), buffer->size, static_cast<GDBusCapabilityFlags>(0), nullptr);

    if (!message) {
        return nullptr;
    }

    GVariant *body = g_dbus_message_get_body(message);
    if (!body) {
        g_object_unref(message);
        return nullptr;
    }

    GVariant *arg0 = g_variant_get_child_value(body, 0);
    if (!arg0 || !g_variant_is_of_type(arg0, G_VARIANT_TYPE_STRING_ARRAY)) {
        if (arg0) g_variant_unref(arg0);
        g_object_unref(message);
        return nullptr;
    }

    const gchar **names = g_variant_get_strv(arg0, nullptr);
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE_STRING_ARRAY);
    
    for (int i = 0; names[i] != nullptr; i++) {
        if (client->get_max_policy(names[i]) >= FLATPAK_POLICY_SEE) {
            g_variant_builder_add(&builder, "s", names[i]);
        }
    }
    
    g_free(names);
    g_variant_unref(arg0);

    GVariant *new_names = g_variant_builder_end(&builder);
    g_dbus_message_set_body(message, g_variant_new_tuple(&new_names, 1));

    Buffer *filtered = message_to_buffer(message);
    g_object_unref(message);
    return filtered;
}

bool message_is_name_owner_changed(Header *header) {
    return header->type == G_DBUS_MESSAGE_TYPE_SIGNAL &&
           header->sender == "org.freedesktop.DBus" &&
           header->interface == "org.freedesktop.DBus" &&
           header->member == "NameOwnerChanged";
}

bool should_filter_name_owner_changed(FlatpakProxyClient *client, Buffer *buffer) {
    GDBusMessage *message = g_dbus_message_new_from_blob(
        buffer->data.data(), buffer->size, static_cast<GDBusCapabilityFlags>(0), nullptr);

    if (!message) {
        return true;
    }

    GVariant *body = g_dbus_message_get_body(message);
    if (!body) {
        g_object_unref(message);
        return true;
    }

    GVariant *arg0 = g_variant_get_child_value(body, 0);
    GVariant *arg2 = g_variant_get_child_value(body, 2);
    
    bool result = true;
    
    if (arg0 && g_variant_is_of_type(arg0, G_VARIANT_TYPE_STRING) &&
        arg2 && g_variant_is_of_type(arg2, G_VARIANT_TYPE_STRING)) {
        
        const char *name = g_variant_get_string(arg0, nullptr);
        const char *new_owner = g_variant_get_string(arg2, nullptr);

        if (client->get_max_policy(name) >= FLATPAK_POLICY_SEE ||
            (client->proxy->sloppy_names && name[0] == ':')) {
            
            if (name[0] != ':' && new_owner[0] != 0) {
                client->add_unique_id_owned_name(new_owner, name);
            }
            
            result = false;
        }
    }

    if (arg0) g_variant_unref(arg0);
    if (arg2) g_variant_unref(arg2);
    g_object_unref(message);
    
    return result;
}

void FlatpakProxyClient::got_buffer_from_bus(Buffer *buffer) {
    ProxySide *side = &bus_side;

    if (auth_state == AUTH_COMPLETE && proxy->filter) {
        Header header;
        try {
            header.parse(buffer);
        } catch (const std::exception &ex) {
            std::cerr << "Invalid message header format from bus: " << ex.what() << "\n";
            side->side_closed();
            buffer->unref();
            return;
        }

        if (!update_socket_messages(side, buffer, &header)) {
            return;
        }

        if (proxy->log_messages) {
            header.print_incoming();
        }

        if (header.has_reply_serial) {
            ExpectedReplyType expected_reply = steal_expected_reply(side->get_other_side(), header.reply_serial);

            switch (expected_reply) {
                case EXPECTED_REPLY_NONE:
                    if (proxy->log_messages) {
                        std::cerr << "*Unexpected reply*\n";
                    }
                    buffer->unref();
                    return;

                case EXPECTED_REPLY_HELLO:
                    if (header.type == G_DBUS_MESSAGE_TYPE_METHOD_RETURN) {
                        std::string my_id = get_arg0_string(buffer);
                        update_unique_id_policy(my_id, FLATPAK_POLICY_TALK);
                    }
                    break;

                case EXPECTED_REPLY_REWRITE: {
                    auto it = rewrite_reply.find(header.reply_serial);
                    if (it != rewrite_reply.end()) {
                        if (proxy->log_messages) {
                            std::cerr << "*REWRITTEN*\n";
                        }
                        g_dbus_message_set_serial(it->second, header.serial);
                        buffer->unref();
                        buffer = message_to_buffer(it->second);
                        g_object_unref(it->second);
                        rewrite_reply.erase(it);
                    }
                    break;
                }

                case EXPECTED_REPLY_FAKE_LIST_NAMES:
                    if (proxy->log_messages) {
                        std::cerr << "*SKIPPED*\n";
                    }
                    buffer->unref();
                    buffer = nullptr;
                    client_side.start_reading();
                    break;

                case EXPECTED_REPLY_FAKE_GET_NAME_OWNER: {
                    auto it = get_owner_reply.find(header.reply_serial);
                    if (it != get_owner_reply.end()) {
                        if (header.type == G_DBUS_MESSAGE_TYPE_METHOD_RETURN) {
                            std::string owner = get_arg0_string(buffer);
                            if (!owner.empty()) {
                                add_unique_id_owned_name(owner, it->second);
                            }
                        }
                        get_owner_reply.erase(it);
                    }
                    if (proxy->log_messages) {
                        std::cerr << "*SKIPPED*\n";
                    }
                    buffer->unref();
                    buffer = nullptr;
                    break;
                }

                case EXPECTED_REPLY_FILTER:
                    if (proxy->log_messages) {
                        std::cerr << "*SKIPPED*\n";
                    }
                    buffer->unref();
                    buffer = nullptr;
                    break;

                case EXPECTED_REPLY_LIST_NAMES:
                    if (header.type == G_DBUS_MESSAGE_TYPE_METHOD_RETURN) {
                        Buffer *filtered = filter_names_list(this, buffer);
                        buffer->unref();
                        buffer = filtered;
                    }
                    break;

                case EXPECTED_REPLY_NORMAL:
                    break;

                default:
                    std::cerr << "Unexpected expected reply type " << expected_reply << "\n";
                    break;
            }
        } else {
            if (header.type == G_DBUS_MESSAGE_TYPE_METHOD_RETURN ||
                header.type == G_DBUS_MESSAGE_TYPE_ERROR) {
                if (proxy->log_messages) {
                    std::cerr << "*Invalid reply*\n";
                }
                buffer->unref();
                buffer = nullptr;
            }

            if (message_is_name_owner_changed(&header)) {
                if (should_filter_name_owner_changed(this, buffer)) {
                    buffer->unref();
                    buffer = nullptr;
                }
            }
        }

        if (buffer && header.type == G_DBUS_MESSAGE_TYPE_SIGNAL && header.destination.empty()) {
            std::vector<Filter *> filters;
            bool filtered = true;

            FlatpakPolicy policy = get_max_policy_and_matched(header.sender, &filters);

            if (policy == FLATPAK_POLICY_OWN ||
                policy == FLATPAK_POLICY_TALK ||
                any_filter_matches(filters, FILTER_TYPE_BROADCAST, header.path, header.interface, header.member)) {
                filtered = false;
            }

            if (filtered) {
                if (proxy->log_messages) {
                    std::cerr << "*FILTERED IN*\n";
                }
                buffer->unref();
                buffer = nullptr;
            }
        }

        if (buffer && !header.sender.empty() && header.sender[0] == ':') {
            update_unique_id_policy(header.sender, FLATPAK_POLICY_SEE);
        }

        if (buffer && header.client_message_generates_reply()) {
            queue_expected_reply(side, header.serial, EXPECTED_REPLY_NORMAL);
        }
    }

    if (buffer) {
        queue_outgoing_buffer(&client_side, buffer);
    }
}

void queue_fake_message(FlatpakProxyClient *client, GDBusMessage *message, ExpectedReplyType reply_type) {
    ++client->last_fake_serial;
    assert(client->last_fake_serial > MAX_CLIENT_SERIAL);
    
    g_dbus_message_set_serial(message, client->last_fake_serial);
    Buffer *buffer = message_to_buffer(message);
    g_object_unref(message);

    queue_outgoing_buffer(&client->bus_side, buffer);
    queue_expected_reply(&client->client_side, client->last_fake_serial, reply_type);
}

void queue_initial_name_ops(FlatpakProxyClient *client) {
    bool has_wildcards = false;

    for (auto &[name, filters] : client->proxy->filters) {
        bool name_needs_subtree = false;
        
        for (auto filter : filters) {
            if (filter->name_is_subtree) {
                name_needs_subtree = true;
                break;
            }
        }

        if (name == "org.freedesktop.DBus") continue;

        GDBusMessage *message = g_dbus_message_new_method_call(
            "org.freedesktop.DBus", "/", "org.freedesktop.DBus", "AddMatch");
            
        GVariant *match;
        if (name_needs_subtree) {
            match = g_variant_new_printf(
                "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
                "member='NameOwnerChanged',arg0namespace='%s'",
                name.c_str());
        } else {
            match = g_variant_new_printf(
                "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
                "member='NameOwnerChanged',arg0='%s'",
                name.c_str());
        }
        
        g_dbus_message_set_body(message, g_variant_new_tuple(&match, 1));
        queue_fake_message(client, message, EXPECTED_REPLY_FILTER);

        if (client->proxy->log_messages) {
            std::cerr << "C" << client->last_fake_serial << ": -> org.freedesktop.DBus fake "
                      << (name_needs_subtree ? "wildcarded " : "") << "AddMatch for " << name << "\n";
        }

        if (!name_needs_subtree) {
            message = g_dbus_message_new_method_call(
                "org.freedesktop.DBus", "/", "org.freedesktop.DBus", "GetNameOwner");
            g_dbus_message_set_body(message, g_variant_new("(s)", name.c_str()));
            queue_fake_message(client, message, EXPECTED_REPLY_FAKE_GET_NAME_OWNER);
            client->get_owner_reply[client->last_fake_serial] = name;

            if (client->proxy->log_messages) {
                std::cerr << "C" << client->last_fake_serial 
                          << ": -> org.freedesktop.DBus fake GetNameOwner for " << name << "\n";
            }
        } else {
            has_wildcards = true;
        }
    }

    if (has_wildcards) {
        GDBusMessage *message = g_dbus_message_new_method_call(
            "org.freedesktop.DBus", "/", "org.freedesktop.DBus", "ListNames");
        g_dbus_message_set_body(message, g_variant_new("()"));
        queue_fake_message(client, message, EXPECTED_REPLY_FAKE_LIST_NAMES);

        if (client->proxy->log_messages) {
            std::cerr << "C" << client->last_fake_serial << ": -> org.freedesktop.DBus fake ListNames\n";
        }

        client->client_side.stop_reading();
    }
}