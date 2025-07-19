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

    GIOStream* stream = g_dbus_address_get_stream_finish(res, NULL, &error);
    if (!stream) {
        std::cerr << "Failed to connect to bus: " << error->message<<"\n";
        if (error) g_error_free(error);
        delete client;
        return;
    }

    GSocketConnection *connection = G_SOCKET_CONNECTION (stream);
    g_socket_set_blocking(g_socket_connection_get_socket(connection), FALSE);
    client->bus_side->connection = connection;

    client->client_side->start_reading();
    client->bus_side->start_reading();
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
        client_side(new ProxySide(this, false)),
        bus_side(new ProxySide(this, true)) {

    client_side->connection = G_SOCKET_CONNECTION(g_object_ref(client_conn->gobj()));
    proxy->clients.push_back(this);
    last_fake_serial = MAX_CLIENT_SERIAL;
}

FlatpakProxyClient::~FlatpakProxyClient() {
    if (proxy) {
        proxy->clients.remove(this);
    }

    delete client_side;
    delete bus_side;

    // todo replace GDBusMessage
    for (auto &[_, msg]: rewrite_reply) {
        g_object_unref(msg);
    }
    rewrite_reply.clear();
    get_owner_reply.clear();
    unique_id_policy.clear();
    unique_id_owned_names.clear();
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

