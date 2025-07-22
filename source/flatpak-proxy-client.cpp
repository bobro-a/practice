#include "headers/flatpak-proxy-client.h"


void FlatpakProxy::set_filter(bool filter) {
    this->filter = filter;
}

void FlatpakProxy::set_sloppy_names(bool sloopy_names) {
    this->sloppy_names = sloopy_names;
}

void FlatpakProxy::set_log_messages(bool log) {
    this->log_messages = log;
}

void FlatpakProxy::add_filter(Filter *filter) {
    this->filters[filter->name].push_back(filter);
}

void
FlatpakProxy::add_policy(std::string name,
                         bool name_is_subtree,
                         FlatpakPolicy policy) {
    Filter *filter = new Filter(name, name_is_subtree, policy);
    this->add_filter(filter);
}


void FlatpakProxy::add_call_rule(std::string name, bool name_is_subtree, std::string rule) {
    Filter *filter = new Filter(name, name_is_subtree, FILTER_TYPE_CALL, rule);
    this->add_filter(filter);
}


void FlatpakProxy::add_broadcast_rule(std::string name, bool name_is_subtree, std::string rule) {
    Filter *filter = new Filter(name, name_is_subtree, FILTER_TYPE_BROADCAST, rule);
    this->add_filter(filter);
}


bool FlatpakProxy::start() {
    std::filesystem::remove(socket_path);

    GError *error = nullptr;
    GSocketAddress *address = g_unix_socket_address_new(socket_path.c_str());
    GSocketListener *listener = G_SOCKET_LISTENER(parent->gobj());

    gboolean ok = g_socket_listener_add_address(
            listener,
            address,
            G_SOCKET_TYPE_STREAM,
            G_SOCKET_PROTOCOL_DEFAULT,
            nullptr,
            nullptr,
            &error
    );
    g_object_unref(address);

    if (!ok) {
        std::cerr << "Ошибка добавления адреса: " << error->message << "\n";
        g_error_free(error);
        return false;
    }
    parent->signal_incoming().connect(
            sigc::mem_fun(*this, &FlatpakProxy::incoming_connection)
    );
    parent->start();
    return true;
}

void FlatpakProxy::stop() {
    std::filesystem::remove(socket_path);
    parent->stop();
}

void client_connected_to_dbus(GObject *source_object,
                              GAsyncResult *res,
                              void *user_data) {
    FlatpakProxyClient *client = static_cast<FlatpakProxyClient *>(user_data);
    GError *error = nullptr;

    GIOStream *stream = g_dbus_address_get_stream_finish(res, NULL, &error);
    if (!stream) {
        std::cerr << "Failed to connect to bus: " << error->message << "\n";
        if (error) g_error_free(error);
        delete client;
        return;
    }

    GSocketConnection *connection = G_SOCKET_CONNECTION (stream);
    g_socket_set_blocking(g_socket_connection_get_socket(connection), FALSE);
    client->bus_side.connection = connection;

    client->client_side.start_reading();
    client->bus_side.start_reading();
}

bool FlatpakProxy::incoming_connection(
        const Glib::RefPtr<Gio::SocketConnection> &connection,
        const Glib::RefPtr<Glib::Object> &source_object
) {
    auto client = new FlatpakProxyClient(this, connection);
    connection->get_socket()->set_blocking(false);

    g_dbus_address_get_stream(dbus_address.c_str(),
                              nullptr,
                              client_connected_to_dbus,
                              client);
    return true;
}

FlatpakProxy::~FlatpakProxy() {
    if (Glib::RefPtr<Gio::SocketService> service = parent) {
        if (service->is_active()) {
            ::unlink(socket_path.c_str());
        }
    }
    assert(clients.empty());
    for (auto &[_, v]: filters) {
        for (auto f: v) {
            delete f;
        }
    }
    filters.clear();
}

FlatpakProxy::FlatpakProxy(std::string dbus_address, std::string socket_path) {
    this->dbus_address = dbus_address;
    this->socket_path = socket_path;
    log_messages = filter = sloppy_names = false;
    parent = Gio::SocketService::create();
    add_policy("org.freedesktop.DBus", false, FLATPAK_POLICY_TALK);
    clients = {};
}

FlatpakProxyClient::FlatpakProxyClient(FlatpakProxy *proxy,
                                       Glib::RefPtr<Gio::SocketConnection> client_conn) :
        proxy(proxy),
        auth_state(AUTH_WAITING_FOR_BEGIN),
        auth_requests(0),
        auth_replies(0),
        hello_serial(0),
        last_fake_serial(0xFFFFFFFF - 65536),
        client_side(ProxySide(this, false)),
        bus_side(ProxySide(this, true)) {

    client_side.connection = G_SOCKET_CONNECTION(g_object_ref(client_conn->gobj()));
    proxy->clients.push_back(this);
    last_fake_serial = MAX_CLIENT_SERIAL;
}

FlatpakProxyClient::~FlatpakProxyClient() {
    if (proxy) {
        proxy->clients.remove(this);
    }

    // todo replace GDBusMessage
    for (auto &[_, msg]: rewrite_reply) {
        g_object_unref(msg);
    }
    rewrite_reply.clear();
    get_owner_reply.clear();
    unique_id_policy.clear();
    unique_id_owned_names.clear();
}

std::list<GSocketControlMessage *>
side_get_n_unix_fds(ProxySide *side, int n_fds) {
    while (!side->control_messages.empty()) {
        auto it = side->control_messages.begin();
        GSocketControlMessage *msg = *it;
        if (G_IS_UNIX_FD_MESSAGE (msg)) {
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

bool
update_socket_messages(ProxySide *side, Buffer *buffer, Header *header) {
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

ExpectedReplyType
steal_expected_reply(ProxySide *side, uint32_t serial) {
    auto it = side->expected_replies.find(serial);
    if (it != side->expected_replies.end()) {
        ExpectedReplyType type = it->second;
        side->expected_replies.erase(it);
        return type;
    }
    return EXPECTED_REPLY_NONE;
}

bool filter_matches(Filter *filter,//todo: later make Filter methods
                    FilterTypeMask type,
                    std::string &path,
                    std::string &interface,
                    std::string &member) {
    if (filter->policy < FLATPAK_POLICY_TALK ||
        (filter->types & type) == 0)
        return false;
    if (!filter->path.empty()) {
        if (path.empty()) return false;
        if (filter->path_is_subtree) {
            if (!path.starts_with(filter->path) ||
                (path.size() != filter->path.size() && path[filter->path.size()] != '/'))
                return false;

        } else if (filter->path != path) return false;
    }
    if (!filter->interface.empty() && filter->interface != interface)
        return false;
    if (!filter->member.empty() && filter->member != member)
        return false;
    return true;
}

bool any_filter_matches(std::vector<Filter *> *filters,
                        FilterTypeMask type,
                        std::string &path,
                        std::string &interface,
                        std::string &member) {
    for (auto filter: *filters) {
        if (filter_matches(filter, type, path, interface, member))
            return true;
    }

    return false;
}

BusHandler
get_dbus_method_handler(FlatpakProxyClient *client, Header *header) {
    if (header->has_reply_serial) {
        ExpectedReplyType expected_reply =
                steal_expected_reply(&client->bus_side,
                                     header->reply_serial);
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
            any_filter_matches(&filters, FILTER_TYPE_CALL,
                               header->path,
                               header->interface,
                               header->member))
            return HANDLE_PASS;

        return HANDLE_DENY;
    }
    if (header->is_introspection_call()) return HANDLE_PASS;
    else if (header->is_dbus_method_call()) {
        std::string method = header->member;
        if (method.empty()) return HANDLE_DENY;
        if (method == "AddMatch") return HANDLE_VALIDATE_MATCH;

        if (method == "Hello" ||
            method == "RemoveMatch" ||
            method == "GetId")
            return HANDLE_PASS;

        if (method == "UpdateActivationEnvironment" ||
            method == "BecomeMonitor")
            return HANDLE_DENY;

        if (method == "RequestName" ||
            method == "ReleaseName" ||
            method == "ListQueuedOwners")
            return HANDLE_VALIDATE_OWN;

        if (method == "NameHasOwner") return HANDLE_FILTER_HAS_OWNER_REPLY;
        if (method == "GetNameOwner") return HANDLE_FILTER_GET_OWNER_REPLY;

        if (method == "GetConnectionUnixProcessID" ||
            method == "GetConnectionCredentials" ||
            method == "GetAdtAuditSessionData" ||
            method == "GetConnectionSELinuxSecurityContext" ||
            method == "GetConnectionUnixUser")
            return HANDLE_VALIDATE_SEE;

        if (method == "StartServiceByName") return HANDLE_VALIDATE_TALK;

        if (method == "ListNames" ||
            method == "ListActivatableNames")
            return HANDLE_FILTER_NAME_LIST_REPLY;

        std::cerr << "Unknown bus method " << method << "\n";
    }
    return HANDLE_DENY;
}

bool FlatpakProxyClient::validate_arg0_name(Buffer *buffer, FlatpakPolicy required_policy, FlatpakPolicy *has_policy) {
    GDBusMessage *message = g_dbus_message_new_from_blob(buffer->data.data(), buffer->size,
                                                         static_cast<GDBusCapabilityFlags>(0), nullptr);
    GVariant *body;
    GVariant *arg0;
    if (has_policy)
        *has_policy = FLATPAK_POLICY_NONE;
    if (message != nullptr &&
        (body = g_dbus_message_get_body(message)) != nullptr &&
        (arg0 = g_variant_get_child_value(body, 0)) != nullptr &&
        g_variant_is_of_type(arg0, G_VARIANT_TYPE_STRING)) {
        std::string name = g_variant_get_string(arg0, nullptr);
        auto name_policy = get_max_policy(name);
        if (has_policy) *has_policy = name_policy;
        if (name_policy >= required_policy){
            g_variant_unref(arg0);
            g_object_unref(message);
            return true;
        }

        if (proxy->log_messages)
            std::cout << "Filtering message due to arg0 " << name << ", policy: " << name_policy << " (required "
                      << required_policy << ")\n";
    }
    g_variant_unref(arg0);
    g_object_unref(message);
    return false;
}


void FlatpakProxyClient::got_buffer_from_client(Buffer *buffer) {
    ExpectedReplyType expecting_reply = EXPECTED_REPLY_NONE;
    auto side = this->client_side;
    if (auth_state == AUTH_COMPLETE && proxy->filter) {
        Header header;
        BusHandler handler;
        try {
            header.parse(buffer);
        } catch (std::exception &ex) {
            std::cerr << "Invalid message header format from client: " << ex.what() << "\n";
            side.side_closed();
            buffer->unref();
            return;
        }
        if (!update_socket_messages(&side, buffer, &header))
            return;
        if (header.serial > MAX_CLIENT_SERIAL) {
            std::cerr << "Invalid client serial: Exceeds maximum value of " << MAX_CLIENT_SERIAL << "\n";
            side.side_closed();
            buffer->unref();
            return;
        }
        if (proxy->log_messages)
            header.print_outgoing();
        if (header.is_dbus_method_call() &&
            header.member == "Hello") {
            expecting_reply = EXPECTED_REPLY_HELLO;
            hello_serial = header.serial;
        }
        handler = get_dbus_method_handler(this, &header);
        switch (handler) {
            case HANDLE_FILTER_HAS_OWNER_REPLY:
            case HANDLE_FILTER_GET_OWNER_REPLY:
                if (!validate_arg0_name(buffer, FLATPAK_POLICY_SEE, nullptr)) {
                    delete buffer;
                    if (handler == HANDLE_FILTER_GET_OWNER_REPLY)
                        buffer = get_error_for_roundtrip (client, header,
                                                          "org.freedesktop.DBus.Error.NameHasNoOwner");
                }//todo
        }

    }
}

void FlatpakProxyClient::got_buffer_from_bus(Buffer *buffer) {
    //todo
}

void FlatpakProxyClient::add_unique_id_owned_name(std::string unique_id, std::string owned_name) {
    bool already_added;
    already_added = (unique_id_owned_names.find(unique_id) != unique_id_owned_names.end());
    if (!already_added) {
        unique_id_owned_names[unique_id].push_back(owned_name);
    }
}

void FlatpakProxyClient::update_unique_id_policy(std::string unique_id,
                                                 FlatpakPolicy policy) {
    if (policy > FLATPAK_POLICY_NONE && policy > unique_id_policy[unique_id])
        unique_id_policy[unique_id] = policy;
}

FlatpakPolicy FlatpakProxyClient::get_max_policy(std::string source) {
    return get_max_policy_and_matched(source, nullptr);
}

FlatpakPolicy FlatpakProxyClient::get_max_policy_and_matched(std::string source,
                                                             std::vector<Filter *> *matched_filters) {
    static Filter *match_all[FLATPAK_POLICY_OWN + 1] = {
            nullptr,
            new Filter("", false, FLATPAK_POLICY_SEE),
            new Filter("", false, FLATPAK_POLICY_TALK),
            new Filter("", false, FLATPAK_POLICY_OWN)
    };
    if (source.empty()) {
        if (matched_filters) matched_filters->push_back(match_all[FLATPAK_POLICY_TALK]);
        return FLATPAK_POLICY_TALK;
    }
    FlatpakPolicy max_policy = FLATPAK_POLICY_NONE;

    if (source[0] == ':') {
        auto it = unique_id_policy.find(source);
        if (it != unique_id_policy.end())
            max_policy = static_cast<FlatpakPolicy>(it->second);
        if (matched_filters && max_policy > FLATPAK_POLICY_NONE)
            matched_filters->push_back(match_all[max_policy]);

        auto owned = unique_id_owned_names.find(source);
        if (owned != unique_id_owned_names.end()) {
            for (const std::string &name: owned->second)
                max_policy = std::max(max_policy, get_max_policy_and_matched(name, matched_filters));
        }
        return max_policy;
    }
    std::string name = source;
    bool exact_match = true;
    while (true) {
        auto it = proxy->filters.find(name);
        if (it != proxy->filters.end()) {
            for (auto *filter: it->second) {
                if (exact_match || filter->name_is_subtree) {
                    max_policy = std::max(max_policy, filter->policy);
                    if (matched_filters) matched_filters->push_back(filter);
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